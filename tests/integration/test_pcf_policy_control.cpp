// Drives nrf and pcf as real, separate OS processes to exercise pcf's Npcf_AMPolicyControl and
// Npcf_SMPolicyControl surfaces (docs/DECISIONS.md ADR-0028) over real TLS 1.3 + mTLS HTTP/2 with
// a real signed OAuth2 token. PCF is standalone this turn (not yet called by AMF/SMF), so this
// test plays the AMF/SMF role directly, same as every other NF's own integration test before its
// callers existed.

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

TEST(PcfIntegration, AmPolicyAssociationFullLifecycle) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7783/npcf-am-policy-control/v1/policies/nonexistent", 50))
        << "pcf never became reachable";

    const std::string token = fetch_token(client, "npcf-am-policy-control");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    sbi_gen::PolicyAssociationRequest_Npcf_AMPolicyControl create_data{};
    create_data.notificationUri = "https://example.com/am-policy-notify";
    create_data.supi = "imsi-999700000000001";
    create_data.suppFeat = "";

    sbi_core::http2::ClientRequest create_req;
    create_req.method = "POST";
    create_req.url = "https://127.0.0.1:7783/npcf-am-policy-control/v1/policies";
    create_req.headers.emplace("content-type", "application/json");
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = json(create_data).dump();

    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    ASSERT_EQ(create_resp->status, 201);
    const auto location_it = create_resp->headers.find("location");
    ASSERT_NE(location_it, create_resp->headers.end());
    const std::string pol_asso_url = "https://127.0.0.1:7783" + location_it->second;

    const auto created =
        json::parse(create_resp->body).get<sbi_gen::PolicyAssociation_Npcf_AMPolicyControl>();
    ASSERT_TRUE(created.request.has_value());
    EXPECT_EQ(created.request->supi, create_data.supi);
    ASSERT_TRUE(
        created.ueAmbr.has_value()); // PCF's default session AMBR, request didn't supply one

    sbi_core::http2::ClientRequest get_req;
    get_req.method = "GET";
    get_req.url = pol_asso_url;
    get_req.headers.emplace("authorization", "Bearer " + token);
    auto get_resp = client.send(get_req);
    ASSERT_TRUE(get_resp.has_value());
    EXPECT_EQ(get_resp->status, 200);

    // ReportObservedEventTriggersForIndividualAMPolicyAssociation: report a trigger, get an
    // updated ueAmbr back.
    sbi_gen::PolicyAssociationUpdateRequest_Npcf_AMPolicyControl update_data{};
    sbi_gen::RequestTrigger_Npcf_AMPolicyControl trigger{};
    trigger.value = "RFSP_CH";
    update_data.triggers = std::vector<sbi_gen::RequestTrigger_Npcf_AMPolicyControl>{trigger};
    sbi_gen::Ambr new_ambr{};
    new_ambr.uplink = "500 Mbps";
    new_ambr.downlink = "500 Mbps";
    update_data.ueAmbr = new_ambr;

    sbi_core::http2::ClientRequest update_req;
    update_req.method = "POST";
    update_req.url = pol_asso_url + "/update";
    update_req.headers.emplace("content-type", "application/json");
    update_req.headers.emplace("authorization", "Bearer " + token);
    update_req.body = json(update_data).dump();
    auto update_resp = client.send(update_req);
    ASSERT_TRUE(update_resp.has_value());
    EXPECT_EQ(update_resp->status, 200);
    const auto update_result =
        json::parse(update_resp->body).get<sbi_gen::PolicyUpdate_Npcf_AMPolicyControl>();
    ASSERT_TRUE(update_result.ueAmbr.has_value());
    EXPECT_EQ(update_result.ueAmbr->uplink, "500 Mbps");
    ASSERT_TRUE(update_result.triggers.has_value());
    ASSERT_EQ(update_result.triggers->size(), 1U);
    EXPECT_EQ((*update_result.triggers)[0].value, "RFSP_CH");

    // Confirm the update is really persisted, not just echoed.
    auto get_after_update = client.send(get_req);
    ASSERT_TRUE(get_after_update.has_value());
    const auto after_update =
        json::parse(get_after_update->body).get<sbi_gen::PolicyAssociation_Npcf_AMPolicyControl>();
    ASSERT_TRUE(after_update.ueAmbr.has_value());
    EXPECT_EQ(after_update.ueAmbr->uplink, "500 Mbps");

    sbi_core::http2::ClientRequest delete_req;
    delete_req.method = "DELETE";
    delete_req.url = pol_asso_url;
    delete_req.headers.emplace("authorization", "Bearer " + token);
    auto delete_resp = client.send(delete_req);
    ASSERT_TRUE(delete_resp.has_value());
    EXPECT_EQ(delete_resp->status, 204);

    auto get_again = client.send(get_req);
    ASSERT_TRUE(get_again.has_value());
    EXPECT_EQ(get_again->status, 404);

    auto delete_again = client.send(delete_req);
    ASSERT_TRUE(delete_again.has_value());
    EXPECT_EQ(delete_again->status, 404);
}

