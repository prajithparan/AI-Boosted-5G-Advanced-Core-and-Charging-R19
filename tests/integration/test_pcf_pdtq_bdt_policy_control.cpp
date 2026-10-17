// Drives nrf and pcf as real, separate OS processes to exercise pcf's Npcf_PDTQPolicyControl
// (TS29543) and Npcf_BDTPolicyControl (TS29554) surfaces (ADR-0206, gap-closure task #163, third
// and final PCF slice) over real TLS 1.3 + mTLS HTTP/2 with a real signed OAuth2 token.

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

json make_time_window() {
    return json{{"startTime", "2026-08-25T00:00:00Z"}, {"stopTime", "2026-08-26T00:00:00Z"}};
}

} // namespace

TEST(PdtqPolicyControlIntegration, CreateReadUpdateDeleteLifecycle) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7783/npcf-pdtq-policy-control/v1/pdtq-policies/nonexistent", 50))
        << "pcf never became reachable";

    const std::string token = fetch_token(client, "npcf-pdtq-policy-control");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const json create_body = json{
        {"aspId", "asp-1"},
        {"desTimeInts", json::array({make_time_window()})},
        {"numOfUes", 5},
        {"qosReference", "qos-ref-1"},
    };
    sbi_core::http2::ClientRequest create_req;
    create_req.method = "POST";
    create_req.url = "https://127.0.0.1:7783/npcf-pdtq-policy-control/v1/pdtq-policies";
    create_req.headers.emplace("content-type", "application/json");
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = create_body.dump();
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    EXPECT_EQ(create_resp->status, 201) << create_resp->body;

    std::string policy_id;
    if (create_resp->status == 201) {
        auto loc = create_resp->headers.find("location");
        EXPECT_NE(loc, create_resp->headers.end());
        if (loc != create_resp->headers.end()) {
            policy_id = loc->second.substr(loc->second.rfind('/') + 1);
        }
        const auto created = json::parse(create_resp->body);
        EXPECT_EQ(created.at("aspId"), "asp-1");
    }
    ASSERT_FALSE(policy_id.empty());

    sbi_core::http2::ClientRequest get_req;
    get_req.method = "GET";
    get_req.url = "https://127.0.0.1:7783/npcf-pdtq-policy-control/v1/pdtq-policies/" + policy_id;
    get_req.headers.emplace("authorization", "Bearer " + token);
    auto get_resp = client.send(get_req);
    ASSERT_TRUE(get_resp.has_value());
    EXPECT_EQ(get_resp->status, 200);

    sbi_core::http2::ClientRequest patch_req;
    patch_req.method = "PATCH";
    patch_req.url = "https://127.0.0.1:7783/npcf-pdtq-policy-control/v1/pdtq-policies/" + policy_id;
    patch_req.headers.emplace("content-type", "application/merge-patch+json");
    patch_req.headers.emplace("authorization", "Bearer " + token);
    patch_req.body = json{{"selPdtqPolicyId", 3}}.dump();
    auto patch_resp = client.send(patch_req);
    ASSERT_TRUE(patch_resp.has_value());
    EXPECT_EQ(patch_resp->status, 200) << patch_resp->body;
    if (patch_resp->status == 200) {
        const auto patched = json::parse(patch_resp->body);
        EXPECT_EQ(patched.at("selPdtqPolicyId"), 3);
    }

    sbi_core::http2::ClientRequest delete_req;
    delete_req.method = "DELETE";
    delete_req.url =
        "https://127.0.0.1:7783/npcf-pdtq-policy-control/v1/pdtq-policies/" + policy_id;
    delete_req.headers.emplace("authorization", "Bearer " + token);
    auto delete_resp = client.send(delete_req);
    ASSERT_TRUE(delete_resp.has_value());
    EXPECT_EQ(delete_resp->status, 204);

    auto delete_again_resp = client.send(delete_req);
    ASSERT_TRUE(delete_again_resp.has_value());
    EXPECT_EQ(delete_again_resp->status, 404);
}

TEST(PdtqPolicyControlIntegration, CreateWithMissingRequiredFieldIs400) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7783/npcf-pdtq-policy-control/v1/pdtq-policies/nonexistent", 50))
        << "pcf never became reachable";

    const std::string token = fetch_token(client, "npcf-pdtq-policy-control");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // numOfUes is required and missing.
    const json bad_body =
        json{{"aspId", "asp-1"}, {"desTimeInts", json::array({make_time_window()})}};
    sbi_core::http2::ClientRequest bad_req;
    bad_req.method = "POST";
    bad_req.url = "https://127.0.0.1:7783/npcf-pdtq-policy-control/v1/pdtq-policies";
    bad_req.headers.emplace("content-type", "application/json");
    bad_req.headers.emplace("authorization", "Bearer " + token);
    bad_req.body = bad_body.dump();
    auto bad_resp = client.send(bad_req);
    ASSERT_TRUE(bad_resp.has_value());
    EXPECT_EQ(bad_resp->status, 400);
}

