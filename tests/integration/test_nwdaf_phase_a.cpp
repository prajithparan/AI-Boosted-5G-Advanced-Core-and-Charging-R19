// NWDAF Phase A end to end (ADR-0358): a real NRF, a real NWDAF registered with it, and the two
// Phase A services exercised over TLS 1.3 + mTLS at the API roots the YAML declares.
//
// The NF_LOAD assertion is the one that proves the analytic is computed from real data: the
// NWDAF discovers NFs through the NRF, and the NRF's registry contains -- at minimum -- the NWDAF
// itself. So a correct answer must list an NWDAF instance with the status the NRF holds for it.
// Nothing seeded, nothing mocked.

#include "sbi_core/http2_client.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <string>
#include <thread>

#include "spawn_guard.hpp"

#include <gtest/gtest.h>

namespace {

using namespace std::chrono_literals;
using nlohmann::json;

constexpr const char* kNwdaf = "https://127.0.0.1:7798";
constexpr const char* kAnalyticsRoot = "/nnwdaf-analyticsinfo/v1";
constexpr const char* kSubsRoot = "/nnwdaf-eventssubscription/v1";

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

} // namespace

TEST(NwdafPhaseA, NfLoadIsComputedFromTheRealNrfRegistry) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    nf_test::SpawnedProcess nwdaf(NWDAF_PATH);
    ASSERT_GT(nrf.pid(), 0);
    ASSERT_GT(nwdaf.pid(), 0);
    auto c = make_client();
    ASSERT_TRUE(wait_up(c, std::string(kNwdaf) + kAnalyticsRoot + "/context", 30s))
        << "NWDAF never came up";
    // Give the NWDAF's NRF registration a moment to land, so it appears in its own discovery.
    std::this_thread::sleep_for(2s);

    sbi_core::http2::ClientRequest req;
    req.method = "GET";
    req.url = std::string(kNwdaf) + kAnalyticsRoot + "/analytics?event-id=NF_LOAD";
    auto r = c.send(req);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->status, 200) << r->body;
    const auto data = json::parse(r->body);
    ASSERT_TRUE(data.contains("nfLoadLevelInfos")) << r->body;
    bool saw_nwdaf = false;
    for (const auto& info : data["nfLoadLevelInfos"]) {
        if (info.value("nfType", "") == "NWDAF") {
            saw_nwdaf = true;
            // NfStatus is a percentage-of-time object, not the NRF enum (TS 29.520).
            ASSERT_TRUE(info.contains("nfStatus")) << info.dump();
            EXPECT_EQ(info["nfStatus"].value("statusRegistered", -1), 100)
                << "status must be what the NRF holds";
            // No NF in this lab reports `load` in its profile, so the analytic must NOT invent one.
            EXPECT_FALSE(info.contains("nfLoadLevelAverage"))
                << "load was fabricated: " << info.dump();
        }
    }
    EXPECT_TRUE(saw_nwdaf) << "NF_LOAD did not come from the NRF registry: " << r->body;
    EXPECT_TRUE(data.contains("timeStampGen"));
}

TEST(NwdafPhaseA, AnUnsupportedAnalyticsIdIsA404NotAnEmpty200) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    nf_test::SpawnedProcess nwdaf(NWDAF_PATH);
    auto c = make_client();
    ASSERT_TRUE(wait_up(c, std::string(kNwdaf) + kAnalyticsRoot + "/context", 30s));
    sbi_core::http2::ClientRequest req;
    req.method = "GET";
    req.url = std::string(kNwdaf) + kAnalyticsRoot + "/analytics?event-id=QOS_SUSTAINABILITY";
    auto r = c.send(req);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 404) << "an analytic this NWDAF does not compute must say so";
    EXPECT_NE(json::parse(r->body).value("detail", "").find("not computed"), std::string::npos);
    // And the YAML's required parameter, missing:
    req.url = std::string(kNwdaf) + kAnalyticsRoot + "/analytics";
    r = c.send(req);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 400);
}

TEST(NwdafPhaseA, SubscriptionLifecycleAtTheYamlsApiRoot) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    nf_test::SpawnedProcess nwdaf(NWDAF_PATH);
    auto c = make_client();
    ASSERT_TRUE(wait_up(c, std::string(kNwdaf) + kAnalyticsRoot + "/context", 30s));

    sbi_core::http2::ClientRequest create;
    create.method = "POST";
    create.url = std::string(kNwdaf) + kSubsRoot + "/subscriptions";
    create.headers.emplace("content-type", "application/json");
    create.body = json{
        {"eventSubscriptions", json::array({json{{"event", "NF_LOAD"}}})},
        {"notifCorrId",
         "corr-1"}}.dump();
    auto r = c.send(create);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->status, 201) << r->body;
    const auto loc = r->headers.find("location");
    ASSERT_NE(loc, r->headers.end());
    EXPECT_NE(loc->second.find(std::string(kSubsRoot) + "/subscriptions/"), std::string::npos);

    sbi_core::http2::ClientRequest del;
    del.method = "DELETE";
    del.url = std::string(kNwdaf) + loc->second;
    r = c.send(del);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 204);
    r = c.send(del); // gone
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 404);

    // An empty subscription set is the YAML's own 400, not a 201 that subscribes to nothing.
    create.body = json{{"eventSubscriptions", json::array()}}.dump();
    r = c.send(create);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 400);
}
