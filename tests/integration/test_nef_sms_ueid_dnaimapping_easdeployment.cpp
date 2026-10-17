// Drives nrf and nef as real, separate OS processes to exercise nef's Nnef_SMService (TS29541),
// Nnef_UEId (TS29591), Nnef_DNAIMapping (TS29591 + TS29522_DNAIMapping.yaml), and
// Nnef_EASDeployment (TS29591) surfaces (ADR-0207, gap-closure task #164, first NEF slice) over
// real TLS 1.3 + mTLS HTTP/2 with a real signed OAuth2 token.

#include "sbi_core/http2_client.hpp"
#include "sbi_core/multipart.hpp"

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
        "&scope=" + scope + "&targetNfType=NEF";
    auto resp = nf_test::send_token_request(req);
    if (!resp.has_value() || resp->status != 200) {
        return "";
    }
    return json::parse(resp->body).at("access_token").get<std::string>();
}

struct Duo {
    nf_test::SpawnedProcess nrf;
    nf_test::SpawnedProcess nef;
};

Duo spawn_all() {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    nf_test::SpawnedProcess nef(NEF_PATH);
    return Duo{std::move(nrf), std::move(nef)};
}

} // namespace

TEST(NefSMServiceIntegration, SendSMSReturnsRealMultipartDeliveryReport) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7790/nnef-smservice/v1/sm-contexts/nonexistent/sendsms", 50))
        << "nef never became reachable";

    const std::string token = fetch_token(client, "nnef-smcontext");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    sbi_core::multipart::Part json_part;
    json_part.content_type = "application/json";
    json_part.body = json{{"smsPayload", json{{"contentId", "smsPayload"}}}}.dump();
    sbi_core::multipart::Part bin_part;
    bin_part.content_type = "application/vnd.3gpp.sms";
    bin_part.content_id = "smsPayload";
    bin_part.body = "fake-sms-pdu-bytes";
    const auto encoded = sbi_core::multipart::encode({json_part, bin_part});

    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = "https://127.0.0.1:7790/nnef-smservice/v1/sm-contexts/imsi-999700000000401/sendsms";
    req.headers.emplace("content-type", encoded.content_type_header);
    req.headers.emplace("authorization", "Bearer " + token);
    req.body = encoded.body;
    auto resp = client.send(req);
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->status, 200) << resp->body;
    if (resp->status == 200) {
        auto content_type_it = resp->headers.find("content-type");
        ASSERT_NE(content_type_it, resp->headers.end());
        auto parts = sbi_core::multipart::parse(content_type_it->second, resp->body);
        ASSERT_TRUE(parts.has_value());
        ASSERT_FALSE(parts->empty());
        const auto delivery = json::parse((*parts)[0].body);
        EXPECT_EQ(delivery.at("smsPayload").at("contentId"), "smsPayload");
    }
}

TEST(NefUEIdIntegration, FetchAndMappingBothReturnRealHonestNoContent) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(client, "https://127.0.0.1:7790/nnef-ueid/v1/fetch", 200))
        << "nef never became reachable";

    const std::string token = fetch_token(client, "nnef-ueid");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    sbi_core::http2::ClientRequest fetch_req;
    fetch_req.method = "POST";
    fetch_req.url = "https://127.0.0.1:7790/nnef-ueid/v1/fetch";
    fetch_req.headers.emplace("content-type", "application/json");
    fetch_req.headers.emplace("authorization", "Bearer " + token);
    fetch_req.body = json{{"gpsi", "msisdn-15555550123"}}.dump();
    auto fetch_resp = client.send(fetch_req);
    ASSERT_TRUE(fetch_resp.has_value());
    // Real, disclosed gap (ADR-0207): no real roaming H-NEF/internal-UE-identity mapping database
    // exists in this build, so this is an honest "does not exist", not a fabricated match.
    EXPECT_EQ(fetch_resp->status, 204);

    sbi_core::http2::ClientRequest mapping_req;
    mapping_req.method = "POST";
    mapping_req.url = "https://127.0.0.1:7790/nnef-ueid/v1/get-ueid-mapping";
    mapping_req.headers.emplace("content-type", "application/json");
    mapping_req.headers.emplace("authorization", "Bearer " + token);
    mapping_req.body = json{{"appLayerId", "app-layer-id-1"}}.dump();
    auto mapping_resp = client.send(mapping_req);
    ASSERT_TRUE(mapping_resp.has_value());
    EXPECT_EQ(mapping_resp->status, 204);
}

