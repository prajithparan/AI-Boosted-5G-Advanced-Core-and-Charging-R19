// Drives nrf, smf (and, for the NIDD test that needs a real live SM context, pcf) as real,
// separate OS processes to exercise smf's TS29508_Nsmf_EventExposure.yaml and
// TS29542_Nsmf_NIDD.yaml routes (ADR-0201) over real TLS 1.3 + mTLS.
//
// Covers: Nsmf_EventExposure's full real create/get/replace/delete subscription lifecycle, and
// Nsmf_NIDD's Deliver real 404 (no matching SM context) vs. real 204 (a real SM context created
// via a real, PCF-backed CreateSMContext -- same real dependency chain as
// test_smf_pdu_session.cpp's own PCF-backed tests).

#include "sbi_core/http2_client.hpp"
#include "sbi_core/multipart.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "TS26510_CommonData_grp.hpp"
#include "TS29542_Nsmf_NIDD.hpp"
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

bool wait_reachable(sbi_core::http2::Client& client,
                    const std::string& url,
                    const std::string& method,
                    int max_attempts) {
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        sbi_core::http2::ClientRequest req;
        req.method = method;
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
        "&scope=" + scope + "&targetNfType=SMF";
    auto resp = nf_test::send_token_request(req);
    if (!resp.has_value() || resp->status != 200) {
        return "";
    }
    return json::parse(resp->body).at("access_token").get<std::string>();
}

sbi_core::multipart::Encoded encode_create_sm_context_body(const std::string& supi,
                                                           std::int64_t pdu_session_id) {
    sbi_core::multipart::Part create_json_part;
    create_json_part.content_type = "application/json";
    create_json_part.body =
        json{
            {"servingNfId", "00000000-0000-4000-8000-0000000000aa"},
            {"servingNetwork", json{{"mcc", "999"}, {"mnc", "70"}}},
            {"anType", "3GPP_ACCESS"},
            {"smContextStatusUri", "https://example.com/sm-status"},
            {"supi", supi},
            {"pduSessionId", pdu_session_id},
            {"dnn", "internet"},
            {"sNssai", json{{"sst", 1}}},
        }
            .dump();
    return sbi_core::multipart::encode({create_json_part});
}

} // namespace

TEST(SmfEventExposureIntegration, CreateGetReplaceDeleteLifecycle) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess smf(SMF_PATH);
    ASSERT_GT(smf.pid(), 0) << "failed to fork smf";

    auto client = make_client();
    ASSERT_TRUE(
        wait_reachable(client,
                       "https://127.0.0.1:7779/nsmf-event-exposure/v1/subscriptions/nonexistent",
                       "GET",
                       50))
        << "smf never became reachable";

    const std::string token = fetch_token(client, "nsmf-event-exposure");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const json create_body = json{{"notifId", "notif-1"},
                                  {"notifUri", "https://example.com/smf-evt-notify"},
                                  {"eventSubs", json::array({json{{"event", "PDU_SES_REL"}}})}};

    sbi_core::http2::ClientRequest create_req;
    create_req.method = "POST";
    create_req.url = "https://127.0.0.1:7779/nsmf-event-exposure/v1/subscriptions";
    create_req.headers.emplace("content-type", "application/json");
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = create_body.dump();
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    ASSERT_EQ(create_resp->status, 201);
    auto loc = create_resp->headers.find("location");
    ASSERT_NE(loc, create_resp->headers.end());
    EXPECT_NE(loc->second.find("/nsmf-event-exposure/v1/subscriptions/"), std::string::npos);
    const auto created = json::parse(create_resp->body).get<sbi_gen::NsmfEventExposure>();
    ASSERT_TRUE(created.subId.has_value());
    EXPECT_EQ(created.notifId, "notif-1");
    const auto sub_id = *created.subId;

    sbi_core::http2::ClientRequest get_req;
    get_req.method = "GET";
    get_req.url = "https://127.0.0.1:7779/nsmf-event-exposure/v1/subscriptions/" + sub_id;
    get_req.headers.emplace("authorization", "Bearer " + token);
    auto get_resp = client.send(get_req);
    ASSERT_TRUE(get_resp.has_value());
    ASSERT_EQ(get_resp->status, 200);
    const auto fetched = json::parse(get_resp->body).get<sbi_gen::NsmfEventExposure>();
    EXPECT_EQ(fetched.notifId, "notif-1");

    sbi_core::http2::ClientRequest missing_get_req = get_req;
    missing_get_req.url =
        "https://127.0.0.1:7779/nsmf-event-exposure/v1/subscriptions/nonexistent-id";
    auto missing_get_resp = client.send(missing_get_req);
    ASSERT_TRUE(missing_get_resp.has_value());
    EXPECT_EQ(missing_get_resp->status, 404);

    const json replace_body = json{{"notifId", "notif-2"},
                                   {"notifUri", "https://example.com/smf-evt-notify-2"},
                                   {"eventSubs", json::array({json{{"event", "UE_IP_CH"}}})}};
    sbi_core::http2::ClientRequest replace_req;
    replace_req.method = "PUT";
    replace_req.url = "https://127.0.0.1:7779/nsmf-event-exposure/v1/subscriptions/" + sub_id;
    replace_req.headers.emplace("content-type", "application/json");
    replace_req.headers.emplace("authorization", "Bearer " + token);
    replace_req.body = replace_body.dump();
    auto replace_resp = client.send(replace_req);
    ASSERT_TRUE(replace_resp.has_value());
    ASSERT_EQ(replace_resp->status, 200);
    const auto replaced = json::parse(replace_resp->body).get<sbi_gen::NsmfEventExposure>();
    EXPECT_EQ(replaced.notifId, "notif-2");

    sbi_core::http2::ClientRequest delete_req;
    delete_req.method = "DELETE";
    delete_req.url = "https://127.0.0.1:7779/nsmf-event-exposure/v1/subscriptions/" + sub_id;
    delete_req.headers.emplace("authorization", "Bearer " + token);
    auto delete_resp = client.send(delete_req);
    ASSERT_TRUE(delete_resp.has_value());
    EXPECT_EQ(delete_resp->status, 204);

    auto delete_again_resp = client.send(delete_req);
    ASSERT_TRUE(delete_again_resp.has_value());
    EXPECT_EQ(delete_again_resp->status, 404);
}

