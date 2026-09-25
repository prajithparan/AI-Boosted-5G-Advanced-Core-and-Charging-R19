// Drives nrf, udm, and ausf as real, separate OS processes to exercise ausf's
// Nausf_UEAuthentication surface (docs/DECISIONS.md ADR-0027) end to end: ausf really calls udm's
// GenerateAuthData over real TLS 1.3 + mTLS + a real signed OAuth2 token (the first NF-to-NF
// business-logic call in this build, not just NRF registration), and this test plays the UE/USIM
// role -- independently re-deriving every key from the same TS 35.207 Test Set 1 (K, OP) values
// nfs/udm/src/main.cpp seeds its test subscribers with -- to cross-check ausf's
// HXRES*/KSEAF/EAP-AKA' output against a second, independent computation, not just round-tripping
// ausf's own numbers back at it.

#include "sbi_core/http2_client.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

#include "TS29509_Nausf_UEAuthentication.hpp"
#include "aka_crypto/eap_aka_prime.hpp"
#include "aka_crypto/hex.hpp"
#include "aka_crypto/kdf.hpp"
#include "aka_crypto/milenage.hpp"
#include "spawn_guard.hpp"

#include <gtest/gtest.h>

namespace {

using nlohmann::json;

constexpr const char* kServingNetworkName = "5G:mnc070.mcc999.3gppnetwork.org";

// Same TS 35.207 Test Set 1 (K, OP) values nfs/udm/src/main.cpp seeds imsi-999700000000001 (5G_AKA)
// and imsi-999700000000002 (EAP_AKA_PRIME) with -- see docs/DECISIONS.md ADR-0026.
aka_crypto::Key128 test_k() {
    return *aka_crypto::from_hex<16>("465b5ce8b199b49faa5f0a2ee238a6bc");
}
aka_crypto::Key128 test_opc() {
    const auto op = *aka_crypto::from_hex<16>("cdc202d5123e20f62b6d676ac72cb318");
    return aka_crypto::derive_opc(test_k(), op);
}

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
               "&targetNfType=AUSF";
    auto resp = client.send(req);
    if (!resp.has_value() || resp->status != 200) {
        return "";
    }
    return json::parse(resp->body).at("access_token").get<std::string>();
}

struct Trio {
    nf_test::SpawnedProcess nrf;
    nf_test::SpawnedProcess udm;
    nf_test::SpawnedProcess ausf;
    nf_test::SpawnedProcess udr;
};

Trio spawn_all() {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    nf_test::SpawnedProcess udm(UDM_PATH);
    nf_test::SpawnedProcess ausf(AUSF_PATH);
    // ADR-0383: UDM reads authentication data from the UDR over Nudr.
    nf_test::SpawnedProcess udr(UDR_PATH);
    nf_test::wait_tcp_listening(7781);
    return Trio{std::move(nrf), std::move(udm), std::move(ausf), std::move(udr)};
}

// The UE/USIM side of MILENAGE: given RAND (from ausf) and AUTN (from ausf, which embeds SQN xor
// AK and AMF -- a real UE never learns SQN any other way either), independently recompute AK, MAC-A
// and verify it against AUTN's embedded MAC, exactly like a real USIM's network-authenticity check.
struct UeComputation {
    aka_crypto::F2345Output f2345_out;
    aka_crypto::Sqn sqn;
    bool network_authenticated;
};

UeComputation ue_compute(const aka_crypto::Key128& rand, const std::array<uint8_t, 16>& autn) {
    const auto opc = test_opc();
    const auto k = test_k();
    const auto out = aka_crypto::f2345(opc, k, rand);

    aka_crypto::Ak48 sqn_xor_ak{};
    std::copy(autn.begin(), autn.begin() + 6, sqn_xor_ak.begin());
    aka_crypto::Amf amf{};
    std::copy(autn.begin() + 6, autn.begin() + 8, amf.begin());
    aka_crypto::Sqn sqn{};
    for (size_t i = 0; i < sqn.size(); ++i) {
        sqn[i] = static_cast<uint8_t>(sqn_xor_ak[i] ^ out.ak[i]);
    }

    const auto mac_a = aka_crypto::f1(opc, k, rand, sqn, amf);
    const bool authenticated = std::equal(mac_a.begin(), mac_a.end(), autn.begin() + 8);

    return UeComputation{out, sqn, authenticated};
}

} // namespace

