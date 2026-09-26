// oam-gui-bff security tests (ADR-0423, ADR-0424, ADR-0425).
//
// Everything real except the far ends: real TLS 1.3 + mTLS on every leg (two independent roots,
// make_test_pki.sh), the real route table, the real IamStore against a PRIVATE PostgreSQL database
// built from deploy/db/operator_iam/*.sql (dropped afterwards), a fake OIDC IdP that signs real
// ES256 ID tokens, and a fake upstream standing in for product-catalog + provisioning. The BFF
// connects to the database as the least-privileged `oam_gui_bff` role, exactly as deployed.
//
// What the owner required these tests to prove:
//   * an agent cannot see or create customers / orders outside their shop;
//   * a maker cannot approve their own request (BFF and DB both refuse);
//   * SIM keys never appear in any response, log line, audit row or stored payload;
//   * every allowed AND denied action lands in the audit trail.
//
// Ports: 2874x on loopback -- outside the 7700-7899 / 9400-9499 ranges the NFs and CI use, and
// below the kernel ephemeral range (32768+), where an outgoing connection can squat a port.

#include "auth.hpp"
#include "bff.hpp"
#include "config_mgmt.hpp"
#include "iam.hpp"
#include "util.hpp"

#include "sbi_core/http2_client.hpp"
#include "sbi_core/http2_server.hpp"

#include <boost/asio/io_context.hpp>
#include <gtest/gtest.h>
#include <jwt-cpp/jwt.h>
#include <jwt-cpp/traits/nlohmann-json/traits.h>
#include <pqxx/pqxx>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef TEST_PKI_SCRIPT
#error "TEST_PKI_SCRIPT must be defined by CMake"
#endif

namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using sbi_core::http2::Client;
using sbi_core::http2::ClientRequest;
using sbi_core::http2::ClientResponse;
using sbi_core::http2::Request;
using sbi_core::http2::Response;
using sbi_core::http2::Server;
using sbi_core::http2::TlsConfig;
using namespace oam_gui_bff;

constexpr unsigned short kUpstreamPort = 28741;
constexpr unsigned short kIdpPort = 28742;
constexpr unsigned short kBffPort = 28743;
constexpr unsigned short kDeadPort = 28744;

constexpr const char* kIssuer = "https://127.0.0.1:28742/realm";
constexpr const char* kSecretK = "0123456789ABCDEF0123456789ABCDEF";
constexpr const char* kSecretOpc = "FEDCBA9876543210FEDCBA9876543210";

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

class RunningServer {
public:
    RunningServer(unsigned short port, TlsConfig tls) : server_(ioc_, "127.0.0.1", port, tls) {}
    Server& server() { return server_; }
    void start() {
        server_.start();
        for (int i = 0; i < 3; ++i) threads_.emplace_back([this] { ioc_.run(); });
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

// ---- the private database ----------------------------------------------------------------------

struct Db {
    std::string admin_base; // postgresql://<superuser>@host:port/
    std::string name;
    std::string admin_url() const { return admin_base + name; }
    std::string bff_url() const {
        return std::regex_replace(admin_base, std::regex("//[^@/]+@"), "//oam_gui_bff@") + name;
    }
};

Db make_db() {
    const char* env = std::getenv("TEST_POSTGRES_URL"); // CI: postgresql://postgres@localhost:15434/charging
    const std::string url = env ? env : "postgresql://postgres@127.0.0.1:5434/postgres";
    std::smatch m;
    const std::regex re("^(postgres(?:ql)?://[^/]+/)[^?]*$");
    if (!std::regex_match(url, m, re)) throw std::runtime_error("unparseable TEST_POSTGRES_URL");
    Db db{m[1].str(), "oam_iam_test_" + std::to_string(::getpid())};
    pqxx::connection admin(db.admin_base + "postgres");
    pqxx::nontransaction n(admin);
    n.exec("DROP DATABASE IF EXISTS " + db.name);
    n.exec("CREATE DATABASE " + db.name);
    pqxx::connection c(db.admin_url());
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(IAM_DDL_DIR)) files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
        if (f.extension() != ".sql") continue;
        pqxx::nontransaction t(c);
        t.exec(slurp(f)); // multi-statement, simple-query protocol
    }
    // Test organisation: hq > retail > dealer1 > {shop-a, shop-b}; plus back office.
    pqxx::nontransaction t(c);
    t.exec(R"SQL(
        SET search_path = iam;
        INSERT INTO org_unit(id, kind, parent_id, name) VALUES
          ('retail','CHANNEL','hq','Retail'), ('dealer1','DEALER','retail','Dealer 1'),
          ('shop-a','SHOP','dealer1','Shop A'), ('shop-b','SHOP','dealer1','Shop B'),
          ('bo','BACK_OFFICE','hq','Back office');
        INSERT INTO operator_user(id, idp_issuer, idp_subject, username, home_org_unit_id) VALUES
          ('u-agent-a', 'https://127.0.0.1:28742/realm', 'sub-agent-a', 'agent.a', 'shop-a'),
          ('u-agent-b', 'https://127.0.0.1:28742/realm', 'sub-agent-b', 'agent.b', 'shop-b'),
          ('u-super',   'https://127.0.0.1:28742/realm', 'sub-super',   'dealer.super', 'dealer1'),
          ('u-pm',      'https://127.0.0.1:28742/realm', 'sub-pm',      'pm', 'hq'),
          ('u-appr',    'https://127.0.0.1:28742/realm', 'sub-appr',    'approver', 'hq'),
          ('u-both',    'https://127.0.0.1:28742/realm', 'sub-both',    'maker.and.checker', 'hq'),
          ('u-cfg',     'https://127.0.0.1:28742/realm', 'sub-cfg',     'cfg.engineer', 'hq'),
          ('u-cfgappr', 'https://127.0.0.1:28742/realm', 'sub-cfgappr', 'cfg.approver', 'hq');
        INSERT INTO operator_user(id, idp_issuer, idp_subject, username, home_org_unit_id,
                                  last_login_at) VALUES
          ('u-dormant', 'https://127.0.0.1:28742/realm', 'sub-dormant', 'dormant', 'shop-a',
           now() - interval '400 days');
        INSERT INTO role_assignment(id, user_id, role_id, org_unit_id) VALUES
          ('ra1','u-agent-a','shop_agent','shop-a'), ('ra2','u-agent-b','shop_agent','shop-b'),
          ('ra3','u-super','shop_supervisor','dealer1'), ('ra4','u-pm','product_manager','hq'),
          ('ra5','u-appr','catalog_approver','hq'),
          -- u-both violates sod-catalog on purpose: the SoD check at grant time is a later
          -- increment, so this proves the four-eyes rule holds even for such a user.
          ('ra6','u-both','product_manager','hq'), ('ra7','u-both','catalog_approver','hq'),
          ('ra8','u-cfg','network_config_engineer','hq'),
          ('ra9','u-cfgappr','network_config_approver','hq'),
          ('ra10','u-dormant','shop_agent','shop-a');
    )SQL");
    return db;
}