TEST(SmfEventExposureIntegration, CreateWithMissingRequiredFieldIs400) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess smf(SMF_PATH);
    ASSERT_GT(smf.pid(), 0) << "failed to fork smf";

    auto client = make_client();
    ASSERT_TRUE(
        wait_reachable(client,
                       "https://127.0.0.1:7779/nsmf-event-exposure/v1/subscriptions/nonexistent",
                       "GET",
                       50))
        << "smf never became reachable";

    const std::string token = fetch_token(client, "nsmf-event-exposure");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // eventSubs is required and missing.
    const json bad_body =
        json{{"notifId", "notif-3"}, {"notifUri", "https://example.com/smf-evt-notify"}};
    sbi_core::http2::ClientRequest bad_req;
    bad_req.method = "POST";
    bad_req.url = "https://127.0.0.1:7779/nsmf-event-exposure/v1/subscriptions";
    bad_req.headers.emplace("content-type", "application/json");
    bad_req.headers.emplace("authorization", "Bearer " + token);
    bad_req.body = bad_body.dump();
    auto bad_resp = client.send(bad_req);
    ASSERT_TRUE(bad_resp.has_value());
    EXPECT_EQ(bad_resp->status, 400);
}

TEST(SmfNiddIntegration, DeliverMissingContextIs404ThenRealContextIs204) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess smf(SMF_PATH);
    ASSERT_GT(smf.pid(), 0) << "failed to fork smf";

    nf_test::SpawnedProcess pcf(PCF_PATH);
    ASSERT_GT(pcf.pid(), 0) << "failed to fork pcf";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7779/nsmf-nidd/v1/pdu-sessions/nonexistent/deliver", "POST", 50))
        << "smf never became reachable";

    const std::string token = fetch_token(client, "nsmf-nidd");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    sbi_core::multipart::Part deliver_json_part;
    deliver_json_part.content_type = "application/json";
    deliver_json_part.body = json{{"mtData", json{{"contentId", "nidd-part-1"}}}}.dump();
    const auto deliver_encoded = sbi_core::multipart::encode({deliver_json_part});

    sbi_core::http2::ClientRequest missing_req;
    missing_req.method = "POST";
    missing_req.url = "https://127.0.0.1:7779/nsmf-nidd/v1/pdu-sessions/nonexistent-ref/deliver";
    missing_req.headers.emplace("content-type", deliver_encoded.content_type_header);
    missing_req.headers.emplace("authorization", "Bearer " + token);
    missing_req.body = deliver_encoded.body;
    auto missing_resp = client.send(missing_req);
    ASSERT_TRUE(missing_resp.has_value());
    EXPECT_EQ(missing_resp->status, 404);

    const std::string pcf_token = fetch_token(client, "npcf-smpolicycontrol");
    ASSERT_FALSE(pcf_token.empty()) << "failed to obtain OAuth2 token for pcf scope";
    const std::string smf_token = fetch_token(client, "nsmf-pdusession");
    ASSERT_FALSE(smf_token.empty()) << "failed to obtain OAuth2 token for smf scope";

    const auto create_encoded = encode_create_sm_context_body("imsi-999700000000401", 5);
    sbi_core::http2::ClientRequest create_req;
    create_req.method = "POST";
    create_req.url = "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts";
    create_req.headers.emplace("content-type", create_encoded.content_type_header);
    create_req.headers.emplace("authorization", "Bearer " + smf_token);
    create_req.body = create_encoded.body;
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    ASSERT_EQ(create_resp->status, 201) << create_resp->body;
    auto loc = create_resp->headers.find("location");
    ASSERT_NE(loc, create_resp->headers.end());
    const auto sm_context_ref = loc->second.substr(loc->second.rfind('/') + 1);

    sbi_core::http2::ClientRequest deliver_req;
    deliver_req.method = "POST";
    deliver_req.url =
        "https://127.0.0.1:7779/nsmf-nidd/v1/pdu-sessions/" + sm_context_ref + "/deliver";
    deliver_req.headers.emplace("content-type", deliver_encoded.content_type_header);
    deliver_req.headers.emplace("authorization", "Bearer " + token);
    deliver_req.body = deliver_encoded.body;
    auto deliver_resp = client.send(deliver_req);
    ASSERT_TRUE(deliver_resp.has_value());
    EXPECT_EQ(deliver_resp->status, 204);
}
