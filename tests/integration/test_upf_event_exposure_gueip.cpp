// Drives nrf and upf as real, separate OS processes to exercise upf's
// TS29564_Nupf_EventExposure.yaml and TS29564_Nupf_GetUEPrivateIPaddrAndIdentifiers.yaml routes
// (ADR-0203) over real TLS 1.3 + mTLS -- UPF's first-ever inbound SBI server, alongside its
// existing PFCP/N4 one.
//
// Covers: Nupf_EventExposure's full real create/modify(RFC 6902 patch)/delete subscription
// lifecycle, and Nupf_GetUEPrivateIPaddrAndIdentifiers's SearchUeIpInfo (real, honestly-empty
// UeIpInfo -- disclosed in ADR-0203, this UPF has no real NAT/private-IP tracking pipeline).

#include "sbi_core/http2_client.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <cstdint>
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

bool wait_reachable(sbi_core::http2::Client& client,
                    const std::string& url,
                    const std::string& method,
                    int max_attempts) {
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        sbi_core::http2::ClientRequest req;
        req.method = method;
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
        "&scope=" + scope + "&targetNfType=UPF";
    auto resp = nf_test::send_token_request(req);
    if (!resp.has_value() || resp->status != 200) {
        return "";
    }
    return json::parse(resp->body).at("access_token").get<std::string>();
}

json make_subscription_body(const std::string& correlation_id) {
    return json{
        {"eventList", json::array({json{{"type", "QOS_MONITORING"}}})},
        {"eventNotifyUri", "https://example.com/upf-evt-notify"},
        {"notifyCorrelationId", correlation_id},
        {"eventReportingMode", json{{"trigger", "PERIODIC"}, {"repPeriod", 60}}},
        {"nfId", "00000000-0000-4000-8000-0000000000bb"},
    };
}

} // namespace

TEST(UpfEventExposureIntegration, CreateModifyDeleteLifecycle) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess upf(UPF_PATH);
    ASSERT_GT(upf.pid(), 0) << "failed to fork upf";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7796/nupf-ee/v1/ee-subscriptions/nonexistent", "DELETE", 50))
        << "upf never became reachable";

    const std::string token = fetch_token(client, "nupf-ee");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    sbi_core::http2::ClientRequest create_req;
    create_req.method = "POST";
    create_req.url = "https://127.0.0.1:7796/nupf-ee/v1/ee-subscriptions";
    create_req.headers.emplace("content-type", "application/json");
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = json{{"subscription", make_subscription_body("corr-1")}}.dump();
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    EXPECT_EQ(create_resp->status, 201) << create_resp->body;

    auto loc = create_resp->headers.find("location");
    ASSERT_NE(loc, create_resp->headers.end());
    EXPECT_NE(loc->second.find("/nupf-ee/v1/ee-subscriptions/"), std::string::npos);

    std::string sub_id;
    if (create_resp->status == 201) {
        const auto created_json = json::parse(create_resp->body);
        ASSERT_TRUE(created_json.contains("subscriptionId"));
        sub_id = created_json.at("subscriptionId").get<std::string>();
        EXPECT_EQ(created_json.at("subscription").at("notifyCorrelationId"), "corr-1");
    }
    ASSERT_FALSE(sub_id.empty());

    const json patch_doc = json::array(
        {json{{"op", "replace"}, {"path", "/notifyCorrelationId"}, {"value", "corr-2"}}});
    sbi_core::http2::ClientRequest patch_req;
    patch_req.method = "PATCH";
    patch_req.url = "https://127.0.0.1:7796/nupf-ee/v1/ee-subscriptions/" + sub_id;
    patch_req.headers.emplace("content-type", "application/json-patch+json");
    patch_req.headers.emplace("authorization", "Bearer " + token);
    patch_req.body = patch_doc.dump();
    auto patch_resp = client.send(patch_req);
    ASSERT_TRUE(patch_resp.has_value());
    EXPECT_EQ(patch_resp->status, 200) << patch_resp->body;
    if (patch_resp->status == 200) {
        const auto patched = json::parse(patch_resp->body);
        EXPECT_EQ(patched.at("notifyCorrelationId"), "corr-2");
    }

    sbi_core::http2::ClientRequest patch_missing_req = patch_req;
    patch_missing_req.url = "https://127.0.0.1:7796/nupf-ee/v1/ee-subscriptions/nonexistent-id";
    auto patch_missing_resp = client.send(patch_missing_req);
    ASSERT_TRUE(patch_missing_resp.has_value());
    EXPECT_EQ(patch_missing_resp->status, 404);

    sbi_core::http2::ClientRequest delete_req;
    delete_req.method = "DELETE";
    delete_req.url = "https://127.0.0.1:7796/nupf-ee/v1/ee-subscriptions/" + sub_id;
    delete_req.headers.emplace("authorization", "Bearer " + token);
    auto delete_resp = client.send(delete_req);
    ASSERT_TRUE(delete_resp.has_value());
    EXPECT_EQ(delete_resp->status, 204);

    auto delete_again_resp = client.send(delete_req);
    ASSERT_TRUE(delete_again_resp.has_value());
    EXPECT_EQ(delete_again_resp->status, 404);
}