TEST(NefDNAIMappingIntegration, CreateReadDeleteLifecycle) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7790/nnef-dnai-mapping/v1/subscriptions/nonexistent", 50))
        << "nef never became reachable";

    const std::string token = fetch_token(client, "nnef-dnai-mapping");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const json create_body = json{
        {"notifUri", "https://example.com/dnai-map-notify"},
        {"notifCorrId", "corr-1"},
        {"fqdns", json::array({"video.example.com"})},
    };
    sbi_core::http2::ClientRequest create_req;
    create_req.method = "POST";
    create_req.url = "https://127.0.0.1:7790/nnef-dnai-mapping/v1/subscriptions";
    create_req.headers.emplace("content-type", "application/json");
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = create_body.dump();
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    EXPECT_EQ(create_resp->status, 201) << create_resp->body;

    std::string sub_id;
    if (create_resp->status == 201) {
        auto loc = create_resp->headers.find("location");
        EXPECT_NE(loc, create_resp->headers.end());
        if (loc != create_resp->headers.end()) {
            sub_id = loc->second.substr(loc->second.rfind('/') + 1);
        }
        const auto created = json::parse(create_resp->body);
        EXPECT_EQ(created.at("notifCorrId"), "corr-1");
    }
    ASSERT_FALSE(sub_id.empty());

    sbi_core::http2::ClientRequest get_req;
    get_req.method = "GET";
    get_req.url = "https://127.0.0.1:7790/nnef-dnai-mapping/v1/subscriptions/" + sub_id;
    get_req.headers.emplace("authorization", "Bearer " + token);
    auto get_resp = client.send(get_req);
    ASSERT_TRUE(get_resp.has_value());
    EXPECT_EQ(get_resp->status, 200);

    sbi_core::http2::ClientRequest delete_req;
    delete_req.method = "DELETE";
    delete_req.url = "https://127.0.0.1:7790/nnef-dnai-mapping/v1/subscriptions/" + sub_id;
    delete_req.headers.emplace("authorization", "Bearer " + token);
    auto delete_resp = client.send(delete_req);
    ASSERT_TRUE(delete_resp.has_value());
    EXPECT_EQ(delete_resp->status, 204);

    auto delete_again_resp = client.send(delete_req);
    ASSERT_TRUE(delete_again_resp.has_value());
    EXPECT_EQ(delete_again_resp->status, 404);
}

TEST(NefDNAIMappingIntegration, CreateWithMissingRequiredFieldIs400) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7790/nnef-dnai-mapping/v1/subscriptions/nonexistent", 50))
        << "nef never became reachable";

    const std::string token = fetch_token(client, "nnef-dnai-mapping");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // notifCorrId is required and missing.
    const json bad_body = json{{"notifUri", "https://example.com/dnai-map-notify"}};
    sbi_core::http2::ClientRequest bad_req;
    bad_req.method = "POST";
    bad_req.url = "https://127.0.0.1:7790/nnef-dnai-mapping/v1/subscriptions";
    bad_req.headers.emplace("content-type", "application/json");
    bad_req.headers.emplace("authorization", "Bearer " + token);
    bad_req.body = bad_body.dump();
    auto bad_resp = client.send(bad_req);
    ASSERT_TRUE(bad_resp.has_value());
    EXPECT_EQ(bad_resp->status, 400);
}

TEST(NefEASDeploymentIntegration, CreateReadDeleteLifecycle) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7790/nnef-eas-deployment/v1/subscriptions/nonexistent", 50))
        << "nef never became reachable";

    const std::string token = fetch_token(client, "nnef-eas-deployment");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const json create_body = json{
        {"eventId", "EAS_INFO_CHG"},
        {"notifId", "notif-1"},
        {"notifUri", "https://example.com/eas-deploy-notify"},
    };
    sbi_core::http2::ClientRequest create_req;
    create_req.method = "POST";
    create_req.url = "https://127.0.0.1:7790/nnef-eas-deployment/v1/subscriptions";
    create_req.headers.emplace("content-type", "application/json");
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = create_body.dump();
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    EXPECT_EQ(create_resp->status, 201) << create_resp->body;

    std::string sub_id;
    if (create_resp->status == 201) {
        auto loc = create_resp->headers.find("location");
        EXPECT_NE(loc, create_resp->headers.end());
        if (loc != create_resp->headers.end()) {
            sub_id = loc->second.substr(loc->second.rfind('/') + 1);
        }
        const auto created = json::parse(create_resp->body);
        EXPECT_EQ(created.at("notifId"), "notif-1");
    }
    ASSERT_FALSE(sub_id.empty());

    sbi_core::http2::ClientRequest get_req;
    get_req.method = "GET";
    get_req.url = "https://127.0.0.1:7790/nnef-eas-deployment/v1/subscriptions/" + sub_id;
    get_req.headers.emplace("authorization", "Bearer " + token);
    auto get_resp = client.send(get_req);
    ASSERT_TRUE(get_resp.has_value());
    EXPECT_EQ(get_resp->status, 200);

    sbi_core::http2::ClientRequest delete_req;
    delete_req.method = "DELETE";
    delete_req.url = "https://127.0.0.1:7790/nnef-eas-deployment/v1/subscriptions/" + sub_id;
    delete_req.headers.emplace("authorization", "Bearer " + token);
    auto delete_resp = client.send(delete_req);
    ASSERT_TRUE(delete_resp.has_value());
    EXPECT_EQ(delete_resp->status, 204);

    auto delete_again_resp = client.send(delete_req);
    ASSERT_TRUE(delete_again_resp.has_value());
    EXPECT_EQ(delete_again_resp->status, 404);
}

TEST(NefEASDeploymentIntegration, CreateWithMissingRequiredFieldIs400) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7790/nnef-eas-deployment/v1/subscriptions/nonexistent", 50))
        << "nef never became reachable";

    const std::string token = fetch_token(client, "nnef-eas-deployment");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // notifId is required and missing.
    const json bad_body =
        json{{"eventId", "EAS_INFO_CHG"}, {"notifUri", "https://example.com/eas-deploy-notify"}};
    sbi_core::http2::ClientRequest bad_req;
    bad_req.method = "POST";
    bad_req.url = "https://127.0.0.1:7790/nnef-eas-deployment/v1/subscriptions";
    bad_req.headers.emplace("content-type", "application/json");
    bad_req.headers.emplace("authorization", "Bearer " + token);
    bad_req.body = bad_body.dump();
    auto bad_resp = client.send(bad_req);
    ASSERT_TRUE(bad_resp.has_value());
    EXPECT_EQ(bad_resp->status, 400);
}
