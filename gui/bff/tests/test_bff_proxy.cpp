// oam-gui-bff proxy tests (ADR-0422). Real TLS 1.3 + mTLS on both legs, a throwaway two-root PKI
// generated at test time (make_test_pki.sh), a fake upstream standing in for product-catalog and
// provisioning, and the BFF's real register_routes(). Each server runs on its own io_context and
// thread: the BFF's handler blocks in Client::send while the upstream answers, so sharing one
// single-threaded loop would deadlock.

#include "bff.hpp"

#include "sbi_core/http2_client.hpp"
#include "sbi_core/http2_server.hpp"

#include <boost/asio/io_context.hpp>
#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#ifndef TEST_PKI_SCRIPT
#error "TEST_PKI_SCRIPT must be defined by CMake"
#endif
#ifndef TEST_WORK_DIR
#error "TEST_WORK_DIR must be defined by CMake"
#endif

namespace {

namespace fs = std::filesystem;
using sbi_core::http2::Client;
using sbi_core::http2::ClientRequest;
using sbi_core::http2::Request;
using sbi_core::http2::Response;
using sbi_core::http2::Server;
using sbi_core::http2::TlsConfig;

// Unusual loopback ports, away from every config/*.json port (7777-7810) and the 199xx range the
// integration tests use.
constexpr unsigned short kUpstreamPort = 47831;
constexpr unsigned short kBffPort = 47832;
constexpr unsigned short kDeadPort = 47833; // nothing listens here

// A recognisable K / OPc that must never appear in any log line.
constexpr const char* kSecretK = "0123456789ABCDEF0123456789ABCDEF";
constexpr const char* kSecretOpc = "FEDCBA9876543210FEDCBA9876543210";

struct Seen {
    std::string method, path, body, content_type, x_requested_by, cookie;
};

class RunningServer {
public:
    RunningServer(unsigned short port, TlsConfig tls) : server_(ioc_, "127.0.0.1", port, tls) {}
    Server& server() { return server_; }
    void start() {
        server_.start();
        for (int i = 0; i < 2; ++i) threads_.emplace_back([this] { ioc_.run(); });
    }
    ~RunningServer() {
        ioc_.stop();
        for (auto& t : threads_) t.join();
    }

private:
    boost::asio::io_context ioc_;
    Server server_;
    std::vector<std::thread> threads_;
};

class BffProxyTest : public ::testing::Test {
protected:
    static inline fs::path pki;
    static void SetUpTestSuite() {
        pki = fs::path(TEST_WORK_DIR) / "pki";
        const std::string cmd = std::string("bash ") + TEST_PKI_SCRIPT + " " + pki.string();
        ASSERT_EQ(std::system(cmd.c_str()), 0) << "test PKI generation failed";
    }

    static TlsConfig tls(const std::string& leaf, const std::string& ca) {
        return TlsConfig{(pki / leaf / "cert.pem").string(), (pki / leaf / "key.pem").string(),
                         (pki / (ca + ".crt")).string()};
    }

