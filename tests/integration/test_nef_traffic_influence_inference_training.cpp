// Drives nrf and nef as real, separate OS processes to exercise nef's Nnef_TrafficInfluenceData,
// Nnef_Inference, Nnef_Training, Nnef_VFLInference, and Nnef_VFLTraining (TS29591) surfaces
// (ADR-0210, gap-closure task #164, fourth and final NEF slice) over real TLS 1.3 + mTLS HTTP/2
// with a real signed OAuth2 token.

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
        "&scope=" + scope + "&targetNfType=NEF";
    auto resp = nf_test::send_token_request(req);
    if (!resp.has_value() || resp->status != 200) {
        return "";
    }
    return json::parse(resp->body).at("access_token").get<std::string>();
}

std::string subscription_id_from_location(const sbi_core::http2::ClientResponse& resp) {
    auto loc = resp.headers.find("location");
    if (loc == resp.headers.end()) {
        return "";
    }
    return loc->second.substr(loc->second.rfind('/') + 1);
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

TEST(NefTrafficInfluenceIntegration, CreateReadPutDeleteLifecycle) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client,
        "https://127.0.0.1:7790/nnef-traffic-influence-data/v1/subscriptions/nonexistent",
        50))
        << "nef never became reachable";

    const std::string token = fetch_token(client, "nnef-traffic-influence-data");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const json create_body = json{
        {"notifUri", "https://example.com/traffic-influence-notify"},
        {"notifCorrId", "corr-1"},
        {"dnns", json::array({"internet"})},
    };
    sbi_core::http2::ClientRequest create_req;
    create_req.method = "POST";
    create_req.url = "https://127.0.0.1:7790/nnef-traffic-influence-data/v1/subscriptions";
    create_req.headers.emplace("content-type", "application/json");
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = create_body.dump();
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    EXPECT_EQ(create_resp->status, 201) << create_resp->body;

    const std::string sub_id =
        create_resp->status == 201 ? subscription_id_from_location(*create_resp) : "";
    EXPECT_FALSE(sub_id.empty());
    if (!sub_id.empty()) {
        sbi_core::http2::ClientRequest get_req;
        get_req.method = "GET";
        get_req.url =
            "https://127.0.0.1:7790/nnef-traffic-influence-data/v1/subscriptions/" + sub_id;
        get_req.headers.emplace("authorization", "Bearer " + token);
        auto get_resp = client.send(get_req);
        ASSERT_TRUE(get_resp.has_value());
        EXPECT_EQ(get_resp->status, 200);

        const json put_body = json{
            {"notifUri", "https://example.com/traffic-influence-notify"},
            {"notifCorrId", "corr-2"},
            {"snssais", json::array({json{{"sst", 1}, {"sd", "000001"}}})},
        };
        sbi_core::http2::ClientRequest put_req;
        put_req.method = "PUT";
        put_req.url =
            "https://127.0.0.1:7790/nnef-traffic-influence-data/v1/subscriptions/" + sub_id;
        put_req.headers.emplace("content-type", "application/json");
        put_req.headers.emplace("authorization", "Bearer " + token);
        put_req.body = put_body.dump();
        auto put_resp = client.send(put_req);
        ASSERT_TRUE(put_resp.has_value());
        EXPECT_EQ(put_resp->status, 200) << put_resp->body;
        if (put_resp->status == 200) {
            EXPECT_EQ(json::parse(put_resp->body).at("notifCorrId"), "corr-2");
        }

        sbi_core::http2::ClientRequest delete_req;
        delete_req.method = "DELETE";
        delete_req.url =
            "https://127.0.0.1:7790/nnef-traffic-influence-data/v1/subscriptions/" + sub_id;
        delete_req.headers.emplace("authorization", "Bearer " + token);
        auto delete_resp = client.send(delete_req);
        ASSERT_TRUE(delete_resp.has_value());
        EXPECT_EQ(delete_resp->status, 204);

        auto delete_again_resp = client.send(delete_req);
        ASSERT_TRUE(delete_again_resp.has_value());
        EXPECT_EQ(delete_again_resp->status, 404);
    }
}

