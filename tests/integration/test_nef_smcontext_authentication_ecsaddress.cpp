// Drives nrf and nef as real, separate OS processes to exercise nef's Nnef_SMContext (TS29541),
// Nnef_Authentication (TS29256), and Nnef_ECSAddress (TS29591 + TS29522_ECSAddress.yaml) surfaces
// (ADR-0208, gap-closure task #164, second NEF slice) over real TLS 1.3 + mTLS HTTP/2 with a real
// signed OAuth2 token.

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

TEST(NefSMContextIntegration, CreateUpdateDeliverReleaseLifecycle) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(client, "https://127.0.0.1:7790/nnef-smcontext/v1/sm-contexts", 200))
        << "nef never became reachable";

    const std::string token = fetch_token(client, "nnef-smcontext");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const json create_body = json{
        {"supi", "imsi-999700000000401"},
        {"pduSessionId", 5},
        {"dnn", "internet"},
        {"snssai", json{{"sst", 1}, {"sd", "000001"}}},
        {"nefId", "nef-1"},
        {"dlNiddEndPoint", "https://example.com/nidd-dl"},
        {"notificationUri", "https://example.com/smcontext-notify"},
    };
    sbi_core::http2::ClientRequest create_req;
    create_req.method = "POST";
    create_req.url = "https://127.0.0.1:7790/nnef-smcontext/v1/sm-contexts";
    create_req.headers.emplace("content-type", "application/json");
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = create_body.dump();
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    EXPECT_EQ(create_resp->status, 201) << create_resp->body;

    std::string sm_context_id;
    if (create_resp->status == 201) {
        auto loc = create_resp->headers.find("location");
        EXPECT_NE(loc, create_resp->headers.end());
        if (loc != create_resp->headers.end()) {
            sm_context_id = loc->second.substr(loc->second.rfind('/') + 1);
        }
        const auto created = json::parse(create_resp->body);
        EXPECT_EQ(created.at("supi"), "imsi-999700000000401");
        EXPECT_EQ(created.at("nefId"), "nef-1");
    }
    ASSERT_FALSE(sm_context_id.empty());

    sbi_core::http2::ClientRequest update_req;
    update_req.method = "POST";
    update_req.url =
        "https://127.0.0.1:7790/nnef-smcontext/v1/sm-contexts/" + sm_context_id + "/update";
    update_req.headers.emplace("content-type", "application/json");
    update_req.headers.emplace("authorization", "Bearer " + token);
    update_req.body = json{{"notificationUri", "https://example.com/smcontext-notify-2"}}.dump();
    auto update_resp = client.send(update_req);
    ASSERT_TRUE(update_resp.has_value());
    EXPECT_EQ(update_resp->status, 204) << update_resp->body;

    sbi_core::multipart::Part json_part;
    json_part.content_type = "application/json";
    json_part.body = json{{"data", json{{"contentId", "binaryMoData"}}}}.dump();
    sbi_core::multipart::Part bin_part;
    bin_part.content_type = "application/octet-stream";
    bin_part.content_id = "binaryMoData";
    bin_part.body = "fake-nidd-mo-payload";
    const auto encoded = sbi_core::multipart::encode({json_part, bin_part});

    sbi_core::http2::ClientRequest deliver_req;
    deliver_req.method = "POST";
    deliver_req.url =
        "https://127.0.0.1:7790/nnef-smcontext/v1/sm-contexts/" + sm_context_id + "/deliver";
    deliver_req.headers.emplace("content-type", encoded.content_type_header);
    deliver_req.headers.emplace("authorization", "Bearer " + token);
    deliver_req.body = encoded.body;
    auto deliver_resp = client.send(deliver_req);
    ASSERT_TRUE(deliver_resp.has_value());
    EXPECT_EQ(deliver_resp->status, 204) << deliver_resp->body;

    sbi_core::http2::ClientRequest release_req;
    release_req.method = "POST";
    release_req.url =
        "https://127.0.0.1:7790/nnef-smcontext/v1/sm-contexts/" + sm_context_id + "/release";
    release_req.headers.emplace("content-type", "application/json");
    release_req.headers.emplace("authorization", "Bearer " + token);
    release_req.body = json{{"cause", "PDU_SESSION_RELEASED"}}.dump();
    auto release_resp = client.send(release_req);
    ASSERT_TRUE(release_resp.has_value());
    EXPECT_EQ(release_resp->status, 204) << release_resp->body;

    // Real, already-released resource: a second release attempt now 404s.
    auto release_again_resp = client.send(release_req);
    ASSERT_TRUE(release_again_resp.has_value());
    EXPECT_EQ(release_again_resp->status, 404);
}

TEST(NefSMContextIntegration, CreateWithMissingRequiredFieldIs400) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(client, "https://127.0.0.1:7790/nnef-smcontext/v1/sm-contexts", 200))
        << "nef never became reachable";

    const std::string token = fetch_token(client, "nnef-smcontext");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // dlNiddEndPoint and notificationUri are required and missing.
    const json bad_body = json{
        {"supi", "imsi-999700000000401"},
        {"pduSessionId", 5},
        {"dnn", "internet"},
        {"snssai", json{{"sst", 1}, {"sd", "000001"}}},
        {"nefId", "nef-1"},
    };
    sbi_core::http2::ClientRequest bad_req;
    bad_req.method = "POST";
    bad_req.url = "https://127.0.0.1:7790/nnef-smcontext/v1/sm-contexts";
    bad_req.headers.emplace("content-type", "application/json");
    bad_req.headers.emplace("authorization", "Bearer " + token);
    bad_req.body = bad_body.dump();
    auto bad_resp = client.send(bad_req);
    ASSERT_TRUE(bad_resp.has_value());
    EXPECT_EQ(bad_resp->status, 400);
}