void drop_db(const Db& db) {
    try {
        pqxx::connection admin(db.admin_base + "postgres");
        pqxx::nontransaction n(admin);
        n.exec("SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname = '" +
               db.name + "' AND pid <> pg_backend_pid()");
        n.exec("DROP DATABASE IF EXISTS " + db.name);
    } catch (const std::exception&) {
    }
}

// ---- fixture -----------------------------------------------------------------------------------

class BffSecurity : public ::testing::Test {
protected:
    static inline fs::path pki;
    static inline Db db;
    static inline fs::path config_dir;

    static void SetUpTestSuite() {
        pki = fs::path(TEST_WORK_DIR) / "pki";
        const std::string cmd = std::string("bash ") + TEST_PKI_SCRIPT + " " + pki.string();
        ASSERT_EQ(std::system(cmd.c_str()), 0);
        db = make_db();
        config_dir = fs::path(TEST_WORK_DIR) / "nf-config";
        fs::create_directories(config_dir);
    }
    static void TearDownTestSuite() { drop_db(db); }

    static TlsConfig tls(const std::string& leaf, const std::string& ca) {
        return TlsConfig{(pki / leaf / "cert.pem").string(), (pki / leaf / "key.pem").string(),
                         (pki / (ca + ".crt")).string()};
    }

    // ----- fake IdP -----
    std::mutex idp_mu_;
    std::string expected_challenge_, next_nonce_, next_sub_;
    json next_amr_ = json::array({"pwd", "otp"});
    std::string tamper_nonce_;

    std::string sign_id_token() {
        const auto priv = slurp(pki / "idp-sign" / "key.pem");
        const auto pub = slurp(pki / "idp-sign" / "pub.pem");
        const auto now = std::chrono::system_clock::now();
        auto b = jwt::create<jwt::traits::nlohmann_json>()
                     .set_type("JWT")
                     .set_key_id("k1")
                     .set_issuer(kIssuer)
                     .set_audience("oam-gui")
                     .set_subject(next_sub_)
                     .set_issued_at(now)
                     .set_expires_at(now + std::chrono::minutes(5))
                     .set_payload_claim("nonce", jwt::basic_claim<jwt::traits::nlohmann_json>(
                                                     tamper_nonce_.empty() ? next_nonce_
                                                                           : tamper_nonce_))
                     .set_payload_claim("sid", jwt::basic_claim<jwt::traits::nlohmann_json>(
                                                   std::string("idp-sid-1")))
                     .set_payload_claim("amr",
                                        jwt::basic_claim<jwt::traits::nlohmann_json>(next_amr_));
        return b.sign(jwt::algorithm::es256(pub, priv, "", ""));
    }

    json jwks() {
        const auto der = slurp(pki / "idp-sign" / "pub.der");
        const std::string xy = der.substr(der.size() - 64); // uncompressed point tail: X || Y
        return json{{"keys", json::array({{{"kty", "EC"}, {"crv", "P-256"}, {"kid", "k1"},
                                           {"use", "sig"}, {"alg", "ES256"},
                                           {"x", base64url(xy.substr(0, 32))},
                                           {"y", base64url(xy.substr(32))}}})}};
    }

    // ----- fake upstream (product-catalog + provisioning) -----
    struct Seen {
        std::string method, path, body;
    };
    std::mutex up_mu_;
    std::vector<Seen> seen_;

