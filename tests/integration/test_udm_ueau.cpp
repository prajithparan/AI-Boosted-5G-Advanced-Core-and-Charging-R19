// Drives nrf and udm as real, separate OS processes to exercise udm's Nudm_UEAU surface (this
// turn's addition to an already-committed NF -- see docs/DECISIONS.md ADR-0026): real Milenage +
// TS 33.501 Annex A key derivation (libs/aka-crypto) against the two fixed test subscribers seeded
// in nfs/udm/src/main.cpp (5G_AKA and EAP_AKA_PRIME), over real TLS 1.3 + mTLS HTTP/2 with a real
// signed OAuth2 token from nrf.

#include "sbi_core/http2_client.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

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

} // namespace

TEST(UdmIntegration, GenerateAuthDataFor5GAkaSubscriberProducesDistinctVectors) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess udm(UDM_PATH);
    ASSERT_GT(udm.pid(), 0) << "failed to fork udm";
    // ADR-0383: UDM reads authentication data from the UDR over Nudr.
    nf_test::SpawnedProcess udr(UDR_PATH);
    ASSERT_TRUE(nf_test::wait_tcp_listening(7781)) << "udr never started listening";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client,
        "https://127.0.0.1:7780/nudm-uecm/v1/nonexistent/registrations/amf-3gpp-access",
        50))
        << "udm never became reachable";

    const std::string token = fetch_token(client, "nudm-ueau");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const std::string supi = "imsi-999700000000001"; // seeded 5G_AKA subscriber, main.cpp

    sbi_gen::AuthenticationInfoRequest body{};
    body.servingNetworkName = "5G:mnc070.mcc999.3gppnetwork.org";
    body.ausfInstanceId = "00000000-0000-4000-8000-000000000ddd";

    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url =
        "https://127.0.0.1:7780/nudm-ueau/v1/" + supi + "/security-information/generate-auth-data";
    req.headers.emplace("content-type", "application/json");
    req.headers.emplace("authorization", "Bearer " + token);
    req.body = json(body).dump();

    auto resp1 = client.send(req);
    ASSERT_TRUE(resp1.has_value());
    EXPECT_EQ(resp1->status, 200);
    const auto result1 = json::parse(resp1->body).get<sbi_gen::AuthenticationInfoResult>();
    EXPECT_EQ(result1.authType.value, sbi_gen::AuthType_Nudm_UEAU::V5G_AKA);
    ASSERT_TRUE(result1.authenticationVector.has_value());
    const auto av1 = result1.authenticationVector->get<sbi_gen::Av5GHeAka>();
    EXPECT_EQ(av1.avType.value, sbi_gen::AvType::V5G_HE_AKA);
    EXPECT_EQ(av1.rand.size(), 32U);     // 16 bytes, hex-encoded
    EXPECT_EQ(av1.xresStar.size(), 32U); // 16 bytes, hex-encoded
    EXPECT_EQ(av1.autn.size(), 32U);     // 16 bytes, hex-encoded
    EXPECT_EQ(av1.kausf.size(), 64U);    // 32 bytes, hex-encoded

    // Every GenerateAuthData call draws a fresh RAND and advances the store's SQN (see
    // UdrAuthSubscriptionSource::get_and_advance_sqn), so a second call must produce a
    // genuinely different vector, not a cached/replayed one.
    auto resp2 = client.send(req);
    ASSERT_TRUE(resp2.has_value());
    EXPECT_EQ(resp2->status, 200);
    const auto result2 = json::parse(resp2->body).get<sbi_gen::AuthenticationInfoResult>();
    const auto av2 = result2.authenticationVector->get<sbi_gen::Av5GHeAka>();
    EXPECT_NE(av1.rand, av2.rand);
    EXPECT_NE(av1.autn, av2.autn);
    EXPECT_NE(av1.kausf, av2.kausf);
}

TEST(UdmIntegration, GenerateAuthDataForEapAkaPrimeSubscriberReturnsEapAkaPrimeVector) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess udm(UDM_PATH);
    ASSERT_GT(udm.pid(), 0) << "failed to fork udm";
    // ADR-0383: UDM reads authentication data from the UDR over Nudr.
    nf_test::SpawnedProcess udr(UDR_PATH);
    ASSERT_TRUE(nf_test::wait_tcp_listening(7781)) << "udr never started listening";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client,
        "https://127.0.0.1:7780/nudm-uecm/v1/nonexistent/registrations/amf-3gpp-access",
        50))
        << "udm never became reachable";

    const std::string token = fetch_token(client, "nudm-ueau");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const std::string supi = "imsi-999700000000002"; // seeded EAP_AKA_PRIME subscriber, main.cpp

    sbi_gen::AuthenticationInfoRequest body{};
    body.servingNetworkName = "5G:mnc070.mcc999.3gppnetwork.org";
    body.ausfInstanceId = "00000000-0000-4000-8000-000000000ddd";

    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url =
        "https://127.0.0.1:7780/nudm-ueau/v1/" + supi + "/security-information/generate-auth-data";
    req.headers.emplace("content-type", "application/json");
    req.headers.emplace("authorization", "Bearer " + token);
    req.body = json(body).dump();

    auto resp = client.send(req);
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->status, 200);
    const auto result = json::parse(resp->body).get<sbi_gen::AuthenticationInfoResult>();
    EXPECT_EQ(result.authType.value, sbi_gen::AuthType_Nudm_UEAU::EAP_AKA_PRIME);
    ASSERT_TRUE(result.authenticationVector.has_value());
    const auto av = result.authenticationVector->get<sbi_gen::AvEapAkaPrime>();
    EXPECT_EQ(av.avType.value, sbi_gen::AvType::EAP_AKA_PRIME);
    EXPECT_EQ(av.xres.size(), 16U);    // RES (not RES*): 8 bytes, hex-encoded
    EXPECT_EQ(av.ckPrime.size(), 32U); // 16 bytes, hex-encoded
    EXPECT_EQ(av.ikPrime.size(), 32U); // 16 bytes, hex-encoded
}

