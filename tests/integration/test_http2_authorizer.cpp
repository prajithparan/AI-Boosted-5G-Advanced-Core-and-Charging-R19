// ADR-0499 phase 1: http2::Server runs its Authorizer before an OAuth2 route's handler and never
// for a None route (notifications, TS 33.501 13.4.1.0). In-process server on an OS-chosen port.
#include "sbi_core/authorizer.hpp"
#include "sbi_core/http2_client.hpp"
#include "sbi_core/http2_server.hpp"
#include "sbi_core/jwt.hpp"
#include "sbi_core/oauth2_client.hpp"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>

#include <gtest/gtest.h>

namespace {

constexpr const char* kNrfId = "6fa459ea-ee8a-3ca4-894e-db77e160355e";

sbi_core::http2::TlsConfig test_tls() {
    return sbi_core::http2::TlsConfig{
        .cert_path = CERTS_DIR "/hello-nf/cert.pem",
        .key_path = CERTS_DIR "/hello-nf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
}

void write_pem(EVP_PKEY* key, bool is_private, const std::filesystem::path& path) {
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free);
    if (is_private) {
        PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0, nullptr, nullptr);
    } else {
        PEM_write_bio_PUBKEY(bio.get(), key);
    }
    char* data = nullptr;
    const long len = BIO_get_mem_data(bio.get(), &data);
    std::ofstream(path, std::ios::binary).write(data, len);
}

