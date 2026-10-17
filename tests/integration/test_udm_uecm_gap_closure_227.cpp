// Drives nrf and udm as real, separate OS processes to exercise UDM's own Nudm_UECM last three
// independent single ops (ADR-0227, docs/CAPABILITY_GAP_ANALYSIS.md's own UDM audit): `Trigger
// P-CSCF Restoration`, `GetLocationInfo`, `authTrigger` -- closing Nudm_UECM's entire remaining
// Tier-B backlog -- over real TLS 1.3 + mTLS HTTP/2 with a real signed OAuth2 token, per
// TS29503_Nudm_UECM.yaml. `GetLocationInfo` is a real, complete local composition (verified against
// the seeded AMF registration's own guami/plmnId/accessType); the other two are real
// accept-and-validate operations with a disclosed non-relay (see main.cpp's own comments).

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
        "&scope=" + scope + "&targetNfType=UDM";
    auto resp = nf_test::send_token_request(req);
    if (!resp.has_value() || resp->status != 200) {
        return "";
    }
    return json::parse(resp->body).at("access_token").get<std::string>();
}

struct Duo {
    nf_test::SpawnedProcess nrf;
    nf_test::SpawnedProcess udm;
};

Duo spawn_nrf_udm() {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    nf_test::SpawnedProcess udm(UDM_PATH);
    return Duo{std::move(nrf), std::move(udm)};
}

} // namespace

TEST(UdmUecmFinalOpsGapClosureIntegration, TriggerPcscfRestorationGetLocationInfoAuthTrigger) {
    auto d = spawn_nrf_udm();
    auto client = make_client();
    const std::string ue_id = "imsi-999700000000001";
    const std::string base = "https://127.0.0.1:7780/nudm-uecm/v1/" + ue_id;
    ASSERT_TRUE(wait_reachable(client, base + "/registrations/amf-3gpp-access", 200))
        << "udm never became reachable";

    const std::string token = fetch_token(client, "nudm-uecm");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // --- Real 404s before any AMF registration exists ---

    sbi_gen::TriggerRequest trigger_body{};
    trigger_body.supi = ue_id;
    sbi_core::http2::ClientRequest restore_req;
    restore_req.method = "POST";
    restore_req.url = "https://127.0.0.1:7780/nudm-uecm/v1/restore-pcscf";
    restore_req.headers.emplace("content-type", "application/json");
    restore_req.headers.emplace("authorization", "Bearer " + token);
    restore_req.body = json(trigger_body).dump();
    auto restore_before_resp = client.send(restore_req);
    ASSERT_TRUE(restore_before_resp.has_value());
    EXPECT_EQ(restore_before_resp->status, 404) << restore_before_resp->body;

    sbi_core::http2::ClientRequest location_req;
    location_req.method = "GET";
    location_req.url = base + "/registrations/location";
    location_req.headers.emplace("authorization", "Bearer " + token);
    auto location_before_resp = client.send(location_req);
    ASSERT_TRUE(location_before_resp.has_value());
    EXPECT_EQ(location_before_resp->status, 404) << location_before_resp->body;

    sbi_gen::AuthTriggerInfo auth_trigger_body{};
    auth_trigger_body.supi = ue_id;
    sbi_core::http2::ClientRequest auth_trigger_req;
    auth_trigger_req.method = "GET";
    auth_trigger_req.url = base + "/registrations/trigger-auth";
    auth_trigger_req.headers.emplace("content-type", "application/json");
    auth_trigger_req.headers.emplace("authorization", "Bearer " + token);
    auth_trigger_req.body = json(auth_trigger_body).dump();
    auto auth_trigger_before_resp = client.send(auth_trigger_req);
    ASSERT_TRUE(auth_trigger_before_resp.has_value());
    EXPECT_EQ(auth_trigger_before_resp->status, 404) << auth_trigger_before_resp->body;

    // --- Seed a real AMF 3GPP-access registration ---

    sbi_gen::Amf3GppAccessRegistration amf_data{};
    amf_data.amfInstanceId = "00000000-0000-4000-8000-000000000aaa";
    amf_data.deregCallbackUri = "https://example.com/dereg";
    amf_data.guami.plmnId.mcc = "999";
    amf_data.guami.plmnId.mnc = "70";
    amf_data.guami.amfId = "ABCDEF";
    amf_data.ratType = "NR";
    sbi_core::http2::ClientRequest amf_put;
    amf_put.method = "PUT";
    amf_put.url = base + "/registrations/amf-3gpp-access";
    amf_put.headers.emplace("content-type", "application/json");
    amf_put.headers.emplace("authorization", "Bearer " + token);
    amf_put.body = json(amf_data).dump();
    auto amf_put_resp = client.send(amf_put);
    ASSERT_TRUE(amf_put_resp.has_value());
    ASSERT_EQ(amf_put_resp->status, 201) << amf_put_resp->body;

    // --- GetLocationInfo: real, complete local composition ---

    auto location_resp = client.send(location_req);
    ASSERT_TRUE(location_resp.has_value());
    ASSERT_EQ(location_resp->status, 200) << location_resp->body;
    auto location_body = json::parse(location_resp->body);
    EXPECT_EQ(location_body.at("supi").get<std::string>(), ue_id);
    ASSERT_EQ(location_body.at("registrationLocationInfoList").size(), 1u);
    const auto& loc = location_body.at("registrationLocationInfoList").at(0);
    EXPECT_EQ(loc.at("amfInstanceId").get<std::string>(), "00000000-0000-4000-8000-000000000aaa");
    EXPECT_EQ(loc.at("plmnId").at("mcc").get<std::string>(), "999");
    EXPECT_EQ(loc.at("plmnId").at("mnc").get<std::string>(), "70");
    ASSERT_EQ(loc.at("accessTypeList").size(), 1u);
    EXPECT_EQ(loc.at("accessTypeList").at(0).get<std::string>(), "3GPP_ACCESS");

    // --- Trigger P-CSCF Restoration: real 204 once a registration exists ---

    auto restore_after_resp = client.send(restore_req);
    ASSERT_TRUE(restore_after_resp.has_value());
    EXPECT_EQ(restore_after_resp->status, 204) << restore_after_resp->body;

    // --- authTrigger: real 204 once a registration exists ---

    auto auth_trigger_after_resp = client.send(auth_trigger_req);
    ASSERT_TRUE(auth_trigger_after_resp.has_value());
    EXPECT_EQ(auth_trigger_after_resp->status, 204) << auth_trigger_after_resp->body;
}
