// Nnwdaf_MLModelTraining subscription CRUD (ADR-0471, increment 1) --
// TS29520_Nnwdaf_MLModelTraining.yaml (v1.1.0), TS 23.288 7.10.2 / 7.10.3. An NWDAF in role mtlf
// behind a real NRF and a real Valkey.
//
// Covers: POST (201 + Location + failEventReports for an event it does not train; 500 when none is
// trainable; 400 for a missing notifCorreId), PUT (200 / 404), PATCH merge-patch (200, a member
// outside NwdafMLModelTrainSubscPatch is 400), DELETE (204 / 404), UnsubscribeInfo (204, 400
// without termCause, 404). It does NOT cover notifications or any federated-learning behaviour:
// neither exists yet (ADR-0471).

#include "sbi_core/http2_client.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sw/redis++/redis++.h>
#include <thread>

#include "spawn_guard.hpp"

#include <gtest/gtest.h>

namespace {

using namespace std::chrono_literals;
using nlohmann::json;

constexpr const char* kNrf = "https://127.0.0.1:7777";
constexpr const char* kMtlf = "https://127.0.0.1:7797";
constexpr const char* kRoot = "/nnwdaf-mlmodeltraining/v1";

sbi_core::http2::Client make_client() {
    sbi_core::http2::TlsConfig tls{
        .cert_path = CERTS_DIR "/hello-nf/cert.pem",
        .key_path = CERTS_DIR "/hello-nf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    return sbi_core::http2::Client(std::move(tls));
}

bool wait_up(sbi_core::http2::Client& c, const std::string& url, std::chrono::seconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = url;
        if (auto r = c.send(req); r && r->status != 0) {
            return true;
        }
        std::this_thread::sleep_for(200ms);
    }
    return false;
}

std::string token(sbi_core::http2::Client& c) {
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = std::string(kNrf) + "/oauth2/token";
    req.headers.emplace("content-type", "application/x-www-form-urlencoded");
    req.body = "grant_type=client_credentials&nfInstanceId=test-client&scope=nnwdaf-mlmodeltraining"
               "&targetNfType=NWDAF";
    auto resp = c.send(req);
    return (resp && resp->status == 200)
               ? json::parse(resp->body).at("access_token").get<std::string>()
               : "";
}

json subscription(const json& events = json::array({json{{"mLEvent", "NF_LOAD"},
                                                         {"mLEventFilter", json::object()}}})) {
    return json{{"mLEventSubscs", events},
                {"notifUri", "https://127.0.0.1:19994/notify"},
                {"notifCorreId", "corr-1"}};
}

} // namespace