TEST(NefAuthenticationIntegration, UAVAuthenticationReturnsRealHonest403NoBackend) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7790/nnef-authentication/v1/uav-authentications", 50))
        << "nef never became reachable";

    const std::string token = fetch_token(client, "nnef-authentication");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const json req_body = json{
        {"gpsi", "msisdn-15555550123"},
        {"serviceLevelId", "level-1"},
        {"nfType", "NEF"},
    };
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = "https://127.0.0.1:7790/nnef-authentication/v1/uav-authentications";
    req.headers.emplace("content-type", "application/json");
    req.headers.emplace("authorization", "Bearer " + token);
    req.body = req_body.dump();
    auto resp = client.send(req);
    ASSERT_TRUE(resp.has_value());
    // Real, disclosed gap (ADR-0208): no real UAS-NF/USS authentication backend exists in this
    // build; 403 UAVAuthFailure is the honest, real-spec-documented response, not a fabricated
    // 200 success.
    EXPECT_EQ(resp->status, 403) << resp->body;
    if (resp->status == 403) {
        const auto failure = json::parse(resp->body);
        EXPECT_EQ(failure.at("error").at("status"), 403);
    }
}

TEST(NefECSAddressIntegration, CreateReadPutPatchDeleteLifecycle) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7790/nnef-ecs-addr-cfg-info/v1/subscriptions/nonexistent", 50))
        << "nef never became reachable";

    const std::string token = fetch_token(client, "nnef-ecs-addr-cfg-info");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const json create_body = json{
        {"notifUri", "https://example.com/ecs-addr-notify"},
        {"notifCorrId", "corr-1"},
    };
    sbi_core::http2::ClientRequest create_req;
    create_req.method = "POST";
    create_req.url = "https://127.0.0.1:7790/nnef-ecs-addr-cfg-info/v1/subscriptions";
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
    get_req.url = "https://127.0.0.1:7790/nnef-ecs-addr-cfg-info/v1/subscriptions/" + sub_id;
    get_req.headers.emplace("authorization", "Bearer " + token);
    auto get_resp = client.send(get_req);
    ASSERT_TRUE(get_resp.has_value());
    EXPECT_EQ(get_resp->status, 200);

    const json put_body = json{
        {"notifUri", "https://example.com/ecs-addr-notify"},
        {"notifCorrId", "corr-2"},
    };
    sbi_core::http2::ClientRequest put_req;
    put_req.method = "PUT";
    put_req.url = "https://127.0.0.1:7790/nnef-ecs-addr-cfg-info/v1/subscriptions/" + sub_id;
    put_req.headers.emplace("content-type", "application/json");
    put_req.headers.emplace("authorization", "Bearer " + token);
    put_req.body = put_body.dump();
    auto put_resp = client.send(put_req);
    ASSERT_TRUE(put_resp.has_value());
    EXPECT_EQ(put_resp->status, 200) << put_resp->body;
    if (put_resp->status == 200) {
        EXPECT_EQ(json::parse(put_resp->body).at("notifCorrId"), "corr-2");
    }

    sbi_core::http2::ClientRequest patch_req;
    patch_req.method = "PATCH";
    patch_req.url = "https://127.0.0.1:7790/nnef-ecs-addr-cfg-info/v1/subscriptions/" + sub_id;
    patch_req.headers.emplace("content-type", "application/merge-patch+json");
    patch_req.headers.emplace("authorization", "Bearer " + token);
    patch_req.body = json{{"notifCorrId", "corr-3"}}.dump();
    auto patch_resp = client.send(patch_req);
    ASSERT_TRUE(patch_resp.has_value());
    EXPECT_EQ(patch_resp->status, 200) << patch_resp->body;
    if (patch_resp->status == 200) {
        const auto patched = json::parse(patch_resp->body);
        EXPECT_EQ(patched.at("notifCorrId"), "corr-3");
        // Real RFC 7396 merge-patch semantics: fields not named in the patch survive untouched.
        EXPECT_EQ(patched.at("notifUri"), "https://example.com/ecs-addr-notify");
    }

    sbi_core::http2::ClientRequest delete_req;
    delete_req.method = "DELETE";
    delete_req.url = "https://127.0.0.1:7790/nnef-ecs-addr-cfg-info/v1/subscriptions/" + sub_id;
    delete_req.headers.emplace("authorization", "Bearer " + token);
    auto delete_resp = client.send(delete_req);
    ASSERT_TRUE(delete_resp.has_value());
    EXPECT_EQ(delete_resp->status, 204);

    auto delete_again_resp = client.send(delete_req);
    ASSERT_TRUE(delete_again_resp.has_value());
    EXPECT_EQ(delete_again_resp->status, 404);
}

TEST(NefECSAddressIntegration, CreateWithMissingRequiredFieldIs400) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7790/nnef-ecs-addr-cfg-info/v1/subscriptions/nonexistent", 50))
        << "nef never became reachable";

    const std::string token = fetch_token(client, "nnef-ecs-addr-cfg-info");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // notifCorrId is required and missing.
    const json bad_body = json{{"notifUri", "https://example.com/ecs-addr-notify"}};
    sbi_core::http2::ClientRequest bad_req;
    bad_req.method = "POST";
    bad_req.url = "https://127.0.0.1:7790/nnef-ecs-addr-cfg-info/v1/subscriptions";
    bad_req.headers.emplace("content-type", "application/json");
    bad_req.headers.emplace("authorization", "Bearer " + token);
    bad_req.body = bad_body.dump();
    auto bad_resp = client.send(bad_req);
    ASSERT_TRUE(bad_resp.has_value());
    EXPECT_EQ(bad_resp->status, 400);
}
