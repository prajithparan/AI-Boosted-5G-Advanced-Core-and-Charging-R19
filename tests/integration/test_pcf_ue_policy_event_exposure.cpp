// Drives nrf and pcf as real, separate OS processes to exercise pcf's Npcf_UEPolicyControl
// (TS29525) and Npcf_EventExposure (TS29523) surfaces (ADR-0204, gap-closure task #163) over real
// TLS 1.3 + mTLS HTTP/2 with a real signed OAuth2 token.

#include "sbi_core/http2_client.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

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
        "&scope=" + scope + "&targetNfType=PCF";
    auto resp = nf_test::send_token_request(req);
    if (!resp.has_value() || resp->status != 200) {
        return "";
    }
    return json::parse(resp->body).at("access_token").get<std::string>();
}

struct Duo {
    nf_test::SpawnedProcess nrf;
    nf_test::SpawnedProcess pcf;
};

Duo spawn_all() {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    nf_test::SpawnedProcess pcf(PCF_PATH);
    return Duo{std::move(nrf), std::move(pcf)};
}

json make_ue_policy_request() {
    return json{
        {"notificationUri", "https://example.com/pcf-ue-policy-notify"},
        {"supi", "imsi-999700000000401"},
        {"suppFeat", ""},
    };
}

} // namespace

TEST(PcfUePolicyIntegration, CreateReadUpdateDeleteLifecycle) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7783/npcf-ue-policy-control/v1/policies/nonexistent", 50))
        << "pcf never became reachable";

    const std::string token = fetch_token(client, "npcf-ue-policy-control");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    sbi_core::http2::ClientRequest create_req;
    create_req.method = "POST";
    create_req.url = "https://127.0.0.1:7783/npcf-ue-policy-control/v1/policies";
    create_req.headers.emplace("content-type", "application/json");
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = make_ue_policy_request().dump();
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    EXPECT_EQ(create_resp->status, 201) << create_resp->body;

    std::string pol_asso_id;
    if (create_resp->status == 201) {
        auto loc = create_resp->headers.find("location");
        EXPECT_NE(loc, create_resp->headers.end());
        if (loc != create_resp->headers.end()) {
            EXPECT_NE(loc->second.find("/npcf-ue-policy-control/v1/policies/"), std::string::npos);
            pol_asso_id = loc->second.substr(loc->second.rfind('/') + 1);
        }
        const auto created = json::parse(create_resp->body);
        EXPECT_EQ(created.at("request").at("supi"), "imsi-999700000000401");
    }
    ASSERT_FALSE(pol_asso_id.empty());

    sbi_core::http2::ClientRequest get_req;
    get_req.method = "GET";
    get_req.url = "https://127.0.0.1:7783/npcf-ue-policy-control/v1/policies/" + pol_asso_id;
    get_req.headers.emplace("authorization", "Bearer " + token);
    auto get_resp = client.send(get_req);
    ASSERT_TRUE(get_resp.has_value());
    EXPECT_EQ(get_resp->status, 200);

    sbi_core::http2::ClientRequest update_req;
    update_req.method = "POST";
    update_req.url =
        "https://127.0.0.1:7783/npcf-ue-policy-control/v1/policies/" + pol_asso_id + "/update";
    update_req.headers.emplace("content-type", "application/json");
    update_req.headers.emplace("authorization", "Bearer " + token);
    update_req.body = json{{"triggers", json::array({"PLMN_CH"})}}.dump();
    auto update_resp = client.send(update_req);
    ASSERT_TRUE(update_resp.has_value());
    EXPECT_EQ(update_resp->status, 200) << update_resp->body;
    if (update_resp->status == 200) {
        const auto update_result = json::parse(update_resp->body);
        EXPECT_EQ(update_result.at("resourceUri"),
                  "/npcf-ue-policy-control/v1/policies/" + pol_asso_id);
        // Disclosed simplification (ADR-0204): no real URSP/ANDSP generation, so uePolicy is
        // honestly absent, not fabricated.
        EXPECT_FALSE(update_result.contains("uePolicy"));
    }

    sbi_core::http2::ClientRequest delete_req;
    delete_req.method = "DELETE";
    delete_req.url = "https://127.0.0.1:7783/npcf-ue-policy-control/v1/policies/" + pol_asso_id;
    delete_req.headers.emplace("authorization", "Bearer " + token);
    auto delete_resp = client.send(delete_req);
    ASSERT_TRUE(delete_resp.has_value());
    EXPECT_EQ(delete_resp->status, 204);

    auto delete_again_resp = client.send(delete_req);
    ASSERT_TRUE(delete_again_resp.has_value());
    EXPECT_EQ(delete_again_resp->status, 404);
}

TEST(PcfUePolicyIntegration, CreateWithMissingRequiredFieldIs400) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7783/npcf-ue-policy-control/v1/policies/nonexistent", 50))
        << "pcf never became reachable";

    const std::string token = fetch_token(client, "npcf-ue-policy-control");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // supi is required and missing.
    json bad_body = make_ue_policy_request();
    bad_body.erase("supi");

    sbi_core::http2::ClientRequest bad_req;
    bad_req.method = "POST";
    bad_req.url = "https://127.0.0.1:7783/npcf-ue-policy-control/v1/policies";
    bad_req.headers.emplace("content-type", "application/json");
    bad_req.headers.emplace("authorization", "Bearer " + token);
    bad_req.body = bad_body.dump();
    auto bad_resp = client.send(bad_req);
    ASSERT_TRUE(bad_resp.has_value());
    EXPECT_EQ(bad_resp->status, 400);
}