TEST(Http2Authorizer, TheServerChecksProtectedRoutesBeforeTheHandlerAndSkipsNotifications) {
    const auto dir =
        std::filesystem::temp_directory_path() / ("http2_authz_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(EVP_EC_gen("P-256"), EVP_PKEY_free);
    ASSERT_NE(key, nullptr);
    write_pem(key.get(), true, dir / "priv.pem");
    write_pem(key.get(), false, dir / "pub.pem");

    boost::asio::io_context ioc;
    sbi_core::http2::Server server(ioc, "127.0.0.1", 0, test_tls());
    std::atomic<int> protected_calls{0};
    server.add_route("POST", "/npcf-am-policy-control/v1/policies", [&](const auto&) {
        ++protected_calls;
        return sbi_core::http2::Response::json(201, "{}");
    });
    server.add_route(
        "POST",
        "/callbacks/notify",
        [](const auto&) { return sbi_core::http2::Response::json(204, ""); },
        sbi_core::http2::RouteAuth::None);
    server.set_authorizer(std::make_shared<const sbi_core::http2::Authorizer>(
        sbi_core::http2::AuthorizerConfig{.required = true, .nf_type = "PCF"},
        std::make_shared<sbi_core::jwt::Verifier>((dir / "pub.pem").string(), kNrfId)));
    server.start();
    const auto base = "https://127.0.0.1:" + std::to_string(server.local_port());
    auto guard = boost::asio::make_work_guard(ioc);
    std::thread runner([&ioc] { ioc.run(); });

    sbi_core::http2::Client client(test_tls());
    const auto post = [&](const std::string& path, const std::string& token) {
        sbi_core::http2::ClientRequest req;
        req.method = "POST";
        req.url = base + path;
        req.body = "{}";
        req.headers.emplace("content-type", "application/json");
        if (!token.empty()) {
            req.headers.emplace("authorization", "Bearer " + token);
        }
        return client.send(req);
    };

    const auto no_token = post("/npcf-am-policy-control/v1/policies", "");
    ASSERT_TRUE(no_token.has_value());
    EXPECT_EQ(no_token->status, 401);
    EXPECT_EQ(no_token->headers.count("www-authenticate"), 1U);
    EXPECT_EQ(protected_calls.load(), 0) << "a rejected request must not reach the handler";

    const auto notify = post("/callbacks/notify", "");
    ASSERT_TRUE(notify.has_value());
    EXPECT_EQ(notify->status, 204);

    sbi_core::jwt::Issuer issuer((dir / "priv.pem").string(), kNrfId);
    const auto token = issuer.issue("1d2c3b4a-0000-4000-8000-000000000001",
                                    "PCF",
                                    "npcf-am-policy-control",
                                    std::chrono::seconds(60));
    const auto ok = post("/npcf-am-policy-control/v1/policies", token.token);
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(ok->status, 201);
    EXPECT_EQ(protected_calls.load(), 1);

    guard.reset();
    ioc.stop();
    runner.join();
    std::filesystem::remove_all(dir);
}

// ADR-0499 phase 3: a client with a TokenSource attaches the token of each request's API (scope
// and targetNfType from the YAML catalog), caches it, never overrides a caller's own header, and
// sends without a token when the NRF refuses one (the NFRegister bootstrap).
TEST(Http2Authorizer, AClientWithATokenSourceSendsEachApisTokenAndFallsBackWithoutOne) {
    boost::asio::io_context ioc;
    sbi_core::http2::Server server(ioc, "127.0.0.1", 0, test_tls());
    std::atomic<int> token_requests{0};
    std::atomic<bool> refuse{false};
    std::string last_form;
    std::mutex form_mutex;
    server.add_route("POST", "/oauth2/token", [&](const sbi_core::http2::Request& req) {
        ++token_requests;
        {
            std::lock_guard lock(form_mutex);
            last_form = req.body;
        }
        if (refuse) {
            return sbi_core::http2::Response::json(400, R"({"error":"invalid_client"})");
        }
        return sbi_core::http2::Response::json(
            200, R"({"access_token":"T1","token_type":"Bearer","expires_in":60})");
    });
    const auto echo = [](const sbi_core::http2::Request& req) {
        const auto it = req.headers.find("authorization");
        return sbi_core::http2::Response::json(200, it == req.headers.end() ? "none" : it->second);
    };
    server.add_route("GET", "/npcf-am-policy-control/v1/policies/x", echo);
    server.add_route("GET", "/nnrf-nfm/v1/nf-instances/x", echo);
    server.add_route("GET", "/udr-oam/v1/x", echo);
    server.start();
    const auto base = "https://127.0.0.1:" + std::to_string(server.local_port());
    auto guard = boost::asio::make_work_guard(ioc);
    std::thread runner([&ioc] { ioc.run(); });

    sbi_core::http2::Client client(test_tls());
    sbi_core::TokenSource tokens(client,
                                 base + "/oauth2/token",
                                 "1d2c3b4a-0000-4000-8000-000000000001",
                                 std::chrono::seconds(60));
    sbi_core::attach_tokens(client, tokens);
    const auto get = [&](const std::string& path, std::optional<std::string> auth = std::nullopt) {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = base + path;
        if (auth) {
            req.headers.emplace("authorization", *auth);
        }
        const auto resp = client.send(req);
        return resp.has_value() ? resp->body : "<no response>";
    };

    EXPECT_EQ(get("/npcf-am-policy-control/v1/policies/x"), "Bearer T1");
    {
        std::lock_guard lock(form_mutex);
        EXPECT_NE(last_form.find("scope=npcf-am-policy-control"), std::string::npos) << last_form;
        EXPECT_NE(last_form.find("targetNfType=PCF"), std::string::npos) << last_form;
    }
    EXPECT_EQ(get("/npcf-am-policy-control/v1/policies/x"), "Bearer T1");
    EXPECT_EQ(token_requests.load(), 1) << "the token is cached";
    EXPECT_EQ(get("/npcf-am-policy-control/v1/policies/x", "Bearer mine"), "Bearer mine");
    EXPECT_EQ(get("/udr-oam/v1/x"), "none") << "no catalog entry: no token";

    refuse = true;
    EXPECT_EQ(get("/nnrf-nfm/v1/nf-instances/x"), "none") << "refused: sent without a token";
    EXPECT_EQ(get("/nnrf-nfm/v1/nf-instances/x"), "none");
    EXPECT_EQ(token_requests.load(), 2) << "a refusal is not retried within retry_after";

    // ADR-0499 phase 5: once registered, the NF clears the back-off and asks again at once.
    refuse = false;
    tokens.clear_backoff();
    EXPECT_EQ(get("/nnrf-nfm/v1/nf-instances/x"), "Bearer T1");
    EXPECT_EQ(token_requests.load(), 3);

    guard.reset();
    ioc.stop();
    runner.join();
}

// ADR-0499 phase 3: the process-wide default reaches every Client without a source of its own,
// and a Client's own source still wins.
TEST(Http2Authorizer, TheDefaultTokenSourceReachesEveryClientWithoutItsOwn) {
    boost::asio::io_context ioc;
    sbi_core::http2::Server server(ioc, "127.0.0.1", 0, test_tls());
    std::atomic<int> token_requests{0};
    server.add_route("POST", "/oauth2/token", [&](const sbi_core::http2::Request&) {
        ++token_requests;
        return sbi_core::http2::Response::json(
            200, R"({"access_token":"D1","token_type":"Bearer","expires_in":60})");
    });
    server.add_route("GET", "/npcf-am-policy-control/v1/policies/x", [](const auto& req) {
        const auto it = req.headers.find("authorization");
        return sbi_core::http2::Response::json(200, it == req.headers.end() ? "none" : it->second);
    });
    server.start();
    const auto base = "https://127.0.0.1:" + std::to_string(server.local_port());
    auto guard = boost::asio::make_work_guard(ioc);
    std::thread runner([&ioc] { ioc.run(); });

    sbi_core::install_default_token_source(
        test_tls(), base + "/oauth2/token", "1d2c3b4a-0000-4000-8000-000000000002");
    sbi_core::http2::Client plain(test_tls());
    sbi_core::http2::Client own(test_tls());
    own.set_token_source([](const std::string&) { return std::optional<std::string>("O1"); });
    const auto get = [&](sbi_core::http2::Client& client) {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = base + "/npcf-am-policy-control/v1/policies/x";
        const auto resp = client.send(req);
        return resp.has_value() ? resp->body : "<no response>";
    };

    EXPECT_EQ(get(plain), "Bearer D1");
    EXPECT_EQ(get(own), "Bearer O1");
    EXPECT_EQ(token_requests.load(), 1) << "the token endpoint itself is sent without a token";

    sbi_core::http2::Client::set_default_token_source({});
    EXPECT_EQ(get(plain), "none");

    guard.reset();
    ioc.stop();
    runner.join();
}

} // namespace
