// Drives nrf, smf, and (for the tests that need it) pcf as real, separate OS processes: nrf issues
// real signed OAuth2 tokens, smf registers with nrf on its own background thread (see
// nfs/smf/src/main.cpp's run_nrf_lifecycle, docs/DECISIONS.md ADR-0006/ADR-0019/ADR-0020), then
// this test acts as an SBI client exercising smf's /sm-contexts routes over real TLS 1.3 + mTLS
// HTTP/2, including a real multipart/related CreateSMContext request (the actual AMF-triggered
// PDU Session Establishment trigger, TS 23.502 clause 4.3.2.2.1 -- CLAUDE.md's stated Phase 2
// end-state goal). This turn (ADR-0029) also proves smf really calls a real pcf process for SM
// Policy Association Establishment/Termination -- not just that CreateSMContext returns 201.

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
        req.method = "POST";
        req.url = url;
        if (client.send(req).has_value()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

std::string fetch_token(sbi_core::http2::Client& client) {
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = "https://127.0.0.1:7777/oauth2/token";
    req.headers.emplace("content-type", "application/x-www-form-urlencoded");
    req.body =
        "grant_type=client_credentials&nfInstanceId=" + std::string(nf_test::kTestAfInstanceId) +
        "&scope=nsmf-pdusession&"
        "targetNfType=SMF";
    auto resp = nf_test::send_token_request(req);
    if (!resp.has_value() || resp->status != 200) {
        return "";
    }
    return json::parse(resp->body).at("access_token").get<std::string>();
}

std::string fetch_pcf_token(sbi_core::http2::Client& client) {
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = "https://127.0.0.1:7777/oauth2/token";
    req.headers.emplace("content-type", "application/x-www-form-urlencoded");
    req.body =
        "grant_type=client_credentials&nfInstanceId=" + std::string(nf_test::kTestAfInstanceId) +
        "&scope=npcf-smpolicycontrol&"
        "targetNfType=PCF";
    auto resp = nf_test::send_token_request(req);
    if (!resp.has_value() || resp->status != 200) {
        return "";
    }
    return json::parse(resp->body).at("access_token").get<std::string>();
}

// A real, mandatory-field-complete CreateSMContext multipart body -- shared by every test below
// that needs SMF to actually reach PCF (see docs/DECISIONS.md ADR-0029: supi/pduSessionId/dnn/
// sNssai are all required in this build even though SmContextCreateData's own schema allows them
// to be absent).
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

TEST(SmfIntegration, FullSmContextLifecycleOverRealHttp2) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess smf(SMF_PATH);
    ASSERT_GT(smf.pid(), 0) << "failed to fork smf";
    nf_test::SpawnedProcess pcf(PCF_PATH);
    ASSERT_GT(pcf.pid(), 0) << "failed to fork pcf";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts/nonexistent/retrieve", 50))
        << "smf never became reachable";
    {
        sbi_core::http2::ClientRequest probe;
        probe.method = "GET";
        probe.url = "https://127.0.0.1:7783/npcf-am-policy-control/v1/policies/nonexistent";
        bool pcf_up = false;
        for (int attempt = 0; attempt < 50 && !pcf_up; ++attempt) {
            pcf_up = client.send(probe).has_value();
            if (!pcf_up) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        ASSERT_TRUE(pcf_up) << "pcf never became reachable";
    }

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";
    const std::string pcf_token = fetch_pcf_token(client);
    ASSERT_FALSE(pcf_token.empty()) << "failed to obtain a PCF-scoped OAuth2 token from nrf";

    // CreateSMContext: multipart/related-only per spec (docs/DECISIONS.md ADR-0020). This turn
    // (ADR-0029) SMF really calls PCF's CreateSMPolicy from this handler -- supi/pduSessionId/dnn/
    // sNssai are all required in this build for that call to have something to send.
    const std::string supi = "imsi-999700000000001";
    const auto encoded = encode_create_sm_context_body(supi, 5);

    sbi_core::http2::ClientRequest create_req;
    create_req.method = "POST";
    create_req.url = "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts";
    create_req.headers.emplace("content-type", encoded.content_type_header);
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = encoded.body;

    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    ASSERT_EQ(create_resp->status, 201);
    const auto created =
        json::parse(create_resp->body).get<sbi_gen::SmContextCreatedData_Nsmf_PDUSession>();
    (void)created; // proves real deserialization, not just a 2xx status
    const auto location_it = create_resp->headers.find("location");
    ASSERT_NE(location_it, create_resp->headers.end());
    const std::string location = location_it->second;
    const auto ref_pos = location.rfind('/');
    ASSERT_NE(ref_pos, std::string::npos);
    const std::string sm_context_ref = location.substr(ref_pos + 1);
    EXPECT_FALSE(sm_context_ref.empty());

    // Not just "SMF returned 201" -- confirm SMF really called PCF: query PCF directly for the SM
    // Policy it should have created. SmContextCreatedData has no field for the smPolicyId (see
    // nfs/smf/src/main.cpp's file header), so this relies on PCF's SmPolicyStore assigning ids
    // sequentially from a freshly-spawned process -- "smpolicy-1" is the first (and, in this test,
    // only) CreateSMPolicy call PCF will have received.
    {
        sbi_core::http2::ClientRequest pcf_get_req;
        pcf_get_req.method = "GET";
        pcf_get_req.url = "https://127.0.0.1:7783/npcf-smpolicycontrol/v1/sm-policies/smpolicy-1";
        pcf_get_req.headers.emplace("authorization", "Bearer " + pcf_token);
        auto pcf_get_resp = client.send(pcf_get_req);
        ASSERT_TRUE(pcf_get_resp.has_value());
        ASSERT_EQ(pcf_get_resp->status, 200) << "smf did not really create an SM Policy at pcf";
        const auto pcf_control = json::parse(pcf_get_resp->body).get<sbi_gen::SmPolicyControl>();
        EXPECT_EQ(pcf_control.context.supi, supi);
        EXPECT_EQ(pcf_control.context.dnn, "internet");
        ASSERT_TRUE(pcf_control.policy.sessRules.has_value());
        EXPECT_TRUE(pcf_control.policy.sessRules->contains("default"));
    }

    // RetrieveSMContext: request body is optional per spec -- send none.
    sbi_core::http2::ClientRequest retrieve_req;
    retrieve_req.method = "POST";
    retrieve_req.url =
        "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts/" + sm_context_ref + "/retrieve";
    retrieve_req.headers.emplace("authorization", "Bearer " + token);
    auto retrieve_resp = client.send(retrieve_req);
    ASSERT_TRUE(retrieve_resp.has_value());
    EXPECT_EQ(retrieve_resp->status, 200);
    const auto retrieved = json::parse(retrieve_resp->body).get<sbi_gen::SmContextRetrievedData>();
    (void)retrieved;

    // UpdateSMContext.
    sbi_core::http2::ClientRequest update_req;
    update_req.method = "POST";
    update_req.url =
        "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts/" + sm_context_ref + "/modify";
    update_req.headers.emplace("content-type", "application/json");
    update_req.headers.emplace("authorization", "Bearer " + token);
    update_req.body = json{{"upCnxState", "ACTIVATED"}}.dump();
    auto update_resp = client.send(update_req);
    ASSERT_TRUE(update_resp.has_value());
    EXPECT_EQ(update_resp->status, 204);

    // ReleaseSMContext, then confirm the context is really gone (second release -> 404).
    sbi_core::http2::ClientRequest release_req;
    release_req.method = "POST";
    release_req.url =
        "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts/" + sm_context_ref + "/release";
    release_req.headers.emplace("authorization", "Bearer " + token);
    auto release_resp = client.send(release_req);
    ASSERT_TRUE(release_resp.has_value());
    EXPECT_EQ(release_resp->status, 204);

    auto release_again = client.send(release_req);
    ASSERT_TRUE(release_again.has_value());
    EXPECT_EQ(release_again->status, 404);

    // Confirm ReleaseSMContext really called PCF's DeleteSMPolicy too, not just cleaned up
    // locally: the SM Policy at PCF must be gone now.
    {
        sbi_core::http2::ClientRequest pcf_get_req;
        pcf_get_req.method = "GET";
        pcf_get_req.url = "https://127.0.0.1:7783/npcf-smpolicycontrol/v1/sm-policies/smpolicy-1";
        pcf_get_req.headers.emplace("authorization", "Bearer " + pcf_token);
        auto pcf_get_resp = client.send(pcf_get_req);
        ASSERT_TRUE(pcf_get_resp.has_value());
        EXPECT_EQ(pcf_get_resp->status, 404)
            << "smf did not really call DeleteSMPolicy at pcf on release";
    }
}

TEST(SmfIntegration, CreateSMContextRequiresSupiPduSessionIdDnnAndSNssai) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess smf(SMF_PATH);
    ASSERT_GT(smf.pid(), 0) << "failed to fork smf";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts/nonexistent/retrieve", 50))
        << "smf never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // Same as ADR-0020's original mandatory-only body -- none of ADR-0029's PCF-required fields
    // (supi/pduSessionId/dnn/sNssai) are present.
    sbi_core::multipart::Part create_json_part;
    create_json_part.content_type = "application/json";
    create_json_part.body =
        json{
            {"servingNfId", "00000000-0000-4000-8000-0000000000aa"},
            {"servingNetwork", json{{"mcc", "999"}, {"mnc", "70"}}},
            {"anType", "3GPP_ACCESS"},
            {"smContextStatusUri", "https://example.com/sm-status"},
        }
            .dump();
    const auto encoded = sbi_core::multipart::encode({create_json_part});

    sbi_core::http2::ClientRequest create_req;
    create_req.method = "POST";
    create_req.url = "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts";
    create_req.headers.emplace("content-type", encoded.content_type_header);
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = encoded.body;

    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    EXPECT_EQ(create_resp->status, 400);
}