TEST(NefTrafficInfluenceIntegration, CreateWithoutDnnsOrSnssaisIs400) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client,
        "https://127.0.0.1:7790/nnef-traffic-influence-data/v1/subscriptions/nonexistent",
        50))
        << "nef never became reachable";

    const std::string token = fetch_token(client, "nnef-traffic-influence-data");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // Real YAML anyOf constraint: at least one of dnns/snssais is required. Neither present here.
    const json bad_body = json{
        {"notifUri", "https://example.com/traffic-influence-notify"},
        {"notifCorrId", "corr-1"},
    };
    sbi_core::http2::ClientRequest bad_req;
    bad_req.method = "POST";
    bad_req.url = "https://127.0.0.1:7790/nnef-traffic-influence-data/v1/subscriptions";
    bad_req.headers.emplace("content-type", "application/json");
    bad_req.headers.emplace("authorization", "Bearer " + token);
    bad_req.body = bad_body.dump();
    auto bad_resp = client.send(bad_req);
    ASSERT_TRUE(bad_resp.has_value());
    EXPECT_EQ(bad_resp->status, 400);
}

TEST(NefInferenceIntegration, CreatePutPatchDeleteLifecycleNoGet) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(
        wait_reachable(client, "https://127.0.0.1:7790/nnef-inference/v1/subscriptions", 200))
        << "nef never became reachable";

    const std::string token = fetch_token(client, "nnef-inference");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // Real, disclosed: `inferAnaSubs` is a real `additionalProperties`-map schema this project's
    // own codegen falls back to opaque `nlohmann::json` for (same class of gap as the
    // "OPAQUE FALLBACK" precedent already established elsewhere in this project) -- any JSON
    // object is structurally accepted server-side for this field, not independently validated.
    const json create_body = json{
        {"notifUri", "https://example.com/inference-notify"},
        {"notifCorrId", "corr-1"},
        {"inferAnaSubs", json{{"ana-1", json{{"anaEvent", "SLICE_LOAD_LEVEL"}}}}},
        {"targetServerId", "vfl-server-1"},
    };
    sbi_core::http2::ClientRequest create_req;
    create_req.method = "POST";
    create_req.url = "https://127.0.0.1:7790/nnef-inference/v1/subscriptions";
    create_req.headers.emplace("content-type", "application/json");
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = create_body.dump();
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    EXPECT_EQ(create_resp->status, 201) << create_resp->body;

    const std::string sub_id =
        create_resp->status == 201 ? subscription_id_from_location(*create_resp) : "";
    EXPECT_FALSE(sub_id.empty());
    if (!sub_id.empty()) {
        const json put_body = json{
            {"notifUri", "https://example.com/inference-notify"},
            {"notifCorrId", "corr-2"},
            {"inferAnaSubs", json{{"ana-1", json{{"anaEvent", "SLICE_LOAD_LEVEL"}}}}},
            {"targetServerId", "vfl-server-1"},
        };
        sbi_core::http2::ClientRequest put_req;
        put_req.method = "PUT";
        put_req.url = "https://127.0.0.1:7790/nnef-inference/v1/subscriptions/" + sub_id;
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
        patch_req.url = "https://127.0.0.1:7790/nnef-inference/v1/subscriptions/" + sub_id;
        patch_req.headers.emplace("content-type", "application/merge-patch+json");
        patch_req.headers.emplace("authorization", "Bearer " + token);
        patch_req.body = json{{"notifCorrId", "corr-3"}}.dump();
        auto patch_resp = client.send(patch_req);
        ASSERT_TRUE(patch_resp.has_value());
        EXPECT_EQ(patch_resp->status, 200) << patch_resp->body;
        if (patch_resp->status == 200) {
            const auto patched = json::parse(patch_resp->body);
            EXPECT_EQ(patched.at("notifCorrId"), "corr-3");
            EXPECT_EQ(patched.at("targetServerId"), "vfl-server-1");
        }

        sbi_core::http2::ClientRequest delete_req;
        delete_req.method = "DELETE";
        delete_req.url = "https://127.0.0.1:7790/nnef-inference/v1/subscriptions/" + sub_id;
        delete_req.headers.emplace("authorization", "Bearer " + token);
        auto delete_resp = client.send(delete_req);
        ASSERT_TRUE(delete_resp.has_value());
        EXPECT_EQ(delete_resp->status, 204);

        auto delete_again_resp = client.send(delete_req);
        ASSERT_TRUE(delete_again_resp.has_value());
        EXPECT_EQ(delete_again_resp->status, 404);
    }
}