TEST(PcfEventExposureIntegration, CreateReadReplaceDeleteLifecycle) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7783/npcf-eventexposure/v1/subscriptions/nonexistent", 50))
        << "pcf never became reachable";

    const std::string token = fetch_token(client, "npcf-eventexposure");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const json create_body = json{
        {"eventSubs", json::array({"AC_TY_CH"})},
        {"notifId", "pcf-notif-1"},
        {"notifUri", "https://example.com/pcf-evt-notify"},
    };
    sbi_core::http2::ClientRequest create_req;
    create_req.method = "POST";
    create_req.url = "https://127.0.0.1:7783/npcf-eventexposure/v1/subscriptions";
    create_req.headers.emplace("content-type", "application/json");
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = create_body.dump();
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    EXPECT_EQ(create_resp->status, 201) << create_resp->body;

    std::string sub_id;
    if (create_resp->status == 201) {
        auto loc = create_resp->headers.find("location");
        EXPECT_NE(loc, create_resp->headers.end());
        if (loc != create_resp->headers.end()) {
            EXPECT_NE(loc->second.find("/npcf-eventexposure/v1/subscriptions/"), std::string::npos);
            sub_id = loc->second.substr(loc->second.rfind('/') + 1);
        }
        const auto created = json::parse(create_resp->body);
        EXPECT_EQ(created.at("notifId"), "pcf-notif-1");
    }
    ASSERT_FALSE(sub_id.empty());

    sbi_core::http2::ClientRequest get_req;
    get_req.method = "GET";
    get_req.url = "https://127.0.0.1:7783/npcf-eventexposure/v1/subscriptions/" + sub_id;
    get_req.headers.emplace("authorization", "Bearer " + token);
    auto get_resp = client.send(get_req);
    ASSERT_TRUE(get_resp.has_value());
    EXPECT_EQ(get_resp->status, 200);

    const json replace_body = json{
        {"eventSubs", json::array({"PLMN_CH"})},
        {"notifId", "pcf-notif-2"},
        {"notifUri", "https://example.com/pcf-evt-notify-2"},
    };
    sbi_core::http2::ClientRequest replace_req;
    replace_req.method = "PUT";
    replace_req.url = "https://127.0.0.1:7783/npcf-eventexposure/v1/subscriptions/" + sub_id;
    replace_req.headers.emplace("content-type", "application/json");
    replace_req.headers.emplace("authorization", "Bearer " + token);
    replace_req.body = replace_body.dump();
    auto replace_resp = client.send(replace_req);
    ASSERT_TRUE(replace_resp.has_value());
    EXPECT_EQ(replace_resp->status, 200) << replace_resp->body;
    if (replace_resp->status == 200) {
        const auto replaced = json::parse(replace_resp->body);
        EXPECT_EQ(replaced.at("notifId"), "pcf-notif-2");
    }

    sbi_core::http2::ClientRequest replace_missing_req = replace_req;
    replace_missing_req.url =
        "https://127.0.0.1:7783/npcf-eventexposure/v1/subscriptions/nonexistent-id";
    auto replace_missing_resp = client.send(replace_missing_req);
    ASSERT_TRUE(replace_missing_resp.has_value());
    EXPECT_EQ(replace_missing_resp->status, 404);

    sbi_core::http2::ClientRequest delete_req;
    delete_req.method = "DELETE";
    delete_req.url = "https://127.0.0.1:7783/npcf-eventexposure/v1/subscriptions/" + sub_id;
    delete_req.headers.emplace("authorization", "Bearer " + token);
    auto delete_resp = client.send(delete_req);
    ASSERT_TRUE(delete_resp.has_value());
    EXPECT_EQ(delete_resp->status, 204);

    auto delete_again_resp = client.send(delete_req);
    ASSERT_TRUE(delete_again_resp.has_value());
    EXPECT_EQ(delete_again_resp->status, 404);
}

TEST(PcfEventExposureIntegration, CreateWithMissingRequiredFieldIs400) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7783/npcf-eventexposure/v1/subscriptions/nonexistent", 50))
        << "pcf never became reachable";

    const std::string token = fetch_token(client, "npcf-eventexposure");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // notifUri is required and missing.
    const json bad_body =
        json{{"eventSubs", json::array({"AC_TY_CH"})}, {"notifId", "pcf-notif-bad"}};
    sbi_core::http2::ClientRequest bad_req;
    bad_req.method = "POST";
    bad_req.url = "https://127.0.0.1:7783/npcf-eventexposure/v1/subscriptions";
    bad_req.headers.emplace("content-type", "application/json");
    bad_req.headers.emplace("authorization", "Bearer " + token);
    bad_req.body = bad_body.dump();
    auto bad_resp = client.send(bad_req);
    ASSERT_TRUE(bad_resp.has_value());
    EXPECT_EQ(bad_resp->status, 400);
}
