// Drives nrf and pcf as real, separate OS processes to exercise pcf's Npcf_AMPolicyAuthorization
// (TS29534), Npcf_MBSPolicyAuthorization (TS29537), and Npcf_MBSPolicyControl (TS29537) surfaces
// (ADR-0205, gap-closure task #163, second PCF slice) over real TLS 1.3 + mTLS HTTP/2 with a real
// signed OAuth2 token.

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

} // namespace

TEST(AmPolicyAuthorizationIntegration, CreateReadUpdateDeleteAndEventsSubscriptionLifecycle) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client,
        "https://127.0.0.1:7783/npcf-am-policyauthorization/v1/app-am-contexts/nonexistent",
        50))
        << "pcf never became reachable";

    const std::string token = fetch_token(client, "npcf-am-policyauthorization");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const json create_body = json{
        {"supi", "imsi-999700000000401"},
        {"termNotifUri", "https://example.com/am-auth-term-notify"},
        {"highThruInd", true},
    };
    sbi_core::http2::ClientRequest create_req;
    create_req.method = "POST";
    create_req.url = "https://127.0.0.1:7783/npcf-am-policyauthorization/v1/app-am-contexts";
    create_req.headers.emplace("content-type", "application/json");
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = create_body.dump();
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    EXPECT_EQ(create_resp->status, 201) << create_resp->body;

    std::string context_id;
    if (create_resp->status == 201) {
        auto loc = create_resp->headers.find("location");
        EXPECT_NE(loc, create_resp->headers.end());
        if (loc != create_resp->headers.end()) {
            context_id = loc->second.substr(loc->second.rfind('/') + 1);
        }
        const auto created = json::parse(create_resp->body);
        EXPECT_EQ(created.at("supi"), "imsi-999700000000401");
    }
    ASSERT_FALSE(context_id.empty());

    sbi_core::http2::ClientRequest get_req;
    get_req.method = "GET";
    get_req.url =
        "https://127.0.0.1:7783/npcf-am-policyauthorization/v1/app-am-contexts/" + context_id;
    get_req.headers.emplace("authorization", "Bearer " + token);
    auto get_resp = client.send(get_req);
    ASSERT_TRUE(get_resp.has_value());
    EXPECT_EQ(get_resp->status, 200);

    sbi_core::http2::ClientRequest patch_req;
    patch_req.method = "PATCH";
    patch_req.url =
        "https://127.0.0.1:7783/npcf-am-policyauthorization/v1/app-am-contexts/" + context_id;
    patch_req.headers.emplace("content-type", "application/merge-patch+json");
    patch_req.headers.emplace("authorization", "Bearer " + token);
    patch_req.body = json{{"highThruInd", false}}.dump();
    auto patch_resp = client.send(patch_req);
    ASSERT_TRUE(patch_resp.has_value());
    EXPECT_EQ(patch_resp->status, 200) << patch_resp->body;
    if (patch_resp->status == 200) {
        const auto patched = json::parse(patch_resp->body);
        EXPECT_EQ(patched.at("highThruInd"), false);
    }

    const json evsubsc_body = json{{"eventNotifUri", "https://example.com/am-auth-ev-notify"}};
    sbi_core::http2::ClientRequest evsubsc_req;
    evsubsc_req.method = "PUT";
    evsubsc_req.url = "https://127.0.0.1:7783/npcf-am-policyauthorization/v1/app-am-contexts/" +
                      context_id + "/events-subscription";
    evsubsc_req.headers.emplace("content-type", "application/json");
    evsubsc_req.headers.emplace("authorization", "Bearer " + token);
    evsubsc_req.body = evsubsc_body.dump();
    auto evsubsc_resp = client.send(evsubsc_req);
    ASSERT_TRUE(evsubsc_resp.has_value());
    EXPECT_EQ(evsubsc_resp->status, 201) << evsubsc_resp->body;
    if (evsubsc_resp->status == 201) {
        auto loc = evsubsc_resp->headers.find("location");
        EXPECT_NE(loc, evsubsc_resp->headers.end());
    }

    // Repeating the PUT against an existing subscription is a real modification -> 200, not 201.
    auto evsubsc_modify_resp = client.send(evsubsc_req);
    ASSERT_TRUE(evsubsc_modify_resp.has_value());
    EXPECT_EQ(evsubsc_modify_resp->status, 200);

    sbi_core::http2::ClientRequest evsubsc_delete_req;
    evsubsc_delete_req.method = "DELETE";
    evsubsc_delete_req.url = evsubsc_req.url;
    evsubsc_delete_req.headers.emplace("authorization", "Bearer " + token);
    auto evsubsc_delete_resp = client.send(evsubsc_delete_req);
    ASSERT_TRUE(evsubsc_delete_resp.has_value());
    EXPECT_EQ(evsubsc_delete_resp->status, 204);

    sbi_core::http2::ClientRequest delete_req;
    delete_req.method = "DELETE";
    delete_req.url =
        "https://127.0.0.1:7783/npcf-am-policyauthorization/v1/app-am-contexts/" + context_id;
    delete_req.headers.emplace("authorization", "Bearer " + token);
    auto delete_resp = client.send(delete_req);
    ASSERT_TRUE(delete_resp.has_value());
    EXPECT_EQ(delete_resp->status, 204);

    auto delete_again_resp = client.send(delete_req);
    ASSERT_TRUE(delete_again_resp.has_value());
    EXPECT_EQ(delete_again_resp->status, 404);
}