    void SetUp() override {
        // Capture every log line at the most verbose level, so "not logged" is proven at trace.
        log_stream_ = std::make_shared<std::ostringstream>();
        auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(*log_stream_);
        auto logger = std::make_shared<spdlog::logger>("bff-test", sink);
        logger->set_level(spdlog::level::trace);
        previous_logger_ = spdlog::default_logger();
        spdlog::set_default_logger(logger);

        // Static app fixture.
        static_dir_ = fs::path(TEST_WORK_DIR) / "static";
        fs::create_directories(static_dir_ / "assets");
        std::ofstream(static_dir_ / "index.html") << "<!doctype html><title>OAM</title>";
        std::ofstream(static_dir_ / "assets" / "app-1.js") << "console.log(1)";
        std::ofstream(static_dir_ / "secret.txt") << "outside assets";

        upstream_ = std::make_unique<RunningServer>(kUpstreamPort, tls("upstream", "lab"));
        const auto record = [this](const Request& r) {
            std::lock_guard lock(mu_);
            const auto h = [&r](const char* n) {
                const auto it = r.headers.find(n);
                return it == r.headers.end() ? std::string() : it->second;
            };
            seen_.push_back({r.method, r.path, r.body, h("content-type"), h("x-requested-by"),
                             h("cookie")});
            upstream_cn_ = r.peer_cert_cn;
        };
        auto& us = upstream_->server();
        const std::string po = std::string(oam_gui_bff::kTmf620Root) + "/productOffering";
        us.add_route("POST", po, [record, po](const Request& r) {
            record(r);
            Response resp = Response::json(201, r.body);
            resp.headers.emplace("location", po + "/42");
            resp.headers.emplace("set-cookie", "leak=1");
            return resp;
        });
        us.add_route("GET", po, [record](const Request& r) {
            record(r);
            return Response::json(200, R"([{"id":"42","name":"Gold"}])");
        });
        us.add_route("GET", po + "/{id}", [record](const Request& r) {
            record(r);
            return Response::json(200, R"({"id":"42"})");
        });
        const std::string co = std::string(oam_gui_bff::kProvisioningRoot) + "/customerOrder";
        us.add_route("POST", co, [record](const Request& r) {
            record(r);
            // The real service never echoes keys; neither does this stand-in.
            return Response::json(201, R"({"orderId":"ord-1","status":"completed"})");
        });
        upstream_->start();

        start_bff("https://127.0.0.1:" + std::to_string(kUpstreamPort));
    }

    void start_bff(const std::string& upstream_base) {
        bff_.reset();
        bff_client_ = std::make_unique<Client>(tls("bff", "lab"));
        bff_ = std::make_unique<RunningServer>(kBffPort, tls("bff", "operator"));
        oam_gui_bff::Config cfg{upstream_base, upstream_base, static_dir_.string()};
        oam_gui_bff::register_routes(bff_->server(), *bff_client_, cfg,
                                     oam_gui_bff::load_static_files(cfg.static_dir));
        bff_->start();
    }

    void TearDown() override {
        bff_.reset();
        upstream_.reset();
        spdlog::set_default_logger(previous_logger_);
    }

    // The operator's browser, reduced to an mTLS client.
    static tl::expected<sbi_core::http2::ClientResponse, std::string>
    call(const std::string& method, const std::string& path, const std::string& body = "",
         std::multimap<std::string, std::string> headers = {},
         const std::string& identity = "operator-alice") {
        Client browser(tls(identity, "lab"));
        return browser.send(ClientRequest{
            method, "https://127.0.0.1:" + std::to_string(kBffPort) + path, std::move(headers),
            body});
    }
    static std::multimap<std::string, std::string> json_post_headers() {
        return {{"content-type", "application/json"}, {"x-requested-by", "oam-gui"}};
    }
    static std::string hdr(const sbi_core::http2::ClientResponse& r, const std::string& n) {
        const auto it = r.headers.find(n);
        return it == r.headers.end() ? std::string() : it->second;
    }
    std::vector<Seen> seen() {
        std::lock_guard lock(mu_);
        return seen_;
    }

    std::shared_ptr<std::ostringstream> log_stream_;
    std::shared_ptr<spdlog::logger> previous_logger_;
    fs::path static_dir_;
    std::mutex mu_;
    std::vector<Seen> seen_;
    std::string upstream_cn_;
    std::unique_ptr<RunningServer> upstream_;
    std::unique_ptr<Client> bff_client_;
    std::unique_ptr<RunningServer> bff_;
};

TEST_F(BffProxyTest, CreateProductOfferingIsForwardedByteForByte) {
    // Deliberately odd formatting: the BFF must not re-serialise.
    const std::string body = R"({ "name" : "Gold",   "isSellable":true })";
    auto r = call("POST", "/api/tmf620/productOffering", body,
                  {{"content-type", "application/json"},
                   {"x-requested-by", "oam-gui"},
                   {"cookie", "session=abc"}});
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 201);
    EXPECT_EQ(r->body, body);
    // Location rewritten from the service's TMF620 path to the browser's path.
    EXPECT_EQ(hdr(*r, "location"), "/api/tmf620/productOffering/42");
    EXPECT_EQ(hdr(*r, "cache-control"), "no-store");
    EXPECT_TRUE(hdr(*r, "set-cookie").empty()) << "upstream headers are allow-listed, not copied";