TEST(PcfIntegration, SmPolicyFullLifecycleUsesRequestSuppliedDefaults) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7783/npcf-am-policy-control/v1/policies/nonexistent", 50))
        << "pcf never became reachable";

    const std::string token = fetch_token(client, "npcf-smpolicycontrol");
    ASSERT_FALSE(token.empty());

    sbi_gen::SmPolicyContextData create_data{};
    create_data.supi = "imsi-999700000000001";
    create_data.pduSessionId = 5;
    create_data.pduSessionType.value = sbi_gen::PduSessionType::IPV4;
    create_data.dnn = "internet";
    create_data.notificationUri = "https://example.com/sm-policy-notify";
    create_data.sliceInfo.sst = 1;
    // Request supplies its own subscribed session AMBR/QoS -- the decision should reflect these,
    // not PCF's fixed defaults (see nfs/pcf/src/main.cpp's build_default_decision).
    sbi_gen::Ambr subs_ambr{};
    subs_ambr.uplink = "200 Mbps";
    subs_ambr.downlink = "100 Mbps";
    create_data.subsSessAmbr = subs_ambr;
    sbi_gen::SubscribedDefaultQos subs_qos{};
    subs_qos.n5qi = 6;
    sbi_gen::Arp arp{};
    arp.priorityLevel = 3;
    arp.preemptCap.value = sbi_gen::PreemptionCapability::MAY_PREEMPT;
    arp.preemptVuln.value = sbi_gen::PreemptionVulnerability::PREEMPTABLE;
    subs_qos.arp = arp;
    create_data.subsDefQos = subs_qos;

    sbi_core::http2::ClientRequest create_req;
    create_req.method = "POST";
    create_req.url = "https://127.0.0.1:7783/npcf-smpolicycontrol/v1/sm-policies";
    create_req.headers.emplace("content-type", "application/json");
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = json(create_data).dump();

    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    ASSERT_EQ(create_resp->status, 201);
    const auto location_it = create_resp->headers.find("location");
    ASSERT_NE(location_it, create_resp->headers.end());
    const std::string sm_policy_url = "https://127.0.0.1:7783" + location_it->second;

    const auto decision = json::parse(create_resp->body).get<sbi_gen::SmPolicyDecision>();
    ASSERT_TRUE(decision.sessRules.has_value());
    ASSERT_TRUE(decision.sessRules->contains("default"));
    const auto session_rule = (*decision.sessRules)["default"].get<sbi_gen::SessionRule>();
    ASSERT_TRUE(session_rule.authSessAmbr.has_value());
    EXPECT_EQ(session_rule.authSessAmbr->uplink, "200 Mbps");
    EXPECT_EQ(session_rule.authSessAmbr->downlink, "100 Mbps");
    ASSERT_TRUE(session_rule.authDefQos.has_value());
    ASSERT_TRUE(session_rule.authDefQos->n5qi.has_value());
    EXPECT_EQ(*session_rule.authDefQos->n5qi, 6);

    // GetSMPolicy returns {context, policy} together.
    sbi_core::http2::ClientRequest get_req;
    get_req.method = "GET";
    get_req.url = sm_policy_url;
    get_req.headers.emplace("authorization", "Bearer " + token);
    auto get_resp = client.send(get_req);
    ASSERT_TRUE(get_resp.has_value());
    EXPECT_EQ(get_resp->status, 200);
    const auto control = json::parse(get_resp->body).get<sbi_gen::SmPolicyControl>();
    EXPECT_EQ(control.context.supi, create_data.supi);
    EXPECT_EQ(control.context.dnn, create_data.dnn);

    // UpdateSMPolicy.
    sbi_core::http2::ClientRequest update_req;
    update_req.method = "POST";
    update_req.url = sm_policy_url + "/update";
    update_req.headers.emplace("content-type", "application/json");
    update_req.headers.emplace("authorization", "Bearer " + token);
    update_req.body = json{{"repPolicyCtrlReqTriggers", json::array()}}.dump();
    auto update_resp = client.send(update_req);
    ASSERT_TRUE(update_resp.has_value());
    EXPECT_EQ(update_resp->status, 200);
    const auto updated_decision = json::parse(update_resp->body).get<sbi_gen::SmPolicyDecision>();
    ASSERT_TRUE(updated_decision.sessRules.has_value());
    ASSERT_TRUE(updated_decision.sessRules->contains("default"));

    // DeleteSMPolicy.
    sbi_core::http2::ClientRequest delete_req;
    delete_req.method = "POST";
    delete_req.url = sm_policy_url + "/delete";
    delete_req.headers.emplace("content-type", "application/json");
    delete_req.headers.emplace("authorization", "Bearer " + token);
    delete_req.body = json::object().dump();
    auto delete_resp = client.send(delete_req);
    ASSERT_TRUE(delete_resp.has_value());
    EXPECT_EQ(delete_resp->status, 204);

    auto get_again = client.send(get_req);
    ASSERT_TRUE(get_again.has_value());
    EXPECT_EQ(get_again->status, 404);

    auto delete_again = client.send(delete_req);
    ASSERT_TRUE(delete_again.has_value());
    EXPECT_EQ(delete_again->status, 404);
}

