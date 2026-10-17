// Drives nrf and amf as real, separate OS processes to exercise amf's TS29518_Namf_AIoT.yaml,
// TS29518_Namf_MBSBroadcast.yaml, TS29518_Namf_MBSCommunication.yaml, and TS29518_Namf_MT.yaml
// routes (ADR-0200) over real TLS 1.3 + mTLS. Same spawn/token pattern as
// test_amf_namf_communication.cpp.
//
// Covers: AIoT MessageDelivery's real 204 (both application/json and multipart/related
// encodings) and real 400 on a missing required field; MBSCommunication N2MessageTransfer's real
// multipart-only 200 with N2_INFO_TRANSFER_INITIATED; MBSBroadcast's full real
// create/update/delete context lifecycle; MT's ProvideDomainSelectionInfo (404-then-honestly-
// empty-200), EnableUeReachability (404-then-real-echo-ack), and EnableGroupReachability (real
// 200 with an honestly-empty ueConnectedList).

#include "sbi_core/http2_client.hpp"
#include "sbi_core/multipart.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "TS26510_CommonData_grp.hpp"
#include "TS29518_Namf_AIoT.hpp"
#include "TS29518_Namf_MBSBroadcast.hpp"
#include "TS29518_Namf_MBSCommunication.hpp"
#include "TS29518_Namf_MT.hpp"
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
        "&scope=namf-aiot%20namf-mbs-"
        "bc%20namf-mbs-comm%20namf-mt%20namf-comm&"
        "targetNfType=AMF";
    auto resp = nf_test::send_token_request(req);
    if (!resp.has_value() || resp->status != 200) {
        return "";
    }
    return json::parse(resp->body).at("access_token").get<std::string>();
}

json make_ran_node_id() {
    return json{{"plmnId", json{{"mcc", "999"}, {"mnc", "70"}}}, {"n3IwfId", "n3iwf-1"}};
}

} // namespace

TEST(AmfAiotIntegration, MessageDeliveryJsonAndMultipartAnd400) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess amf(AMF_PATH);
    ASSERT_GT(amf.pid(), 0) << "failed to fork amf";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(client, "https://127.0.0.1:7778/namf-aiot/v1/transfer", 200))
        << "amf never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const json aiot_message =
        json{{"ngapMessageType", 1}, {"aiotNgapData", json{{"contentId", "aiot-part-1"}}}};
    const json valid_body = json{{"ranNodeId", make_ran_node_id()},
                                 {"aiotMessage", aiot_message},
                                 {"aiotMessageType", "INVENTORY_REQUEST"},
                                 {"notifUri", "https://example.com/aiot-notify"},
                                 {"aiotfId", "5ba9a927-1d31-4c8e-8a10-0000000000aa"},
                                 {"correlationId", 42}};

    sbi_core::http2::ClientRequest json_req;
    json_req.method = "POST";
    json_req.url = "https://127.0.0.1:7778/namf-aiot/v1/transfer";
    json_req.headers.emplace("content-type", "application/json");
    json_req.headers.emplace("authorization", "Bearer " + token);
    json_req.body = valid_body.dump();
    auto json_resp = client.send(json_req);
    ASSERT_TRUE(json_resp.has_value());
    EXPECT_EQ(json_resp->status, 204);

    auto multipart_req = make_multipart_request(
        "POST", "https://127.0.0.1:7778/namf-aiot/v1/transfer", token, valid_body);
    auto multipart_resp = client.send(multipart_req);
    ASSERT_TRUE(multipart_resp.has_value());
    EXPECT_EQ(multipart_resp->status, 204);

    // Missing required field (correlationId) must 400.
    json bad_body = valid_body;
    bad_body.erase("correlationId");
    sbi_core::http2::ClientRequest bad_req;
    bad_req.method = "POST";
    bad_req.url = "https://127.0.0.1:7778/namf-aiot/v1/transfer";
    bad_req.headers.emplace("content-type", "application/json");
    bad_req.headers.emplace("authorization", "Bearer " + token);
    bad_req.body = bad_body.dump();
    auto bad_resp = client.send(bad_req);
    ASSERT_TRUE(bad_resp.has_value());
    EXPECT_EQ(bad_resp->status, 400);
}