    const auto s = seen();
    ASSERT_EQ(s.size(), 1u);
    EXPECT_EQ(s[0].method, "POST");
    EXPECT_EQ(s[0].path, "/tmf-api/productCatalogManagement/v4/productOffering");
    EXPECT_EQ(s[0].body, body);
    EXPECT_EQ(s[0].content_type, "application/json");
    EXPECT_TRUE(s[0].x_requested_by.empty()) << "browser-only headers must not travel upstream";
    EXPECT_TRUE(s[0].cookie.empty());
    // The service sees the BFF's own lab identity, never the operator's.
    EXPECT_EQ(upstream_cn_, "bff");
}

TEST_F(BffProxyTest, ListAndGetAreForwarded) {
    auto list = call("GET", "/api/tmf620/productOffering");
    ASSERT_TRUE(list.has_value()) << list.error();
    EXPECT_EQ(list->status, 200);
    EXPECT_EQ(list->body, R"([{"id":"42","name":"Gold"}])");
    auto one = call("GET", "/api/tmf620/productOffering/42");
    ASSERT_TRUE(one.has_value());
    EXPECT_EQ(one->status, 200);
    const auto s = seen();
    ASSERT_EQ(s.size(), 2u);
    EXPECT_EQ(s[1].path, "/tmf-api/productCatalogManagement/v4/productOffering/42");
}

TEST_F(BffProxyTest, PostWithoutCsrfHeaderIsRefusedAndNeverForwarded) {
    auto r = call("POST", "/api/tmf620/productOffering", R"({"name":"x"})",
                  {{"content-type", "application/json"}});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 403);
    EXPECT_TRUE(seen().empty());
}

TEST_F(BffProxyTest, PostWithFormContentTypeIsRefused) {
    // What a cross-site <form> could send without a preflight.
    auto r = call("POST", "/api/provisioning/customerOrder", "supi=imsi-1",
                  {{"content-type", "application/x-www-form-urlencoded"},
                   {"x-requested-by", "oam-gui"}});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 415);
    EXPECT_TRUE(seen().empty());
}

TEST_F(BffProxyTest, OnlyAllowListedRoutesExist) {
    // DELETE exists on the service but is not exposed; productSpecification is not a screen;
    // an arbitrary upstream path cannot be reached through the BFF.
    for (const auto& [m, p] : std::vector<std::pair<std::string, std::string>>{
             {"DELETE", "/api/tmf620/productOffering/42"},
             {"GET", "/api/tmf620/productSpecification"},
             {"GET", "/tmf-api/productCatalogManagement/v4/productOffering"},
             {"GET", "/api/provisioning/customerOrder"},
             {"OPTIONS", "/api/tmf620/productOffering"}}) {
        auto r = call(m, p);
        ASSERT_TRUE(r.has_value()) << m << " " << p;
        EXPECT_EQ(r->status, 404) << m << " " << p;
    }
    EXPECT_TRUE(seen().empty());
}

TEST_F(BffProxyTest, UnsafeIdIsRejectedNotEncoded) {
    // Percent-encoded separators reach the handler as-is and are refused there.
    for (const std::string id : {"a%2Fb", "a%3Fx=1", "a%00b"}) {
        auto r = call("GET", "/api/tmf620/productOffering/" + id);
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(r->status, 400) << id;
    }
    // Dot segments are normalised away by the client (RFC 3986 §5.2.4) before they are sent, so
    // they land on no route at all; either way nothing is forwarded.
    for (const std::string id : {"..", "%2e%2e"}) {
        auto r = call("GET", "/api/tmf620/productOffering/" + id);
        ASSERT_TRUE(r.has_value());
        EXPECT_TRUE(r->status == 400 || r->status == 404) << id << " -> " << r->status;
    }
    EXPECT_TRUE(seen().empty());
    EXPECT_FALSE(oam_gui_bff::is_safe_id(".."));
    EXPECT_TRUE(oam_gui_bff::is_safe_id("ord-imsi_999.70~1"));
    EXPECT_FALSE(oam_gui_bff::is_safe_id(""));
    EXPECT_FALSE(oam_gui_bff::is_safe_id("a/b"));
}