TEST(UdmIntegration, ConfirmAuthThenDeleteAuthLifecycle) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess udm(UDM_PATH);
    ASSERT_GT(udm.pid(), 0) << "failed to fork udm";
    // ADR-0383: UDM reads authentication data from the UDR over Nudr.
    nf_test::SpawnedProcess udr(UDR_PATH);
    ASSERT_TRUE(nf_test::wait_tcp_listening(7781)) << "udr never started listening";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client,
        "https://127.0.0.1:7780/nudm-uecm/v1/nonexistent/registrations/amf-3gpp-access",
        50))
        << "udm never became reachable";

    const std::string token = fetch_token(client, "nudm-ueau");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const std::string supi = "imsi-999700000000001";

    sbi_gen::AuthEvent event{};
    event.nfInstanceId = "00000000-0000-4000-8000-000000000eee";
    event.success = true;
    event.timeStamp = "2026-08-06T00:00:00Z";
    event.authType.value = sbi_gen::AuthType_Nudm_UEAU::V5G_AKA;
    event.servingNetworkName = "5G:mnc070.mcc999.3gppnetwork.org";

    sbi_core::http2::ClientRequest confirm_req;
    confirm_req.method = "POST";
    confirm_req.url = "https://127.0.0.1:7780/nudm-ueau/v1/" + supi + "/auth-events";
    confirm_req.headers.emplace("content-type", "application/json");
    confirm_req.headers.emplace("authorization", "Bearer " + token);
    confirm_req.body = json(event).dump();

    auto confirm_resp = client.send(confirm_req);
    ASSERT_TRUE(confirm_resp.has_value());
    EXPECT_EQ(confirm_resp->status, 201);
    const auto location_it = confirm_resp->headers.find("location");
    ASSERT_NE(location_it, confirm_resp->headers.end());
    const std::string location = location_it->second;
    const auto confirmed = json::parse(confirm_resp->body).get<sbi_gen::AuthEvent>();
    EXPECT_EQ(confirmed.nfInstanceId, event.nfInstanceId);

    // Location: {apiRoot}/nudm-ueau/v1/{supi}/auth-events/{authEventId} -- pull authEventId back
    // out to build the DeleteAuth URL, same as a real AUSF client would.
    const std::string auth_event_id = location.substr(location.find_last_of('/') + 1);
    ASSERT_FALSE(auth_event_id.empty());

    sbi_core::http2::ClientRequest delete_req;
    delete_req.method = "PUT";
    delete_req.url =
        "https://127.0.0.1:7780/nudm-ueau/v1/" + supi + "/auth-events/" + auth_event_id;
    delete_req.headers.emplace("content-type", "application/json");
    delete_req.headers.emplace("authorization", "Bearer " + token);
    delete_req.body = json(event).dump();

    auto delete_resp = client.send(delete_req);
    ASSERT_TRUE(delete_resp.has_value());
    EXPECT_EQ(delete_resp->status, 204);

    // Second DeleteAuth for the same, now-removed authEventId must 404.
    auto delete_again = client.send(delete_req);
    ASSERT_TRUE(delete_again.has_value());
    EXPECT_EQ(delete_again->status, 404);
}

TEST(UdmIntegration, GenerateAuthDataUnknownSupiIs404AndTamperedTokenIs401) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess udm(UDM_PATH);
    ASSERT_GT(udm.pid(), 0) << "failed to fork udm";
    // ADR-0383: UDM reads authentication data from the UDR over Nudr.
    nf_test::SpawnedProcess udr(UDR_PATH);
    ASSERT_TRUE(nf_test::wait_tcp_listening(7781)) << "udr never started listening";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client,
        "https://127.0.0.1:7780/nudm-uecm/v1/nonexistent/registrations/amf-3gpp-access",
        50))
        << "udm never became reachable";

    const std::string token = fetch_token(client, "nudm-ueau");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    sbi_gen::AuthenticationInfoRequest body{};
    body.servingNetworkName = "5G:mnc070.mcc999.3gppnetwork.org";
    body.ausfInstanceId = "00000000-0000-4000-8000-000000000ddd";

    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = "https://127.0.0.1:7780/nudm-ueau/v1/imsi-999700000000099/"
              "security-information/generate-auth-data";
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
