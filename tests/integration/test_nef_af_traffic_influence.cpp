// ADR-0302: NEF's AF-facing TrafficInfluence (TS 29.522), and the assertion that matters.
//
// ADR-0294 found that this project's NEF had no outbound HTTP at all beyond NRF registration:
// everything an AF gave it landed in an in-process map. Traffic-influence data that never reaches
// UDR influences nothing, because SMF reads it from UDR. So the load-bearing assertion here is not
// that NEF answered 201 -- it is that the record is READABLE FROM UDR afterwards, checked directly
// against UDR rather than through NEF. A NEF that returns a correct 201 and brokers nothing would
// pass a NEF-only test and be useless in a real network.
//
// Also asserted: the two `oneOf` mutual-exclusivity constraints, because ADR-0301 established that
// the generated DTO cannot enforce them -- every field is optional in the struct, so if NEF's
// handler does not check them, nothing does.

#include "sbi_core/http2_client.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <string>
#include <thread>

#include "spawn_guard.hpp"

#include <gtest/gtest.h>

namespace {

using nlohmann::json;

constexpr const char* kAfId = "af-test-1";

sbi_core::http2::Client make_client() {
    sbi_core::http2::TlsConfig tls{
        .cert_path = CERTS_DIR "/hello-nf/cert.pem",
        .key_path = CERTS_DIR "/hello-nf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    return sbi_core::http2::Client(std::move(tls));
}

bool wait_reachable(sbi_core::http2::Client& client, const std::string& url, int attempts) {
    for (int i = 0; i < attempts; ++i) {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = url;
        if (client.send(req).has_value()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

} // namespace

TEST(NefAfTrafficInfluence, SubscriptionIsBrokeredToUdrAndDeletedFromIt) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0);
    nf_test::SpawnedProcess udr(UDR_PATH);
    ASSERT_GT(udr.pid(), 0);
    nf_test::SpawnedProcess nef(NEF_PATH);
    ASSERT_GT(nef.pid(), 0);

    auto client = make_client();
    const std::string nef_base =
        "https://127.0.0.1:7790/3gpp-traffic-influence/v1/" + std::string(kAfId) + "/subscriptions";
    const std::string udr_base = "https://127.0.0.1:7781/nudr-dr/v2/application-data/influenceData";
    ASSERT_TRUE(wait_reachable(client, nef_base, 200)) << "nef never reachable";
    ASSERT_TRUE(wait_reachable(client, udr_base, 200)) << "udr never reachable";

    // A valid subscription: exactly one traffic selector (afAppId) and exactly one target
    // (anyUeInd), as the spec's two oneOf groups require.
    sbi_core::http2::ClientRequest create;
    create.method = "POST";
    create.url = nef_base;
    create.headers.emplace("content-type", "application/json");
    create.body =
        json{
            {"afAppId", "test-app"},
            {"anyUeInd", true},
            {"dnn", "internet"},
            {"snssai", json{{"sst", 1}, {"sd", "000001"}}},
        }
            .dump();
    auto create_resp = client.send(create);
    ASSERT_TRUE(create_resp.has_value());
    ASSERT_EQ(create_resp->status, 201) << create_resp->body;
    const auto location = create_resp->headers.find("location");
    ASSERT_NE(location, create_resp->headers.end()) << "201 must carry a Location";
    const auto sub_id = location->second.substr(location->second.rfind('/') + 1);

    // THE assertion: the influence record exists in UDR, read from UDR directly.
    //
    // Read via the COLLECTION with the `influence-Ids` filter, not an individual GET: TS 29.519
    // defines only `put` on `influenceData/{influenceId}` -- there is no individual GET in the real
    // spec, and UDR correctly does not invent one. (My first version of this test asserted against
    // an individual GET and failed with 404 while the broker was working perfectly; the test was
    // wrong, not the code.)
    const std::string influence_id = std::string(kAfId) + "-" + sub_id;
    sbi_core::http2::ClientRequest udr_get;
    udr_get.method = "GET";
    udr_get.url = udr_base + "?influence-Ids=" + influence_id;
    auto udr_resp = client.send(udr_get);
    ASSERT_TRUE(udr_resp.has_value());
    ASSERT_EQ(udr_resp->status, 200)
        << "the AF's traffic influence never reached UDR -- no SMF would ever act on it, which is "
           "the exact gap ADR-0294 recorded and this ADR exists to close";
    const auto listed = json::parse(udr_resp->body);
    ASSERT_TRUE(listed.is_array()) << udr_resp->body;
    ASSERT_EQ(listed.size(), 1u) << "expected exactly the one brokered record: " << udr_resp->body;
    EXPECT_EQ(listed[0].value("afAppId", ""), "test-app");
    EXPECT_EQ(listed[0].value("dnn", ""), "internet");

    // Deleting at NEF must remove it from UDR too -- an orphaned rule would keep steering traffic
    // for a subscription the AF believes is gone.
    sbi_core::http2::ClientRequest del;
    del.method = "DELETE";
    del.url = nef_base + "/" + sub_id;
    auto del_resp = client.send(del);
    ASSERT_TRUE(del_resp.has_value());
    EXPECT_EQ(del_resp->status, 204);

    auto after = client.send(udr_get);
    ASSERT_TRUE(after.has_value());
    ASSERT_EQ(after->status, 200) << after->body;
    EXPECT_TRUE(json::parse(after->body).empty())
        << "the influence rule outlived the subscription that created it: " << after->body;
}

TEST(NefAfTrafficInfluence, MutualExclusivityConstraintsAreEnforcedByTheHandler) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    nf_test::SpawnedProcess udr(UDR_PATH);
    nf_test::SpawnedProcess nef(NEF_PATH);
    ASSERT_GT(nef.pid(), 0);

    auto client = make_client();
    const std::string nef_base =
        "https://127.0.0.1:7790/3gpp-traffic-influence/v1/" + std::string(kAfId) + "/subscriptions";
    ASSERT_TRUE(wait_reachable(client, nef_base, 200));

    auto post = [&](const json& body) {
        sbi_core::http2::ClientRequest req;
        req.method = "POST";
        req.url = nef_base;
        req.headers.emplace("content-type", "application/json");
        req.body = body.dump();
        return client.send(req);
    };

    // Neither traffic selector nor target: the DTO accepts this happily (every field is optional,
    // ADR-0301), so only the handler can reject it.
    auto none = post(json{{"dnn", "internet"}});
    ASSERT_TRUE(none.has_value());
    EXPECT_EQ(none->status, 400) << "a subscription selecting no traffic and no target was "
                                    "accepted -- it could never match anything";

    // Two traffic selectors.
    auto two_traffic =
        post(json{{"afAppId", "a"}, {"trafficDataSets", json::array()}, {"anyUeInd", true}});
    ASSERT_TRUE(two_traffic.has_value());
    EXPECT_EQ(two_traffic->status, 400);

    // Two targets.
    auto two_targets = post(json{{"afAppId", "a"}, {"anyUeInd", true}, {"gpsi", "msisdn-123"}});
    ASSERT_TRUE(two_targets.has_value());
    EXPECT_EQ(two_targets->status, 400);
}
