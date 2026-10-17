// Drives nrf and udm as real, separate OS processes to exercise UDM's Nudm_SDM Modify gap-closure
// (ADR-0232, docs/CAPABILITY_GAP_ANALYSIS.md's own UDM audit): real RFC 7396 JSON Merge Patch
// (PATCH /{ueId}/sdm-subscriptions/{subscriptionId}), completing Subscribe/Unsubscribe/Modify.

#include "sbi_core/http2_client.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "TS26510_CommonData_grp.hpp"
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

TEST(UdmSdmGapClosure232Integration, ModifyMergePatchesExistingSubscription) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess udm(UDM_PATH);
    ASSERT_GT(udm.pid(), 0) << "failed to fork udm";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client,
        "https://127.0.0.1:7780/nudm-uecm/v1/nonexistent/registrations/amf-3gpp-access",
        50))
        << "udm never became reachable";

    const std::string token = fetch_token(client, "nudm-sdm");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const std::string ue_id = "imsi-999700000000004";
    const std::string base = "https://127.0.0.1:7780/nudm-sdm/v2/" + ue_id + "/sdm-subscriptions";

    // Subscribe first (already-real, pre-existing route).
    sbi_gen::SdmSubscription sub_data{};
    sub_data.nfInstanceId = "00000000-0000-4000-8000-000000000bbb";
    sub_data.callbackReference = "https://example.com/cb";
    sub_data.monitoredResourceUris = {"https://example.com/res"};
    sbi_core::http2::ClientRequest sub_req;
    sub_req.method = "POST";
    sub_req.url = base;
    sub_req.headers.emplace("content-type", "application/json");
    sub_req.headers.emplace("authorization", "Bearer " + token);
    sub_req.body = json(sub_data).dump();
    auto sub_resp = client.send(sub_req);
    ASSERT_TRUE(sub_resp.has_value());
    ASSERT_EQ(sub_resp->status, 201) << sub_resp->body;
    const auto created = json::parse(sub_resp->body).get<sbi_gen::SdmSubscription>();
    ASSERT_TRUE(created.subscriptionId.has_value());
    const std::string subscription_id = *created.subscriptionId;

    // Modify: real RFC 7396 merge-patch, only `expires` changes.
    sbi_core::http2::ClientRequest patch_req;
    patch_req.method = "PATCH";
    patch_req.url = base + "/" + subscription_id;
    patch_req.headers.emplace("content-type", "application/merge-patch+json");
    patch_req.headers.emplace("authorization", "Bearer " + token);
    patch_req.body = json{{"expires", "2027-01-01T00:00:00Z"}}.dump();
    auto patch_resp = client.send(patch_req);
    ASSERT_TRUE(patch_resp.has_value());
    ASSERT_EQ(patch_resp->status, 200) << patch_resp->body;
    const auto patched = json::parse(patch_resp->body).get<sbi_gen::SdmSubscription>();
    ASSERT_TRUE(patched.expires.has_value());
    EXPECT_EQ(*patched.expires, "2027-01-01T00:00:00Z");
    // Merge-patch semantics: fields not mentioned in the patch survive unchanged.
    EXPECT_EQ(patched.nfInstanceId, sub_data.nfInstanceId);
    EXPECT_EQ(patched.callbackReference, sub_data.callbackReference);

    // A subscriptionId that exists but belongs to a different ueId real-404s.
    sbi_core::http2::ClientRequest wrong_owner_req;
    wrong_owner_req.method = "PATCH";
    wrong_owner_req.url = "https://127.0.0.1:7780/nudm-sdm/v2/imsi-999999999999999/"
                          "sdm-subscriptions/" +
                          subscription_id;
    wrong_owner_req.headers.emplace("content-type", "application/merge-patch+json");
    wrong_owner_req.headers.emplace("authorization", "Bearer " + token);
    wrong_owner_req.body = json{{"expires", "2027-01-01T00:00:00Z"}}.dump();
    auto wrong_owner_resp = client.send(wrong_owner_req);
    ASSERT_TRUE(wrong_owner_resp.has_value());
    EXPECT_EQ(wrong_owner_resp->status, 404) << wrong_owner_resp->body;

    // A genuinely unknown subscriptionId real-404s.
    sbi_core::http2::ClientRequest unknown_req;
    unknown_req.method = "PATCH";
    unknown_req.url = base + "/nonexistent";
    unknown_req.headers.emplace("content-type", "application/merge-patch+json");
    unknown_req.headers.emplace("authorization", "Bearer " + token);
    unknown_req.body = json{{"expires", "2027-01-01T00:00:00Z"}}.dump();
    auto unknown_resp = client.send(unknown_req);
    ASSERT_TRUE(unknown_resp.has_value());
    EXPECT_EQ(unknown_resp->status, 404) << unknown_resp->body;
}