TEST_F(BffProxyTest, SimKeysAreNeverLoggedEvenAtTrace) {
    const std::string body = std::string(R"({"supi":"imsi-999700000000001","sim":{"k":")") +
                             kSecretK + R"(","opc":")" + kSecretOpc + R"("}})";
    auto r = call("POST", "/api/provisioning/customerOrder", body, json_post_headers());
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 201);
    ASSERT_EQ(seen().size(), 1u);
    EXPECT_EQ(seen()[0].body, body) << "keys still reach the service unchanged";

    // Also the failure path: the same order against a dead upstream.
    start_bff("https://127.0.0.1:" + std::to_string(kDeadPort));
    auto dead = call("POST", "/api/provisioning/customerOrder", body, json_post_headers());
    ASSERT_TRUE(dead.has_value());
    EXPECT_EQ(dead->status, 502);
    EXPECT_EQ(dead->body.find(kSecretK), std::string::npos);

    spdlog::default_logger()->flush();
    const std::string logs = log_stream_->str();
    ASSERT_NE(logs.find("oam-gui-bff: operator='operator-alice' POST /api/provisioning/customerOrder"),
              std::string::npos)
        << "the capture must actually contain the access log, or this test proves nothing:\n"
        << logs;
    EXPECT_EQ(logs.find(kSecretK), std::string::npos) << logs;
    EXPECT_EQ(logs.find(kSecretOpc), std::string::npos) << logs;
    EXPECT_EQ(logs.find("0123456789abcdef"), std::string::npos);
}

TEST_F(BffProxyTest, UpstreamDownIsA502ProblemDetails) {
    start_bff("https://127.0.0.1:" + std::to_string(kDeadPort));
    auto r = call("GET", "/api/tmf620/productOffering");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 502);
    EXPECT_EQ(hdr(*r, "content-type"), "application/problem+json");
}

TEST_F(BffProxyTest, AnNfCertificateCannotOpenTheGui) {
    // rogue-nf is signed by the lab (service) CA: valid towards every NF, but not an operator.
    auto r = call("GET", "/api/tmf620/productOffering", "", {}, "rogue-nf");
    EXPECT_FALSE(r.has_value()) << "handshake must fail, got HTTP " << r->status;
    EXPECT_TRUE(seen().empty());
}

TEST_F(BffProxyTest, StaticAppIsServedWithCspAndNothingElseOnDisk) {
    auto index = call("GET", "/");
    ASSERT_TRUE(index.has_value());
    EXPECT_EQ(index->status, 200);
    EXPECT_NE(index->body.find("<title>OAM</title>"), std::string::npos);
    EXPECT_NE(hdr(*index, "content-security-policy").find("default-src 'self'"),
              std::string::npos);
    EXPECT_EQ(hdr(*index, "x-frame-options"), "DENY");

    auto js = call("GET", "/assets/app-1.js");
    ASSERT_TRUE(js.has_value());
    EXPECT_EQ(js->status, 200);
    EXPECT_EQ(hdr(*js, "content-type"), "text/javascript; charset=utf-8");

    for (const std::string p : {"/assets/..%2Fsecret.txt", "/assets/../secret.txt",
                                "/secret.txt", "/assets/missing.js"}) {
        auto r = call("GET", p);
        ASSERT_TRUE(r.has_value()) << p;
        EXPECT_EQ(r->status, 404) << p;
        EXPECT_EQ(r->body.find("outside assets"), std::string::npos) << p;
    }
}

} // namespace