    void SetUp() override {
        log_ = std::make_shared<std::ostringstream>();
        auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(*log_);
        auto logger = std::make_shared<spdlog::logger>("bff-sec", sink);
        logger->set_level(spdlog::level::trace);
        prev_logger_ = spdlog::default_logger();
        spdlog::set_default_logger(logger);

        std::ofstream(config_dir / "product-catalog.json")
            << json{{"port", 7785},
                    {"db_pool_size", 16},
                    {"advertised_ipv4", "127.0.0.1"},
                    {"metrics_bind_address", "0.0.0.0:9473"},
                    {"database_url", "postgresql://postgres:s3cr3t@127.0.0.1:5434/charging"}}
                   .dump(2);

        static_dir_ = fs::path(TEST_WORK_DIR) / "static";
        fs::create_directories(static_dir_ / "assets");
        std::ofstream(static_dir_ / "index.html") << "<!doctype html><title>OAM</title>";

        upstream_ = std::make_unique<RunningServer>(kUpstreamPort, tls("upstream", "lab"));
        auto& us = upstream_->server();
        const auto rec = [this](const Request& r) {
            std::lock_guard l(up_mu_);
            seen_.push_back({r.method, r.path, r.body});
        };
        us.add_route("POST", std::string(kTmf620Root) + "/productOffering", [rec](const Request& r) {
            rec(r);
            auto j = json::parse(r.body);
            j["id"] = "po-1";
            return Response::json(201, j.dump());
        });
        us.add_route("POST", std::string(kProvisioningRoot) + "/customerOrder",
                     [rec](const Request& r) {
                         rec(r);
                         const auto in = json::parse(r.body);
                         const std::string supi = in.value("supi", "");
                         std::string d;
                         for (char ch : supi) if (ch >= '0' && ch <= '9') d += ch;
                         // A deliberately LEAKY stand-in: it echoes the whole request back
                         // (keys included). The real service never does; the BFF must still
                         // keep the keys off the screen and out of the audit trail.
                         return Response::json(
                             201, json{{"orderId", "ord-" + in.value("idempotencyKey", "")},
                                       {"accountId", "acc-" + d},
                                       {"subscriberId", "sub-" + d},
                                       {"supi", supi},
                                       {"msisdn", in.value("msisdn", "")},
                                       {"status", "completed"},
                                       {"echo", in}}
                                      .dump());
                     });
        us.add_route("GET", std::string(kProvisioningRoot) + "/customerOrder/{id}",
                     [rec](const Request& r) {
                         rec(r);
                         return Response::json(200, json{{"id", r.path_params.at("id")},
                                                         {"state", "completed"},
                                                         {"customerId", "acc-999700000000001"}}
                                                        .dump());
                     });
        upstream_->start();

        idp_ = std::make_unique<RunningServer>(kIdpPort, tls("idp", "lab"));
        idp_->server().add_route("GET", "/realm/certs", [this](const Request&) {
            return Response::json(200, jwks().dump());
        });
        idp_->server().add_route("POST", "/realm/token", [this](const Request& r) {
            std::lock_guard l(idp_mu_);
            // Parse the form minimally and enforce PKCE the way a real IdP does.
            std::map<std::string, std::string> f;
            std::stringstream ss(r.body);
            std::string kv;
            while (std::getline(ss, kv, '&')) {
                const auto eq = kv.find('=');
                f[kv.substr(0, eq)] = kv.substr(eq + 1);
            }
            if (f["code"] != "code-ok" || f["client_secret"] != "test-secret" ||
                pkce_challenge(f["code_verifier"]) != expected_challenge_) {
                return Response::json(400, R"({"error":"invalid_grant"})");
            }
            return Response::json(200, json{{"id_token", sign_id_token()},
                                            {"access_token", "opaque"},
                                            {"token_type", "Bearer"}}
                                           .dump());
        });
        idp_->start();

        start_bff("https://127.0.0.1:" + std::to_string(kUpstreamPort));
    }

    void start_bff(const std::string& upstream) {
        bff_.reset();
        iam_ = std::make_unique<IamStore>(db.bff_url(), 4, "test-chain");
        iam_->ensure_audit_partitions();
        OidcConfig oc;
        oc.issuer = kIssuer;
        oc.authorization_endpoint = std::string(kIssuer) + "/auth";
        oc.token_endpoint = std::string(kIssuer) + "/token";
        oc.jwks_uri = std::string(kIssuer) + "/certs";
        oc.client_id = "oam-gui";
        oc.client_secret = "test-secret";
        oc.redirect_uri = "https://127.0.0.1:28743/auth/callback";
        oc.mfa_amr = {"otp"};
        auth_ = std::make_unique<OidcAuthenticator>(oc, *iam_, tls("bff", "lab"));
        configs_ = std::make_unique<NfConfigManager>(config_dir.string(), NF_CONFIG_SCHEMA_DIR,
                                                     std::set<std::string>{"product-catalog"});
        services_ = std::make_unique<Client>(tls("bff", "lab"));
        deps_ = std::make_unique<Deps>(Deps{*services_, *iam_, *auth_, *configs_,
                                            Config{upstream, upstream, static_dir_.string()}});
        bff_ = std::make_unique<RunningServer>(kBffPort, tls("bff", "operator"));
        register_routes(bff_->server(), *deps_, load_static_files(static_dir_.string()));
        bff_->start();
    }

    void TearDown() override {
        bff_.reset();
        idp_.reset();
        upstream_.reset();
        spdlog::set_default_logger(prev_logger_);
    }

