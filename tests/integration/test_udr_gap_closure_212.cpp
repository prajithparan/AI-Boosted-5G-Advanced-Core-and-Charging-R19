// Drives nrf and udr as real, separate OS processes to exercise UDR's own Tier-B gap-closure
// operations (ADR-0212, docs/CAPABILITY_GAP_ANALYSIS.md's own UDR audit): the Individual
// Authentication Status (Document) resource (CreateIndividualAuthenticationStatus/
// QueryIndividualAuthenticationStatus/DeleteIndividualAuthenticationStatus) and the Provisioned
// Data (Document) aggregate resource (QueryProvisionedData) -- over real TLS 1.3 + mTLS HTTP/2
// with a real signed OAuth2 token, per TS29505_Subscription_Data.yaml.

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

std::string fetch_token(sbi_core::http2::Client& client) {
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = "https://127.0.0.1:7777/oauth2/token";
    req.headers.emplace("content-type", "application/x-www-form-urlencoded");
    req.body =
        "grant_type=client_credentials&nfInstanceId=" + std::string(nf_test::kTestAfInstanceId) +
        "&scope=nudr-dr&"
        "targetNfType=UDR";
    auto resp = nf_test::send_token_request(req);
    if (!resp.has_value() || resp->status != 200) {
        return "";
    }
    return json::parse(resp->body).at("access_token").get<std::string>();
}

struct Duo {
    nf_test::SpawnedProcess nrf;
    nf_test::SpawnedProcess udr;
};

Duo spawn_nrf_udr() {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    nf_test::SpawnedProcess udr(UDR_PATH);
    return Duo{std::move(nrf), std::move(udr)};
}

} // namespace

TEST(UdrGapClosureIntegration, IndividualAuthenticationStatusLifecycle) {
    auto d = spawn_nrf_udr();
    auto client = make_client();
    const std::string base_url =
        "https://127.0.0.1:7781/nudr-dr/v2/subscription-data/imsi-999700000000001/"
        "authentication-data/authentication-status/5G:mnc070.mcc999.3gppnetwork.org";
    ASSERT_TRUE(wait_reachable(client, base_url, 200)) << "udr never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // GET before PUT: real 404, no individual authentication status exists yet.
    sbi_core::http2::ClientRequest get_before_req;
    get_before_req.method = "GET";
    get_before_req.url = base_url;
    get_before_req.headers.emplace("authorization", "Bearer " + token);
    auto get_before_resp = client.send(get_before_req);
    ASSERT_TRUE(get_before_resp.has_value());
    EXPECT_EQ(get_before_resp->status, 404);

    // PUT: real AuthEvent body (TS29503_Nudm_UEAU.yaml).
    const json auth_event = json{
        {"nfInstanceId", "00000000-0000-4000-8000-000000000bbb"},
        {"success", true},
        {"timeStamp", "2026-08-25T00:00:00Z"},
        {"authType", "5G_AKA"},
        {"servingNetworkName", "5G:mnc070.mcc999.3gppnetwork.org"},
    };
    sbi_core::http2::ClientRequest put_req;
    put_req.method = "PUT";
    put_req.url = base_url;
    put_req.headers.emplace("content-type", "application/json");
    put_req.headers.emplace("authorization", "Bearer " + token);
    put_req.body = auth_event.dump();
    auto put_resp = client.send(put_req);
    ASSERT_TRUE(put_resp.has_value());
    EXPECT_EQ(put_resp->status, 204) << put_resp->body;

    // GET after PUT: real 200, real round-tripped AuthEvent.
    sbi_core::http2::ClientRequest get_req;
    get_req.method = "GET";
    get_req.url = base_url;
    get_req.headers.emplace("authorization", "Bearer " + token);
    auto get_resp = client.send(get_req);
    ASSERT_TRUE(get_resp.has_value());
    EXPECT_EQ(get_resp->status, 200) << get_resp->body;
    if (get_resp->status == 200) {
        const auto got = json::parse(get_resp->body);
        EXPECT_EQ(got.at("authType").get<std::string>(), "5G_AKA");
        EXPECT_EQ(got.at("success").get<bool>(), true);
    }

    // The bare, ueId-only authentication-status resource stays independent -- real, disclosed
    // distinct-key precedent this route's own header comment describes.
    sbi_core::http2::ClientRequest bare_get_req;
    bare_get_req.method = "GET";
    bare_get_req.url = "https://127.0.0.1:7781/nudr-dr/v2/subscription-data/"
                       "imsi-999700000000001/authentication-data/authentication-status";
    bare_get_req.headers.emplace("authorization", "Bearer " + token);
    auto bare_get_resp = client.send(bare_get_req);
    ASSERT_TRUE(bare_get_resp.has_value());
    EXPECT_EQ(bare_get_resp->status, 404)
        << "bare authentication-status must stay independent of the individual resource";

    // DELETE: real 204.
    sbi_core::http2::ClientRequest delete_req;
    delete_req.method = "DELETE";
    delete_req.url = base_url;
    delete_req.headers.emplace("authorization", "Bearer " + token);
    auto delete_resp = client.send(delete_req);
    ASSERT_TRUE(delete_resp.has_value());
    EXPECT_EQ(delete_resp->status, 204);

    // GET after DELETE: real 404 again.
    auto get_after_delete_resp = client.send(get_req);
    ASSERT_TRUE(get_after_delete_resp.has_value());
    EXPECT_EQ(get_after_delete_resp->status, 404);
}

