// Drives nrf/udr/udm as real, separate OS processes to exercise UDM's Nudm_SDM identifier-lookup
// group gap-closure (ADR-0236, docs/CAPABILITY_GAP_ANALYSIS.md's own UDM audit): GetSupiOrGpsi
// (GET /{ueId}/id-translation-result) and GetMultipleIdentifiers (GET /multiple-identifiers).
// Real, disclosed bound: the forward direction (SUPI -> GPSI, via a new real `gpsis` field on
// UDR's seeded AccessAndMobilitySubscriptionData) is genuinely closed; the reverse direction
// (GPSI -> SUPI) is a real, disclosed gap -- no query-by-gpsi capability exists anywhere in the
// real Nudr_DR API.

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

TEST(UdmSdmGapClosure236Integration, IdentifierLookupOpsReturnRealGpsiDataAndDiscloseReverseGap) {
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

    // Real seeded test subscribers (nfs/udr/src/main.cpp's own startup seed, ADR-0236 adds real
    // gpsis to am-data for both).
    const std::string supi1 = "imsi-999700000000001";
    const std::string supi2 = "imsi-999700000000002";

    // GetSupiOrGpsi: SUPI-shaped ueId returns a real IdTranslationResult with the real seeded
    // gpsi attached.
    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = "https://127.0.0.1:7780/nudm-sdm/v2/" + supi1 + "/id-translation-result";
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        ASSERT_EQ(resp->status, 200) << resp->body;
        const auto result = json::parse(resp->body).get<sbi_gen::IdTranslationResult>();
        EXPECT_EQ(result.supi, supi1);
        ASSERT_TRUE(result.gpsi.has_value());
        EXPECT_EQ(*result.gpsi, "msisdn-9997000001");
    }

    // An unseeded SUPI-shaped ueId correctly 404s (fetch_from_udr's own real existence check).
    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = "https://127.0.0.1:7780/nudm-sdm/v2/imsi-999999999999999/id-translation-result";
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 404) << resp->body;
    }

    // A GPSI-shaped ueId correctly 404s -- real, disclosed gap: no reverse GPSI->SUPI lookup
    // capability exists in this build.
    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = "https://127.0.0.1:7780/nudm-sdm/v2/msisdn-9997000001/id-translation-result";
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 404) << resp->body;
    }

    // GetMultipleIdentifiers: supi-list with a mix of known SUPIs returns a real ueIdGpsiList
    // populated from real seeded gpsis for each.
    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = "https://127.0.0.1:7780/nudm-sdm/v2/multiple-identifiers?supi-list=" + supi1 +
                  "," + supi2;
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        ASSERT_EQ(resp->status, 200) << resp->body;
        const auto result = json::parse(resp->body).get<sbi_gen::UeIdentifiers>();
        ASSERT_TRUE(result.ueIdGpsiList.has_value());
        const auto& map = *result.ueIdGpsiList;
        ASSERT_TRUE(map.contains(supi1));
        EXPECT_EQ(map.at(supi1).at("gpsiList").at(0).get<std::string>(), "msisdn-9997000001");
        ASSERT_TRUE(map.contains(supi2));
        EXPECT_EQ(map.at(supi2).at("gpsiList").at(0).get<std::string>(), "msisdn-9997000002");
    }

    // GetMultipleIdentifiers with no params at all returns a real, honest empty result.
    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = "https://127.0.0.1:7780/nudm-sdm/v2/multiple-identifiers";
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        ASSERT_EQ(resp->status, 200) << resp->body;
        const auto result = json::parse(resp->body).get<sbi_gen::UeIdentifiers>();
        EXPECT_FALSE(result.ueIdGpsiList.has_value());
        EXPECT_FALSE(result.ueIdList.has_value());
    }
}