    // ----- helpers -----
    tl::expected<ClientResponse, std::string>
    call(const std::string& method, const std::string& path, const std::string& body = "",
         std::multimap<std::string, std::string> headers = {},
         const std::string& terminal = "terminal-shop-a") {
        Client browser(tls(terminal, "lab"));
        auto r = browser.send(ClientRequest{
            method, "https://127.0.0.1:" + std::to_string(kBffPort) + path, std::move(headers),
            body});
        if (r) bodies_.push_back(r->body);
        return r;
    }
    std::multimap<std::string, std::string> as(const std::string& session, bool write = false) {
        std::multimap<std::string, std::string> h{
            {"cookie", std::string(kSessionCookie) + "=" + session}};
        if (write) {
            h.emplace("content-type", "application/json");
            h.emplace("x-requested-by", "oam-gui");
        }
        return h;
    }
    // A session opened directly in the store (the OIDC path has its own tests below).
    std::string session_for(const std::string& subject, const std::string& terminal) {
        const auto u = iam_->find_user(kIssuer, subject);
        EXPECT_TRUE(u.has_value()) << subject;
        return iam_->create_session(*u, "test", "", "", {"otp"}, terminal, "127.0.0.1", "gtest");
    }
    static std::string header(const ClientResponse& r, const std::string& n) {
        const auto it = r.headers.find(n);
        return it == r.headers.end() ? std::string() : it->second;
    }
    static std::string set_cookie_value(const ClientResponse& r, const std::string& name) {
        const auto [lo, hi] = r.headers.equal_range("set-cookie");
        for (auto it = lo; it != hi; ++it) {
            if (it->second.rfind(name + "=", 0) == 0) {
                const auto v = it->second.substr(name.size() + 1);
                return v.substr(0, v.find(';'));
            }
        }
        return {};
    }
    // Audit rows, read as the superuser (the BFF role could too; the auditor screen will).
    std::vector<json> audit_rows(const std::string& where = "true") {
        pqxx::connection c(db.admin_url());
        pqxx::nontransaction t(c);
        std::vector<json> out;
        for (const auto& r : t.exec("SELECT row_to_json(e)::text FROM iam.audit_export e WHERE " +
                                    where + " ORDER BY chain_seq")) {
            out.push_back(json::parse(r[0].as<std::string>()));
        }
        return out;
    }
    std::size_t count_audit(const std::string& where) { return audit_rows(where).size(); }
    std::vector<Seen> seen() {
        std::lock_guard l(up_mu_);
        return seen_;
    }

    std::shared_ptr<std::ostringstream> log_;
    std::shared_ptr<spdlog::logger> prev_logger_;
    fs::path static_dir_;
    std::vector<std::string> bodies_;
    std::unique_ptr<RunningServer> upstream_, idp_, bff_;
    std::unique_ptr<IamStore> iam_;
    std::unique_ptr<OidcAuthenticator> auth_;
    std::unique_ptr<NfConfigManager> configs_;
    std::unique_ptr<Client> services_;
    std::unique_ptr<Deps> deps_;
};

