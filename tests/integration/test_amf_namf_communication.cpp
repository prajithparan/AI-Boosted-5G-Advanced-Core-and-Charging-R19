// Drives nrf and amf as real, separate OS processes: nrf issues real signed OAuth2 tokens, amf
// registers with nrf on its own background thread (see docs/DECISIONS.md ADR-0006's
// dedicated-thread note in nfs/amf/src/main.cpp), then this test acts as an SBI client (reusing
// hello-nf's lab cert purely as a caller identity, same as the manual curl verification recorded
// in docs/TRACEABILITY.md) exercising amf's Namf_Communication routes over real TLS 1.3 + mTLS.
//
// Covers: the procedures that don't require a pre-existing UE context
// (N1N2MessageSubscribe/UnSubscribe, AMFStatusChangeSubscribe, NonUeN2MessageTransfer), the
// 404/401 error paths, and -- since sbi_core::multipart landed (ADR-0020) -- a real multipart/
// related CreateUEContext over the wire followed by EBIAssignment/ReleaseUEContext/
// RelocateUEContext/CancelRelocateUEContext's previously-unreachable "found" branches.
// UEContextTransfer/RegistrationStatusUpdate's "found" branches remain unverified by any
// automated test -- disclosed in nfs/amf/src/main.cpp's file header, not hidden.

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

// Builds a real multipart/related request (single jsonData part, RFC 2046/2387 wire bytes via
// sbi_core::multipart::encode -- the same codec amf's own handlers use to parse) for the three
// multipart-only operations (CreateUEContext, RelocateUEContext, CancelRelocateUEContext).
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

std::string fetch_token(sbi_core::http2::Client& client) {
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = "https://127.0.0.1:7777/oauth2/token";
    req.headers.emplace("content-type", "application/x-www-form-urlencoded");
    req.body =
        "grant_type=client_credentials&nfInstanceId=" + std::string(nf_test::kTestAfInstanceId) +
        "&scope=namf-comm&"
        "targetNfType=AMF";
    auto resp = nf_test::send_token_request(req);
    if (!resp.has_value() || resp->status != 200) {
        return "";
    }
    return json::parse(resp->body).at("access_token").get<std::string>();
}

} // namespace

TEST(AmfIntegration, N1N2SubscribeAndUnsubscribe) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess amf(AMF_PATH);
    ASSERT_GT(amf.pid(), 0) << "failed to fork amf";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7778/namf-comm/v1/subscriptions/nonexistent", 200))
        << "amf never became reachable";

    // amf's own NRF-registration background thread races this test; give it a moment. (Every
    // request below authenticates via its own bearer token regardless, so this is only about amf
    // having had time to register, not a correctness dependency of the requests themselves.)
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    sbi_core::http2::ClientRequest sub_req;
    sub_req.method = "POST";
    sub_req.url = "https://127.0.0.1:7778/namf-comm/v1/ue-contexts/imsi-999700000000001/"
                  "n1-n2-messages/subscriptions";
    sub_req.headers.emplace("content-type", "application/json");
    sub_req.headers.emplace("authorization", "Bearer " + token);
    sub_req.body = json{{"n1NotifyCallbackUri", "https://example.com/n1"}}.dump();

    auto sub_resp = client.send(sub_req);
    ASSERT_TRUE(sub_resp.has_value());
    EXPECT_EQ(sub_resp->status, 201);
    const auto created =
        json::parse(sub_resp->body).get<sbi_gen::UeN1N2InfoSubscriptionCreatedData>();
    EXPECT_FALSE(created.n1n2NotifySubscriptionId.empty());

    sbi_core::http2::ClientRequest unsub_req;
    unsub_req.method = "DELETE";
    unsub_req.url = "https://127.0.0.1:7778/namf-comm/v1/ue-contexts/imsi-999700000000001/"
                    "n1-n2-messages/subscriptions/" +
                    created.n1n2NotifySubscriptionId;
    unsub_req.headers.emplace("authorization", "Bearer " + token);
    auto unsub_resp = client.send(unsub_req);
    ASSERT_TRUE(unsub_resp.has_value());
    EXPECT_EQ(unsub_resp->status, 204);

    // Removed already -- unsubscribing again must 404, not silently succeed twice.
    auto unsub_again = client.send(unsub_req);
    ASSERT_TRUE(unsub_again.has_value());
    EXPECT_EQ(unsub_again->status, 404);
}