TEST(SmfIntegration, CreateSMContextFailsClosedWhenPcfUnreachable) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess smf(SMF_PATH);
    ASSERT_GT(smf.pid(), 0) << "failed to fork smf";
    // Deliberately NOT spawning pcf -- proves CreateSMContext really depends on reaching a real
    // PCF rather than the PCF call being a no-op/best-effort like ReleaseSMContext's.

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts/nonexistent/retrieve", 50))
        << "smf never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const auto encoded = encode_create_sm_context_body("imsi-999700000000001", 5);

    sbi_core::http2::ClientRequest create_req;
    create_req.method = "POST";
    create_req.url = "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts";
    create_req.headers.emplace("content-type", encoded.content_type_header);
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = encoded.body;

    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    EXPECT_EQ(create_resp->status, 500);
}

TEST(SmfIntegration, MissingSmContextIs404AndTamperedTokenIs401AndNonMultipartCreateIs400) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess smf(SMF_PATH);
    ASSERT_GT(smf.pid(), 0) << "failed to fork smf";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts/nonexistent/retrieve", 50))
        << "smf never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    sbi_core::http2::ClientRequest retrieve_req;
    retrieve_req.method = "POST";
    retrieve_req.url = "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts/nonexistent/retrieve";
    retrieve_req.headers.emplace("authorization", "Bearer " + token);
    auto retrieve_resp = client.send(retrieve_req);
    ASSERT_TRUE(retrieve_resp.has_value());
    EXPECT_EQ(retrieve_resp->status, 404);

    sbi_core::http2::ClientRequest tampered_req;
    tampered_req.method = "POST";
    tampered_req.url = "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts/nonexistent/release";
    tampered_req.headers.emplace("authorization", "Bearer " + token + "tampered");
    auto tampered_resp = client.send(tampered_req);
    ASSERT_TRUE(tampered_resp.has_value());
    EXPECT_EQ(tampered_resp->status, 401);

    // CreateSMContext requires multipart/related -- a plain JSON body must be rejected, not
    // silently accepted.
    sbi_core::http2::ClientRequest wrong_content_type_req;
    wrong_content_type_req.method = "POST";
    wrong_content_type_req.url = "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts";
    wrong_content_type_req.headers.emplace("content-type", "application/json");
    wrong_content_type_req.headers.emplace("authorization", "Bearer " + token);
    wrong_content_type_req.body = json{{"anType", "3GPP_ACCESS"}}.dump();
    auto wrong_content_type_resp = client.send(wrong_content_type_req);
    ASSERT_TRUE(wrong_content_type_resp.has_value());
    EXPECT_EQ(wrong_content_type_resp->status, 400);
}