TEST(BdtPolicyControlIntegration, CreateReadUpdateDeleteLifecycle) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7783/npcf-bdtpolicycontrol/v1/bdtpolicies/nonexistent", 50))
        << "pcf never became reachable";

    const std::string token = fetch_token(client, "npcf-bdtpolicycontrol");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const json create_body = json{
        {"aspId", "asp-1"},
        {"desTimeInt", make_time_window()},
        {"numOfUes", 5},
        {"volPerUe", json::object()},
    };
    sbi_core::http2::ClientRequest create_req;
    create_req.method = "POST";
    create_req.url = "https://127.0.0.1:7783/npcf-bdtpolicycontrol/v1/bdtpolicies";
    create_req.headers.emplace("content-type", "application/json");
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = create_body.dump();
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    EXPECT_EQ(create_resp->status, 201) << create_resp->body;

    std::string policy_id;
    if (create_resp->status == 201) {
        auto loc = create_resp->headers.find("location");
        EXPECT_NE(loc, create_resp->headers.end());
        if (loc != create_resp->headers.end()) {
            policy_id = loc->second.substr(loc->second.rfind('/') + 1);
        }
        const auto created = json::parse(create_resp->body);
        EXPECT_EQ(created.at("bdtReqData").at("aspId"), "asp-1");
        // Disclosed simplification (ADR-0206): no real BDT decision engine, so bdtPolData is
        // honestly absent, not fabricated.
        EXPECT_FALSE(created.contains("bdtPolData"));
    }
    ASSERT_FALSE(policy_id.empty());

    sbi_core::http2::ClientRequest get_req;
    get_req.method = "GET";
    get_req.url = "https://127.0.0.1:7783/npcf-bdtpolicycontrol/v1/bdtpolicies/" + policy_id;
    get_req.headers.emplace("authorization", "Bearer " + token);
    auto get_resp = client.send(get_req);
    ASSERT_TRUE(get_resp.has_value());
    EXPECT_EQ(get_resp->status, 200);

    sbi_core::http2::ClientRequest patch_req;
    patch_req.method = "PATCH";
    patch_req.url = "https://127.0.0.1:7783/npcf-bdtpolicycontrol/v1/bdtpolicies/" + policy_id;
    patch_req.headers.emplace("content-type", "application/merge-patch+json");
    patch_req.headers.emplace("authorization", "Bearer " + token);
    patch_req.body = json{{"bdtReqData", json{{"warnNotifReq", true}}}}.dump();
    auto patch_resp = client.send(patch_req);
    ASSERT_TRUE(patch_resp.has_value());
    EXPECT_EQ(patch_resp->status, 200) << patch_resp->body;
    if (patch_resp->status == 200) {
        const auto patched = json::parse(patch_resp->body);
        EXPECT_EQ(patched.at("bdtReqData").at("warnNotifReq"), true);
    }

    // Real, disclosed constraint: selecting a transfer policy is rejected since none were ever
    // offered (no real BDT decision engine exists in this build).
    sbi_core::http2::ClientRequest select_req;
    select_req.method = "PATCH";
    select_req.url = patch_req.url;
    select_req.headers.emplace("content-type", "application/merge-patch+json");
    select_req.headers.emplace("authorization", "Bearer " + token);
    select_req.body = json{{"bdtPolData", json{{"selTransPolicyId", 1}}}}.dump();
    auto select_resp = client.send(select_req);
    ASSERT_TRUE(select_resp.has_value());
    EXPECT_EQ(select_resp->status, 400);

    sbi_core::http2::ClientRequest delete_req;
    delete_req.method = "DELETE";
    delete_req.url = "https://127.0.0.1:7783/npcf-bdtpolicycontrol/v1/bdtpolicies/" + policy_id;
    delete_req.headers.emplace("authorization", "Bearer " + token);
    auto delete_resp = client.send(delete_req);
    ASSERT_TRUE(delete_resp.has_value());
    EXPECT_EQ(delete_resp->status, 204);

    auto delete_again_resp = client.send(delete_req);
    ASSERT_TRUE(delete_again_resp.has_value());
    EXPECT_EQ(delete_again_resp->status, 404);
}

TEST(BdtPolicyControlIntegration, CreateWithMissingRequiredFieldIs400) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7783/npcf-bdtpolicycontrol/v1/bdtpolicies/nonexistent", 50))
        << "pcf never became reachable";

    const std::string token = fetch_token(client, "npcf-bdtpolicycontrol");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // volPerUe is required and missing.
    const json bad_body =
        json{{"aspId", "asp-1"}, {"desTimeInt", make_time_window()}, {"numOfUes", 5}};
    sbi_core::http2::ClientRequest bad_req;
    bad_req.method = "POST";
    bad_req.url = "https://127.0.0.1:7783/npcf-bdtpolicycontrol/v1/bdtpolicies";
    bad_req.headers.emplace("content-type", "application/json");
    bad_req.headers.emplace("authorization", "Bearer " + token);
    bad_req.body = bad_body.dump();
    auto bad_resp = client.send(bad_req);
    ASSERT_TRUE(bad_resp.has_value());
    EXPECT_EQ(bad_resp->status, 400);
}