TEST(PcfIntegration, SmPolicyWithoutSubscribedDefaultsUsesFixedFallback) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7783/npcf-am-policy-control/v1/policies/nonexistent", 50))
        << "pcf never became reachable";

    const std::string token = fetch_token(client, "npcf-smpolicycontrol");
    ASSERT_FALSE(token.empty());

    sbi_gen::SmPolicyContextData create_data{};
    create_data.supi = "imsi-999700000000002";
    create_data.pduSessionId = 7;
    create_data.pduSessionType.value = sbi_gen::PduSessionType::IPV4V6;
    create_data.dnn = "internet";
    create_data.notificationUri = "https://example.com/sm-policy-notify";
    create_data.sliceInfo.sst = 1;
    // No subsSessAmbr/subsDefQos supplied -- decision must fall back to PCF's fixed defaults.

    sbi_core::http2::ClientRequest create_req;
    create_req.method = "POST";
    create_req.url = "https://127.0.0.1:7783/npcf-smpolicycontrol/v1/sm-policies";
    create_req.headers.emplace("content-type", "application/json");
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = json(create_data).dump();

    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    ASSERT_EQ(create_resp->status, 201);
    const auto decision = json::parse(create_resp->body).get<sbi_gen::SmPolicyDecision>();
    ASSERT_TRUE(decision.sessRules.has_value());
    const auto session_rule = (*decision.sessRules)["default"].get<sbi_gen::SessionRule>();
    ASSERT_TRUE(session_rule.authSessAmbr.has_value());
    EXPECT_EQ(session_rule.authSessAmbr->uplink, "1 Gbps");
    ASSERT_TRUE(session_rule.authDefQos.has_value());
    ASSERT_TRUE(session_rule.authDefQos->n5qi.has_value());
    EXPECT_EQ(*session_rule.authDefQos->n5qi, 9);
}

TEST(PcfIntegration, MissingResourceIs404AndTamperedTokenIs401) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7783/npcf-am-policy-control/v1/policies/nonexistent", 50))
        << "pcf never became reachable";

    const std::string token = fetch_token(client, "npcf-am-policy-control");
    ASSERT_FALSE(token.empty());

    sbi_core::http2::ClientRequest get_req;
    get_req.method = "GET";
    get_req.url = "https://127.0.0.1:7783/npcf-am-policy-control/v1/policies/nonexistent";
    get_req.headers.emplace("authorization", "Bearer " + token);
    auto get_resp = client.send(get_req);
    ASSERT_TRUE(get_resp.has_value());
    EXPECT_EQ(get_resp->status, 404);

    sbi_core::http2::ClientRequest tampered_req = get_req;
    tampered_req.headers.erase("authorization");
    tampered_req.headers.emplace("authorization", "Bearer " + token + "tampered");
    auto tampered_resp = client.send(tampered_req);
    ASSERT_TRUE(tampered_resp.has_value());
    EXPECT_EQ(tampered_resp->status, 401);
}
