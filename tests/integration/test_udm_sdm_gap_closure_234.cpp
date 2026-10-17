// Drives nrf/udr/udm as real, separate OS processes to exercise UDM's Nudm_SDM ack-ops
// gap-closure (ADR-0234, docs/CAPABILITY_GAP_ANALYSIS.md's own UDM audit): SorAckInfo, UpuAck,
// `S-NSSAIs Ack`, `CAG Ack` -- 4 real, structurally identical accept-and-validate PUT ops under
// /{supi}/am-data/*-ack, per TS29503_Nudm_SDM.yaml.

#include "sbi_core/http2_client.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

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

TEST(UdmSdmGapClosure234Integration, AckOpsReturn204ForKnownUeAnd404ForUnknown) {
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

    // Real seeded test subscriber (nfs/udr/src/main.cpp's own startup seed, ADR-0069) -- has real
    // am-data, so the existence probe these ack ops share with GetAmData/Nudm_MT succeeds.
    const std::string supi = "imsi-999700000000001";
    const json ack_body = {{"provisioningTime", "2026-01-01T00:00:00Z"}};

    for (const std::string segment : {"sor-ack", "upu-ack", "subscribed-snssais-ack", "cag-ack"}) {
        sbi_core::http2::ClientRequest req;
        req.method = "PUT";
        req.url = "https://127.0.0.1:7780/nudm-sdm/v2/" + supi + "/am-data/" + segment;
        req.headers.emplace("content-type", "application/json");
        req.headers.emplace("authorization", "Bearer " + token);
        req.body = ack_body.dump();
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value()) << segment;
        EXPECT_EQ(resp->status, 204) << segment << ": " << resp->body;
    }

    // A genuinely unseeded SUPI (no am-data) correctly 404s on all 4.
    for (const std::string segment : {"sor-ack", "upu-ack", "subscribed-snssais-ack", "cag-ack"}) {
        sbi_core::http2::ClientRequest req;
        req.method = "PUT";
        req.url = "https://127.0.0.1:7780/nudm-sdm/v2/imsi-999999999999999/am-data/" + segment;
        req.headers.emplace("content-type", "application/json");
        req.headers.emplace("authorization", "Bearer " + token);
        req.body = ack_body.dump();
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value()) << segment;
        EXPECT_EQ(resp->status, 404) << segment << ": " << resp->body;
    }

    // A real, malformed body (missing required provisioningTime) correctly 400s.
    {
        sbi_core::http2::ClientRequest req;
        req.method = "PUT";
        req.url = "https://127.0.0.1:7780/nudm-sdm/v2/" + supi + "/am-data/sor-ack";
        req.headers.emplace("content-type", "application/json");
        req.headers.emplace("authorization", "Bearer " + token);
        req.body = json::object().dump();
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 400) << resp->body;
    }
}
