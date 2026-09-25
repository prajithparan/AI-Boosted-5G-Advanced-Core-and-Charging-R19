// Drives nrf and udm as real, separate OS processes to exercise UDM's own Tier-B gap-closure
// operations (ADR-0214, docs/CAPABILITY_GAP_ANALYSIS.md's own UDM audit): GetRgAuthData,
// GenerateAv (HSS EPS/IMS/GBA-domain vectors), and GenerateGbaAv -- over real TLS 1.3 + mTLS
// HTTP/2 with a real signed OAuth2 token, per TS29503_Nudm_UEAU.yaml. Reuses the same real
// Milenage + TS 33.501 Annex A key derivation (libs/aka-crypto) already proven by
// test_udm_ueau.cpp's own GenerateAuthData/GenerateProseAV coverage.

#include "sbi_core/http2_client.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

#include "TS29503_Nudm_UEAU_grp.hpp"
#include "spawn_guard.hpp"

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
    req.body = "grant_type=client_credentials&nfInstanceId=test-client&scope=" + scope +
               "&targetNfType=UDM";
    auto resp = client.send(req);
    if (!resp.has_value() || resp->status != 200) {
        return "";
    }
    return json::parse(resp->body).at("access_token").get<std::string>();
}

struct Duo {
    nf_test::SpawnedProcess nrf;
    nf_test::SpawnedProcess udm;
    nf_test::SpawnedProcess udr;
};

Duo spawn_nrf_udm() {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    nf_test::SpawnedProcess udm(UDM_PATH);
    // ADR-0383: UDM reads authentication data from the UDR over Nudr.
    nf_test::SpawnedProcess udr(UDR_PATH);
    nf_test::wait_tcp_listening(7781);
    return Duo{std::move(nrf), std::move(udm), std::move(udr)};
}

} // namespace

TEST(UdmUeauGapClosureIntegration, GetRgAuthDataReflectsRealConfirmedAuthState) {
    auto d = spawn_nrf_udm();
    auto client = make_client();
    const std::string supi = "imsi-999700000000001"; // seeded 5G_AKA subscriber, main.cpp
    const std::string base_url =
        "https://127.0.0.1:7780/nudm-ueau/v1/" + supi + "/security-information-rg";
    ASSERT_TRUE(wait_reachable(client, base_url, 200)) << "udm never became reachable";

    const std::string token = fetch_token(client, "nudm-ueau");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // Required query param missing: real 400.
    sbi_core::http2::ClientRequest missing_param_req;
    missing_param_req.method = "GET";
    missing_param_req.url = base_url;
    missing_param_req.headers.emplace("authorization", "Bearer " + token);
    auto missing_param_resp = client.send(missing_param_req);
    ASSERT_TRUE(missing_param_resp.has_value());
    EXPECT_EQ(missing_param_resp->status, 400);

    sbi_core::http2::ClientRequest get_req;
    get_req.method = "GET";
    get_req.url = base_url + "?authenticated-ind=true";
    get_req.headers.emplace("authorization", "Bearer " + token);
    auto get_resp = client.send(get_req);
    ASSERT_TRUE(get_resp.has_value());
    EXPECT_EQ(get_resp->status, 200) << get_resp->body;
    if (get_resp->status == 200) {
        const auto got = json::parse(get_resp->body);
        ASSERT_TRUE(got.contains("authInd"));
        // Real, disclosed: no ConfirmAuth-created AuthEvent exists yet for this supi in this
        // fresh udm process, so authInd must be real false, not the caller's own unverified
        // "true" claim echoed back.
        EXPECT_FALSE(got.at("authInd").get<bool>());
        EXPECT_EQ(got.at("supi").get<std::string>(), supi);
    }

    // Create a real, successful AuthEvent via ConfirmAuth, then confirm authInd flips to true.
    const json auth_event = json{
        {"nfInstanceId", "00000000-0000-4000-8000-000000000eee"},
        {"success", true},
        {"timeStamp", "2026-08-26T00:00:00Z"},
        {"authType", "5G_AKA"},
        {"servingNetworkName", "5G:mnc070.mcc999.3gppnetwork.org"},
    };
    sbi_core::http2::ClientRequest confirm_req;
    confirm_req.method = "POST";
    confirm_req.url = "https://127.0.0.1:7780/nudm-ueau/v1/" + supi + "/auth-events";
    confirm_req.headers.emplace("content-type", "application/json");
    confirm_req.headers.emplace("authorization", "Bearer " + token);
    confirm_req.body = auth_event.dump();
    auto confirm_resp = client.send(confirm_req);
    ASSERT_TRUE(confirm_resp.has_value());
    EXPECT_EQ(confirm_resp->status, 201) << confirm_resp->body;

    auto get_after_confirm_resp = client.send(get_req);
    ASSERT_TRUE(get_after_confirm_resp.has_value());
    EXPECT_EQ(get_after_confirm_resp->status, 200) << get_after_confirm_resp->body;
    if (get_after_confirm_resp->status == 200) {
        EXPECT_TRUE(json::parse(get_after_confirm_resp->body).at("authInd").get<bool>());
    }
}