TEST(AmPolicyAuthorizationIntegration, CreateWithMissingRequiredFieldIs400) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client,
        "https://127.0.0.1:7783/npcf-am-policyauthorization/v1/app-am-contexts/nonexistent",
        50))
        << "pcf never became reachable";

    const std::string token = fetch_token(client, "npcf-am-policyauthorization");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // termNotifUri is required and missing.
    const json bad_body = json{{"supi", "imsi-999700000000401"}};
    sbi_core::http2::ClientRequest bad_req;
    bad_req.method = "POST";
    bad_req.url = "https://127.0.0.1:7783/npcf-am-policyauthorization/v1/app-am-contexts";
    bad_req.headers.emplace("content-type", "application/json");
    bad_req.headers.emplace("authorization", "Bearer " + token);
    bad_req.body = bad_body.dump();
    auto bad_resp = client.send(bad_req);
    ASSERT_TRUE(bad_resp.has_value());
    EXPECT_EQ(bad_resp->status, 400);
}

TEST(MbsPolicyAuthorizationIntegration, CreateReadModifyDeleteLifecycle) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7783/npcf-mbspolicyauth/v1/contexts/nonexistent", 50))
        << "pcf never became reachable";

    const std::string token = fetch_token(client, "npcf-mbspolicyauth");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // mbsSessionId's own subfields (tmgi/ssm/nid) are all optional per TS 29.571 -- an empty
    // object is a real, structurally-valid value, not a placeholder.
    const json create_body = json{{"mbsSessionId", json::object()}};
    sbi_core::http2::ClientRequest create_req;
    create_req.method = "POST";
    create_req.url = "https://127.0.0.1:7783/npcf-mbspolicyauth/v1/contexts";
    create_req.headers.emplace("content-type", "application/json");
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = create_body.dump();
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    EXPECT_EQ(create_resp->status, 201) << create_resp->body;

    std::string context_id;
    if (create_resp->status == 201) {
        auto loc = create_resp->headers.find("location");
        EXPECT_NE(loc, create_resp->headers.end());
        if (loc != create_resp->headers.end()) {
            context_id = loc->second.substr(loc->second.rfind('/') + 1);
        }
    }
    ASSERT_FALSE(context_id.empty());

    sbi_core::http2::ClientRequest get_req;
    get_req.method = "GET";
    get_req.url = "https://127.0.0.1:7783/npcf-mbspolicyauth/v1/contexts/" + context_id;
    get_req.headers.emplace("authorization", "Bearer " + token);
    auto get_resp = client.send(get_req);
    ASSERT_TRUE(get_resp.has_value());
    EXPECT_EQ(get_resp->status, 200);

    sbi_core::http2::ClientRequest patch_req;
    patch_req.method = "PATCH";
    patch_req.url = "https://127.0.0.1:7783/npcf-mbspolicyauth/v1/contexts/" + context_id;
    patch_req.headers.emplace("content-type", "application/merge-patch+json");
    patch_req.headers.emplace("authorization", "Bearer " + token);
    patch_req.body = json{}.dump();
    auto patch_resp = client.send(patch_req);
    ASSERT_TRUE(patch_resp.has_value());
    EXPECT_EQ(patch_resp->status, 200) << patch_resp->body;

    sbi_core::http2::ClientRequest delete_req;
    delete_req.method = "DELETE";
    delete_req.url = "https://127.0.0.1:7783/npcf-mbspolicyauth/v1/contexts/" + context_id;
    delete_req.headers.emplace("authorization", "Bearer " + token);
    auto delete_resp = client.send(delete_req);
    ASSERT_TRUE(delete_resp.has_value());
    EXPECT_EQ(delete_resp->status, 204);

    auto delete_again_resp = client.send(delete_req);
    ASSERT_TRUE(delete_again_resp.has_value());
    EXPECT_EQ(delete_again_resp->status, 404);
}