std::map<std::string, std::string> query_of(const std::string& url) {
    std::map<std::string, std::string> q;
    std::stringstream ss(url.substr(url.find('?') + 1));
    std::string kv;
    while (std::getline(ss, kv, '&')) {
        const auto eq = kv.find('=');
        q[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
    return q;
}

// ================================== authentication ==============================================

TEST_F(BffSecurity, OidcLoginWithMfaCreatesATerminalBoundSessionAndIsAudited) {
    auto start = call("GET", "/auth/login");
    ASSERT_TRUE(start.has_value()) << start.error();
    ASSERT_EQ(start->status, 302);
    const auto loc = header(*start, "location");
    ASSERT_EQ(loc.rfind(std::string(kIssuer) + "/auth?", 0), 0u) << loc;
    const auto q = query_of(loc);
    EXPECT_EQ(q.at("code_challenge_method"), "S256");
    const std::string login = set_cookie_value(*start, kLoginCookie);
    ASSERT_FALSE(login.empty());
    {
        std::lock_guard l(idp_mu_);
        expected_challenge_ = q.at("code_challenge");
        next_nonce_ = q.at("nonce");
        next_sub_ = "sub-agent-a";
    }
    auto cb = call("GET", "/auth/callback?code=code-ok&state=" + q.at("state"), "",
                   {{"cookie", std::string(kLoginCookie) + "=" + login}});
    ASSERT_TRUE(cb.has_value());
    ASSERT_EQ(cb->status, 200) << cb->body;
    const std::string session = set_cookie_value(*cb, kSessionCookie);
    ASSERT_FALSE(session.empty());

    auto me = call("GET", "/api/me", "", as(session));
    ASSERT_TRUE(me.has_value());
    ASSERT_EQ(me->status, 200) << [&] {
        std::string d;
        for (const auto& row : audit_rows("outcome = 'DENIED'")) d += row.dump() + "\n";
        return d;
    }();
    const auto j = json::parse(me->body);
    EXPECT_EQ(j["username"], "agent.a");
    EXPECT_EQ(j["orgUnit"], "shop-a");
    EXPECT_EQ(j["terminal"], "terminal-shop-a");

    EXPECT_EQ(count_audit("action = 'auth.login' AND outcome = 'ALLOWED' AND user_id = "
                          "'u-agent-a' AND terminal_cn = 'terminal-shop-a' AND client_ip IS NOT NULL"),
              1u);

    // The same state cannot be replayed.
    auto replay = call("GET", "/auth/callback?code=code-ok&state=" + q.at("state"), "",
                       {{"cookie", std::string(kLoginCookie) + "=" + login}});
    ASSERT_TRUE(replay.has_value());
    EXPECT_EQ(replay->status, 403);

    // The session cookie replayed from ANOTHER terminal is refused and the session is ended.
    auto stolen = call("GET", "/api/me", "", as(session), "terminal-shop-b");
    ASSERT_TRUE(stolen.has_value());
    EXPECT_EQ(stolen->status, 401);
    auto after = call("GET", "/api/me", "", as(session));
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->status, 401) << "a hijack attempt must kill the session everywhere";
    EXPECT_EQ(count_audit("action = 'auth.whoami' AND outcome = 'DENIED' AND reason = "
                          "'session presented from a different terminal'"),
              1u);
}

TEST_F(BffSecurity, LoginWithoutMfaEvidenceIsRefusedAndAudited) {
    auto start = call("GET", "/auth/login");
    ASSERT_TRUE(start.has_value());
    const auto q = query_of(header(*start, "location"));
    {
        std::lock_guard l(idp_mu_);
        expected_challenge_ = q.at("code_challenge");
        next_nonce_ = q.at("nonce");
        next_sub_ = "sub-agent-b";
        next_amr_ = json::array({"pwd"});
    }
    auto cb = call("GET", "/auth/callback?code=code-ok&state=" + q.at("state"), "",
                   {{"cookie", std::string(kLoginCookie) + "=" +
                                   set_cookie_value(*start, kLoginCookie)}});
    ASSERT_TRUE(cb.has_value());
    EXPECT_EQ(cb->status, 403);
    EXPECT_TRUE(set_cookie_value(*cb, kSessionCookie).empty());
    EXPECT_EQ(count_audit("action = 'auth.login' AND outcome = 'DENIED' AND user_id = "
                          "'u-agent-b' AND reason LIKE 'no MFA evidence%'"),
              1u);
}

TEST_F(BffSecurity, NonceMismatchForeignBrowserAndDormantAccountAreRefused) {
    const auto foreign_before = count_audit(
        "action = 'auth.login' AND reason LIKE 'unknown, expired, replayed or foreign-browser%'");
    const auto attempt = [this](const std::string& sub, bool swap_cookie, bool bad_nonce) {
        auto start = call("GET", "/auth/login");
        const auto q = query_of(header(*start, "location"));
        {
            std::lock_guard l(idp_mu_);
            expected_challenge_ = q.at("code_challenge");
            next_nonce_ = q.at("nonce");
            next_sub_ = sub;
            next_amr_ = json::array({"otp"});
            tamper_nonce_ = bad_nonce ? "not-the-nonce" : "";
        }
        const std::string login = swap_cookie ? "another-browser" : set_cookie_value(*start, kLoginCookie);
        return call("GET", "/auth/callback?code=code-ok&state=" + q.at("state"), "",
                    {{"cookie", std::string(kLoginCookie) + "=" + login}});
    };
    EXPECT_EQ(attempt("sub-agent-a", false, true)->status, 403);
    EXPECT_EQ(attempt("sub-agent-a", true, false)->status, 403);
    EXPECT_EQ(attempt("sub-dormant", false, false)->status, 403);
    EXPECT_EQ(attempt("sub-nobody", false, false)->status, 403);
    EXPECT_EQ(count_audit("action = 'auth.login' AND reason = 'id_token nonce mismatch'"), 1u);
    EXPECT_EQ(count_audit("action = 'auth.login' AND reason LIKE 'unknown, expired, replayed or "
                          "foreign-browser%'"),
              foreign_before + 1);
    EXPECT_EQ(count_audit("action = 'auth.login' AND user_id = 'u-dormant' AND reason LIKE "
                          "'account dormant%'"),
              1u);
    EXPECT_EQ(count_audit("action = 'auth.login' AND resource_ref = 'idp-subject/sub-nobody'"), 1u);
    pqxx::connection c(db.admin_url());
    pqxx::nontransaction t(c);
    EXPECT_EQ(t.exec("SELECT status FROM iam.operator_user WHERE id = 'u-dormant'")
                  .one_field()
                  .as<std::string>(),
              "DORMANT");
}

TEST_F(BffSecurity, UnauthenticatedCallsAreDeniedAndAudited) {
    const auto before = count_audit("user_id IS NULL AND outcome = 'DENIED' AND action = "
                                    "'customer_order:create'");
    auto r = call("POST", "/api/provisioning/customerOrder", R"({"supi":"imsi-1"})",
                  {{"content-type", "application/json"}, {"x-requested-by", "oam-gui"}});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 401);
    EXPECT_EQ(count_audit("user_id IS NULL AND outcome = 'DENIED' AND action = "
                          "'customer_order:create' AND terminal_cn = 'terminal-shop-a'"),
              before + 1);
    EXPECT_TRUE(seen().empty());
}

TEST_F(BffSecurity, AnNfCertificateCannotEvenConnect) {
    auto r = call("GET", "/", "", {}, "rogue-nf");
    EXPECT_FALSE(r.has_value()) << "lab-CA (NF) certificates are not operator terminals";
}

// ================================== shop scope ==================================================

