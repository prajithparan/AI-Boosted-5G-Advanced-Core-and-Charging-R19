// Drives nrf/udr/udm as real, separate OS processes to exercise UDM's Nudm_SDM group C1
// gap-closure (ADR-0230, docs/CAPABILITY_GAP_ANALYSIS.md's own UDM audit): 5 real ops closed with
// data already available -- GetNSSAI (a real subset view into UDR-backed am-data), GetEcrData (an
// existing UDR coverage-restriction-data route under a nested path), and GetUeCtxInAmfData/
// GetUeCtxInSmfData/GetUeCtxInSmsfData (real, complete-or-near-complete local composition from
// UDM's own already-stored AMF/SMF/SMSF registration records), per TS29503_Nudm_SDM.yaml.

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

TEST(UdmSdmGapClosure230Integration, UdrBackedOpsReturnRealData) {
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

    // Real seeded test subscriber (nfs/udr/src/main.cpp's own startup seed, ADR-0069/ADR-0102).
    const std::string supi = "imsi-999700000000001";
    const std::string base = "https://127.0.0.1:7780/nudm-sdm/v2/" + supi;

    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = base + "/nssai";
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        ASSERT_EQ(resp->status, 200) << resp->body;
        const auto nssai = json::parse(resp->body).get<sbi_gen::Nssai>();
        ASSERT_EQ(nssai.defaultSingleNssais.size(), 1u);
        EXPECT_EQ(nssai.defaultSingleNssais[0].sst, 1);
        ASSERT_TRUE(nssai.defaultSingleNssais[0].sd.has_value());
        EXPECT_EQ(*nssai.defaultSingleNssais[0].sd, "000001");
    }
    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = base + "/am-data/ecr-data";
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        ASSERT_EQ(resp->status, 200) << resp->body;
        const auto ecr = json::parse(resp->body).get<sbi_gen::EnhancedCoverageRestrictionData>();
        ASSERT_TRUE(ecr.plmnEcInfoList.has_value());
        ASSERT_EQ(ecr.plmnEcInfoList->size(), 1u);
    }

    // A genuinely unseeded SUPI correctly 404s on both.
    for (const std::string path : {"nssai", "am-data/ecr-data"}) {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = "https://127.0.0.1:7780/nudm-sdm/v2/imsi-999999999999999/" + path;
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value()) << path;
        EXPECT_EQ(resp->status, 404) << path;
    }
}