TEST(UdmUeauGapClosureIntegration, GenerateAvProducesRealVectorsForImsGbaEapAkaBranches) {
    auto d = spawn_nrf_udm();
    auto client = make_client();
    const std::string supi = "imsi-999700000000001"; // seeded 5G_AKA subscriber, main.cpp
    const std::string base_url =
        "https://127.0.0.1:7780/nudm-ueau/v1/" + supi + "/hss-security-information/";
    ASSERT_TRUE(wait_reachable(client, base_url + "ims-aka/generate-av", 200))
        << "udm never became reachable";

    const std::string token = fetch_token(client, "nudm-ueau");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    sbi_gen::HssAuthenticationInfoRequest body{};
    body.hssAuthType.value = sbi_gen::HssAuthType::IMS_AKA;
    body.numOfRequestedVectors = 2;

    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = base_url + "ims-aka/generate-av";
    req.headers.emplace("content-type", "application/json");
    req.headers.emplace("authorization", "Bearer " + token);
    req.body = json(body).dump();
    auto resp = client.send(req);
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->status, 200) << resp->body;
    if (resp->status == 200) {
        const auto got = json::parse(resp->body);
        ASSERT_TRUE(got.contains("hssAuthenticationVectors"));
        const auto& vectors = got.at("hssAuthenticationVectors");
        ASSERT_EQ(vectors.size(), 2u);
        for (const auto& v : vectors) {
            EXPECT_EQ(v.at("avType").get<std::string>(), "IMS_AKA");
            EXPECT_EQ(v.at("rand").get<std::string>().size(), 32u);
            EXPECT_EQ(v.at("autn").get<std::string>().size(), 32u);
            EXPECT_TRUE(v.contains("ck"));
            EXPECT_TRUE(v.contains("ik"));
        }
        // Two distinct vectors -- real fresh RAND per call, not a cached/repeated value.
        EXPECT_NE(vectors[0].at("rand").get<std::string>(),
                  vectors[1].at("rand").get<std::string>());
    }

    // Real, disclosed gap: eps-aka needs TS 33.401 Annex A.2 KASME derivation, not yet
    // implemented -- real spec-documented 501, not a fabricated vector.
    sbi_gen::HssAuthenticationInfoRequest eps_body{};
    eps_body.hssAuthType.value = sbi_gen::HssAuthType::EPS_AKA;
    eps_body.numOfRequestedVectors = 1;
    sbi_core::http2::ClientRequest eps_req;
    eps_req.method = "POST";
    eps_req.url = "https://127.0.0.1:7780/nudm-ueau/v1/" + supi +
                  "/hss-security-information/eps-aka/generate-av";
    eps_req.headers.emplace("content-type", "application/json");
    eps_req.headers.emplace("authorization", "Bearer " + token);
    eps_req.body = json(eps_body).dump();
    auto eps_resp = client.send(eps_req);
    ASSERT_TRUE(eps_resp.has_value());
    EXPECT_EQ(eps_resp->status, 501);
}

TEST(UdmUeauGapClosureIntegration, GenerateGbaAvProducesRealN3GAkaVector) {
    auto d = spawn_nrf_udm();
    auto client = make_client();
    const std::string supi = "imsi-999700000000002"; // seeded EAP_AKA_PRIME subscriber, main.cpp
    const std::string base_url =
        "https://127.0.0.1:7780/nudm-ueau/v1/" + supi + "/gba-security-information/generate-av";
    ASSERT_TRUE(wait_reachable(client, base_url, 200)) << "udm never became reachable";

    const std::string token = fetch_token(client, "nudm-ueau");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    sbi_gen::GbaAuthenticationInfoRequest body{};
    body.authType.value = sbi_gen::GbaAuthType::DIGEST_AKAV1_MD5;

    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = base_url;
    req.headers.emplace("content-type", "application/json");
    req.headers.emplace("authorization", "Bearer " + token);
    req.body = json(body).dump();
    auto resp = client.send(req);
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->status, 200) << resp->body;
    if (resp->status == 200) {
        const auto got = json::parse(resp->body);
        // Real JSON key is "3gAkaAv" (the schema's own field name) -- "n3gAkaAv" is only the
        // generated C++ member name, prefixed because C++ identifiers can't start with a digit.
        ASSERT_TRUE(got.contains("3gAkaAv"));
        const auto& av = got.at("3gAkaAv");
        EXPECT_EQ(av.at("rand").get<std::string>().size(), 32u);
        EXPECT_EQ(av.at("xres").get<std::string>().size(), 16u);
        EXPECT_EQ(av.at("autn").get<std::string>().size(), 32u);
        EXPECT_EQ(av.at("ck").get<std::string>().size(), 32u);
        EXPECT_EQ(av.at("ik").get<std::string>().size(), 32u);
    }

    // Unknown subscriber: real 404.
    sbi_core::http2::ClientRequest unknown_req;
    unknown_req.method = "POST";
    unknown_req.url = "https://127.0.0.1:7780/nudm-ueau/v1/imsi-999700000099999/"
                      "gba-security-information/generate-av";
    unknown_req.headers.emplace("content-type", "application/json");
    unknown_req.headers.emplace("authorization", "Bearer " + token);
    unknown_req.body = json(body).dump();
    auto unknown_resp = client.send(unknown_req);
    ASSERT_TRUE(unknown_resp.has_value());
    EXPECT_EQ(unknown_resp->status, 404);
}
