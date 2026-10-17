// Drives nrf and amf as real, separate OS processes to exercise amf's TS29518_Namf_Location.yaml
// (ADR-0199) and TS29518_Namf_EventExposure.yaml (ADR-0199) routes over real TLS 1.3 + mTLS.
// Same spawn/token pattern as test_amf_namf_communication.cpp.
//
// Covers: ProvidePositioningInfo's real, disclosed 501 (no LPP/GNSS/PRU capability);
// ProvideLocationInfo's real 404-then-honestly-empty-200 (mirrors ReleaseUEContext's own
// get/404 precedent, via a real multipart CreateUEContext); CancelLocation's real 404 (no
// ldrReference is ever issued, since ProvidePositioningInfo always 501s); and full real
// create+modify(RFC 6902 PATCH)+delete subscription CRUD for both the individual /subscriptions
// and AMF-Set-level /set-subscriptions families of Namf_EventExposure.

#include "sbi_core/http2_client.hpp"
#include "sbi_core/multipart.hpp"

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

sbi_core::http2::ClientRequest make_multipart_request(const std::string& method,
                                                      const std::string& url,
                                                      const std::string& token,
                                                      const json& json_data) {
    sbi_core::multipart::Part root;
    root.content_type = "application/json";
    root.body = json_data.dump();
    const auto encoded = sbi_core::multipart::encode({root});

    sbi_core::http2::ClientRequest req;
    req.method = method;
    req.url = url;
    req.headers.emplace("content-type", encoded.content_type_header);
    req.headers.emplace("authorization", "Bearer " + token);
    req.body = encoded.body;
    return req;
}

// ADR-0499: one token naming every Namf service this file calls (space-separated scope).
std::string fetch_token(sbi_core::http2::Client& client) {
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = "https://127.0.0.1:7777/oauth2/token";
    req.headers.emplace("content-type", "application/x-www-form-urlencoded");
    req.body =
        "grant_type=client_credentials&nfInstanceId=" + std::string(nf_test::kTestAfInstanceId) +
        "&scope=namf-loc%20namf-evts%"
        "20namf-comm&"
        "targetNfType=AMF";
    auto resp = nf_test::send_token_request(req);
    if (!resp.has_value() || resp->status != 200) {
        return "";
    }
    return json::parse(resp->body).at("access_token").get<std::string>();
}

sbi_gen::AmfCreateEventSubscription make_create_subscription(const std::string& notify_uri) {
    sbi_gen::AmfCreateEventSubscription body;
    sbi_gen::AmfEvent evt;
    evt.type.value = sbi_gen::AmfEventType::LOCATION_REPORT;
    body.subscription.eventList = {evt};
    body.subscription.eventNotifyUri = notify_uri;
    body.subscription.notifyCorrelationId = "corr-1";
    body.subscription.nfId = "5ba9a927-1d31-4c8e-8a10-000000000099";
    return body;
}

} // namespace

TEST(AmfLocationIntegration, ProvidePositioningInfoIs501) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess amf(AMF_PATH);
    ASSERT_GT(amf.pid(), 0) << "failed to fork amf";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7778/namf-loc/v1/nonexistent/provide-loc-info", 50))
        << "amf never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = "https://127.0.0.1:7778/namf-loc/v1/imsi-999700000000201/provide-pos-info";
    req.headers.emplace("content-type", "application/json");
    req.headers.emplace("authorization", "Bearer " + token);
    req.body =
        json{{"lcsClientType", "EMERGENCY_SERVICES"}, {"lcsLocation", "CURRENT_LOCATION"}}.dump();
    auto resp = client.send(req);
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->status, 501);

    // Missing required field (lcsLocation) must 400, not 501 -- structural validation still runs.
    sbi_core::http2::ClientRequest bad_req;
    bad_req.method = "POST";
    bad_req.url = "https://127.0.0.1:7778/namf-loc/v1/imsi-999700000000201/provide-pos-info";
    bad_req.headers.emplace("content-type", "application/json");
    bad_req.headers.emplace("authorization", "Bearer " + token);
    bad_req.body = json{{"lcsClientType", "EMERGENCY_SERVICES"}}.dump();
    auto bad_resp = client.send(bad_req);
    ASSERT_TRUE(bad_resp.has_value());
    EXPECT_EQ(bad_resp->status, 400);
}