TEST(MbsPolicyAuthorizationIntegration, CreateWithMissingRequiredFieldIs400) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7783/npcf-mbspolicyauth/v1/contexts/nonexistent", 50))
        << "pcf never became reachable";

    const std::string token = fetch_token(client, "npcf-mbspolicyauth");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // mbsSessionId is required and missing.
    sbi_core::http2::ClientRequest bad_req;
    bad_req.method = "POST";
    bad_req.url = "https://127.0.0.1:7783/npcf-mbspolicyauth/v1/contexts";
    bad_req.headers.emplace("content-type", "application/json");
    bad_req.headers.emplace("authorization", "Bearer " + token);
    bad_req.body = json{{"dnn", "internet"}}.dump();
    auto bad_resp = client.send(bad_req);
    ASSERT_TRUE(bad_resp.has_value());
    EXPECT_EQ(bad_resp->status, 400);
}

TEST(MbsPolicyControlIntegration, CreateReadUpdateDeleteLifecycle) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7783/npcf-mbspolicycontrol/v1/mbs-policies/nonexistent", 50))
        << "pcf never became reachable";

    const std::string token = fetch_token(client, "npcf-mbspolicycontrol");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const json create_body = json{{"mbsSessionId", json::object()}, {"dnn", "internet"}};
    sbi_core::http2::ClientRequest create_req;
    create_req.method = "POST";
    create_req.url = "https://127.0.0.1:7783/npcf-mbspolicycontrol/v1/mbs-policies";
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
        EXPECT_EQ(created.at("mbsPolicyCtxtData").at("dnn"), "internet");
    }
    ASSERT_FALSE(policy_id.empty());

    sbi_core::http2::ClientRequest get_req;
    get_req.method = "GET";
    get_req.url = "https://127.0.0.1:7783/npcf-mbspolicycontrol/v1/mbs-policies/" + policy_id;
    get_req.headers.emplace("authorization", "Bearer " + token);
    auto get_resp = client.send(get_req);
    ASSERT_TRUE(get_resp.has_value());
    EXPECT_EQ(get_resp->status, 200);

    sbi_core::http2::ClientRequest update_req;
    update_req.method = "POST";
    update_req.url =
        "https://127.0.0.1:7783/npcf-mbspolicycontrol/v1/mbs-policies/" + policy_id + "/update";
    update_req.headers.emplace("content-type", "application/json");
    update_req.headers.emplace("authorization", "Bearer " + token);
    update_req.body = json{}.dump();
    auto update_resp = client.send(update_req);
    ASSERT_TRUE(update_resp.has_value());
    EXPECT_EQ(update_resp->status, 200) << update_resp->body;

    sbi_core::http2::ClientRequest delete_req;
    delete_req.method = "DELETE";
    delete_req.url = "https://127.0.0.1:7783/npcf-mbspolicycontrol/v1/mbs-policies/" + policy_id;
    delete_req.headers.emplace("authorization", "Bearer " + token);
    auto delete_resp = client.send(delete_req);
    ASSERT_TRUE(delete_resp.has_value());
    EXPECT_EQ(delete_resp->status, 204);

    auto delete_again_resp = client.send(delete_req);
    ASSERT_TRUE(delete_again_resp.has_value());
    EXPECT_EQ(delete_again_resp->status, 404);
}

TEST(MbsPolicyControlIntegration, CreateWithMissingRequiredFieldIs400) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7783/npcf-mbspolicycontrol/v1/mbs-policies/nonexistent", 50))
        << "pcf never became reachable";

    const std::string token = fetch_token(client, "npcf-mbspolicycontrol");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // mbsSessionId is required and missing.
    sbi_core::http2::ClientRequest bad_req;
    bad_req.method = "POST";
    bad_req.url = "https://127.0.0.1:7783/npcf-mbspolicycontrol/v1/mbs-policies";
    bad_req.headers.emplace("content-type", "application/json");
    bad_req.headers.emplace("authorization", "Bearer " + token);
    bad_req.body = json{{"dnn", "internet"}}.dump();
    auto bad_resp = client.send(bad_req);
    ASSERT_TRUE(bad_resp.has_value());
    EXPECT_EQ(bad_resp->status, 400);
}