TEST(AmfMbsCommunicationIntegration, N2MessageTransferReturnsInitiated) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess amf(AMF_PATH);
    ASSERT_GT(amf.pid(), 0) << "failed to fork amf";

    auto client = make_client();
    ASSERT_TRUE(
        wait_reachable(client, "https://127.0.0.1:7778/namf-mbs-comm/v1/n2-messages/transfer", 200))
        << "amf never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const json n2_info =
        json{{"ngapIeType", "MBS_SES_ACT_REQ"}, {"ngapData", json{{"contentId", "mbs-n2-1"}}}};
    const json body = json{{"mbsSessionId",
                            json{{"ssm",
                                  json{{"sourceIpAddr", json{{"ipv4Addr", "10.0.0.1"}}},
                                       {"destIpAddr", json{{"ipv4Addr", "225.0.0.1"}}}}}}},
                           {"n2MbsSmInfo", n2_info}};

    auto req = make_multipart_request(
        "POST", "https://127.0.0.1:7778/namf-mbs-comm/v1/n2-messages/transfer", token, body);
    auto resp = client.send(req);
    ASSERT_TRUE(resp.has_value());
    ASSERT_EQ(resp->status, 200);
    const auto result = json::parse(resp->body).get<sbi_gen::MbsN2MessageTransferRspData>();
    EXPECT_EQ(result.result.value,
              sbi_gen::N2InformationTransferResult::N2_INFO_TRANSFER_INITIATED);
}

TEST(AmfMbsBroadcastIntegration, ContextCreateUpdateDeleteLifecycle) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess amf(AMF_PATH);
    ASSERT_GT(amf.pid(), 0) << "failed to fork amf";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7778/namf-mbs-bc/v1/mbs-contexts/nonexistent", 50))
        << "amf never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const json n2_info =
        json{{"ngapIeType", "MBS_SES_REQ"}, {"ngapData", json{{"contentId", "mbs-bc-1"}}}};
    const json create_body = json{{"mbsSessionId",
                                   json{{"ssm",
                                         json{{"sourceIpAddr", json{{"ipv4Addr", "10.0.0.2"}}},
                                              {"destIpAddr", json{{"ipv4Addr", "225.0.0.2"}}}}}}},
                                  {"mbsServiceArea", json::object()},
                                  {"n2MbsSmInfo", n2_info},
                                  {"notifyUri", "https://example.com/mbs-bc-notify"},
                                  {"snssai", json{{"sst", 1}}}};

    auto create_req = make_multipart_request(
        "POST", "https://127.0.0.1:7778/namf-mbs-bc/v1/mbs-contexts", token, create_body);
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    ASSERT_EQ(create_resp->status, 201);
    auto loc = create_resp->headers.find("location");
    ASSERT_NE(loc, create_resp->headers.end());
    EXPECT_NE(loc->second.find("/namf-mbs-bc/v1/mbs-contexts/"), std::string::npos);
    const auto ctx_ref = loc->second.substr(loc->second.rfind('/') + 1);
    const auto created = json::parse(create_resp->body).get<sbi_gen::ContextCreateRspData>();
    ASSERT_TRUE(created.mbsSessionId.ssm.has_value());
    ASSERT_TRUE(created.mbsSessionId.ssm->sourceIpAddr.ipv4Addr.has_value());
    EXPECT_EQ(created.mbsSessionId.ssm->sourceIpAddr.ipv4Addr.value(), "10.0.0.2");

    const json update_body = json{{"n2MbsInfoChangeInd", true}};
    auto update_req = make_multipart_request("POST",
                                             "https://127.0.0.1:7778/namf-mbs-bc/v1/mbs-contexts/" +
                                                 ctx_ref + "/update",
                                             token,
                                             update_body);
    auto update_resp = client.send(update_req);
    ASSERT_TRUE(update_resp.has_value());
    EXPECT_EQ(update_resp->status, 204);

    // Update against a nonexistent context must 404.
    sbi_core::http2::ClientRequest missing_update_req = update_req;
    missing_update_req.url =
        "https://127.0.0.1:7778/namf-mbs-bc/v1/mbs-contexts/nonexistent-ref/update";
    auto missing_update_resp = client.send(missing_update_req);
    ASSERT_TRUE(missing_update_resp.has_value());
    EXPECT_EQ(missing_update_resp->status, 404);

    sbi_core::http2::ClientRequest delete_req;
    delete_req.method = "DELETE";
    delete_req.url = "https://127.0.0.1:7778/namf-mbs-bc/v1/mbs-contexts/" + ctx_ref;
    delete_req.headers.emplace("authorization", "Bearer " + token);
    auto delete_resp = client.send(delete_req);
    ASSERT_TRUE(delete_resp.has_value());
    EXPECT_EQ(delete_resp->status, 204);

    auto delete_again_resp = client.send(delete_req);
    ASSERT_TRUE(delete_again_resp.has_value());
    EXPECT_EQ(delete_again_resp->status, 404);
}

