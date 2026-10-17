// Drives nrf and udm as real, separate OS processes to exercise UDM's own Nudm_UECM IP-SM-GW
// registration resource (ADR-0218, docs/CAPABILITY_GAP_ANALYSIS.md's own UDM audit):
// IpSmGwRegistration/GetIpSmGwRegistration/IpSmGwDeregistration -- over real TLS 1.3 + mTLS
// HTTP/2 with a real signed OAuth2 token, per TS29503_Nudm_UECM.yaml.

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

TEST(UdmIpSmGwGapClosureIntegration, IpSmGwRegistrationFullLifecycle) {
    auto d = spawn_nrf_udm();
    auto client = make_client();
    const std::string ue_id = "imsi-999700000000001";
    const std::string url =
        "https://127.0.0.1:7780/nudm-uecm/v1/" + ue_id + "/registrations/ip-sm-gw";
    ASSERT_TRUE(wait_reachable(client, url, 200)) << "udm never became reachable";

    const std::string token = fetch_token(client, "nudm-uecm");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    sbi_core::http2::ClientRequest get_req;
    get_req.method = "GET";
    get_req.url = url;
    get_req.headers.emplace("authorization", "Bearer " + token);
    auto get_before_resp = client.send(get_req);
    ASSERT_TRUE(get_before_resp.has_value());
    EXPECT_EQ(get_before_resp->status, 404);

    sbi_gen::IpSmGwRegistration create_data{};
    create_data.ipsmgwFqdn = "ipsmgw.example.com";

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

    auto update_resp = client.send(create_req);
    ASSERT_TRUE(update_resp.has_value());
    EXPECT_EQ(update_resp->status, 200) << update_resp->body;

    auto get_resp = client.send(get_req);
    ASSERT_TRUE(get_resp.has_value());
    EXPECT_EQ(get_resp->status, 200) << get_resp->body;
    if (get_resp->status == 200) {
        EXPECT_EQ(json::parse(get_resp->body).at("ipsmgwFqdn").get<std::string>(),
                  "ipsmgw.example.com");
    }

    sbi_core::http2::ClientRequest delete_req;
    delete_req.method = "DELETE";
    delete_req.url = url;
    delete_req.headers.emplace("authorization", "Bearer " + token);
    auto delete_resp = client.send(delete_req);
    ASSERT_TRUE(delete_resp.has_value());
    EXPECT_EQ(delete_resp->status, 204);

    auto get_after_delete_resp = client.send(get_req);
    ASSERT_TRUE(get_after_delete_resp.has_value());
    EXPECT_EQ(get_after_delete_resp->status, 404);

    auto delete_again_resp = client.send(delete_req);
    ASSERT_TRUE(delete_again_resp.has_value());
    EXPECT_EQ(delete_again_resp->status, 404);
}