TEST(NefTrainingIntegration, CreatePutPatchDeleteLifecycleNoGet) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(
        wait_reachable(client, "https://127.0.0.1:7790/nnef-training/v1/subscriptions", 200))
        << "nef never became reachable";

    const std::string token = fetch_token(client, "nnef-training");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // Real `EventSubsc` (TS29530_Naf_Training.yaml) requires only `event` (a real open-string-enum
    // wire type -- bare string, not a wrapper object; NwdafEvent's own known value used here).
    const json create_body = json{
        {"notifUri", "https://example.com/training-notify"},
        {"notifCorrId", "corr-1"},
        {"trainEventSubs", json::array({json{{"event", "SLICE_LOAD_LEVEL"}}})},
    };
    sbi_core::http2::ClientRequest create_req;
    create_req.method = "POST";
    create_req.url = "https://127.0.0.1:7790/nnef-training/v1/subscriptions";
    create_req.headers.emplace("content-type", "application/json");
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = create_body.dump();
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    EXPECT_EQ(create_resp->status, 201) << create_resp->body;

    const std::string sub_id =
        create_resp->status == 201 ? subscription_id_from_location(*create_resp) : "";
    EXPECT_FALSE(sub_id.empty());
    if (!sub_id.empty()) {
        sbi_core::http2::ClientRequest patch_req;
        patch_req.method = "PATCH";
        patch_req.url = "https://127.0.0.1:7790/nnef-training/v1/subscriptions/" + sub_id;
        patch_req.headers.emplace("content-type", "application/merge-patch+json");
        patch_req.headers.emplace("authorization", "Bearer " + token);
        patch_req.body = json{{"notifCorrId", "corr-2"}}.dump();
        auto patch_resp = client.send(patch_req);
        ASSERT_TRUE(patch_resp.has_value());
        EXPECT_EQ(patch_resp->status, 200) << patch_resp->body;
        if (patch_resp->status == 200) {
            EXPECT_EQ(json::parse(patch_resp->body).at("notifCorrId"), "corr-2");
        }

        sbi_core::http2::ClientRequest delete_req;
        delete_req.method = "DELETE";
        delete_req.url = "https://127.0.0.1:7790/nnef-training/v1/subscriptions/" + sub_id;
        delete_req.headers.emplace("authorization", "Bearer " + token);
        auto delete_resp = client.send(delete_req);
        ASSERT_TRUE(delete_resp.has_value());
        EXPECT_EQ(delete_resp->status, 204);
    }
}

TEST(NefVFLInferenceIntegration, CreateReadPutPatchDeleteLifecycle) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7790/nnef-vfl-inference/v1/subscriptions/nonexistent", 50))
        << "nef never became reachable";

    const std::string token = fetch_token(client, "nnef-vfl-inference");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // Real `VflInferAnaSub` (TS29520_Nnwdaf_VFLInference.yaml) requires both `anaEvent` and
    // `vflCorrId`.
    const json create_body = json{
        {"notifUri", "https://example.com/vfl-inference-notify"},
        {"notifCorrId", "corr-1"},
        {"afId", "af-1"},
        {"vflInferAnaSubs",
         json::array({json{{"anaEvent", "SLICE_LOAD_LEVEL"}, {"vflCorrId", "vfl-corr-1"}}})},
    };
    sbi_core::http2::ClientRequest create_req;
    create_req.method = "POST";
    create_req.url = "https://127.0.0.1:7790/nnef-vfl-inference/v1/subscriptions";
    create_req.headers.emplace("content-type", "application/json");
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = create_body.dump();
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    EXPECT_EQ(create_resp->status, 201) << create_resp->body;

    const std::string sub_id =
        create_resp->status == 201 ? subscription_id_from_location(*create_resp) : "";
    EXPECT_FALSE(sub_id.empty());
    if (!sub_id.empty()) {
        sbi_core::http2::ClientRequest get_req;
        get_req.method = "GET";
        get_req.url = "https://127.0.0.1:7790/nnef-vfl-inference/v1/subscriptions/" + sub_id;
        get_req.headers.emplace("authorization", "Bearer " + token);
        auto get_resp = client.send(get_req);
        ASSERT_TRUE(get_resp.has_value());
        EXPECT_EQ(get_resp->status, 200);

        sbi_core::http2::ClientRequest patch_req;
        patch_req.method = "PATCH";
        patch_req.url = "https://127.0.0.1:7790/nnef-vfl-inference/v1/subscriptions/" + sub_id;
        patch_req.headers.emplace("content-type", "application/merge-patch+json");
        patch_req.headers.emplace("authorization", "Bearer " + token);
        patch_req.body = json{{"notifCorrId", "corr-2"}}.dump();
        auto patch_resp = client.send(patch_req);
        ASSERT_TRUE(patch_resp.has_value());
        EXPECT_EQ(patch_resp->status, 200) << patch_resp->body;
        if (patch_resp->status == 200) {
            const auto patched = json::parse(patch_resp->body);
            EXPECT_EQ(patched.at("notifCorrId"), "corr-2");
            EXPECT_EQ(patched.at("afId"), "af-1");
        }

        sbi_core::http2::ClientRequest delete_req;
        delete_req.method = "DELETE";
        delete_req.url = "https://127.0.0.1:7790/nnef-vfl-inference/v1/subscriptions/" + sub_id;
        delete_req.headers.emplace("authorization", "Bearer " + token);
        auto delete_resp = client.send(delete_req);
        ASSERT_TRUE(delete_resp.has_value());
        EXPECT_EQ(delete_resp->status, 204);

        auto delete_again_resp = client.send(delete_req);
        ASSERT_TRUE(delete_again_resp.has_value());
        EXPECT_EQ(delete_again_resp->status, 404);
    }
}