TEST(AusfIntegration, FiveGAkaSuccessfulAuthenticationCrossChecksHxresAndKseaf) {
    auto t = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client,
        "https://127.0.0.1:7782/nausf-auth/v1/ue-authentications/nonexistent/eap-session",
        50))
        << "ausf never became reachable";

    const std::string token = fetch_token(client, "nausf-auth");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const std::string supi = "imsi-999700000000001"; // seeded 5G_AKA subscriber

    sbi_gen::AuthenticationInfo body{};
    body.supiOrSuci = supi;
    body.servingNetworkName = kServingNetworkName;

    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = "https://127.0.0.1:7782/nausf-auth/v1/ue-authentications";
    req.headers.emplace("content-type", "application/json");
    req.headers.emplace("authorization", "Bearer " + token);
    req.body = json(body).dump();

    auto resp = client.send(req);
    ASSERT_TRUE(resp.has_value());
    ASSERT_EQ(resp->status, 201);
    const auto location_it = resp->headers.find("location");
    ASSERT_NE(location_it, resp->headers.end());
    const std::string auth_ctx_url = "https://127.0.0.1:7782" + location_it->second;

    const auto ctx = json::parse(resp->body).get<sbi_gen::UEAuthenticationCtx>();
    EXPECT_EQ(ctx.authType.value, sbi_gen::AuthType_Nausf_UEAuthentication::V5G_AKA);
    const auto av = ctx.n5gAuthData.get<sbi_gen::Av5gAka>();

    const auto rand = *aka_crypto::from_hex<16>(av.rand);
    const auto autn = *aka_crypto::from_hex<16>(av.autn);
    const auto ue = ue_compute(rand, autn);
    ASSERT_TRUE(ue.network_authenticated) << "MAC-A verification failed -- ausf sent a bad AUTN";

    const auto xres_star = aka_crypto::derive_res_star(
        ue.f2345_out.ck, ue.f2345_out.ik, kServingNetworkName, rand, ue.f2345_out.res);
    const auto hxres_star_expected = aka_crypto::derive_hxres_star(rand, xres_star);
    EXPECT_EQ(aka_crypto::to_hex(hxres_star_expected), av.hxresStar)
        << "ausf's HXRES* doesn't match an independent computation from the same RAND/XRES*";

    const auto sqn_xor_ak = aka_crypto::sqn_xor_ak(ue.sqn, ue.f2345_out.ak);
    const auto kausf_expected =
        aka_crypto::derive_kausf(ue.f2345_out.ck, ue.f2345_out.ik, kServingNetworkName, sqn_xor_ak);
    const auto kseaf_expected = aka_crypto::derive_kseaf(kausf_expected, kServingNetworkName);

    sbi_gen::ConfirmationData confirm{};
    confirm.resStar = aka_crypto::to_hex(xres_star);

    sbi_core::http2::ClientRequest confirm_req;
    confirm_req.method = "PUT";
    confirm_req.url = auth_ctx_url + "/5g-aka-confirmation";
    confirm_req.headers.emplace("content-type", "application/json");
    confirm_req.headers.emplace("authorization", "Bearer " + token);
    confirm_req.body = json(confirm).dump();

    auto confirm_resp = client.send(confirm_req);
    ASSERT_TRUE(confirm_resp.has_value());
    EXPECT_EQ(confirm_resp->status, 200);
    const auto confirmed = json::parse(confirm_resp->body).get<sbi_gen::ConfirmationDataResponse>();
    EXPECT_EQ(confirmed.authResult.value,
              sbi_gen::AuthResult_Nausf_UEAuthentication::AUTHENTICATION_SUCCESS);
    ASSERT_TRUE(confirmed.supi.has_value());
    EXPECT_EQ(*confirmed.supi, supi);
    ASSERT_TRUE(confirmed.kseaf.has_value());
    EXPECT_EQ(*confirmed.kseaf, aka_crypto::to_hex(kseaf_expected))
        << "ausf's KSEAF doesn't match an independent computation from the same key material";

    sbi_core::http2::ClientRequest delete_req;
    delete_req.method = "DELETE";
    delete_req.url = confirm_req.url;
    delete_req.headers.emplace("authorization", "Bearer " + token);
    auto delete_resp = client.send(delete_req);
    ASSERT_TRUE(delete_resp.has_value());
    EXPECT_EQ(delete_resp->status, 204);

    auto delete_again = client.send(delete_req);
    ASSERT_TRUE(delete_again.has_value());
    EXPECT_EQ(delete_again->status, 404);
}