TEST(AmfLocationIntegration, ProvideLocationInfo404ThenHonestlyEmpty200) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess amf(AMF_PATH);
    ASSERT_GT(amf.pid(), 0) << "failed to fork amf";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7778/namf-loc/v1/nonexistent/provide-loc-info", 50))
        << "amf never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const std::string ue_context_id = "imsi-999700000000202";

    sbi_core::http2::ClientRequest missing_req;
    missing_req.method = "POST";
    missing_req.url = "https://127.0.0.1:7778/namf-loc/v1/" + ue_context_id + "/provide-loc-info";
    missing_req.headers.emplace("content-type", "application/json");
    missing_req.headers.emplace("authorization", "Bearer " + token);
    missing_req.body = json::object().dump();
    auto missing_resp = client.send(missing_req);
    ASSERT_TRUE(missing_resp.has_value());
    EXPECT_EQ(missing_resp->status, 404);

    const json create_json = sbi_gen::UeContextCreateData{};
    auto create_req =
        make_multipart_request("PUT",
                               "https://127.0.0.1:7778/namf-comm/v1/ue-contexts/" + ue_context_id,
                               token,
                               create_json);
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    ASSERT_EQ(create_resp->status, 201);

    auto found_resp = client.send(missing_req);
    ASSERT_TRUE(found_resp.has_value());
    EXPECT_EQ(found_resp->status, 200);
    // Proves real deserialization as ProvideLocInfo; honestly empty since UeContextStore carries
    // no real RAT-type/location data (see nfs/amf/src/main.cpp's own ADR-0199 comment).
    const auto loc_info = json::parse(found_resp->body).get<sbi_gen::ProvideLocInfo>();
    EXPECT_FALSE(loc_info.ratType.has_value());
    EXPECT_FALSE(loc_info.location.has_value());
}

TEST(AmfLocationIntegration, CancelLocationIs404) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess amf(AMF_PATH);
    ASSERT_GT(amf.pid(), 0) << "failed to fork amf";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7778/namf-loc/v1/nonexistent/provide-loc-info", 50))
        << "amf never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = "https://127.0.0.1:7778/namf-loc/v1/imsi-999700000000203/cancel-pos-info";
    req.headers.emplace("content-type", "application/json");
    req.headers.emplace("authorization", "Bearer " + token);
    req.body = json{
        {"supi", "imsi-999700000000203"},
        {"hgmlcCallBackURI", "https://example.com/gmlc"},
        {"ldrReference",
         "ldr-42"}}.dump();
    auto resp = client.send(req);
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->status, 404);
}

TEST(AmfEventExposureIntegration, IndividualSubscriptionCreateModifyDelete) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess amf(AMF_PATH);
    ASSERT_GT(amf.pid(), 0) << "failed to fork amf";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7778/namf-evts/v1/subscriptions/nonexistent", 200))
        << "amf never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const json create_json = make_create_subscription("https://example.com/evtnotify");
    sbi_core::http2::ClientRequest create_req;
    create_req.method = "POST";
    create_req.url = "https://127.0.0.1:7778/namf-evts/v1/subscriptions";
    create_req.headers.emplace("content-type", "application/json");
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = create_json.dump();
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    ASSERT_EQ(create_resp->status, 201);
    auto loc = create_resp->headers.find("location");
    ASSERT_NE(loc, create_resp->headers.end());
    EXPECT_NE(loc->second.find("/namf-evts/v1/subscriptions/"), std::string::npos);
    const auto created = json::parse(create_resp->body).get<sbi_gen::AmfCreatedEventSubscription>();
    EXPECT_FALSE(created.subscriptionId.empty());
    EXPECT_EQ(created.subscription.notifyCorrelationId, "corr-1");

    // Real RFC 6902 JSON Patch: replace notifyCorrelationId.
    sbi_core::http2::ClientRequest patch_req;
    patch_req.method = "PATCH";
    patch_req.url = "https://127.0.0.1:7778/namf-evts/v1/subscriptions/" + created.subscriptionId;
    patch_req.headers.emplace("content-type", "application/json-patch+json");
    patch_req.headers.emplace("authorization", "Bearer " + token);
    patch_req.body =
        json::array(
            {json{{"op", "replace"}, {"path", "/notifyCorrelationId"}, {"value", "corr-2"}}})
            .dump();
    auto patch_resp = client.send(patch_req);
    ASSERT_TRUE(patch_resp.has_value());
    ASSERT_EQ(patch_resp->status, 200);
    const auto updated = json::parse(patch_resp->body).get<sbi_gen::AmfUpdatedEventSubscription>();
    EXPECT_EQ(updated.subscription.notifyCorrelationId, "corr-2");

    // Modify against a nonexistent subscription must 404.
    sbi_core::http2::ClientRequest missing_patch_req = patch_req;
    missing_patch_req.url = "https://127.0.0.1:7778/namf-evts/v1/subscriptions/nonexistent-id";
    auto missing_patch_resp = client.send(missing_patch_req);
    ASSERT_TRUE(missing_patch_resp.has_value());
    EXPECT_EQ(missing_patch_resp->status, 404);

    sbi_core::http2::ClientRequest delete_req;
    delete_req.method = "DELETE";
    delete_req.url = "https://127.0.0.1:7778/namf-evts/v1/subscriptions/" + created.subscriptionId;
    delete_req.headers.emplace("authorization", "Bearer " + token);
    auto delete_resp = client.send(delete_req);
    ASSERT_TRUE(delete_resp.has_value());
    EXPECT_EQ(delete_resp->status, 204);

    auto delete_again_resp = client.send(delete_req);
    ASSERT_TRUE(delete_again_resp.has_value());
    EXPECT_EQ(delete_again_resp->status, 404);
}