TEST(UdrGapClosureIntegration, QueryProvisionedDataComposesAllSeededFields) {
    auto d = spawn_nrf_udr();
    auto client = make_client();
    const std::string base_url =
        "https://127.0.0.1:7781/nudr-dr/v2/subscription-data/imsi-999700000000001/99970/"
        "provisioned-data";
    ASSERT_TRUE(wait_reachable(client, base_url, 200)) << "udr never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // No filter: every seeded ProvisionedDataSets field (nfs/udr/src/main.cpp's own startup seed,
    // both the 7 PLMN-keyed and the non-PLMN-keyed ones) should be present.
    sbi_core::http2::ClientRequest req;
    req.method = "GET";
    req.url = base_url;
    req.headers.emplace("authorization", "Bearer " + token);
    auto resp = client.send(req);
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->status, 200) << resp->body;
    if (resp->status == 200) {
        const auto got = json::parse(resp->body);
        // PLMN-keyed (servingPlmnId is REQUIRED here -- always composed, unlike the bare
        // QueryUeSubscribedData aggregate where it's an optional query param).
        EXPECT_TRUE(got.contains("amData"));
        EXPECT_TRUE(got.contains("smData"));
        EXPECT_TRUE(got.contains("lcsBcaData"));
        EXPECT_TRUE(got.contains("smsMngData"));
        EXPECT_TRUE(got.contains("smsSubsData"));
        EXPECT_TRUE(got.contains("traceData"));
        // Non-PLMN-keyed, same seeded UE.
        EXPECT_TRUE(got.contains("lcsPrivacyData"));
        EXPECT_TRUE(got.contains("v2xData"));
        // Real, disclosed gap: niddAuthData is never composed here (needs
        // mtc-provider-information, not exposed by this resource).
        EXPECT_FALSE(got.contains("niddAuthData"));
    }

    // Real, optional dataset-names filter: AM only.
    sbi_core::http2::ClientRequest filtered_req;
    filtered_req.method = "GET";
    filtered_req.url = base_url + "?dataset-names=AM";
    filtered_req.headers.emplace("authorization", "Bearer " + token);
    auto filtered_resp = client.send(filtered_req);
    ASSERT_TRUE(filtered_resp.has_value());
    EXPECT_EQ(filtered_resp->status, 200) << filtered_resp->body;
    if (filtered_resp->status == 200) {
        const auto got = json::parse(filtered_resp->body);
        EXPECT_TRUE(got.contains("amData"));
        EXPECT_FALSE(got.contains("smData"));
        EXPECT_FALSE(got.contains("lcsPrivacyData"));
    }

    // Unknown UE: real 200 {} (live view, no independent existence -- same precedent as
    // QueryUeSubscribedData/QueryContextData).
    sbi_core::http2::ClientRequest empty_req;
    empty_req.method = "GET";
    empty_req.url = "https://127.0.0.1:7781/nudr-dr/v2/subscription-data/"
                    "imsi-999700000099999/99970/provisioned-data";
    empty_req.headers.emplace("authorization", "Bearer " + token);
    auto empty_resp = client.send(empty_req);
    ASSERT_TRUE(empty_resp.has_value());
    EXPECT_EQ(empty_resp->status, 200) << empty_resp->body;
    if (empty_resp->status == 200) {
        EXPECT_TRUE(json::parse(empty_resp->body).empty());
    }
}
