// Live-verifies AUSF's Nausf_UPUProtection surface (docs/DECISIONS.md ADR-0195, gap-closure per
// ADR-0193) against real, separate nrf+udm+ausf OS processes. Reuses the same real 5G-AKA
// initiate+confirm flow as test_ausf_ue_authentication.cpp to establish a real KAUSF on record for
// a SUPI (the real precondition Nausf_UPUProtection needs, TS 33.501 clause 6.15.1: "the AUSF
// shall store the latest KAUSF after the completion of the latest primary authentication"), then
// exercises the real POST /nausf-upuprotection/v1/{supi}/ue-upu route's honest 400/404/501
// behavior -- this build has no TS 24.501 §9.11.3.53A NAS encoder, so a real, structurally-valid
// request against an authenticated SUPI gets a real, disclosed 501, not a fabricated MAC.

#include "sbi_core/http2_client.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

#include "TS26510_CommonData_grp.hpp"
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

// Real 5G-AKA initiate+confirm, same flow as test_ausf_ue_authentication.cpp -- establishes the
// real KAUSF this project's own KausfStore then holds for `supi`.
void authenticate(sbi_core::http2::Client& client,
                  const std::string& token,
                  const std::string& supi) {
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
    const auto av = ctx.n5gAuthData.get<sbi_gen::Av5gAka>();

    const auto rand = *aka_crypto::from_hex<16>(av.rand);
    const auto autn = *aka_crypto::from_hex<16>(av.autn);
    const auto ue = ue_compute(rand, autn);
    ASSERT_TRUE(ue.network_authenticated);

    const auto xres_star = aka_crypto::derive_res_star(
        ue.f2345_out.ck, ue.f2345_out.ik, kServingNetworkName, rand, ue.f2345_out.res);

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
    ASSERT_EQ(confirm_resp->status, 200);
}

} // namespace

TEST(AusfUpuProtectionIntegration, RealUpuFlowHonestlyReports404Then400Then501) {
    auto t = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client,
        "https://127.0.0.1:7782/nausf-auth/v1/ue-authentications/nonexistent/eap-session",
        50))
        << "ausf never became reachable";

    const std::string token = fetch_token(client, "nausf-auth");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const std::string authenticated_supi = "imsi-999700000000001";
    const std::string never_authenticated_supi = "imsi-999700000000999";

    // (a) No KAUSF on record for a SUPI that never authenticated -> real 404.
    {
        sbi_gen::UpuInfo_Nausf_UPUProtection body{};
        sbi_gen::UpuData_Nausf_UPUProtection data{};
        data.drei = true;
        body.upuDataList = {data};
        body.upuAckInd = false;

        sbi_core::http2::ClientRequest req;
        req.method = "POST";
        req.url =
            "https://127.0.0.1:7782/nausf-upuprotection/v1/" + never_authenticated_supi + "/ue-upu";
        req.headers.emplace("content-type", "application/json");
        req.headers.emplace("authorization", "Bearer " + token);
        req.body = json(body).dump();
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 404)
            << "no KAUSF on record for a never-authenticated SUPI should be a real 404";
    }

    // Establish a real KAUSF for authenticated_supi via the real 5G-AKA flow.
    authenticate(client, token, authenticated_supi);

    // (b) Empty upuDataList against the now-authenticated SUPI -> real 400 (real declared
    // minItems: 1).
    {
        sbi_gen::UpuInfo_Nausf_UPUProtection body{};
        body.upuAckInd = false;

        sbi_core::http2::ClientRequest req;
        req.method = "POST";
        req.url = "https://127.0.0.1:7782/nausf-upuprotection/v1/" + authenticated_supi + "/ue-upu";
        req.headers.emplace("content-type", "application/json");
        req.headers.emplace("authorization", "Bearer " + token);
        req.body = json(body).dump();
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 400) << "empty upuDataList should be a real 400";
    }

    // (c) Real, structurally-valid request against the authenticated SUPI -> real, disclosed 501
    // (this build has no TS 24.501 §9.11.3.53A NAS encoder for UE Parameters Update Data).
    {
        sbi_gen::UpuInfo_Nausf_UPUProtection body{};
        sbi_gen::UpuData_Nausf_UPUProtection data{};
        data.drei = true;
        body.upuDataList = {data};
        body.upuAckInd = false;

        sbi_core::http2::ClientRequest req;
        req.method = "POST";
        req.url = "https://127.0.0.1:7782/nausf-upuprotection/v1/" + authenticated_supi + "/ue-upu";
        req.headers.emplace("content-type", "application/json");
        req.headers.emplace("authorization", "Bearer " + token);
        req.body = json(body).dump();
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 501)
            << "real KAUSF on record but no NAS encoder available should be a real, disclosed 501";
    }
}