TEST(AmfEventExposureIntegration, AmfSetLevelBulkSubscriptionCreateModifyDelete) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess amf(AMF_PATH);
    ASSERT_GT(amf.pid(), 0) << "failed to fork amf";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7778/namf-evts/v1/set-subscriptions/nonexistent", 50))
        << "amf never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const json create_json = make_create_subscription("https://example.com/setevtnotify");
    sbi_core::http2::ClientRequest create_req;
    create_req.method = "POST";
    create_req.url = "https://127.0.0.1:7778/namf-evts/v1/set-subscriptions";
    create_req.headers.emplace("content-type", "application/json");
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = create_json.dump();
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    ASSERT_EQ(create_resp->status, 201);
    auto loc = create_resp->headers.find("location");
    ASSERT_NE(loc, create_resp->headers.end());
    EXPECT_NE(loc->second.find("/namf-evts/v1/set-subscriptions/"), std::string::npos);
    const auto created = json::parse(create_resp->body).get<sbi_gen::AmfCreatedEventSubscription>();
    EXPECT_FALSE(created.subscriptionId.empty());

    // Individual and set-level stores must be genuinely separate id spaces: an individual-family
    // DELETE against a set-level-only id must 404, not accidentally succeed.
    sbi_core::http2::ClientRequest cross_delete_req;
    cross_delete_req.method = "DELETE";
    cross_delete_req.url =
        "https://127.0.0.1:7778/namf-evts/v1/subscriptions/" + created.subscriptionId;
    cross_delete_req.headers.emplace("authorization", "Bearer " + token);
    auto cross_delete_resp = client.send(cross_delete_req);
    ASSERT_TRUE(cross_delete_resp.has_value());
    EXPECT_EQ(cross_delete_resp->status, 404);

    sbi_core::http2::ClientRequest patch_req;
    patch_req.method = "PATCH";
    patch_req.url =
        "https://127.0.0.1:7778/namf-evts/v1/set-subscriptions/" + created.subscriptionId;
    patch_req.headers.emplace("content-type", "application/json-patch+json");
    patch_req.headers.emplace("authorization", "Bearer " + token);
    patch_req.body =
        json::array(
            {json{{"op", "replace"}, {"path", "/notifyCorrelationId"}, {"value", "corr-3"}}})
            .dump();
    auto patch_resp = client.send(patch_req);
    ASSERT_TRUE(patch_resp.has_value());
    ASSERT_EQ(patch_resp->status, 200);
    const auto updated = json::parse(patch_resp->body).get<sbi_gen::AmfUpdatedEventSubscription>();
    EXPECT_EQ(updated.subscription.notifyCorrelationId, "corr-3");

    sbi_core::http2::ClientRequest delete_req;
    delete_req.method = "DELETE";
    delete_req.url =
        "https://127.0.0.1:7778/namf-evts/v1/set-subscriptions/" + created.subscriptionId;
    delete_req.headers.emplace("authorization", "Bearer " + token);
    auto delete_resp = client.send(delete_req);
    ASSERT_TRUE(delete_resp.has_value());
    EXPECT_EQ(delete_resp->status, 204);
}