TEST_F(BffSecurity, AnAgentCannotSeeOrCreateCustomersOutsideTheirShop) {
    const auto a = session_for("sub-agent-a", "terminal-shop-a");
    const auto b = session_for("sub-agent-b", "terminal-shop-b");
    const std::string order =
        std::string(R"({"supi":"imsi-999700000000001","msisdn":"4670000001","idempotencyKey":"k1",)") +
        R"("sim":{"k":")" + kSecretK + R"(","opc":")" + kSecretOpc + R"("}})";

    auto created = call("POST", "/api/provisioning/customerOrder", order, as(a, true));
    ASSERT_TRUE(created.has_value());
    ASSERT_EQ(created->status, 201) << created->body;
    const auto cj = json::parse(created->body);
    EXPECT_EQ(cj["orderId"], "ord-shop-a.k1") << "idempotency keys are namespaced per unit";
    EXPECT_EQ(cj["supi"], "****************0001") << "PII masked by default";
    EXPECT_EQ(cj["accountId"], "***************0001");

    // Agent B: reading A's order is indistinguishable from a missing one.
    auto peek = call("GET", "/api/provisioning/customerOrder/ord-shop-a.k1", "", as(b),
                     "terminal-shop-b");
    ASSERT_TRUE(peek.has_value());
    EXPECT_EQ(peek->status, 404);
    // Agent B: onboarding the same subscriber (A's customer) is refused before any forwarding.
    const auto forwarded_before = seen().size();
    auto steal = call("POST", "/api/provisioning/customerOrder", order, as(b, true),
                      "terminal-shop-b");
    ASSERT_TRUE(steal.has_value());
    EXPECT_EQ(steal->status, 409);
    EXPECT_EQ(seen().size(), forwarded_before);
    // Agent B reusing A's idempotency key for a different subscriber lands in B's namespace.
    auto own = call("POST", "/api/provisioning/customerOrder",
                    R"({"supi":"imsi-999700000000002","idempotencyKey":"k1","sim":{"k":")" +
                        std::string(kSecretK) + R"(","opc":")" + kSecretOpc + R"("}})",
                    as(b, true), "terminal-shop-b");
    ASSERT_TRUE(own.has_value());
    EXPECT_EQ(json::parse(own->body)["orderId"], "ord-shop-b.k1");

    // Agent A reads their own order; the dealer supervisor (anchored above both shops) too.
    auto mine = call("GET", "/api/provisioning/customerOrder/ord-shop-a.k1", "", as(a));
    ASSERT_TRUE(mine.has_value());
    EXPECT_EQ(mine->status, 200);
    EXPECT_EQ(json::parse(mine->body)["customerId"], "***************0001");
    const auto sup = session_for("sub-super", "terminal-shop-a");
    auto sup_read = call("GET", "/api/provisioning/customerOrder/ord-shop-a.k1", "", as(sup));
    ASSERT_TRUE(sup_read.has_value());
    EXPECT_EQ(sup_read->status, 200);
    // Unmask: agent A lacks pii:unmask (403, audited); the supervisor has it (audited with reason).
    auto a_unmask = call("GET", "/api/provisioning/customerOrder/ord-shop-a.k1", "",
                         [&] { auto h = as(a); h.emplace("x-oam-unmask-reason", "curious"); return h; }());
    EXPECT_EQ(a_unmask->status, 403);
    auto s_unmask = call("GET", "/api/provisioning/customerOrder/ord-shop-a.k1", "",
                         [&] { auto h = as(sup); h.emplace("x-oam-unmask-reason", "ticket 42"); return h; }());
    ASSERT_EQ(s_unmask->status, 200);
    EXPECT_EQ(json::parse(s_unmask->body)["customerId"], "acc-999700000000001");

    // Every one of those decisions is in the trail -- allowed and denied.
    EXPECT_EQ(count_audit("user_id = 'u-agent-b' AND action = 'customer_order:read' AND "
                          "outcome = 'DENIED' AND http_status = 404 AND customer_ref = "
                          "'imsi-999700000000001'"),
              1u);
    EXPECT_EQ(count_audit("user_id = 'u-agent-b' AND action = 'customer_order:create' AND "
                          "outcome = 'DENIED' AND http_status = 409"),
              1u);
    EXPECT_EQ(count_audit("user_id = 'u-agent-a' AND action = 'customer_order:create' AND "
                          "outcome = 'ALLOWED' AND reason = 'decision'"),
              1u);
    EXPECT_EQ(count_audit("user_id = 'u-agent-a' AND action = 'pii:unmask' AND outcome = 'DENIED'"),
              1u);
    EXPECT_EQ(count_audit("user_id = 'u-super' AND action = 'pii:unmask' AND outcome = "
                          "'ALLOWED' AND reason = 'ticket 42'"),
              1u);
}

