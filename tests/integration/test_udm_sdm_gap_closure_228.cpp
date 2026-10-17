// Drives nrf, udr, and udm as real, separate OS processes to exercise UDM's Nudm_SDM group A+B
// gap-closure (ADR-0228, docs/CAPABILITY_GAP_ANALYSIS.md's own UDM audit): 4 real
// individual-resource GET ops backed by UDR's own individual provisioned-data routes
// (GetSmsData/GetSmsMngtData/GetTraceConfigData/GetLcsBcaData -- "group A"), and 9 real
// individual-resource GET ops backed by UDR's own bulk ProvisionedDataSets aggregate, with no
// individual UDR route of their own (GetLcsPrivacyData/GetLcsMoData/GetLcsSubscriptionData/
// GetV2xData/GetProseData/GetMbsData/GetUcData/GetA2xData/GetRangingSlPrivacyData -- "group B"),
// over real TLS 1.3 + mTLS HTTP/2 with a real signed OAuth2 token, per TS29503_Nudm_SDM.yaml.

#include "sbi_core/http2_client.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

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

struct Trio {
    nf_test::SpawnedProcess nrf;
    nf_test::SpawnedProcess udr;
    nf_test::SpawnedProcess udm;
};

Trio spawn_nrf_udr_udm() {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    nf_test::SpawnedProcess udr(UDR_PATH);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    nf_test::SpawnedProcess udm(UDM_PATH);
    return Trio{std::move(nrf), std::move(udr), std::move(udm)};
}

} // namespace

TEST(UdmSdmGapClosure228Integration, GroupAIndividualUdrRoutesReturnRealSeededData) {
    auto t = spawn_nrf_udr_udm();
    auto client = make_client();
    // Real, disclosed timing note (task #166's own class of issue): udm's port comes up fast and
    // independently of udr, so waiting on udm reachability alone isn't enough here -- this test's
    // very first real request needs udr already listening (udm proxies to it), so wait on udr's
    // own reachability explicitly too.
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7781/nudr-dr/v2/subscription-data/nonexistent", 50))
        << "udr never became reachable";
    ASSERT_TRUE(wait_reachable(
        client,
        "https://127.0.0.1:7780/nudm-uecm/v1/nonexistent/registrations/amf-3gpp-access",
        50))
        << "udm never became reachable";

    const std::string token = fetch_token(client, "nudm-sdm");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // Real seeded test subscriber (nfs/udr/src/main.cpp's own startup seed, ADR-0069/ADR-0125/
    // ADR-0126/ADR-0127/ADR-0106).
    const std::string supi = "imsi-999700000000001";
    const std::string base = "https://127.0.0.1:7780/nudm-sdm/v2/" + supi;

    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = base + "/sms-data";
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 200) << resp->body;
        if (resp->status == 200) {
            EXPECT_EQ(json::parse(resp->body).at("smsSubscribed").get<bool>(), true);
        }
    }
    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = base + "/sms-mng-data";
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 200) << resp->body;
        if (resp->status == 200) {
            EXPECT_EQ(json::parse(resp->body).at("mtSmsSubscribed").get<bool>(), true);
        }
    }
    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = base + "/trace-data";
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 200) << resp->body;
        if (resp->status == 200) {
            EXPECT_EQ(json::parse(resp->body).at("traceRef").get<std::string>(), "99970-A1B2C3");
        }
    }
    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = base + "/lcs-bca-data";
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 200) << resp->body;
        if (resp->status == 200) {
            EXPECT_EQ(json::parse(resp->body).at("locationAssistanceType").get<std::string>(),
                      "dGVzdA==");
        }
    }

    // A genuinely unseeded SUPI correctly 404s on each real UDR miss.
    for (const std::string path : {"sms-data", "sms-mng-data", "trace-data", "lcs-bca-data"}) {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = "https://127.0.0.1:7780/nudm-sdm/v2/imsi-999999999999999/" + path;
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value()) << path;
        EXPECT_EQ(resp->status, 404) << path;
    }
}

TEST(UdmSdmGapClosure228Integration, GroupBBulkExtractedFieldsReturnRealSeededData) {
    auto t = spawn_nrf_udr_udm();
    auto client = make_client();
    // See GroupAIndividualUdrRoutesReturnRealSeededData's own comment: udr must be waited on
    // explicitly, not just udm.
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7781/nudr-dr/v2/subscription-data/nonexistent", 50))
        << "udr never became reachable";
    ASSERT_TRUE(wait_reachable(
        client,
        "https://127.0.0.1:7780/nudm-uecm/v1/nonexistent/registrations/amf-3gpp-access",
        50))
        << "udm never became reachable";

    const std::string token = fetch_token(client, "nudm-sdm");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const std::string supi = "imsi-999700000000001";
    const std::string base = "https://127.0.0.1:7780/nudm-sdm/v2/" + supi;

    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = base + "/lcs-privacy-data";
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 200) << resp->body;
        if (resp->status == 200) {
            EXPECT_EQ(json::parse(resp->body).at("lpi").at("locationPrivacyInd").get<std::string>(),
                      "LOCATION_ALLOWED");
        }
    }
    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = base + "/lcs-mo-data";
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 200) << resp->body;
    }
    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = base + "/lcs-subscription-data";
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 200) << resp->body;
    }
    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = base + "/v2x-data";
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 200) << resp->body;
    }
    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = base + "/prose-data";
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 200) << resp->body;
    }
    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = base + "/5mbs-data";
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 200) << resp->body;
    }
    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = base + "/uc-data";
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 200) << resp->body;
    }
    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = base + "/a2x-data";
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 200) << resp->body;
    }
    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = base + "/rangingsl-privacy-data";
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 200) << resp->body;
    }

    // A genuinely unseeded SUPI correctly 404s: the UDR bulk aggregate itself always 200s
    // (ADR-0212, a live view), so this proves the real per-field-absence 404 in UDM's own
    // fetch_from_udr_bulk_field helper, not a UDR-level 404.
    for (const std::string path : {"lcs-privacy-data",
                                   "lcs-mo-data",
                                   "lcs-subscription-data",
                                   "v2x-data",
                                   "prose-data",
                                   "5mbs-data",
                                   "uc-data",
                                   "a2x-data",
                                   "rangingsl-privacy-data"}) {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = "https://127.0.0.1:7780/nudm-sdm/v2/imsi-999999999999999/" + path;
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value()) << path;
        EXPECT_EQ(resp->status, 404) << path;
    }
}
