// NWDAF Phase A end to end (ADR-0358): a real NRF, a real NWDAF registered with it, and the two
// Phase A services exercised over TLS 1.3 + mTLS at the API roots the YAML declares.
//
// The NF_LOAD assertion is the one that proves the analytic is computed from real data: the
// NWDAF discovers NFs through the NRF, and the NRF's registry contains -- at minimum -- the NWDAF
// itself. So a correct answer must list an NWDAF instance with the status the NRF holds for it.
// Nothing seeded, nothing mocked.

#include "sbi_core/http2_client.hpp"
#include "sbi_core/http2_server.hpp"

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

// ADR-0360: subscription state is in Valkey, so two NWDAF replicas are interchangeable. A
// subscription created on replica A is updated and deleted through replica B, and A then reports
// it gone. With the Phase A in-process map this test fails at the first PUT with a 404 -- which is
// exactly the failure an operator would have hit the day they ran a second instance.
TEST(NwdafPhaseA, SubscriptionsAreSharedAcrossReplicas) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    nf_test::SpawnedProcess replica_a(NWDAF_PATH);
    // The second replica's port and metrics endpoint come from the same env overrides an operator
    // would use (config mandate: nothing hardcoded); set only around the fork so replica A is not
    // affected.
    setenv("NWDAF_PORT", "7799", 1);
    setenv("NWDAF_METRICS_BIND_ADDRESS", "0.0.0.0:9486", 1);
    nf_test::SpawnedProcess replica_b(NWDAF_PATH);
    unsetenv("NWDAF_PORT");
    unsetenv("NWDAF_METRICS_BIND_ADDRESS");
    const std::string a = kNwdaf;
    const std::string b = "https://127.0.0.1:7799";

    auto c = make_client();
    ASSERT_TRUE(wait_up(c, a + kAnalyticsRoot + "/context", 30s)) << "replica A never came up";
    ASSERT_TRUE(wait_up(c, b + kAnalyticsRoot + "/context", 30s)) << "replica B never came up";

    sbi_core::http2::ClientRequest create;
    create.method = "POST";
    create.url = a + kSubsRoot + "/subscriptions";
    create.headers.emplace("content-type", "application/json");
    create.body = json{{"eventSubscriptions", json::array({json{{"event", "NF_LOAD"}}})}}.dump();
    auto r = c.send(create);
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 201) << r->body;
    const auto loc_it = r->headers.find("location");
    ASSERT_NE(loc_it, r->headers.end());
    const std::string location = loc_it->second; // r is reassigned below; an iterator would dangle

    // Update through B: the resource A created must be visible to B.
    sbi_core::http2::ClientRequest put;
    put.method = "PUT";
    put.url = b + location;
    put.headers.emplace("content-type", "application/json");
    put.body =
        json{{"eventSubscriptions",
              json::array({json{{"event", "NF_LOAD"}}, json{{"event", "ABNORMAL_BEHAVIOUR"}}})}}
            .dump();
    r = c.send(put);
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 200) << r->body;
    EXPECT_EQ(json::parse(r->body)["eventSubscriptions"].size(), 2U);

    // Delete through B, then A agrees it is gone.
    sbi_core::http2::ClientRequest del;
    del.method = "DELETE";
    del.url = b + location;
    r = c.send(del);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 204);
    del.url = a + location;
    r = c.send(del);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 404);

    // A PUT on an id nobody created is a 404 on either replica -- never a silent upsert.
    put.url = a + kSubsRoot + "/subscriptions/nwdaf-sub-does-not-exist";
    r = c.send(put);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 404);
}

// ADR-0365: sharing the subscription STATE (ADR-0360) is not the same as sharing the WORK. Before
// the per-subscription lease, each replica ran its own notifier over the shared store and a
// consumer received one copy of every notification per replica. Two replicas, one subscription,
// a 2-second interval, ~2.5 intervals of listening: the receiver must see notifications (the
// notifier works) and never two within one interval (the lease works). The replicas' ticks are
// not synchronised, which is the whole point -- the lease, not timing, prevents the duplicate.
TEST(NwdafPhaseA, ReplicasDeliverEachNotificationOnce) {
    boost::asio::io_context receiver_ioc;
    sbi_core::http2::TlsConfig receiver_tls{
        .cert_path = CERTS_DIR "/hello-nf/cert.pem",
        .key_path = CERTS_DIR "/hello-nf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Server receiver(receiver_ioc, "127.0.0.1", 19996, receiver_tls);
    std::mutex arrivals_mutex;
    std::vector<std::chrono::steady_clock::time_point> arrivals;
    receiver.add_route("POST", "/nwdaf-notify", [&](const sbi_core::http2::Request& req) {
        const auto body = json::parse(req.body);
        EXPECT_TRUE(body.contains("eventNotifications")) << req.body;
        const std::lock_guard<std::mutex> lock(arrivals_mutex);
        arrivals.push_back(std::chrono::steady_clock::now());
        sbi_core::http2::Response resp;
        resp.status = 204;
        return resp;
    });
    receiver.start();
    std::thread receiver_thread([&receiver_ioc] { receiver_ioc.run(); });

    nf_test::SpawnedProcess nrf(NRF_PATH);
    setenv("NWDAF_NOTIFICATION_INTERVAL_SECONDS", "2", 1);
    nf_test::SpawnedProcess replica_a(NWDAF_PATH);
    setenv("NWDAF_PORT", "7799", 1);
    setenv("NWDAF_METRICS_BIND_ADDRESS", "0.0.0.0:9486", 1);
    nf_test::SpawnedProcess replica_b(NWDAF_PATH);
    unsetenv("NWDAF_PORT");
    unsetenv("NWDAF_METRICS_BIND_ADDRESS");
    unsetenv("NWDAF_NOTIFICATION_INTERVAL_SECONDS");
    const std::string a = kNwdaf;
    const std::string b = "https://127.0.0.1:7799";

    auto c = make_client();
    ASSERT_TRUE(wait_up(c, a + kAnalyticsRoot + "/context", 30s)) << "replica A never came up";
    ASSERT_TRUE(wait_up(c, b + kAnalyticsRoot + "/context", 30s)) << "replica B never came up";

    sbi_core::http2::ClientRequest create;
    create.method = "POST";
    create.url = a + kSubsRoot + "/subscriptions";
    create.headers.emplace("content-type", "application/json");
    create.body = json{{"eventSubscriptions", json::array({json{{"event", "NF_LOAD"}}})},
                       {"notificationURI", "https://127.0.0.1:19996/nwdaf-notify"}}
                      .dump();
    auto r = c.send(create);
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 201) << r->body;
    const std::string location = r->headers.find("location")->second;

    std::this_thread::sleep_for(5s);

    sbi_core::http2::ClientRequest del;
    del.method = "DELETE";
    del.url = b + location;
    r = c.send(del);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 204);

    receiver_ioc.stop();
    receiver_thread.join();

    const std::lock_guard<std::mutex> lock(arrivals_mutex);
    ASSERT_GE(arrivals.size(), 1U) << "no replica delivered anything";
    // With a 2 s interval and a 1.8 s lease, two deliveries closer than 1.8 s apart can only be
    // two replicas both delivering the same tick -- the duplicate this test exists to catch.
    for (std::size_t i = 1; i < arrivals.size(); ++i) {
        const auto gap =
            std::chrono::duration_cast<std::chrono::milliseconds>(arrivals[i] - arrivals[i - 1]);
        EXPECT_GE(gap.count(), 1700)
            << "two replicas delivered the same interval (" << gap.count() << " ms apart)";
    }
}