TEST_F(BffSecurity, SimKeysNeverAppearInAnyResponseLogAuditRowOrStoredPayload) {
    const auto a = session_for("sub-agent-a", "terminal-shop-a");
    const auto b = session_for("sub-agent-b", "terminal-shop-b");
    const std::string order =
        std::string(R"({"supi":"imsi-999700000000009","sim":{"k":")") + kSecretK +
        R"(","opc":")" + kSecretOpc + R"(","sqn":"000000000001"}})";
    auto ok = call("POST", "/api/provisioning/customerOrder", order, as(a, true));
    ASSERT_EQ(ok->status, 201);
    ASSERT_EQ(seen().back().body.find(kSecretK) != std::string::npos, true)
        << "the keys must still reach the provisioning service";
    EXPECT_EQ(call("POST", "/api/provisioning/customerOrder", order, as(b, true),
                   "terminal-shop-b")->status, 409);
    EXPECT_EQ(call("POST", "/api/provisioning/customerOrder", "{\"supi\":\"imsi-1\",\"sim\":{\"k\":\"" +
                                                        std::string(kSecretK) + "\"",
         as(a, true))->status, 400); // malformed JSON
    start_bff("https://127.0.0.1:" + std::to_string(kDeadPort));
    auto dead = call("POST", "/api/provisioning/customerOrder",
                     std::string(R"({"supi":"imsi-999700000000010","sim":{"k":")") + kSecretK +
                         R"(","opc":")" + kSecretOpc + R"("}})",
                     as(a, true));
    EXPECT_EQ(dead->status, 502);

    spdlog::default_logger()->flush();
    const std::string logs = log_->str();
    ASSERT_NE(logs.find("POST /api/provisioning/customerOrder"), std::string::npos)
        << "the log capture must contain the access log, or this proves nothing";
    std::string everything = logs;
    for (const auto& body : bodies_) everything += body;
    for (const auto& row : audit_rows()) everything += row.dump();
    {
        pqxx::connection c(db.admin_url());
        pqxx::nontransaction t(c);
        for (const auto& r : t.exec("SELECT payload::text FROM iam.approval_request")) {
            everything += r[0].as<std::string>();
        }
    }
    for (const std::string& secret : {std::string(kSecretK), std::string(kSecretOpc)}) {
        std::string lower = secret;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        EXPECT_EQ(everything.find(secret), std::string::npos) << secret;
        EXPECT_EQ(everything.find(lower), std::string::npos) << lower;
    }
    EXPECT_NE(everything.find("[secret: not retained]"), std::string::npos);
}

// ================================== maker-checker ===============================================

TEST_F(BffSecurity, AMakerCannotApproveTheirOwnRequestAndACheckerExecutesIt) {
    const auto both = session_for("sub-both", "terminal-shop-a");
    const auto appr = session_for("sub-appr", "terminal-shop-a");
    const auto agent = session_for("sub-agent-a", "terminal-shop-a");
    auto h = as(both, true);
    h.emplace("x-oam-reason", "launch Gold 5G");
    auto proposed = call("POST", "/api/tmf620/productOffering", R"({"name":"Gold 5G"})", h);
    ASSERT_TRUE(proposed.has_value());
    ASSERT_EQ(proposed->status, 202) << proposed->body;
    const std::string id = json::parse(proposed->body)["approvalRequestId"];
    EXPECT_TRUE(seen().empty()) << "nothing reaches the catalog before approval";

    // Agents cannot propose catalog changes at all.
    auto ah = as(agent, true);
    ah.emplace("x-oam-reason", "x");
    EXPECT_EQ(call("POST", "/api/tmf620/productOffering", R"({"name":"X"})", ah)->status, 403);

    // The maker -- who DOES hold the approver permission -- cannot decide it.
    auto self = call("POST", "/api/approvals/" + id + "/decision",
                     R"({"decision":"approve","reason":"lgtm"})", as(both, true));
    ASSERT_TRUE(self.has_value());
    EXPECT_EQ(self->status, 403);
    // And the database refuses it even if the BFF check were bypassed.
    {
        pqxx::connection c(db.bff_url());
        pqxx::work t(c);
        EXPECT_THROW(t.exec("UPDATE iam.approval_request SET status = 'APPROVED', decided_by = "
                            "requested_by, decided_at = now() WHERE id = '" + id + "'"),
                     pqxx::check_violation);
    }
    // A different checker approves; the change executes under their session.
    auto ok = call("POST", "/api/approvals/" + id + "/decision",
                   R"({"decision":"approve","reason":"reviewed tariff sheet"})", as(appr, true));
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(ok->status, 200) << ok->body;
    ASSERT_EQ(seen().size(), 1u);
    EXPECT_EQ(seen()[0].path, "/tmf-api/productCatalogManagement/v4/productOffering");
    // Deciding twice is a conflict.
    EXPECT_EQ(call("POST", "/api/approvals/" + id + "/decision",
                   R"({"decision":"reject","reason":"late"})", as(appr, true))->status,
              409);

    EXPECT_EQ(count_audit("user_id = 'u-both' AND outcome = 'PENDING_APPROVAL' AND "
                          "approval_request_id = '" + id + "'"),
              1u);
    EXPECT_EQ(count_audit("user_id = 'u-both' AND outcome = 'DENIED' AND reason LIKE "
                          "'four-eyes%'"),
              1u);
    EXPECT_EQ(count_audit("user_id = 'u-appr' AND outcome = 'APPROVED' AND "
                          "approval_request_id = '" + id + "'"),
              1u);
    EXPECT_EQ(count_audit("user_id = 'u-appr' AND action = 'product_offering:create' AND "
                          "outcome = 'ALLOWED' AND approval_request_id = '" + id + "'"),
              1u);
    EXPECT_EQ(count_audit("user_id = 'u-agent-a' AND action = 'product_offering:create' AND "
                          "outcome = 'DENIED'"),
              1u);
}

// ================================== NF configuration ============================================