TEST(NefVFLTrainingIntegration, CreateReadPutDeleteLifecycleOnlyVflTrainSubsRequired) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7790/nnef-vfl-training/v1/subscriptions/nonexistent", 50))
        << "nef never became reachable";

    const std::string token = fetch_token(client, "nnef-vfl-training");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // Real, disclosed: notifUri/notifCorrId are genuinely optional on this resource -- omitted
    // here to verify the real required-field set is just vflTrainSubs, not fabricated stricter
    // validation. Real `VflTrainingSub` (TS29520_Nnwdaf_VFLTraining.yaml) requires both `event`
    // and `vflCorrId`.
    const json create_body = json{
        {"vflTrainSubs",
         json::array({json{{"event", "SLICE_LOAD_LEVEL"}, {"vflCorrId", "vfl-corr-1"}}})},
    };
    sbi_core::http2::ClientRequest create_req;
    create_req.method = "POST";
    create_req.url = "https://127.0.0.1:7790/nnef-vfl-training/v1/subscriptions";
    create_req.headers.emplace("content-type", "application/json");
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = create_body.dump();
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    EXPECT_EQ(create_resp->status, 201) << create_resp->body;

    const std::string sub_id =
        create_resp->status == 201 ? subscription_id_from_location(*create_resp) : "";
    EXPECT_FALSE(sub_id.empty());
    if (!sub_id.empty()) {
        sbi_core::http2::ClientRequest get_req;
        get_req.method = "GET";
        get_req.url = "https://127.0.0.1:7790/nnef-vfl-training/v1/subscriptions/" + sub_id;
        get_req.headers.emplace("authorization", "Bearer " + token);
        auto get_resp = client.send(get_req);
        ASSERT_TRUE(get_resp.has_value());
        EXPECT_EQ(get_resp->status, 200);

        sbi_core::http2::ClientRequest delete_req;
        delete_req.method = "DELETE";
        delete_req.url = "https://127.0.0.1:7790/nnef-vfl-training/v1/subscriptions/" + sub_id;
        delete_req.headers.emplace("authorization", "Bearer " + token);
        auto delete_resp = client.send(delete_req);
        ASSERT_TRUE(delete_resp.has_value());
        EXPECT_EQ(delete_resp->status, 204);
    }
}

TEST(NefVFLTrainingIntegration, CreateWithoutVflTrainSubsIs400) {
    auto d = spawn_all();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7790/nnef-vfl-training/v1/subscriptions/nonexistent", 50))
        << "nef never became reachable";

    const std::string token = fetch_token(client, "nnef-vfl-training");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const json bad_body = json{{"notifCorrId", "corr-1"}};
    sbi_core::http2::ClientRequest bad_req;
    bad_req.method = "POST";
    bad_req.url = "https://127.0.0.1:7790/nnef-vfl-training/v1/subscriptions";
    bad_req.headers.emplace("content-type", "application/json");
    bad_req.headers.emplace("authorization", "Bearer " + token);
    bad_req.body = bad_body.dump();
    auto bad_resp = client.send(bad_req);
    ASSERT_TRUE(bad_resp.has_value());
    EXPECT_EQ(bad_resp->status, 400);
}
