// Drives nrf and udm as real, separate OS processes to exercise UDM's own Nudm_UECM
// SendRoutingInfoSm custom operation (ADR-0221, docs/CAPABILITY_GAP_ANALYSIS.md's own UDM
// audit) -- the last open item in Nudm_UECM's own Tier-B backlog -- over real TLS 1.3 + mTLS
// HTTP/2 with a real signed OAuth2 token, per TS29503_Nudm_UECM.yaml. Verifies a real 404 for a
// ueId with no SMSF/IP-SM-GW registration at all, then seeds real SMSF_3GPP + IP_SM_GW
// sub-resources and confirms RoutingInfoSmResponse correctly composes both.

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

TEST(UdmSendRoutingInfoSmGapClosureIntegration, ComposesRealRoutingInfoFromSeededStores) {
    auto d = spawn_nrf_udm();
    auto client = make_client();
    const std::string ue_id = "imsi-999700000000001";
    const std::string base = "https://127.0.0.1:7780/nudm-uecm/v1/" + ue_id;
    const std::string route_url = base + "/registrations/send-routing-info-sm";
    ASSERT_TRUE(wait_reachable(client, base + "/registrations/smsf-3gpp-access", 200))
        << "udm never became reachable";

    const std::string token = fetch_token(client, "nudm-uecm");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    sbi_gen::RoutingInfoSmRequest request_body{};
    request_body.ipSmGwInd = false;

    sbi_core::http2::ClientRequest route_req;
    route_req.method = "POST";
    route_req.url = route_url;
    route_req.headers.emplace("content-type", "application/json");
    route_req.headers.emplace("authorization", "Bearer " + token);
    route_req.body = json(request_body).dump();

    // No SMSF/IP-SM-GW registration for this ueId yet -- real 404.
    auto before_resp = client.send(route_req);
    ASSERT_TRUE(before_resp.has_value());
    EXPECT_EQ(before_resp->status, 404) << before_resp->body;

    // Seed SMSF_3GPP.
    sbi_gen::SmsfRegistration smsf_data{};
    smsf_data.smsfInstanceId = "00000000-0000-4000-8000-000000000ddd";
    smsf_data.plmnId.mcc = "999";
    smsf_data.plmnId.mnc = "70";
    sbi_core::http2::ClientRequest smsf_put;
    smsf_put.method = "PUT";
    smsf_put.url = base + "/registrations/smsf-3gpp-access";
    smsf_put.headers.emplace("content-type", "application/json");
    smsf_put.headers.emplace("authorization", "Bearer " + token);
    smsf_put.body = json(smsf_data).dump();
    auto smsf_put_resp = client.send(smsf_put);
    ASSERT_TRUE(smsf_put_resp.has_value());
    ASSERT_EQ(smsf_put_resp->status, 201) << smsf_put_resp->body;

    // Seed IP_SM_GW.
    sbi_gen::IpSmGwRegistration ip_sm_gw_data{};
    ip_sm_gw_data.ipsmgwFqdn = "ipsmgw.example.com";
    sbi_core::http2::ClientRequest ip_sm_gw_put;
    ip_sm_gw_put.method = "PUT";
    ip_sm_gw_put.url = base + "/registrations/ip-sm-gw";
    ip_sm_gw_put.headers.emplace("content-type", "application/json");
    ip_sm_gw_put.headers.emplace("authorization", "Bearer " + token);
    ip_sm_gw_put.body = json(ip_sm_gw_data).dump();
    auto ip_sm_gw_put_resp = client.send(ip_sm_gw_put);
    ASSERT_TRUE(ip_sm_gw_put_resp.has_value());
    ASSERT_EQ(ip_sm_gw_put_resp->status, 201) << ip_sm_gw_put_resp->body;

    auto after_resp = client.send(route_req);
    ASSERT_TRUE(after_resp.has_value());
    ASSERT_EQ(after_resp->status, 200) << after_resp->body;
    auto after_body = json::parse(after_resp->body);
    EXPECT_EQ(after_body.at("supi").get<std::string>(), ue_id);
    EXPECT_TRUE(after_body.contains("smsf3Gpp"));
    EXPECT_EQ(after_body.at("smsf3Gpp").at("smsfInstanceId").get<std::string>(),
              "00000000-0000-4000-8000-000000000ddd");
    EXPECT_FALSE(after_body.contains("smsfNon3Gpp"));
    EXPECT_TRUE(after_body.contains("ipSmGw"));
    EXPECT_EQ(after_body.at("ipSmGw").at("ipSmGwRegistration").at("ipsmgwFqdn").get<std::string>(),
              "ipsmgw.example.com");
    EXPECT_FALSE(after_body.contains("smsRouter"));
}