TEST(AmfMtIntegration, ProvideDomainSelectionInfo404ThenHonestlyEmpty200) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess amf(AMF_PATH);
    ASSERT_GT(amf.pid(), 0) << "failed to fork amf";

    auto client = make_client();
    ASSERT_TRUE(
        wait_reachable(client, "https://127.0.0.1:7778/namf-mt/v1/ue-contexts/nonexistent", 200))
        << "amf never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const std::string ue_context_id = "imsi-999700000000301";

    sbi_core::http2::ClientRequest get_req;
    get_req.method = "GET";
    get_req.url = "https://127.0.0.1:7778/namf-mt/v1/ue-contexts/" + ue_context_id;
    get_req.headers.emplace("authorization", "Bearer " + token);
    auto missing_resp = client.send(get_req);
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

    auto found_resp = client.send(get_req);
    ASSERT_TRUE(found_resp.has_value());
    EXPECT_EQ(found_resp->status, 200);
    const auto ctx_info = json::parse(found_resp->body).get<sbi_gen::UeContextInfo>();
    EXPECT_FALSE(ctx_info.ratType.has_value());
    EXPECT_FALSE(ctx_info.supportVoPS.has_value());
}

TEST(AmfMtIntegration, EnableUeReachability404ThenUnreachableWithoutNgap) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess amf(AMF_PATH);
    ASSERT_GT(amf.pid(), 0) << "failed to fork amf";

    auto client = make_client();
    ASSERT_TRUE(
        wait_reachable(client, "https://127.0.0.1:7778/namf-mt/v1/ue-contexts/nonexistent", 200))
        << "amf never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const std::string ue_context_id = "imsi-999700000000302";

    sbi_core::http2::ClientRequest reach_req;
    reach_req.method = "PUT";
    reach_req.url =
        "https://127.0.0.1:7778/namf-mt/v1/ue-contexts/" + ue_context_id + "/ue-reachind";
    reach_req.headers.emplace("content-type", "application/json");
    reach_req.headers.emplace("authorization", "Bearer " + token);
    reach_req.body = json{{"reachability", "REACHABLE"}}.dump();
    auto missing_resp = client.send(reach_req);
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

    auto found_resp = client.send(reach_req);
    ASSERT_TRUE(found_resp.has_value());
    ASSERT_EQ(found_resp->status, 200);
    const auto ack = json::parse(found_resp->body).get<sbi_gen::EnableUeReachabilityRspData>();
    // ADR-0486 increment 4: the context exists but the UE has no live NGAP association (it was
    // created over SBI only), and this AMF does not page -- so UNREACHABLE, not an echo of the
    // requested REACHABLE. The CM-CONNECTED REACHABLE case is covered by
    // tests/integration/test_amf_sms_registration.cpp's MT SMS test.
    EXPECT_EQ(ack.reachability.value, "UNREACHABLE");
}

TEST(AmfMtIntegration, EnableGroupReachabilityReturnsHonestlyEmptyList) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess amf(AMF_PATH);
    ASSERT_GT(amf.pid(), 0) << "failed to fork amf";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7778/namf-mt/v1/ue-contexts/enable-group-reachability", 50))
        << "amf never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const json body =
        json{{"ueInfoList", json::array({json{{"ueList", json::array({"imsi-999700000000303"})}}})},
             {"tmgi",
              json{{"mbsServiceId", "000001"}, {"plmnId", json{{"mcc", "999"}, {"mnc", "70"}}}}}};

    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = "https://127.0.0.1:7778/namf-mt/v1/ue-contexts/enable-group-reachability";
    req.headers.emplace("content-type", "application/json");
    req.headers.emplace("authorization", "Bearer " + token);
    req.body = body.dump();
    auto resp = client.send(req);
    ASSERT_TRUE(resp.has_value());
    ASSERT_EQ(resp->status, 200);
    const auto result = json::parse(resp->body).get<sbi_gen::EnableGroupReachabilityRspData>();
    EXPECT_FALSE(result.ueConnectedList.has_value());

    // Missing required field (tmgi) must 400.
    json bad_body = body;
    bad_body.erase("tmgi");
    sbi_core::http2::ClientRequest bad_req;
    bad_req.method = "POST";
    bad_req.url = "https://127.0.0.1:7778/namf-mt/v1/ue-contexts/enable-group-reachability";
    bad_req.headers.emplace("content-type", "application/json");
    bad_req.headers.emplace("authorization", "Bearer " + token);
    bad_req.body = bad_body.dump();
    auto bad_resp = client.send(bad_req);
    ASSERT_TRUE(bad_resp.has_value());
    EXPECT_EQ(bad_resp->status, 400);
}
