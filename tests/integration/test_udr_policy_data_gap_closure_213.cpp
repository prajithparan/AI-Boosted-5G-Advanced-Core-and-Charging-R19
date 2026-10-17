// Drives nrf and udr as real, separate OS processes to exercise UDR's own Policy_Data.yaml
// Tier-B gap-closure operations (ADR-0213, docs/CAPABILITY_GAP_ANALYSIS.md's own UDR audit):
// the bare Usage Monitoring Information (Document) resource, the bare `/policy-data/ues/{ueId}`
// aggregate (ReadPolicyData), the bare `bdt-data` collection GET (ReadBdtData), and the
// `policy-data` group's own Policy Data Subscriptions (Collection) + Individual Policy (Data)
// Subscription (Document) -- over real TLS 1.3 + mTLS HTTP/2 with a real signed OAuth2 token, per
// TS29519_Policy_Data.yaml.

#include "sbi_core/http2_client.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

#include "spawn_guard.hpp"
#include "test_tokens.hpp"

#include <gtest/gtest.h>

namespace {

using nlohmann::json;

sbi_core::http2::Client make_client() {
    sbi_core::http2::TlsConfig tls{
        .cert_path = CERTS_DIR "/hello-nf/cert.pem",
        .key_path = CERTS_DIR "/hello-nf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    return sbi_core::http2::Client(std::move(tls));
}

bool wait_reachable(sbi_core::http2::Client& client, const std::string& url, int max_attempts) {
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
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

std::string fetch_token(sbi_core::http2::Client& client) {
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = "https://127.0.0.1:7777/oauth2/token";
    req.headers.emplace("content-type", "application/x-www-form-urlencoded");
    req.body =
        "grant_type=client_credentials&nfInstanceId=" + std::string(nf_test::kTestAfInstanceId) +
        "&scope=nudr-dr&"
        "targetNfType=UDR";
    auto resp = nf_test::send_token_request(req);
    if (!resp.has_value() || resp->status != 200) {
        return "";
    }
    return json::parse(resp->body).at("access_token").get<std::string>();
}

struct Duo {
    nf_test::SpawnedProcess nrf;
    nf_test::SpawnedProcess udr;
};

Duo spawn_nrf_udr() {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    nf_test::SpawnedProcess udr(UDR_PATH);
    return Duo{std::move(nrf), std::move(udr)};
}

} // namespace

TEST(UdrPolicyDataGapClosureIntegration, UsageMonitoringResourceLifecycle) {
    auto d = spawn_nrf_udr();
    auto client = make_client();
    const std::string base_url =
        "https://127.0.0.1:7781/nudr-dr/v2/policy-data/ues/imsi-999700000000001/sm-data/limit-1";
    ASSERT_TRUE(wait_reachable(client, base_url, 200)) << "udr never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    sbi_core::http2::ClientRequest get_before_req;
    get_before_req.method = "GET";
    get_before_req.url = base_url;
    get_before_req.headers.emplace("authorization", "Bearer " + token);
    auto get_before_resp = client.send(get_before_req);
    ASSERT_TRUE(get_before_resp.has_value());
    EXPECT_EQ(get_before_resp->status, 404);

    const json usage_mon_data = json{{"limitId", "limit-1"}, {"umLevel", "PDU_SESSION_LEVEL"}};
    sbi_core::http2::ClientRequest put_req;
    put_req.method = "PUT";
    put_req.url = base_url;
    put_req.headers.emplace("content-type", "application/json");
    put_req.headers.emplace("authorization", "Bearer " + token);
    put_req.body = usage_mon_data.dump();
    auto put_resp = client.send(put_req);
    ASSERT_TRUE(put_resp.has_value());
    EXPECT_EQ(put_resp->status, 201) << put_resp->body;
    EXPECT_NE(put_resp->headers.find("location"), put_resp->headers.end());

    sbi_core::http2::ClientRequest get_req;
    get_req.method = "GET";
    get_req.url = base_url;
    get_req.headers.emplace("authorization", "Bearer " + token);
    auto get_resp = client.send(get_req);
    ASSERT_TRUE(get_resp.has_value());
    EXPECT_EQ(get_resp->status, 200) << get_resp->body;
    if (get_resp->status == 200) {
        EXPECT_EQ(json::parse(get_resp->body).at("limitId").get<std::string>(), "limit-1");
    }

    sbi_core::http2::ClientRequest delete_req;
    delete_req.method = "DELETE";
    delete_req.url = base_url;
    delete_req.headers.emplace("authorization", "Bearer " + token);
    auto delete_resp = client.send(delete_req);
    ASSERT_TRUE(delete_resp.has_value());
    EXPECT_EQ(delete_resp->status, 204);

    auto get_after_delete_resp = client.send(get_req);
    ASSERT_TRUE(get_after_delete_resp.has_value());
    EXPECT_EQ(get_after_delete_resp->status, 404);
}

TEST(UdrPolicyDataGapClosureIntegration, ReadPolicyDataComposesSeededSubResources) {
    auto d = spawn_nrf_udr();
    auto client = make_client();
    const std::string ue_id = "imsi-999700000000002";
    const std::string base_url = "https://127.0.0.1:7781/nudr-dr/v2/policy-data/ues/" + ue_id;
    ASSERT_TRUE(wait_reachable(client, base_url, 200)) << "udr never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // Seed am-data (real PATCH, upsert-capable).
    sbi_core::http2::ClientRequest am_patch_req;
    am_patch_req.method = "PATCH";
    am_patch_req.url = "https://127.0.0.1:7781/nudr-dr/v2/policy-data/ues/" + ue_id + "/am-data";
    am_patch_req.headers.emplace("content-type", "application/merge-patch+json");
    am_patch_req.headers.emplace("authorization", "Bearer " + token);
    am_patch_req.body = json{{"subscCats", json::array({"cat1"})}}.dump();
    auto am_patch_resp = client.send(am_patch_req);
    ASSERT_TRUE(am_patch_resp.has_value());
    EXPECT_EQ(am_patch_resp->status, 200) << am_patch_resp->body;

    // Seed ue-policy-set (real PUT).
    sbi_core::http2::ClientRequest ups_put_req;
    ups_put_req.method = "PUT";
    ups_put_req.url =
        "https://127.0.0.1:7781/nudr-dr/v2/policy-data/ues/" + ue_id + "/ue-policy-set";
    ups_put_req.headers.emplace("content-type", "application/json");
    ups_put_req.headers.emplace("authorization", "Bearer " + token);
    ups_put_req.body = json{{"andspInd", true}}.dump();
    auto ups_put_resp = client.send(ups_put_req);
    ASSERT_TRUE(ups_put_resp.has_value());
    // Real 201 on first create, real 204 on update -- this project's own persistent PostgreSQL
    // container is shared across test runs, so a prior run against this same seeded ueId may
    // already have created this row.
    EXPECT_TRUE(ups_put_resp->status == 201 || ups_put_resp->status == 204) << ups_put_resp->body;

    // Seed usage-mon-data (this ADR's own new resource).
    sbi_core::http2::ClientRequest umd_put_req;
    umd_put_req.method = "PUT";
    umd_put_req.url =
        "https://127.0.0.1:7781/nudr-dr/v2/policy-data/ues/" + ue_id + "/sm-data/limit-2";
    umd_put_req.headers.emplace("content-type", "application/json");
    umd_put_req.headers.emplace("authorization", "Bearer " + token);
    umd_put_req.body = json{{"limitId", "limit-2"}}.dump();
    auto umd_put_resp = client.send(umd_put_req);
    ASSERT_TRUE(umd_put_resp.has_value());
    EXPECT_EQ(umd_put_resp->status, 201) << umd_put_resp->body;

    // No filter: everything seeded should be present.
    sbi_core::http2::ClientRequest req;
    req.method = "GET";
    req.url = base_url;
    req.headers.emplace("authorization", "Bearer " + token);
    auto resp = client.send(req);
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->status, 200) << resp->body;
    if (resp->status == 200) {
        const auto got = json::parse(resp->body);
        EXPECT_TRUE(got.contains("amPolicyDataSet"));
        EXPECT_TRUE(got.contains("uePolicyDataSet"));
        ASSERT_TRUE(got.contains("umData"));
        EXPECT_TRUE(got.at("umData").contains("limit-2"));
        // Real, disclosed: this project's own persistent PostgreSQL container is shared across
        // every test run (not reset per-test), so `smPolicyDataSet`/`operatorSpecificDataSet` may
        // already be populated for this seeded test ueId by an unrelated, earlier test (e.g. the
        // real N28 end-to-end SM policy wiring, ADR-0072/ADR-0084) -- not asserted absent here,
        // since that would be asserting on ambient shared-DB state this test doesn't control.
    }

    // Real, optional data-subset-names filter: AM_POLICY_DATA only.
    sbi_core::http2::ClientRequest filtered_req;
    filtered_req.method = "GET";
    filtered_req.url = base_url + "?data-subset-names=AM_POLICY_DATA";
    filtered_req.headers.emplace("authorization", "Bearer " + token);
    auto filtered_resp = client.send(filtered_req);
    ASSERT_TRUE(filtered_resp.has_value());
    EXPECT_EQ(filtered_resp->status, 200) << filtered_resp->body;
    if (filtered_resp->status == 200) {
        const auto got = json::parse(filtered_resp->body);
        EXPECT_TRUE(got.contains("amPolicyDataSet"));
        EXPECT_FALSE(got.contains("uePolicyDataSet"));
        EXPECT_FALSE(got.contains("umData"));
    }

    // Unknown ueId: real 200 {} (live view, no independent existence).
    sbi_core::http2::ClientRequest empty_req;
    empty_req.method = "GET";
    empty_req.url = "https://127.0.0.1:7781/nudr-dr/v2/policy-data/ues/imsi-999700000099999";
    empty_req.headers.emplace("authorization", "Bearer " + token);
    auto empty_resp = client.send(empty_req);
    ASSERT_TRUE(empty_resp.has_value());
    EXPECT_EQ(empty_resp->status, 200) << empty_resp->body;
    if (empty_resp->status == 200) {
        EXPECT_TRUE(json::parse(empty_resp->body).empty());
    }
}

TEST(UdrPolicyDataGapClosureIntegration, ReadBdtDataListsSeededDataAndHonorsFilter) {
    auto d = spawn_nrf_udr();
    auto client = make_client();
    const std::string collection_url = "https://127.0.0.1:7781/nudr-dr/v2/policy-data/bdt-data";
    ASSERT_TRUE(wait_reachable(client, collection_url, 200)) << "udr never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const json bdt_data = json{
        {"aspId", "asp-1"},
        {"bdtRefId", "bdt-test-1"},
        {"transPolicy",
         json{{"ratingGroup", 1},
              {"transPolicyId", 1},
              {"recTimeInt",
               json{{"startTime", "2026-08-26T00:00:00Z"}, {"stopTime", "2026-08-26T01:00:00Z"}}}}},
    };
    sbi_core::http2::ClientRequest put_req;
    put_req.method = "PUT";
    put_req.url = "https://127.0.0.1:7781/nudr-dr/v2/policy-data/bdt-data/bdt-test-1";
    put_req.headers.emplace("content-type", "application/json");
    put_req.headers.emplace("authorization", "Bearer " + token);
    put_req.body = bdt_data.dump();
    auto put_resp = client.send(put_req);
    ASSERT_TRUE(put_resp.has_value());
    EXPECT_EQ(put_resp->status, 201) << put_resp->body;