TEST_F(BffSecurity, NfConfigChangeIsSchemaCheckedFourEyesVersionedAndRollbackable) {
    const auto eng = session_for("sub-cfg", "terminal-shop-a");
    const auto appr = session_for("sub-cfgappr", "terminal-shop-a");
    const auto agent = session_for("sub-agent-a", "terminal-shop-a");

    EXPECT_EQ(call("GET", "/api/config/product-catalog", "", as(agent))->status, 403);

    auto view = call("GET", "/api/config/product-catalog", "", as(eng));
    ASSERT_EQ(view->status, 200) << view->body;
    auto v = json::parse(view->body);
    EXPECT_EQ(v["content"]["database_url"], kCredentialMask);
    EXPECT_EQ(v["apply"], "restart");
    ASSERT_EQ(v["versions"].size(), 1u) << "the file on disk is imported as version 1";

    // An invented key is refused by the derived schema.
    json bad = v["content"];
    bad["max_connections"] = 5;
    auto rej = call("POST", "/api/config/product-catalog",
                    json{{"content", bad}, {"reason", "tune"}}.dump(), as(eng, true));
    EXPECT_EQ(rej->status, 400);

    json change = v["content"]; // credential stays masked = unchanged
    change["db_pool_size"] = 24;
    auto prop = call("POST", "/api/config/product-catalog",
                     json{{"content", change}, {"reason", "peak season"}}.dump(), as(eng, true));
    ASSERT_EQ(prop->status, 202) << prop->body;
    const std::string id = json::parse(prop->body)["approvalRequestId"];
    // The engineer cannot approve (no approver role; and would be the maker anyway).
    EXPECT_EQ(call("POST", "/api/approvals/" + id + "/decision",
                   R"({"decision":"approve","reason":"x"})", as(eng, true))->status,
              403);
    auto ok = call("POST", "/api/approvals/" + id + "/decision",
                   R"({"decision":"approve","reason":"CAB-7"})", as(appr, true));
    ASSERT_EQ(ok->status, 200) << ok->body;
    auto on_disk = json::parse(slurp(config_dir / "product-catalog.json"));
    EXPECT_EQ(on_disk["db_pool_size"], 24);
    EXPECT_EQ(on_disk["database_url"], "postgresql://postgres:s3cr3t@127.0.0.1:5434/charging")
        << "a masked credential in the proposal means unchanged";

    // Roll back to version 1 through the same four-eyes path.
    auto rb = call("POST", "/api/config/product-catalog/rollback",
                   R"({"version":1,"reason":"revert after peak"})", as(eng, true));
    ASSERT_EQ(rb->status, 202) << rb->body;
    const std::string rid = json::parse(rb->body)["approvalRequestId"];
    ASSERT_EQ(call("POST", "/api/approvals/" + rid + "/decision",
                   R"({"decision":"approve","reason":"CAB-8"})", as(appr, true))->status,
              200);
    EXPECT_EQ(json::parse(slurp(config_dir / "product-catalog.json"))["db_pool_size"], 16);
    auto hist = json::parse(call("GET", "/api/config/product-catalog", "", as(eng))->body);
    ASSERT_EQ(hist["versions"].size(), 3u);
    EXPECT_EQ(hist["versions"][0]["comment"], "rollback to version 1");
    EXPECT_EQ(hist["versions"][1]["createdBy"], "cfg.engineer");

    // The credential never reaches the audit trail or an approvals listing in clear.
    std::string everything;
    for (const auto& row : audit_rows()) everything += row.dump();
    everything += call("GET", "/api/approvals", "", as(appr))->body;
    EXPECT_EQ(everything.find("s3cr3t"), std::string::npos);
    EXPECT_EQ(count_audit("user_id = 'u-agent-a' AND action = 'nf_config:read' AND outcome = "
                          "'DENIED'"),
              1u);
    EXPECT_GE(count_audit("action = 'nf_config:change' AND outcome = 'DENIED' AND reason = "
                          "'schema validation failed'"),
              1u);
}

// ================================== the audit trail itself ======================================

TEST_F(BffSecurity, TheAuditTrailIsAppendOnlyForTheBffAndTamperingIsDetected) {
    const auto a = session_for("sub-agent-a", "terminal-shop-a");
    ASSERT_EQ(call("GET", "/api/me", "", as(a))->status, 200);
    {
        pqxx::connection c(db.bff_url());
        pqxx::nontransaction t(c);
        EXPECT_THROW(t.exec("UPDATE iam.audit_event SET outcome = 'ALLOWED'"), pqxx::sql_error);
        EXPECT_THROW(t.exec("DELETE FROM iam.audit_event"), pqxx::sql_error);
        EXPECT_THROW(t.exec("TRUNCATE iam.audit_event"), pqxx::sql_error);
        EXPECT_TRUE(t.exec("SELECT * FROM iam.audit_verify_chain('test-chain')").empty());
    }
    // A superuser can switch the triggers off -- and verification then exposes the edit.
    pqxx::connection c(db.admin_url());
    pqxx::nontransaction t(c);
    ASSERT_GE(count_audit("chain_key = 'test-chain'"), 1u) << "nothing to tamper with";
    t.exec("ALTER TABLE iam.audit_event DISABLE TRIGGER USER");
    t.exec("UPDATE iam.audit_event SET reason = 'nothing to see' WHERE chain_seq = 1");
    t.exec("ALTER TABLE iam.audit_event ENABLE TRIGGER USER");
    const auto bad = t.exec("SELECT bad_id, problem FROM iam.audit_verify_chain('test-chain')");
    ASSERT_EQ(bad.size(), 1u);
    EXPECT_EQ(bad[0][1].as<std::string>(), "row content changed");
}

} // namespace
