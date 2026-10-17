// ADR-0495: AKMA end to end through real processes -- UDR (the subscription with akmaAllowed /
// routingId, provisioned over Nudr like any OAM write), UDM (akmaInd + routingId in
// GenerateAuthData), AUSF (5G-AKA; after confirmation derives KAKMA and the A-KID and registers
// them at the AAnF, TS 33.535 6.1), AAnF (Naanf_AKMA). The test is the UE and the AF: it derives
// KAUSF, KAKMA, the A-KID and KAF itself and checks the AAnF returns the same KAF (6.2.1).

#include "sbi_core/http2_client.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <thread>

#include "TS29509_Nausf_UEAuthentication.hpp"
#include "aka_crypto/akma.hpp"
#include "aka_crypto/hex.hpp"
#include "aka_crypto/kdf.hpp"
#include "aka_crypto/milenage.hpp"
#include "spawn_guard.hpp"
#include "test_tokens.hpp"

#include <gtest/gtest.h>

namespace {

using nlohmann::json;
using namespace std::chrono_literals;

constexpr const char* kServingNetworkName = "5G:mnc070.mcc999.3gppnetwork.org";
constexpr const char* kSupi = "imsi-999700000000001"; // seeded 5G_AKA subscriber (ADR-0026)
constexpr const char* kRid = "678";
constexpr const char* kAfId = "af.example.com.0100BC0001"; // TS 29.522's own AfId example form
constexpr const char* kUdrSub =
    "https://127.0.0.1:7781/nudr-dr/v2/subscription-data/imsi-999700000000001/"
    "authentication-data/authentication-subscription";
constexpr const char* kAanf = "https://127.0.0.1:7824/naanf-akma/v1";

aka_crypto::Key128 test_k() {
    return *aka_crypto::from_hex<16>("465b5ce8b199b49faa5f0a2ee238a6bc");
}
aka_crypto::Key128 test_opc() {
    return aka_crypto::derive_opc(test_k(),
                                  *aka_crypto::from_hex<16>("cdc202d5123e20f62b6d676ac72cb318"));
}

sbi_core::http2::Client make_client() {
    return sbi_core::http2::Client(sbi_core::http2::TlsConfig{
        .cert_path = CERTS_DIR "/hello-nf/cert.pem",
        .key_path = CERTS_DIR "/hello-nf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    });
}

// REVIEW SEC-03: the AAnF binds an AF's afId to the FQDN its certificate names, and takes context
// removal only from the AUSF -- so those requests come from clients holding those certificates.
sbi_core::http2::Client make_client_as(const std::string& name) {
    return sbi_core::http2::Client(sbi_core::http2::TlsConfig{
        .cert_path = std::string(CERTS_DIR) + "/" + name + "/cert.pem",
        .key_path = std::string(CERTS_DIR) + "/" + name + "/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    });
}

std::string token(sbi_core::http2::Client& c, const std::string& scope, const std::string& nf) {
    for (int i = 0; i < 100; ++i) {
        sbi_core::http2::ClientRequest req;
        req.method = "POST";
        req.url = "https://127.0.0.1:7777/oauth2/token";
        req.headers.emplace("content-type", "application/x-www-form-urlencoded");
        req.body = "grant_type=client_credentials&nfInstanceId=" +
                   std::string(nf_test::kTestAfInstanceId) + "&scope=" + scope +
                   "&targetNfType=" + nf;
        if (auto r = nf_test::send_token_request(req); r && r->status == 200) {
            return json::parse(r->body).at("access_token").get<std::string>();
        }
        std::this_thread::sleep_for(100ms);
    }
    return "";
}

sbi_core::http2::ClientResponse send(sbi_core::http2::Client& c,
                                     const std::string& method,
                                     const std::string& url,
                                     const std::string& bearer,
                                     const std::string& content_type,
                                     const std::string& body) {
    sbi_core::http2::ClientRequest req;
    req.method = method;
    req.url = url;
    if (!bearer.empty()) {
        req.headers.emplace("authorization", "Bearer " + bearer);
    }
    if (!body.empty()) {
        req.headers.emplace("content-type", content_type);
        req.body = body;
    }
    for (int i = 0; i < 100; ++i) {
        if (auto r = c.send(req)) {
            return *r;
        }
        std::this_thread::sleep_for(100ms);
    }
    ADD_FAILURE() << method << " " << url << " never answered";
    return {};
}

// UDR provisioning: akmaAllowed + routingId on (or off) for the subscriber.
void provision_akma(sbi_core::http2::Client& c, const std::string& udr_token, bool on) {
    json ops = on ? json::array({json{{"op", "add"}, {"path", "/akmaAllowed"}, {"value", true}},
                                 json{{"op", "add"}, {"path", "/routingId"}, {"value", kRid}}})
                  : json::array({json{{"op", "remove"}, {"path", "/akmaAllowed"}},
                                 json{{"op", "remove"}, {"path", "/routingId"}}});
    const auto r = send(c, "PATCH", kUdrSub, udr_token, "application/json-patch+json", ops.dump());
    ASSERT_TRUE(r.status == 200 || r.status == 204) << r.status << " " << r.body;
}

} // namespace

TEST(AkmaEndToEnd, AusfRegistersKakmaAndTheAanfGivesTheAfTheSameKafTheUeDerives) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    std::this_thread::sleep_for(200ms);
    nf_test::SpawnedProcess udr(UDR_PATH);
    nf_test::SpawnedProcess udm(UDM_PATH);
    nf_test::SpawnedProcess ausf(AUSF_PATH);
    // The AAnF with a GPSI policy for one internal AF (ADR-0496).
    std::string aanf_cfg;
    {
        std::ifstream in(AANF_CONFIG_TEMPLATE);
        json cfg = json::parse(in);
        cfg["gpsi_af_fqdns"] = json::array({"gpsi-af.example.com"});
        cfg["deny_roaming_ues"] = true; // ADR-0497
        aanf_cfg = std::string(::testing::TempDir()) + "aanf_akma.json";
        std::ofstream(aanf_cfg) << cfg.dump(2);
    }
    ::setenv("AANF_CONFIG_FILE", aanf_cfg.c_str(), 1);
    nf_test::SpawnedProcess aanf(AANF_PATH);
    ::unsetenv("AANF_CONFIG_FILE");
    nf_test::SpawnedProcess nef(NEF_PATH);
    nf_test::wait_tcp_listening(7790);
    nf_test::wait_tcp_listening(7781);
    nf_test::wait_tcp_listening(7782);
    nf_test::wait_tcp_listening(7824);

    auto c = make_client();
    auto af = make_client_as("af.example.com");
    auto gpsi_af = make_client_as("gpsi-af.example.com");
    auto ausf_client = make_client_as("ausf");
    nf_test::attach_test_tokens(af);
    nf_test::attach_test_tokens(gpsi_af);
    nf_test::attach_test_tokens(ausf_client);
    nf_test::attach_test_tokens(c); // ADR-0499: the NEF enforces tokens
    const auto udr_token = token(c, "nudr-dr", "UDR");
    const auto ausf_token = token(c, "nausf-auth", "AUSF");
    ASSERT_FALSE(udr_token.empty());
    ASSERT_FALSE(ausf_token.empty());
    provision_akma(c, udr_token, true);

    // 5G-AKA through the AUSF, the test as the UE (same flow as test_ausf_ue_authentication.cpp).
    sbi_gen::AuthenticationInfo info{};
    info.supiOrSuci = kSupi;
    info.servingNetworkName = kServingNetworkName;
    const auto created = send(c,
                              "POST",
                              "https://127.0.0.1:7782/nausf-auth/v1/ue-authentications",
                              ausf_token,
                              "application/json",
                              json(info).dump());
    ASSERT_EQ(created.status, 201) << created.body;
    const auto av = json::parse(created.body)
                        .get<sbi_gen::UEAuthenticationCtx>()
                        .n5gAuthData.get<sbi_gen::Av5gAka>();
    const auto rand = *aka_crypto::from_hex<16>(av.rand);
    const auto autn = *aka_crypto::from_hex<16>(av.autn);
    const auto out = aka_crypto::f2345(test_opc(), test_k(), rand);
    aka_crypto::Ak48 sqn_xor_ak{};
    std::copy(autn.begin(), autn.begin() + 6, sqn_xor_ak.begin());
    const auto res_star =
        aka_crypto::derive_res_star(out.ck, out.ik, kServingNetworkName, rand, out.res);
    const auto kausf = aka_crypto::derive_kausf(out.ck, out.ik, kServingNetworkName, sqn_xor_ak);
    sbi_gen::ConfirmationData confirm{};
    confirm.resStar = aka_crypto::to_hex(res_star);
    const auto ctx_url = "https://127.0.0.1:7782" + created.headers.find("location")->second;
    const auto confirmed = send(c,
                                "PUT",
                                ctx_url + "/5g-aka-confirmation",
                                ausf_token,
                                "application/json",
                                json(confirm).dump());
    ASSERT_EQ(confirmed.status, 200) << confirmed.body;
    EXPECT_EQ(json::parse(confirmed.body).at("authResult"), "AUTHENTICATION_SUCCESS");

    // The UE side of TS 33.535 6.1 / Annex A: KAKMA, A-TID, A-KID; then KAF for this AF (A.4).
    const auto kakma = aka_crypto::derive_kakma(kausf, "999700000000001");
    const auto a_kid = aka_crypto::make_a_kid(
        kRid, aka_crypto::derive_a_tid(kausf, "999700000000001"), "999", "70");
    EXPECT_EQ(a_kid.rfind("rid678.tid", 0), 0U) << a_kid;
    EXPECT_NE(a_kid.find("@5gc.mnc070.mcc999.3gppnetwork.org"), std::string::npos) << a_kid;
    const auto kaf = aka_crypto::derive_kaf(kakma, *aka_crypto::af_id_octets(kAfId));

    // ADR-0497: the test as the AMF registers the UE at the UDM in its home PLMN (999/70).
    const auto amf_token = token(c, "nudm-uecm", "UDM");
    ASSERT_FALSE(amf_token.empty());
    const auto register_amf = [&](const std::string& mcc, const std::string& mnc) {
        const json reg{
            {"amfInstanceId", "5ba9a927-1d31-4c8e-8a10-00000000a0f1"},
            {"deregCallbackUri", "https://127.0.0.1:9/dereg"},
            {"guami", json{{"plmnId", json{{"mcc", mcc}, {"mnc", mnc}}}, {"amfId", "010041"}}},
            {"ratType", "NR"}};
        const auto r = send(c,
                            "PUT",
                            std::string("https://127.0.0.1:7780/nudm-uecm/v1/") + kSupi +
                                "/registrations/amf-3gpp-access",
                            amf_token,
                            "application/json",
                            reg.dump());
        ASSERT_TRUE(r.status == 200 || r.status == 201) << r.status << " " << r.body;
    };
    register_amf("999", "70");

    // The AF asks the AAnF (6.2.1), with the SUPI-access scope.
    const auto af_token = token(c,
                                "naanf-akma%20naanf-akma:applicationkeyget%20"
                                "naanf-akma:applicationkeyget:supi-access",
                                "AANF");
    ASSERT_FALSE(af_token.empty());
    const json request{{"afId", kAfId}, {"aKId", a_kid}};
    sbi_core::http2::ClientResponse got;
    for (int i = 0; i < 50; ++i) { // the AUSF registers right after the confirmation
        got = send(af,
                   "POST",
                   std::string(kAanf) + "/retrieve-applicationkey",
                   af_token,
                   "application/json",
                   request.dump());
        if (got.status == 200) {
            break;
        }
        std::this_thread::sleep_for(100ms);
    }
    ASSERT_EQ(got.status, 200) << got.body;
    const auto data = json::parse(got.body);
    EXPECT_EQ(data.at("kaf"), aka_crypto::to_hex(kaf)) << "AAnF and UE disagree on KAF";
    EXPECT_EQ(data.at("supi"), kSupi);
    EXPECT_TRUE(data.contains("expiry"));

    // Anonymous user (6.2.2): KAF, no SUPI.
    json anon = request;
    anon["anonInd"] = true;
    const auto a = send(af,
                        "POST",
                        std::string(kAanf) + "/retrieve-applicationkey",
                        af_token,
                        "application/json",
                        anon.dump());
    ASSERT_EQ(a.status, 200) << a.body;
    EXPECT_FALSE(json::parse(a.body).contains("supi"));

    // An A-KID the AAnF does not know: 403 K_AKMA_NOT_PRESENT (TS 29.535 table 5.1.7.3-1).
    json unknown = request;
    unknown["aKId"] = "rid0.tid00@5gc.mnc070.mcc999.3gppnetwork.org";
    const auto u = send(af,
                        "POST",
                        std::string(kAanf) + "/retrieve-applicationkey",
                        af_token,
                        "application/json",
                        unknown.dump());
    EXPECT_EQ(u.status, 403);
    EXPECT_EQ(json::parse(u.body).value("cause", ""), "K_AKMA_NOT_PRESENT");

    // ADR-0497, 6.2.1 steps 5-8: the UE now serves from a visited PLMN ->
    // ROAMING_AKMA_SERVICE_DENIED.
    register_amf("001", "01");
    const auto roam = send(af,
                           "POST",
                           std::string(kAanf) + "/retrieve-applicationkey",
                           af_token,
                           "application/json",
                           request.dump());
    EXPECT_EQ(roam.status, 403) << roam.body;
    EXPECT_EQ(json::parse(roam.body).value("cause", ""), "ROAMING_AKMA_SERVICE_DENIED");
    register_amf("999", "70");

    // ADR-0496, 6.2.1 steps 3-4 / 8: an internal AF the AAnF's policy gives the GPSI, not the SUPI.
    json gpsi_req = request;
    gpsi_req["afId"] = "gpsi-af.example.com.0100BC0001";
    const auto g = send(gpsi_af,
                        "POST",
                        std::string(kAanf) + "/retrieve-applicationkey",
                        af_token,
                        "application/json",
                        gpsi_req.dump());
    ASSERT_EQ(g.status, 200) << g.body;
    const auto gd = json::parse(g.body);
    ASSERT_TRUE(gd.contains("gpsi")) << g.body;
    EXPECT_FALSE(gd.contains("supi"));
    const std::string gpsi = gd.at("gpsi");

    // ADR-0496, 6.3: an AF outside the operator's network, via the NEF (TS 29.522 3gpp-akma).
    const auto ext = send(c,
                          "POST",
                          "https://127.0.0.1:7790/3gpp-akma/v1/retrieve",
                          "",
                          "application/json",
                          request.dump());
    ASSERT_EQ(ext.status, 200) << ext.body;
    const auto ed = json::parse(ext.body);
    EXPECT_EQ(ed.at("kaf"), aka_crypto::to_hex(kaf));
    EXPECT_FALSE(ed.contains("supi")) << "the NEF shall not send the SUPI to the AF";
    EXPECT_EQ(ed.value("gpsi", ""), gpsi);
    // "UE Id not needed": no GPSI either.
    const auto ext_anon = send(c,
                               "POST",
                               "https://127.0.0.1:7790/3gpp-akma/v1/retrieve",
                               "",
                               "application/json",
                               anon.dump());
    ASSERT_EQ(ext_anon.status, 200) << ext_anon.body;
    EXPECT_FALSE(json::parse(ext_anon.body).contains("gpsi"));
    // An unknown A-KID through the NEF: the AAnF's 403 K_AKMA_NOT_PRESENT, relayed.
    const auto ext_unknown = send(c,
                                  "POST",
                                  "https://127.0.0.1:7790/3gpp-akma/v1/retrieve",
                                  "",
                                  "application/json",
                                  unknown.dump());
    EXPECT_EQ(ext_unknown.status, 403);

    // Context removal (6.6): 204, then the key is gone, then 404 AKMA_CONTEXT_NOT_FOUND.
    const json remove{{"supi", kSupi}};
    const auto anchor_token = token(c, "naanf-akma%20naanf-akma:anchorkey", "AANF");
    EXPECT_EQ(send(ausf_client,
                   "POST",
                   std::string(kAanf) + "/remove-context",
                   anchor_token,
                   "application/json",
                   remove.dump())
                  .status,
              204);
    EXPECT_EQ(send(af,
                   "POST",
                   std::string(kAanf) + "/retrieve-applicationkey",
                   af_token,
                   "application/json",
                   request.dump())
                  .status,
              403);
    const auto again = send(ausf_client,
                            "POST",
                            std::string(kAanf) + "/remove-context",
                            anchor_token,
                            "application/json",
                            remove.dump());
    EXPECT_EQ(again.status, 404);
    EXPECT_EQ(json::parse(again.body).value("cause", ""), "AKMA_CONTEXT_NOT_FOUND");

    provision_akma(c, udr_token, false);
}
