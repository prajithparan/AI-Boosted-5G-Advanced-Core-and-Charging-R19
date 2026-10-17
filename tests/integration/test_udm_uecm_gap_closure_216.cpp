// Drives nrf and udm as real, separate OS processes to exercise UDM's own Nudm_UECM AMF
// non-3GPP-access registration group (ADR-0216, docs/CAPABILITY_GAP_ANALYSIS.md's own UDM audit):
// Non3GppRegistration, GetNon3GppRegistration, UpdateNon3GppRegistration -- over real TLS 1.3 +
// mTLS HTTP/2 with a real signed OAuth2 token, per TS29503_Nudm_UECM.yaml.

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

TEST(UdmAmfNon3GppGapClosureIntegration, Non3GppRegistrationFullLifecycle) {
    auto d = spawn_nrf_udm();
    auto client = make_client();
    const std::string ue_id = "imsi-999700000000001";
    const std::string url =
        "https://127.0.0.1:7780/nudm-uecm/v1/" + ue_id + "/registrations/amf-non-3gpp-access";
    ASSERT_TRUE(wait_reachable(client, url, 200)) << "udm never became reachable";

    const std::string token = fetch_token(client, "nudm-uecm");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // GET before create: real 404.
    sbi_core::http2::ClientRequest get_before_req;
    get_before_req.method = "GET";
    get_before_req.url = url;
    get_before_req.headers.emplace("authorization", "Bearer " + token);
    auto get_before_resp = client.send(get_before_req);
    ASSERT_TRUE(get_before_resp.has_value());
    EXPECT_EQ(get_before_resp->status, 404);

    // Real create.
    sbi_gen::AmfNon3GppAccessRegistration create_data{};
    create_data.amfInstanceId = "00000000-0000-4000-8000-000000000ccc";
    create_data.imsVoPs.value = sbi_gen::ImsVoPs::HOMOGENEOUS_SUPPORT;
    create_data.deregCallbackUri = "https://example.com/dereg-non3gpp";
    create_data.guami.plmnId.mcc = "999";
    create_data.guami.plmnId.mnc = "70";
    create_data.guami.amfId = "ABCDEF";
    create_data.ratType = "WLAN"; // RatType is an opaque nlohmann::json fallback, not a struct

    sbi_core::http2::ClientRequest create_req;
    create_req.method = "PUT";
    create_req.url = url;
    create_req.headers.emplace("content-type", "application/json");
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = json(create_data).dump();
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    EXPECT_EQ(create_resp->status, 201) << create_resp->body;
    EXPECT_NE(create_resp->headers.find("location"), create_resp->headers.end());

    // Real re-PUT (update): real 200, not 201.
    auto update_resp = client.send(create_req);
    ASSERT_TRUE(update_resp.has_value());
    EXPECT_EQ(update_resp->status, 200) << update_resp->body;

    // Real GET after create.
    auto get_resp = client.send(get_before_req);
    ASSERT_TRUE(get_resp.has_value());
    EXPECT_EQ(get_resp->status, 200) << get_resp->body;
    if (get_resp->status == 200) {
        const auto got = json::parse(get_resp->body);
        EXPECT_EQ(got.at("guami").at("amfId").get<std::string>(), "ABCDEF");
        EXPECT_EQ(got.at("imsVoPs").get<std::string>(), "HOMOGENEOUS_SUPPORT");
    }

    // Real PATCH (RFC 7396 merge-patch+json): real 204, no body.
    sbi_gen::AmfNon3GppAccessRegistrationModification patch{};
    patch.guami.plmnId.mcc = "999";
    patch.guami.plmnId.mnc = "70";
    patch.guami.amfId = "FEDCBA";
    sbi_core::http2::ClientRequest patch_req;
    patch_req.method = "PATCH";
    patch_req.url = url;
    patch_req.headers.emplace("content-type", "application/merge-patch+json");
    patch_req.headers.emplace("authorization", "Bearer " + token);
    patch_req.body = json(patch).dump();
    auto patch_resp = client.send(patch_req);
    ASSERT_TRUE(patch_resp.has_value());
    EXPECT_EQ(patch_resp->status, 204) << patch_resp->body;

    // Confirm the real merge landed.
    auto get_after_patch_resp = client.send(get_before_req);
    ASSERT_TRUE(get_after_patch_resp.has_value());
    EXPECT_EQ(get_after_patch_resp->status, 200) << get_after_patch_resp->body;
    if (get_after_patch_resp->status == 200) {
        const auto got = json::parse(get_after_patch_resp->body);
        EXPECT_EQ(got.at("guami").at("amfId").get<std::string>(), "FEDCBA");
        // Real, disclosed: merge, not replace -- fields not in the patch survive.
        EXPECT_EQ(got.at("deregCallbackUri").get<std::string>(),
                  "https://example.com/dereg-non3gpp");
    }

    // PATCH against an unknown ueId: real 404.
    sbi_core::http2::ClientRequest patch_missing_req;
    patch_missing_req.method = "PATCH";
    patch_missing_req.url = "https://127.0.0.1:7780/nudm-uecm/v1/imsi-999700000099999/"
                            "registrations/amf-non-3gpp-access";
    patch_missing_req.headers.emplace("content-type", "application/merge-patch+json");
    patch_missing_req.headers.emplace("authorization", "Bearer " + token);
    patch_missing_req.body = json(patch).dump();
    auto patch_missing_resp = client.send(patch_missing_req);
    ASSERT_TRUE(patch_missing_resp.has_value());
    EXPECT_EQ(patch_missing_resp->status, 404);
}