TEST(NwdafMlTrainingSub, SubscriptionLifecycle) {
    const std::string redis_url =
        std::getenv("NWDAF_REDIS_URL") ? std::getenv("NWDAF_REDIS_URL") : "tcp://127.0.0.1:6379";
    try {
        sw::redis::Redis(redis_url).ping();
    } catch (const std::exception& e) {
        GTEST_SKIP() << "no Valkey at " << redis_url << " (" << e.what()
                     << ") -- skipped, not passed";
    }
    auto c = make_client();
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_TRUE(wait_up(c, std::string(kNrf) + "/nnrf-nfm/v1/nf-instances", 30s));
    setenv("NWDAF_ROLE", "mtlf", 1);
    setenv("NWDAF_PORT", "7797", 1);
    setenv("NWDAF_METRICS_BIND_ADDRESS", "0.0.0.0:9486", 1);
    setenv("NWDAF_SELF_BASE_URL", kMtlf, 1);
    nf_test::SpawnedProcess mtlf(NWDAF_PATH);
    for (const char* k :
         {"NWDAF_ROLE", "NWDAF_PORT", "NWDAF_METRICS_BIND_ADDRESS", "NWDAF_SELF_BASE_URL"}) {
        unsetenv(k);
    }
    ASSERT_TRUE(wait_up(c, std::string(kMtlf) + kRoot + "/subscriptions", 30s))
        << "MTLF never came up";

    const auto tok = token(c);
    ASSERT_FALSE(tok.empty());
    const auto call = [&](const std::string& method,
                          const std::string& path,
                          const json* body,
                          const char* content_type = "application/json") {
        sbi_core::http2::ClientRequest req;
        req.method = method;
        req.url = std::string(kMtlf) + path;
        req.headers.emplace("authorization", "Bearer " + tok);
        if (body != nullptr) {
            req.headers.emplace("content-type", content_type);
            req.body = body->dump();
        }
        return c.send(req);
    };

    // POST: 400 without notifCorreId, 500 when no event is trainable, 201 + Location otherwise.
    json missing = subscription();
    missing.erase("notifCorreId");
    auto r = call("POST", std::string(kRoot) + "/subscriptions", &missing);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 400) << r->body;

    const json untrainable = subscription(
        json::array({json{{"mLEvent", "UE_MOBILITY"}, {"mLEventFilter", json::object()}}}));
    r = call("POST", std::string(kRoot) + "/subscriptions", &untrainable);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 500) << r->body;

    const json mixed = subscription(
        json::array({json{{"mLEvent", "NF_LOAD"}, {"mLEventFilter", json::object()}},
                     json{{"mLEvent", "UE_MOBILITY"}, {"mLEventFilter", json::object()}}}));
    r = call("POST", std::string(kRoot) + "/subscriptions", &mixed);
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 201) << r->body;
    EXPECT_EQ(json::parse(r->body).at("failEventReports").size(), 1U) << r->body;
    const auto loc = r->headers.find("location");
    ASSERT_NE(loc, r->headers.end());
    const std::string item = loc->second.substr(loc->second.find(kRoot));

    // PUT replaces; a stale failEventReports from the old state does not survive.
    const json replacement = subscription();
    r = call("PUT", item, &replacement);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 200) << r->body;
    EXPECT_FALSE(json::parse(r->body).contains("failEventReports")) << r->body;
    r = call("PUT", std::string(kRoot) + "/subscriptions/does-not-exist", &replacement);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 404) << r->body;

    // PATCH (RFC 7396): changes notifUri and adds roundInd; a member outside the patch schema is
    // 400.
    const json patch = {{"notifUri", "https://127.0.0.1:19994/other"}, {"roundInd", 2}};
    r = call("PATCH", item, &patch, "application/merge-patch+json");
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 200) << r->body;
    EXPECT_EQ(json::parse(r->body).at("notifUri"), "https://127.0.0.1:19994/other");
    EXPECT_EQ(json::parse(r->body).at("roundInd"), 2);
    EXPECT_EQ(json::parse(r->body).at("notifCorreId"), "corr-1");
    const json bad_patch = {{"notifCorreId", "changed"}};
    r = call("PATCH", item, &bad_patch, "application/merge-patch+json");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 400) << r->body;

    // UnsubscribeInfo: 400 without termCause; 204 removes; then 404.
    const json no_cause = json::object();
    r = call("POST", item + "/unsubscribe-info", &no_cause);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 400) << r->body;
    // termCause is an FLServerTermCause (FL_CLI_UNSELECTED, FL_SUSPENDED, FL_FINISHED, OTHER).
    const json with_cause = {{"termCause", "FL_FINISHED"}};
    r = call("POST", item + "/unsubscribe-info", &with_cause);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 204) << r->body;
    r = call("POST", item + "/unsubscribe-info", &with_cause);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 404) << r->body;

    // DELETE: 204 then 404.
    r = call("POST", std::string(kRoot) + "/subscriptions", &replacement);
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 201) << r->body;
    const auto l2 = r->headers.find("location");
    ASSERT_NE(l2, r->headers.end());
    const std::string item2 = l2->second.substr(l2->second.find(kRoot));
    r = call("DELETE", item2, nullptr);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 204);
    r = call("DELETE", item2, nullptr);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 404);
}