TEST(AusfIntegration, FiveGAkaWrongResStarIsAuthenticationFailure) {
    auto t = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client,
        "https://127.0.0.1:7782/nausf-auth/v1/ue-authentications/nonexistent/eap-session",
        50))
        << "ausf never became reachable";

    const std::string token = fetch_token(client, "nausf-auth");
    ASSERT_FALSE(token.empty());

    sbi_gen::AuthenticationInfo body{};
    body.supiOrSuci = "imsi-999700000000001";
    body.servingNetworkName = kServingNetworkName;

    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = "https://127.0.0.1:7782/nausf-auth/v1/ue-authentications";
    req.headers.emplace("content-type", "application/json");
    req.headers.emplace("authorization", "Bearer " + token);
    req.body = json(body).dump();

    auto resp = client.send(req);
    ASSERT_TRUE(resp.has_value());
    ASSERT_EQ(resp->status, 201);
    const auto location_it = resp->headers.find("location");
    ASSERT_NE(location_it, resp->headers.end());
    const std::string auth_ctx_url = "https://127.0.0.1:7782" + location_it->second;

    sbi_gen::ConfirmationData confirm{};
    confirm.resStar = std::string(32, '0'); // deliberately wrong

    sbi_core::http2::ClientRequest confirm_req;
    confirm_req.method = "PUT";
    confirm_req.url = auth_ctx_url + "/5g-aka-confirmation";
    confirm_req.headers.emplace("content-type", "application/json");
    confirm_req.headers.emplace("authorization", "Bearer " + token);
    confirm_req.body = json(confirm).dump();

    auto confirm_resp = client.send(confirm_req);
    ASSERT_TRUE(confirm_resp.has_value());
    EXPECT_EQ(confirm_resp->status, 200); // spec: same status for match or mismatch
    const auto confirmed = json::parse(confirm_resp->body).get<sbi_gen::ConfirmationDataResponse>();
    EXPECT_EQ(confirmed.authResult.value,
              sbi_gen::AuthResult_Nausf_UEAuthentication::AUTHENTICATION_FAILURE);
    EXPECT_FALSE(confirmed.kseaf.has_value());
}

