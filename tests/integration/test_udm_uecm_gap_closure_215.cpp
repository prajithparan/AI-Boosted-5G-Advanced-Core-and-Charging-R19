// Drives nrf and udm as real, separate OS processes to exercise UDM's own Nudm_UECM Tier-B
// gap-closure operations (ADR-0215, docs/CAPABILITY_GAP_ANALYSIS.md's own UDM audit): PeiUpdate
// and UpdateRoamingInformation -- over real TLS 1.3 + mTLS HTTP/2 with a real signed OAuth2 token,
// per TS29503_Nudm_UECM.yaml.

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

TEST(UdmUecmGapClosureIntegration, PeiUpdateMergesIntoExistingAmfRegistration) {
    auto d = spawn_nrf_udm();
    auto client = make_client();
    const std::string ue_id = "imsi-999700000000001";
    const std::string amf_reg_url =
        "https://127.0.0.1:7780/nudm-uecm/v1/" + ue_id + "/registrations/amf-3gpp-access";
    ASSERT_TRUE(wait_reachable(client, amf_reg_url, 200)) << "udm never became reachable";

    const std::string token = fetch_token(client, "nudm-uecm");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // PeiUpdate against a ueId with no existing AMF registration: real 404.
    const json pei_update = json{{"pei", "imei-490154203237518"}};
    sbi_core::http2::ClientRequest missing_req;
    missing_req.method = "POST";
    missing_req.url = "https://127.0.0.1:7780/nudm-uecm/v1/imsi-999700000099999/registrations/"
                      "amf-3gpp-access/pei-update";
    missing_req.headers.emplace("content-type", "application/json");
    missing_req.headers.emplace("authorization", "Bearer " + token);
    missing_req.body = pei_update.dump();
    auto missing_resp = client.send(missing_req);
    ASSERT_TRUE(missing_resp.has_value());
    EXPECT_EQ(missing_resp->status, 404);

    // Seed a real AMF 3GPP-access registration first.
    sbi_gen::Amf3GppAccessRegistration create_data{};
    create_data.amfInstanceId = "00000000-0000-4000-8000-000000000bbb";
    create_data.deregCallbackUri = "https://example.com/dereg";
    create_data.guami.plmnId.mcc = "999";
    create_data.guami.plmnId.mnc = "70";
    create_data.guami.amfId = "ABCDEF";
    create_data.ratType = "NR";
    sbi_core::http2::ClientRequest create_req;
    create_req.method = "PUT";
    create_req.url = amf_reg_url;
    create_req.headers.emplace("content-type", "application/json");
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = json(create_data).dump();
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    EXPECT_TRUE(create_resp->status == 200 || create_resp->status == 201) << create_resp->body;

    sbi_core::http2::ClientRequest pei_req;
    pei_req.method = "POST";
    pei_req.url = amf_reg_url + "/pei-update";
    pei_req.headers.emplace("content-type", "application/json");
    pei_req.headers.emplace("authorization", "Bearer " + token);
    pei_req.body = pei_update.dump();
    auto pei_resp = client.send(pei_req);
    ASSERT_TRUE(pei_resp.has_value());
    EXPECT_EQ(pei_resp->status, 204) << pei_resp->body;

    // Confirm the real merge landed in the AMF registration document itself.
    sbi_core::http2::ClientRequest get_req;
    get_req.method = "GET";
    get_req.url = amf_reg_url;
    get_req.headers.emplace("authorization", "Bearer " + token);
    auto get_resp = client.send(get_req);
    ASSERT_TRUE(get_resp.has_value());
    EXPECT_EQ(get_resp->status, 200) << get_resp->body;
    if (get_resp->status == 200) {
        const auto got = json::parse(get_resp->body);
        ASSERT_TRUE(got.contains("pei"));
        EXPECT_EQ(got.at("pei").get<std::string>(), "imei-490154203237518");
        // Real, disclosed: merge, not replace -- fields set at creation survive.
        EXPECT_EQ(got.at("guami").at("amfId").get<std::string>(), "ABCDEF");
    }
}

TEST(UdmUecmGapClosureIntegration, UpdateRoamingInformationRealCreateThenUpdate) {
    auto d = spawn_nrf_udm();
    auto client = make_client();
    const std::string ue_id = "imsi-999700000000002";
    const std::string url = "https://127.0.0.1:7780/nudm-uecm/v1/" + ue_id +
                            "/registrations/amf-3gpp-access/roaming-info-update";
    ASSERT_TRUE(wait_reachable(client,
                               "https://127.0.0.1:7780/nudm-uecm/v1/" + ue_id +
                                   "/registrations/amf-3gpp-access",
                               50))
        << "udm never became reachable";

    const std::string token = fetch_token(client, "nudm-uecm");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    sbi_gen::RoamingInfoUpdate first{};
    first.roaming = true;
    first.servingPlmn.mcc = "999";
    first.servingPlmn.mnc = "70";

    sbi_core::http2::ClientRequest create_req;
    create_req.method = "POST";
    create_req.url = url;
    create_req.headers.emplace("content-type", "application/json");
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = json(first).dump();
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    EXPECT_EQ(create_resp->status, 201) << create_resp->body;
    EXPECT_NE(create_resp->headers.find("location"), create_resp->headers.end());
    if (create_resp->status == 201) {
        const auto got = json::parse(create_resp->body);
        EXPECT_TRUE(got.at("roaming").get<bool>());
        EXPECT_EQ(got.at("servingPlmn").at("mcc").get<std::string>(), "999");
    }

    // Real update (same resource already exists): real 204, no body.
    sbi_gen::RoamingInfoUpdate second{};
    second.roaming = false;
    second.servingPlmn.mcc = "999";
    second.servingPlmn.mnc = "71";
    sbi_core::http2::ClientRequest update_req;
    update_req.method = "POST";
    update_req.url = url;
    update_req.headers.emplace("content-type", "application/json");
    update_req.headers.emplace("authorization", "Bearer " + token);
    update_req.body = json(second).dump();
    auto update_resp = client.send(update_req);
    ASSERT_TRUE(update_resp.has_value());
    EXPECT_EQ(update_resp->status, 204) << update_resp->body;
}