TEST(UdmSdmGapClosure230Integration, LocalCompositionOpsReturnRealSeededData) {
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
    // ADR-0499: the UDM checks each API's own scope; the UECM registrations need nudm-uecm.
    const std::string uecm_token = fetch_token(client, "nudm-uecm");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const std::string ue_id = "imsi-999700000000003";
    const std::string uecm_base = "https://127.0.0.1:7780/nudm-uecm/v1/" + ue_id;
    const std::string sdm_base = "https://127.0.0.1:7780/nudm-sdm/v2/" + ue_id;

    // --- Before any registration exists: real honest 200 with no composed sub-fields ---

    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = sdm_base + "/ue-context-in-amf-data";
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        ASSERT_EQ(resp->status, 200) << resp->body;
        EXPECT_FALSE(json::parse(resp->body).contains("amfInfo"));
    }
    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = sdm_base + "/ue-context-in-smf-data";
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        ASSERT_EQ(resp->status, 200) << resp->body;
        EXPECT_FALSE(json::parse(resp->body).contains("pduSessions"));
    }
    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = sdm_base + "/ue-context-in-smsf-data";
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        ASSERT_EQ(resp->status, 200) << resp->body;
        const auto body = json::parse(resp->body);
        EXPECT_FALSE(body.contains("smsfInfo3GppAccess"));
        EXPECT_FALSE(body.contains("smsfInfoNon3GppAccess"));
    }

    // --- Seed a real AMF 3GPP-access registration ---

    sbi_gen::Amf3GppAccessRegistration amf_data{};
    amf_data.amfInstanceId = "00000000-0000-4000-8000-000000000ddd";
    amf_data.deregCallbackUri = "https://example.com/dereg";
    amf_data.guami.plmnId.mcc = "999";
    amf_data.guami.plmnId.mnc = "70";
    amf_data.guami.amfId = "ABCDEF";
    sbi_core::http2::ClientRequest amf_put;
    amf_put.method = "PUT";
    amf_put.url = uecm_base + "/registrations/amf-3gpp-access";
    amf_put.headers.emplace("content-type", "application/json");
    amf_put.headers.emplace("authorization", "Bearer " + uecm_token);
    amf_put.body = json(amf_data).dump();
    auto amf_put_resp = client.send(amf_put);
    ASSERT_TRUE(amf_put_resp.has_value());
    ASSERT_EQ(amf_put_resp->status, 201) << amf_put_resp->body;

    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = sdm_base + "/ue-context-in-amf-data";
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        ASSERT_EQ(resp->status, 200) << resp->body;
        const auto result = json::parse(resp->body).get<sbi_gen::UeContextInAmfData>();
        ASSERT_TRUE(result.amfInfo.has_value());
        ASSERT_EQ(result.amfInfo->size(), 1u);
        EXPECT_EQ((*result.amfInfo)[0].amfInstanceId, amf_data.amfInstanceId);
        EXPECT_EQ((*result.amfInfo)[0].guami.amfId, "ABCDEF");
        ASSERT_TRUE((*result.amfInfo)[0].accessType.has_value());
        EXPECT_EQ((*result.amfInfo)[0].accessType->value, sbi_gen::AccessType::V3GPP_ACCESS);
        EXPECT_FALSE(result.epsInterworkingInfo.has_value());
    }

    // --- Seed a real SMF registration with a real dnn ---

    sbi_gen::SmfRegistration smf_data{};
    smf_data.smfInstanceId = "00000000-0000-4000-8000-000000000eee";
    smf_data.pduSessionId = 7;
    smf_data.singleNssai.sst = 1;
    smf_data.plmnId.mcc = "999";
    smf_data.plmnId.mnc = "70";
    smf_data.dnn = "internet";
    sbi_core::http2::ClientRequest smf_put;
    smf_put.method = "PUT";
    smf_put.url = uecm_base + "/registrations/smf-registrations/7";
    smf_put.headers.emplace("content-type", "application/json");
    smf_put.headers.emplace("authorization", "Bearer " + uecm_token);
    smf_put.body = json(smf_data).dump();
    auto smf_put_resp = client.send(smf_put);
    ASSERT_TRUE(smf_put_resp.has_value());
    ASSERT_EQ(smf_put_resp->status, 201) << smf_put_resp->body;

    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = sdm_base + "/ue-context-in-smf-data";
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        ASSERT_EQ(resp->status, 200) << resp->body;
        const auto body = json::parse(resp->body);
        ASSERT_TRUE(body.contains("pduSessions"));
        ASSERT_TRUE(body.at("pduSessions").contains("7"));
        const auto session = body.at("pduSessions").at("7").get<sbi_gen::PduSession>();
        EXPECT_EQ(session.dnn, "internet");
        EXPECT_EQ(session.smfInstanceId, smf_data.smfInstanceId);
        EXPECT_EQ(session.plmnId.mcc, "999");
        ASSERT_TRUE(session.singleNssai.has_value());
        EXPECT_EQ(session.singleNssai->sst, 1);
    }

    // --- Seed real SMSF 3GPP-access and non-3GPP-access registrations ---

    sbi_gen::SmsfRegistration smsf_3gpp{};
    smsf_3gpp.smsfInstanceId = "00000000-0000-4000-8000-000000000fff";
    smsf_3gpp.plmnId.mcc = "999";
    smsf_3gpp.plmnId.mnc = "70";
    sbi_core::http2::ClientRequest smsf_3gpp_put;
    smsf_3gpp_put.method = "PUT";
    smsf_3gpp_put.url = uecm_base + "/registrations/smsf-3gpp-access";
    smsf_3gpp_put.headers.emplace("content-type", "application/json");
    smsf_3gpp_put.headers.emplace("authorization", "Bearer " + uecm_token);
    smsf_3gpp_put.body = json(smsf_3gpp).dump();
    auto smsf_3gpp_resp = client.send(smsf_3gpp_put);
    ASSERT_TRUE(smsf_3gpp_resp.has_value());
    ASSERT_EQ(smsf_3gpp_resp->status, 201) << smsf_3gpp_resp->body;

    sbi_gen::SmsfRegistration smsf_non3gpp{};
    smsf_non3gpp.smsfInstanceId = "00000000-0000-4000-8000-000000000abc";
    smsf_non3gpp.plmnId.mcc = "999";
    smsf_non3gpp.plmnId.mnc = "70";
    sbi_core::http2::ClientRequest smsf_non3gpp_put;
    smsf_non3gpp_put.method = "PUT";
    smsf_non3gpp_put.url = uecm_base + "/registrations/smsf-non-3gpp-access";
    smsf_non3gpp_put.headers.emplace("content-type", "application/json");
    smsf_non3gpp_put.headers.emplace("authorization", "Bearer " + uecm_token);
    smsf_non3gpp_put.body = json(smsf_non3gpp).dump();
    auto smsf_non3gpp_resp = client.send(smsf_non3gpp_put);
    ASSERT_TRUE(smsf_non3gpp_resp.has_value());
    ASSERT_EQ(smsf_non3gpp_resp->status, 201) << smsf_non3gpp_resp->body;

    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = sdm_base + "/ue-context-in-smsf-data";
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        ASSERT_EQ(resp->status, 200) << resp->body;
        const auto result = json::parse(resp->body).get<sbi_gen::UeContextInSmsfData>();
        ASSERT_TRUE(result.smsfInfo3GppAccess.has_value());
        EXPECT_EQ(result.smsfInfo3GppAccess->smsfInstanceId, smsf_3gpp.smsfInstanceId);
        ASSERT_TRUE(result.smsfInfoNon3GppAccess.has_value());
        EXPECT_EQ(result.smsfInfoNon3GppAccess->smsfInstanceId, smsf_non3gpp.smsfInstanceId);
    }
}