TEST(AusfIntegration, EapAkaPrimeSuccessfulAuthenticationCrossChecksMacKseafAndMsk) {
    auto t = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client,
        "https://127.0.0.1:7782/nausf-auth/v1/ue-authentications/nonexistent/eap-session",
        50))
        << "ausf never became reachable";

    const std::string token = fetch_token(client, "nausf-auth");
    ASSERT_FALSE(token.empty());

    const std::string supi = "imsi-999700000000002"; // seeded EAP_AKA_PRIME subscriber

    sbi_gen::AuthenticationInfo body{};
    body.supiOrSuci = supi;
    body.servingNetworkName = kServingNetworkName;

    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = "https://127.0.0.1:7782/nausf-auth/v1/ue-authentications";
    req.headers.emplace("content-type", "application/json");
    req.headers.emplace("authorization", "Bearer " + token);
    req.body = json(body).dump();

    auto resp = client.send(req);
    ASSERT_TRUE(resp.has_value());
    ASSERT_EQ(resp->status, 201);
    const auto location_it = resp->headers.find("location");
    ASSERT_NE(location_it, resp->headers.end());
    const std::string auth_ctx_url = "https://127.0.0.1:7782" + location_it->second;

    const auto ctx = json::parse(resp->body).get<sbi_gen::UEAuthenticationCtx>();
    EXPECT_EQ(ctx.authType.value, sbi_gen::AuthType_Nausf_UEAuthentication::EAP_AKA_PRIME);
    const std::string eap_payload_b64 = ctx.n5gAuthData.get<std::string>();
    const auto packet = aka_crypto::eap::base64_decode(eap_payload_b64);
    ASSERT_TRUE(packet.has_value());

    const auto parsed = aka_crypto::eap::parse_challenge_request(*packet);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->kdf_input, kServingNetworkName);

    const auto ue = ue_compute(parsed->rand, parsed->autn);
    ASSERT_TRUE(ue.network_authenticated);

    const auto sqn_xor_ak = aka_crypto::sqn_xor_ak(ue.sqn, ue.f2345_out.ak);
    const auto ck_ik_prime = aka_crypto::derive_ck_ik_prime(
        ue.f2345_out.ck, ue.f2345_out.ik, kServingNetworkName, sqn_xor_ak);
    const auto keys = aka_crypto::eap::derive_keys(ck_ik_prime.first, ck_ik_prime.second, supi);

    EXPECT_TRUE(aka_crypto::eap::verify_mac(*packet, keys.k_aut))
        << "ausf's Request/AKA'-Challenge AT_MAC doesn't verify against an independently derived "
           "K_aut";

    const std::vector<uint8_t> res(ue.f2345_out.res.begin(), ue.f2345_out.res.end());
    const auto response_packet =
        aka_crypto::eap::build_challenge_response((*packet)[1], res, keys.k_aut);

    sbi_gen::EapSession eap_body{};
    eap_body.eapPayload = aka_crypto::eap::base64_encode(response_packet);

    sbi_core::http2::ClientRequest eap_req;
    eap_req.method = "POST";
    eap_req.url = auth_ctx_url + "/eap-session";
    eap_req.headers.emplace("content-type", "application/json");
    eap_req.headers.emplace("authorization", "Bearer " + token);
    eap_req.body = json(eap_body).dump();

    auto eap_resp = client.send(eap_req);
    ASSERT_TRUE(eap_resp.has_value());
    EXPECT_EQ(eap_resp->status, 200);
    const auto eap_result = json::parse(eap_resp->body).get<sbi_gen::EapSession>();
    ASSERT_TRUE(eap_result.authResult.has_value());
    EXPECT_EQ(eap_result.authResult->value,
              sbi_gen::AuthResult_Nausf_UEAuthentication::AUTHENTICATION_SUCCESS);
    ASSERT_TRUE(eap_result.supi.has_value());
    EXPECT_EQ(*eap_result.supi, supi);

    const auto success_packet = aka_crypto::eap::base64_decode(eap_result.eapPayload);
    ASSERT_TRUE(success_packet.has_value());
    ASSERT_EQ(success_packet->size(), 4U);
    EXPECT_EQ((*success_packet)[0], static_cast<uint8_t>(aka_crypto::eap::Code::kSuccess));

    // KAUSF for EAP-AKA': same Annex A.2 KDF as 5G-AKA (derive_kausf, FC=0x6A), keyed on CK'/IK'
    // instead of CK/IK -- see docs/DECISIONS.md ADR-0027.
    const auto kausf_expected = aka_crypto::derive_kausf(
        ck_ik_prime.first, ck_ik_prime.second, kServingNetworkName, sqn_xor_ak);
    const auto kseaf_expected = aka_crypto::derive_kseaf(kausf_expected, kServingNetworkName);
    ASSERT_TRUE(eap_result.kSeaf.has_value());
    EXPECT_EQ(*eap_result.kSeaf, aka_crypto::to_hex(kseaf_expected))
        << "ausf's KSEAF doesn't match an independent computation from CK'/IK'";
    ASSERT_TRUE(eap_result.msk.has_value());
    EXPECT_EQ(*eap_result.msk,
              aka_crypto::to_hex(std::vector<uint8_t>(keys.msk.begin(), keys.msk.end())));

    sbi_core::http2::ClientRequest delete_req;
    delete_req.method = "DELETE";
    delete_req.url = eap_req.url;
    delete_req.headers.emplace("authorization", "Bearer " + token);
    auto delete_resp = client.send(delete_req);
    ASSERT_TRUE(delete_resp.has_value());
    EXPECT_EQ(delete_resp->status, 204);
}