TEST(AmfIntegration, AmfStatusChangeSubscribeAndNonUeN2Transfer) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess amf(AMF_PATH);
    ASSERT_GT(amf.pid(), 0) << "failed to fork amf";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7778/namf-comm/v1/subscriptions/nonexistent", 200))
        << "amf never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    sbi_core::http2::ClientRequest amf_status_req;
    amf_status_req.method = "POST";
    amf_status_req.url = "https://127.0.0.1:7778/namf-comm/v1/subscriptions";
    amf_status_req.headers.emplace("content-type", "application/json");
    amf_status_req.headers.emplace("authorization", "Bearer " + token);
    amf_status_req.body = json{{"amfStatusUri", "https://example.com/amfstatus"}}.dump();
    auto amf_status_resp = client.send(amf_status_req);
    ASSERT_TRUE(amf_status_resp.has_value());
    EXPECT_EQ(amf_status_resp->status, 201);
    auto loc = amf_status_resp->headers.find("location");
    ASSERT_NE(loc, amf_status_resp->headers.end());
    EXPECT_NE(loc->second.find("/namf-comm/v1/subscriptions/"), std::string::npos);

    sbi_core::http2::ClientRequest n2_req;
    n2_req.method = "POST";
    n2_req.url = "https://127.0.0.1:7778/namf-comm/v1/non-ue-n2-messages/transfer";
    n2_req.headers.emplace("content-type", "application/json");
    n2_req.headers.emplace("authorization", "Bearer " + token);
    n2_req.body = json{{"n2Information", json{{"n2InformationClass", "PWS"}}}}.dump();
    auto n2_resp = client.send(n2_req);
    ASSERT_TRUE(n2_resp.has_value());
    EXPECT_EQ(n2_resp->status, 200);
    const auto result = json::parse(n2_resp->body).get<sbi_gen::N2InformationTransferRspData>();
    EXPECT_EQ(result.result.value,
              sbi_gen::N2InformationTransferResult::N2_INFO_TRANSFER_INITIATED);
}

TEST(AmfIntegration, MissingUeContextIs404AndTamperedTokenIs401) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess amf(AMF_PATH);
    ASSERT_GT(amf.pid(), 0) << "failed to fork amf";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7778/namf-comm/v1/subscriptions/nonexistent", 200))
        << "amf never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    sbi_core::http2::ClientRequest release_req;
    release_req.method = "POST";
    release_req.url =
        "https://127.0.0.1:7778/namf-comm/v1/ue-contexts/imsi-999700000000002/release";
    release_req.headers.emplace("content-type", "application/json");
    release_req.headers.emplace("authorization", "Bearer " + token);
    release_req.body = json{{"ngapCause", json{{"group", 0}, {"value", 0}}}}.dump();
    auto release_resp = client.send(release_req);
    ASSERT_TRUE(release_resp.has_value());
    EXPECT_EQ(release_resp->status, 404);

    sbi_core::http2::ClientRequest tampered_req;
    tampered_req.method = "POST";
    tampered_req.url = "https://127.0.0.1:7778/namf-comm/v1/subscriptions";
    tampered_req.headers.emplace("content-type", "application/json");
    tampered_req.headers.emplace("authorization", "Bearer " + token + "tampered");
    tampered_req.body = json{{"amfStatusUri", "https://example.com/amfstatus"}}.dump();
    auto tampered_resp = client.send(tampered_req);
    ASSERT_TRUE(tampered_resp.has_value());
    EXPECT_EQ(tampered_resp->status, 401);
}

