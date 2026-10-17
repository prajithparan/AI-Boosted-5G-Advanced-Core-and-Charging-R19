// Drives nrf/udr/udm as real, separate OS processes to exercise UDM's Nudm_SDM shared-data group
// gap-closure (ADR-0235, docs/CAPABILITY_GAP_ANALYSIS.md's own UDM audit): GetIndividualSharedData,
// GetSharedData, GetGroupIdentifiers (all real proxies/compositions over UDR's own already-live
// SharedDataStore/GroupIdentifiersStore, ADR-0110/ADR-0140), plus SubscribeToSharedData/
// UnsubscribeForSharedData/ModifySharedDataSubs (backed by a brand-new UDM-local
// SharedDataSubscriptionStore).

#include "sbi_core/http2_client.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

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

std::string fetch_token(sbi_core::http2::Client& client, const std::string& scope) {
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = "https://127.0.0.1:7777/oauth2/token";
    req.headers.emplace("content-type", "application/x-www-form-urlencoded");
    req.body =
        "grant_type=client_credentials&nfInstanceId=" + std::string(nf_test::kTestAfInstanceId) +
        "&scope=" + scope + "&targetNfType=UDM";
    auto resp = nf_test::send_token_request(req);
    if (!resp.has_value() || resp->status != 200) {
        return "";
    }
    return json::parse(resp->body).at("access_token").get<std::string>();
}

} // namespace

TEST(UdmSdmGapClosure235Integration, SharedDataAndGroupIdentifiersReturnRealUdrSeededData) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess udr(UDR_PATH);
    ASSERT_GT(udr.pid(), 0) << "failed to fork udr";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7781/nudr-dr/v2/subscription-data/nonexistent", 50))
        << "udr never became reachable";

    nf_test::SpawnedProcess udm(UDM_PATH);
    ASSERT_GT(udm.pid(), 0) << "failed to fork udm";
    ASSERT_TRUE(wait_reachable(
        client,
        "https://127.0.0.1:7780/nudm-uecm/v1/nonexistent/registrations/amf-3gpp-access",
        50))
        << "udm never became reachable";

    const std::string token = fetch_token(client, "nudm-sdm");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // GetIndividualSharedData: real proxy to UDR's own seeded "10000-default"
    // (nfs/udr/src/main.cpp, ADR-0110).
    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = "https://127.0.0.1:7780/nudm-sdm/v2/shared-data/10000-default";
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 200) << resp->body;
        EXPECT_EQ(json::parse(resp->body).at("sharedDataId").get<std::string>(), "10000-default");
    }

    // GetIndividualSharedData for an unknown id correctly 404s.
    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = "https://127.0.0.1:7780/nudm-sdm/v2/shared-data/99999-nonexistent";
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 404) << resp->body;
    }

    // GetSharedData: real bulk composition over N individual UDR calls -- a mix of one known and
    // one unknown id in the same request; the unknown one is silently omitted (disclosed design,
    // ADR-0235), so the result array must contain exactly the known entry.
    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = "https://127.0.0.1:7780/nudm-sdm/v2/shared-data?shared-data-ids=10000-default,"
                  "99999-nonexistent";
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 200) << resp->body;
        auto result = json::parse(resp->body);
        ASSERT_TRUE(result.is_array());
        ASSERT_EQ(result.size(), 1u);
        EXPECT_EQ(result.at(0).at("sharedDataId").get<std::string>(), "10000-default");
    }

    // GetSharedData without the required shared-data-ids query param correctly 400s.
    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = "https://127.0.0.1:7780/nudm-sdm/v2/shared-data";
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 400) << resp->body;
    }

    // GetGroupIdentifiers: real proxy to UDR's own seeded group, by ext-group-id.
    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = "https://127.0.0.1:7780/nudm-sdm/v2/group-data/group-identifiers?ext-group-id="
                  "extgroupid-group1@example.com";
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 200) << resp->body;
        EXPECT_EQ(json::parse(resp->body).at("intGroupId").get<std::string>(),
                  "A1B2C3D4-001-01-AB");
    }

    // GetGroupIdentifiers by int-group-id.
    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = "https://127.0.0.1:7780/nudm-sdm/v2/group-data/group-identifiers?int-group-id="
                  "A1B2C3D4-001-01-AB";
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 200) << resp->body;
        EXPECT_EQ(json::parse(resp->body).at("extGroupId").get<std::string>(),
                  "extgroupid-group1@example.com");
    }

    // GetGroupIdentifiers with neither query param correctly 400s.
    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = "https://127.0.0.1:7780/nudm-sdm/v2/group-data/group-identifiers";
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 400) << resp->body;
    }

    // SubscribeToSharedData / ModifySharedDataSubs / UnsubscribeForSharedData: real UDM-local
    // SharedDataSubscriptionStore CRUD lifecycle, genuinely global (no ue_id ownership).
    std::string subscription_id;
    {
        json sub_body;
        sub_body["nfInstanceId"] = "5ba9a927-1d31-4c8e-8a10-000000000099";
        sub_body["callbackReference"] = "https://127.0.0.1:9999/callback";
        sub_body["monitoredResourceUris"] = json::array({"https://127.0.0.1:9999/resource"});
        sbi_core::http2::ClientRequest req;
        req.method = "POST";
        req.url = "https://127.0.0.1:7780/nudm-sdm/v2/shared-data-subscriptions";
        req.headers.emplace("content-type", "application/json");
        req.headers.emplace("authorization", "Bearer " + token);
        req.body = sub_body.dump();
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 201) << resp->body;
        EXPECT_TRUE(resp->headers.count("location") > 0);
        auto result = json::parse(resp->body);
        ASSERT_TRUE(result.contains("subscriptionId"));
        subscription_id = result.at("subscriptionId").get<std::string>();
        EXPECT_FALSE(subscription_id.empty());
    }

    {
        json patch_body;
        patch_body["nfInstanceId"] = "test-nf-instance-modified";
        sbi_core::http2::ClientRequest req;
        req.method = "PATCH";
        req.url = "https://127.0.0.1:7780/nudm-sdm/v2/shared-data-subscriptions/" + subscription_id;
        req.headers.emplace("content-type", "application/merge-patch+json");
        req.headers.emplace("authorization", "Bearer " + token);
        req.body = patch_body.dump();
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 200) << resp->body;
        EXPECT_EQ(json::parse(resp->body).at("nfInstanceId").get<std::string>(),
                  "test-nf-instance-modified");
    }

    {
        sbi_core::http2::ClientRequest req;
        req.method = "DELETE";
        req.url = "https://127.0.0.1:7780/nudm-sdm/v2/shared-data-subscriptions/" + subscription_id;
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 204) << resp->body;
    }

    // A second delete of the same, now-removed, subscription correctly 404s.
    {
        sbi_core::http2::ClientRequest req;
        req.method = "DELETE";
        req.url = "https://127.0.0.1:7780/nudm-sdm/v2/shared-data-subscriptions/" + subscription_id;
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 404) << resp->body;
    }
}