TEST(AusfIntegration, EapAkaPrimeWrongResIsAuthenticationFailure) {
    auto t = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client,
        "https://127.0.0.1:7782/nausf-auth/v1/ue-authentications/nonexistent/eap-session",
        50))
        << "ausf never became reachable";

    const std::string token = fetch_token(client, "nausf-auth");
    ASSERT_FALSE(token.empty());

    const std::string supi = "imsi-999700000000002";

    sbi_gen::AuthenticationInfo body{};
    body.supiOrSuci = supi;
    body.servingNetworkName = kServingNetworkName;

    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = "https://127.0.0.1:7782/nausf-auth/v1/ue-authentications";
    req.headers.emplace("content-type", "application/json");
    req.headers.emplace("authorization", "Bearer " + token);
    req.body = json(body).dump();

    auto resp = client.send(req);
    ASSERT_TRUE(resp.has_value());
    ASSERT_EQ(resp->status, 201);
    const auto location_it = resp->headers.find("location");
    ASSERT_NE(location_it, resp->headers.end());
    const std::string auth_ctx_url = "https://127.0.0.1:7782" + location_it->second;

    const auto ctx = json::parse(resp->body).get<sbi_gen::UEAuthenticationCtx>();
    const std::string eap_payload_b64 = ctx.n5gAuthData.get<std::string>();
    const auto packet = aka_crypto::eap::base64_decode(eap_payload_b64);
    ASSERT_TRUE(packet.has_value());
    const auto parsed = aka_crypto::eap::parse_challenge_request(*packet);
    ASSERT_TRUE(parsed.has_value());

    const auto ue = ue_compute(parsed->rand, parsed->autn);
    const auto sqn_xor_ak = aka_crypto::sqn_xor_ak(ue.sqn, ue.f2345_out.ak);
    const auto ck_ik_prime = aka_crypto::derive_ck_ik_prime(
        ue.f2345_out.ck, ue.f2345_out.ik, kServingNetworkName, sqn_xor_ak);
    const auto keys = aka_crypto::eap::derive_keys(ck_ik_prime.first, ck_ik_prime.second, supi);

    const std::vector<uint8_t> wrong_res{0, 0, 0, 0, 0, 0, 0, 0};
    const auto response_packet =
        aka_crypto::eap::build_challenge_response((*packet)[1], wrong_res, keys.k_aut);

    sbi_gen::EapSession eap_body{};
    eap_body.eapPayload = aka_crypto::eap::base64_encode(response_packet);

    sbi_core::http2::ClientRequest eap_req;
    eap_req.method = "POST";
    eap_req.url = auth_ctx_url + "/eap-session";
    eap_req.headers.emplace("content-type", "application/json");
    eap_req.headers.emplace("authorization", "Bearer " + token);
    eap_req.body = json(eap_body).dump();

    auto eap_resp = client.send(eap_req);
    ASSERT_TRUE(eap_resp.has_value());
    EXPECT_EQ(eap_resp->status, 200);
    const auto eap_result = json::parse(eap_resp->body).get<sbi_gen::EapSession>();
    ASSERT_TRUE(eap_result.authResult.has_value());
    EXPECT_EQ(eap_result.authResult->value,
              sbi_gen::AuthResult_Nausf_UEAuthentication::AUTHENTICATION_FAILURE);
    EXPECT_FALSE(eap_result.kSeaf.has_value());

    const auto failure_packet = aka_crypto::eap::base64_decode(eap_result.eapPayload);
    ASSERT_TRUE(failure_packet.has_value());
    ASSERT_EQ(failure_packet->size(), 4U);
    EXPECT_EQ((*failure_packet)[0], static_cast<uint8_t>(aka_crypto::eap::Code::kFailure));
}