TEST(AmfIntegration, CreateUEContextOverMultipartThenEBIAssignmentAndRelease) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess amf(AMF_PATH);
    ASSERT_GT(amf.pid(), 0) << "failed to fork amf";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7778/namf-comm/v1/subscriptions/nonexistent", 200))
        << "amf never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const std::string ue_context_id = "imsi-999700000000099";

    // Default-constructed UeContextCreateData: every mandatory field's key is still present after
    // to_json (nlohmann always emits non-optional fields), just with default/empty values -- a
    // structurally-valid request per spec, since this test only needs to prove the multipart
    // plumbing (encode -> real HTTP/2 wire bytes -> amf's parser -> store), not exercise real
    // inter-AMF handover semantics (which this lab has no second AMF to produce anyway).
    const json create_json = sbi_gen::UeContextCreateData{};
    auto create_req =
        make_multipart_request("PUT",
                               "https://127.0.0.1:7778/namf-comm/v1/ue-contexts/" + ue_context_id,
                               token,
                               create_json);
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    EXPECT_EQ(create_resp->status, 201);
    // Proves the response deserializes as real UeContextCreatedData, not just "some JSON".
    const auto created = json::parse(create_resp->body).get<sbi_gen::UeContextCreatedData>();
    EXPECT_FALSE(created.targetToSourceData.ngapData.contentId.empty());

    // EBIAssignment's "found" branch -- unreachable before CreateUEContext existed.
    sbi_core::http2::ClientRequest ebi_req;
    ebi_req.method = "POST";
    ebi_req.url =
        "https://127.0.0.1:7778/namf-comm/v1/ue-contexts/" + ue_context_id + "/assign-ebi";
    ebi_req.headers.emplace("content-type", "application/json");
    ebi_req.headers.emplace("authorization", "Bearer " + token);
    ebi_req.body = json{{"pduSessionId", 5}}.dump();
    auto ebi_resp = client.send(ebi_req);
    ASSERT_TRUE(ebi_resp.has_value());
    EXPECT_EQ(ebi_resp->status, 200);

    // ReleaseUEContext's "found" branch.
    sbi_core::http2::ClientRequest release_req;
    release_req.method = "POST";
    release_req.url =
        "https://127.0.0.1:7778/namf-comm/v1/ue-contexts/" + ue_context_id + "/release";
    release_req.headers.emplace("content-type", "application/json");
    release_req.headers.emplace("authorization", "Bearer " + token);
    release_req.body = json{{"ngapCause", json{{"group", 0}, {"value", 0}}}}.dump();
    auto release_resp = client.send(release_req);
    ASSERT_TRUE(release_resp.has_value());
    EXPECT_EQ(release_resp->status, 204);

    // Context is gone now -- releasing again must 404, not silently succeed twice.
    auto release_again = client.send(release_req);
    ASSERT_TRUE(release_again.has_value());
    EXPECT_EQ(release_again->status, 404);
}

TEST(AmfIntegration, RelocateAndCancelRelocateUEContextOverMultipart) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess amf(AMF_PATH);
    ASSERT_GT(amf.pid(), 0) << "failed to fork amf";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7778/namf-comm/v1/subscriptions/nonexistent", 200))
        << "amf never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const std::string ue_context_id = "imsi-999700000000098";

    const json create_json = sbi_gen::UeContextCreateData{};
    auto create_req =
        make_multipart_request("PUT",
                               "https://127.0.0.1:7778/namf-comm/v1/ue-contexts/" + ue_context_id,
                               token,
                               create_json);
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    ASSERT_EQ(create_resp->status, 201);

    const json relocate_json = sbi_gen::UeContextRelocateData{};
    auto relocate_req = make_multipart_request("POST",
                                               "https://127.0.0.1:7778/namf-comm/v1/ue-contexts/" +
                                                   ue_context_id + "/relocate",
                                               token,
                                               relocate_json);
    auto relocate_resp = client.send(relocate_req);
    ASSERT_TRUE(relocate_resp.has_value());
    EXPECT_EQ(relocate_resp->status, 201);
    const auto relocated = json::parse(relocate_resp->body).get<sbi_gen::UeContextRelocatedData>();
    (void)relocated; // proves real deserialization, not just a 2xx status

    const json cancel_json = sbi_gen::UeContextCancelRelocateData{};
    auto cancel_req = make_multipart_request("POST",
                                             "https://127.0.0.1:7778/namf-comm/v1/ue-contexts/" +
                                                 ue_context_id + "/cancel-relocate",
                                             token,
                                             cancel_json);
    auto cancel_resp = client.send(cancel_req);
    ASSERT_TRUE(cancel_resp.has_value());
    EXPECT_EQ(cancel_resp->status, 204);

    // Not a UE context that exists -- relocate on it must 404.
    auto relocate_missing_req = make_multipart_request(
        "POST",
        "https://127.0.0.1:7778/namf-comm/v1/ue-contexts/imsi-999700000000097/relocate",
        token,
        relocate_json);
    auto relocate_missing_resp = client.send(relocate_missing_req);
    ASSERT_TRUE(relocate_missing_resp.has_value());
    EXPECT_EQ(relocate_missing_resp->status, 404);

    // Non-multipart body on a multipart-only operation must 400, not be silently accepted.
    sbi_core::http2::ClientRequest wrong_content_type_req;
    wrong_content_type_req.method = "PUT";
    wrong_content_type_req.url = "https://127.0.0.1:7778/namf-comm/v1/ue-contexts/some-other-id";
    wrong_content_type_req.headers.emplace("content-type", "application/json");
    wrong_content_type_req.headers.emplace("authorization", "Bearer " + token);
    wrong_content_type_req.body = create_json.dump();
    auto wrong_content_type_resp = client.send(wrong_content_type_req);
    ASSERT_TRUE(wrong_content_type_resp.has_value());
    EXPECT_EQ(wrong_content_type_resp->status, 400);
}