    sbi_core::http2::ClientRequest list_req;
    list_req.method = "GET";
    list_req.url = collection_url;
    list_req.headers.emplace("authorization", "Bearer " + token);
    auto list_resp = client.send(list_req);
    ASSERT_TRUE(list_resp.has_value());
    EXPECT_EQ(list_resp->status, 200) << list_resp->body;
    if (list_resp->status == 200) {
        const auto arr = json::parse(list_resp->body);
        bool found = false;
        for (const auto& item : arr) {
            if (item.value("bdtRefId", "") == "bdt-test-1") {
                found = true;
            }
        }
        EXPECT_TRUE(found);
    }

    sbi_core::http2::ClientRequest filtered_req;
    filtered_req.method = "GET";
    filtered_req.url = collection_url + "?bdt-ref-ids=nonexistent-ref";
    filtered_req.headers.emplace("authorization", "Bearer " + token);
    auto filtered_resp = client.send(filtered_req);
    ASSERT_TRUE(filtered_resp.has_value());
    EXPECT_EQ(filtered_resp->status, 200) << filtered_resp->body;
    if (filtered_resp->status == 200) {
        EXPECT_TRUE(json::parse(filtered_resp->body).empty());
    }
}

TEST(UdrPolicyDataGapClosureIntegration, PolicyDataSubsToNotifyLifecycle) {
    auto d = spawn_nrf_udr();
    auto client = make_client();
    const std::string collection_url =
        "https://127.0.0.1:7781/nudr-dr/v2/policy-data/subs-to-notify";
    ASSERT_TRUE(wait_reachable(client, collection_url, 200)) << "udr never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const json subscription = json{
        {"notificationUri", "https://example.com/policy-data-notify"},
        {"monitoredResourceUris",
         json::array({"https://127.0.0.1:7781/nudr-dr/v2/policy-data/ues/imsi-999700000000001"})},
    };
    sbi_core::http2::ClientRequest create_req;
    create_req.method = "POST";
    create_req.url = collection_url;
    create_req.headers.emplace("content-type", "application/json");
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = subscription.dump();
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    EXPECT_EQ(create_resp->status, 201) << create_resp->body;

    std::string subs_id;
    if (create_resp->status == 201) {
        const auto loc_it = create_resp->headers.find("location");
        ASSERT_NE(loc_it, create_resp->headers.end());
        const auto pos = loc_it->second.find_last_of('/');
        ASSERT_NE(pos, std::string::npos);
        subs_id = loc_it->second.substr(pos + 1);
    }
    ASSERT_FALSE(subs_id.empty());
    const std::string individual_url = collection_url + "/" + subs_id;

    sbi_core::http2::ClientRequest list_req;
    list_req.method = "GET";
    list_req.url = collection_url;
    list_req.headers.emplace("authorization", "Bearer " + token);
    auto list_resp = client.send(list_req);
    ASSERT_TRUE(list_resp.has_value());
    EXPECT_EQ(list_resp->status, 200) << list_resp->body;
    if (list_resp->status == 200) {
        EXPECT_GE(json::parse(list_resp->body).size(), 1u);
    }

    sbi_core::http2::ClientRequest get_req;
    get_req.method = "GET";
    get_req.url = individual_url;
    get_req.headers.emplace("authorization", "Bearer " + token);
    auto get_resp = client.send(get_req);
    ASSERT_TRUE(get_resp.has_value());
    EXPECT_EQ(get_resp->status, 200) << get_resp->body;

    const json replaced = json{
        {"notificationUri", "https://example.com/policy-data-notify-v2"},
        {"monitoredResourceUris",
         json::array({"https://127.0.0.1:7781/nudr-dr/v2/policy-data/ues/imsi-999700000000001"})},
    };
    sbi_core::http2::ClientRequest put_req;
    put_req.method = "PUT";
    put_req.url = individual_url;
    put_req.headers.emplace("content-type", "application/json");
    put_req.headers.emplace("authorization", "Bearer " + token);
    put_req.body = replaced.dump();
    auto put_resp = client.send(put_req);
    ASSERT_TRUE(put_resp.has_value());
    EXPECT_EQ(put_resp->status, 200) << put_resp->body;
    if (put_resp->status == 200) {
        EXPECT_EQ(json::parse(put_resp->body).at("notificationUri").get<std::string>(),
                  "https://example.com/policy-data-notify-v2");
    }

    sbi_core::http2::ClientRequest delete_req;
    delete_req.method = "DELETE";
    delete_req.url = individual_url;
    delete_req.headers.emplace("authorization", "Bearer " + token);
    auto delete_resp = client.send(delete_req);
    ASSERT_TRUE(delete_resp.has_value());
    EXPECT_EQ(delete_resp->status, 204);

    auto get_after_delete_resp = client.send(get_req);
    ASSERT_TRUE(get_after_delete_resp.has_value());
    EXPECT_EQ(get_after_delete_resp->status, 404);
}