TEST(AusfIntegration, DeregisterRemovesContextThenSecondDeregisterIs404) {
    auto t = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client,
        "https://127.0.0.1:7782/nausf-auth/v1/ue-authentications/nonexistent/eap-session",
        50))
        << "ausf never became reachable";

    const std::string token = fetch_token(client, "nausf-auth");
    ASSERT_FALSE(token.empty());

    const std::string supi = "imsi-999700000000001";

    sbi_gen::AuthenticationInfo body{};
    body.supiOrSuci = supi;
    body.servingNetworkName = kServingNetworkName;

    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = "https://127.0.0.1:7782/nausf-auth/v1/ue-authentications";
    req.headers.emplace("content-type", "application/json");
    req.headers.emplace("authorization", "Bearer " + token);
    req.body = json(body).dump();
    auto resp = client.send(req);
    ASSERT_TRUE(resp.has_value());
    ASSERT_EQ(resp->status, 201);

    sbi_gen::DeregistrationInfo dereg{};
    dereg.supi = supi;

    sbi_core::http2::ClientRequest dereg_req;
    dereg_req.method = "POST";
    dereg_req.url = "https://127.0.0.1:7782/nausf-auth/v1/ue-authentications/deregister";
    dereg_req.headers.emplace("content-type", "application/json");
    dereg_req.headers.emplace("authorization", "Bearer " + token);
    dereg_req.body = json(dereg).dump();

    auto dereg_resp = client.send(dereg_req);
    ASSERT_TRUE(dereg_resp.has_value());
    EXPECT_EQ(dereg_resp->status, 204);

    auto dereg_again = client.send(dereg_req);
    ASSERT_TRUE(dereg_again.has_value());
    EXPECT_EQ(dereg_again->status, 404);
}

TEST(AusfIntegration, UnknownSupiIs404AndTamperedTokenIs401) {
    auto t = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client,
        "https://127.0.0.1:7782/nausf-auth/v1/ue-authentications/nonexistent/eap-session",
        50))
        << "ausf never became reachable";

    const std::string token = fetch_token(client, "nausf-auth");
    ASSERT_FALSE(token.empty());

    sbi_gen::AuthenticationInfo body{};
    body.supiOrSuci = "imsi-999700000000099"; // not seeded in udm
    body.servingNetworkName = kServingNetworkName;

    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = "https://127.0.0.1:7782/nausf-auth/v1/ue-authentications";
    req.headers.emplace("content-type", "application/json");
    req.headers.emplace("authorization", "Bearer " + token);
    req.body = json(body).dump();

    auto resp = client.send(req);
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->status, 404);

    sbi_core::http2::ClientRequest tampered_req = req;
    tampered_req.headers.erase("authorization");
    tampered_req.headers.emplace("authorization", "Bearer " + token + "tampered");
    auto tampered_resp = client.send(tampered_req);
    ASSERT_TRUE(tampered_resp.has_value());
    EXPECT_EQ(tampered_resp->status, 401);
}