TEST(UpfEventExposureIntegration, CreateWithMissingRequiredFieldIs400) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess upf(UPF_PATH);
    ASSERT_GT(upf.pid(), 0) << "failed to fork upf";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7796/nupf-ee/v1/ee-subscriptions/nonexistent", "DELETE", 50))
        << "upf never became reachable";

    const std::string token = fetch_token(client, "nupf-ee");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // eventNotifyUri is required and missing.
    json bad_subscription = make_subscription_body("corr-bad");
    bad_subscription.erase("eventNotifyUri");

    sbi_core::http2::ClientRequest bad_req;
    bad_req.method = "POST";
    bad_req.url = "https://127.0.0.1:7796/nupf-ee/v1/ee-subscriptions";
    bad_req.headers.emplace("content-type", "application/json");
    bad_req.headers.emplace("authorization", "Bearer " + token);
    bad_req.body = json{{"subscription", bad_subscription}}.dump();
    auto bad_resp = client.send(bad_req);
    ASSERT_TRUE(bad_resp.has_value());
    EXPECT_EQ(bad_resp->status, 400);
}

TEST(UpfGetUeIpInfoIntegration, SearchUeIpInfoReturnsHonestlyEmptyResult) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess upf(UPF_PATH);
    ASSERT_GT(upf.pid(), 0) << "failed to fork upf";

    auto client = make_client();
    ASSERT_TRUE(
        wait_reachable(client, "https://127.0.0.1:7796/nupf-gueip/v1/ue-ip-info", "GET", 200))
        << "upf never became reachable";

    const std::string token = fetch_token(client, "nupf-gueip");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    sbi_core::http2::ClientRequest req;
    req.method = "GET";
    req.url = "https://127.0.0.1:7796/nupf-gueip/v1/ue-ip-info?supi=imsi-999700000000401";
    req.headers.emplace("authorization", "Bearer " + token);
    auto resp = client.send(req);
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->status, 200) << resp->body;
    if (resp->status == 200) {
        // Disclosed simplification (ADR-0203): no real per-UE private/public IP or NAT-mapping
        // pipeline exists yet, so every real, structurally-valid field is honestly absent rather
        // than fabricated.
        const auto body = json::parse(resp->body);
        EXPECT_TRUE(body.is_object());
        EXPECT_FALSE(body.contains("privateIpv4Address"));
    }

    // Note: a missing Authorization header is deliberately NOT itself a 401 in this project's
    // established check_bearer pattern (see this file's own comment on it, mirrored from
    // nfs/smf, nfs/amf, nfs/nrf) -- it's a real bootstrap-security alternative
    // (`security: [{}, oAuth2ClientCredentials:[...]]`), not tested again per-NF here.
}
