// Pins the AMF<->SMF contract that ADR-0258 made AMF depend on: SMF's answer to
// Nsmf_PDUSession_UpdateSMContext with n2SmInfoType=HANDOVER_REQUIRED (TS 23.502 §4.9.1.3 step 3).
//
// Why this test exists. ADR-0249 built SMF's HANDOVER_REQUIRED answer expressly so that AMF could
// stop fabricating a TEID=0/0.0.0.0 handover transfer -- but nothing called it, and nothing tested
// it. ADR-0258 wired AMF to call it and DELETED the fabrication, so AMF now hard-depends on the
// exact shape asserted below. Without this test that dependency is only a comment.
//
// Honest scope limit, stated rather than implied: this drives SMF's side over real SBI. It does
// NOT drive AMF's NGAP side. UERANSIM (ADR-0016) is vendored and drives real NGAP, but its gNB
// implements no handover procedure at all (HandoverRequired appears in its generated ASN.1 only,
// never in src/gnb/), so the whole NGAP handover chain (ADR-0095/0096/0248/0249/0258) still has no
// end-to-end test. See ADR-0258.

#include "sbi_core/http2_client.hpp"
#include "sbi_core/multipart.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <optional>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "spawn_guard.hpp"

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
    req.body = "grant_type=client_credentials&nfInstanceId=test-client&scope=nsmf-pdusession&"
               "targetNfType=SMF";
    auto resp = client.send(req);
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

TEST(SmfHandoverN2SmInfoIntegration, HandoverRequiredReturnsRealUpfTunnelNotAPlaceholder) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // upf is spawned deliberately: the real UPF N3 uplink F-TEID SMF must answer with only exists
    // once a real PFCP N4 session has been established against a real UPF.
    nf_test::SpawnedProcess upf(UPF_PATH);
    ASSERT_GT(upf.pid(), 0) << "failed to fork upf";
    nf_test::SpawnedProcess smf(SMF_PATH);
    ASSERT_GT(smf.pid(), 0) << "failed to fork smf";
    nf_test::SpawnedProcess pcf(PCF_PATH);
    ASSERT_GT(pcf.pid(), 0) << "failed to fork pcf";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts/nonexistent/retrieve", 80))
        << "smf never became reachable";
    // CreateSMContext needs PCF (SM Policy Association) and does not retry it, so a run where PCF
    // came up after SMF answered 500 "could not reach PCF" (seen in CI's ASan job, 2026-10-05).
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7783/npcf-am-policy-control/v1/policies/none", 80))
        << "pcf never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const std::string supi = "imsi-999700000000042";

    // SMF only gets a real N3 uplink F-TEID once it has DISCOVERED UPF through NRF and completed
    // a PFCP Sx Association -- SMF's own retry loop, on a 2s cadence. A session created before
    // that association exists carries no ulTeid/ulIpv4, and HANDOVER_REQUIRED then correctly
    // answers 500 rather than fabricating a tunnel. So this retries the whole create+prepare pair
    // until the association is up, instead of sleeping a guessed interval; a genuine regression
    // still fails, with the last response body reported.
    std::optional<sbi_core::http2::ClientResponse> ho_resp;
    std::string last_failure;
    for (int attempt = 0; attempt < 40; ++attempt) {
        const auto encoded = encode_create_sm_context_body(supi, 5 + attempt);
        sbi_core::http2::ClientRequest create_req;
        create_req.method = "POST";
        create_req.url = "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts";
        create_req.headers.emplace("content-type", encoded.content_type_header);
        create_req.headers.emplace("authorization", "Bearer " + token);
        create_req.body = encoded.body;
        auto create_resp = client.send(create_req);
        ASSERT_TRUE(create_resp.has_value());
        ASSERT_EQ(create_resp->status, 201) << create_resp->body;

        const auto location_it = create_resp->headers.find("location");
        ASSERT_NE(location_it, create_resp->headers.end())
            << "TS 29.502 requires Location on the 201 -- AMF derives its smContextRef from it";
        const std::string location = location_it->second;
        const std::string sm_context_ref = location.substr(location.rfind('/') + 1);
        ASSERT_FALSE(sm_context_ref.empty());

        sbi_core::http2::ClientRequest ho_req;
        ho_req.method = "POST";
        ho_req.url =
            "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts/" + sm_context_ref + "/modify";
        ho_req.headers.emplace("content-type", "application/json");
        ho_req.headers.emplace("authorization", "Bearer " + token);
        // Plain JSON, no N2 SM info: exactly what AMF sends for this branch (ADR-0258). If SMF
        // ever starts requiring multipart here, AMF breaks and this assertion is what says so.
        ho_req.body = json{{"n2SmInfoType", "HANDOVER_REQUIRED"}}.dump();
        auto resp = client.send(ho_req);
        ASSERT_TRUE(resp.has_value());
        if (resp->status == 200) {
            ho_resp = std::move(*resp);
            break;
        }
        last_failure = std::to_string(resp->status) + " " + resp->body;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    ASSERT_TRUE(ho_resp.has_value())
        << "SMF never answered HANDOVER_REQUIRED with a real transfer; last: " << last_failure;

    const auto ct = ho_resp->headers.find("content-type");
    ASSERT_NE(ct, ho_resp->headers.end());
    ASSERT_TRUE(sbi_core::multipart::is_multipart_related(ct->second))
        << "AMF parses this answer as multipart/related; got: " << ct->second;

    auto parts = sbi_core::multipart::parse(ct->second, ho_resp->body);
    ASSERT_TRUE(parts.has_value()) << parts.error();
    ASSERT_GE(parts->size(), 2u) << "expected a jsonData root part plus a binary n2SmInfo part";

    const auto root = json::parse((*parts)[0].body);
    // AMF relays this transfer to the TARGET gNB as a PDUSessionResourceSetupRequestTransfer, so
    // the declared type must stay PDU_RES_SETUP_REQ.
    ASSERT_TRUE(root.contains("n2SmInfoType"));
    EXPECT_EQ(root.at("n2SmInfoType").get<std::string>(), "PDU_RES_SETUP_REQ");
    ASSERT_TRUE(root.contains("n2SmInfo"));
    ASSERT_TRUE(root.at("n2SmInfo").contains("contentId"));
    const auto content_id = root.at("n2SmInfo").at("contentId").get<std::string>();

    // AMF resolves the binary part by contentId rather than by position -- assert that is possible.
    const auto bin = std::find_if(parts->begin(), parts->end(), [&](const auto& p) {
        return p.content_id.has_value() && *p.content_id == content_id;
    });
    ASSERT_NE(bin, parts->end()) << "no part carried contentId '" << content_id << "'";
    ASSERT_FALSE(bin->body.empty()) << "the PER-encoded transfer must not be empty";

    // The whole point of ADR-0249/0258: this is a REAL tunnel, not the deleted placeholder. The
    // placeholder encoded TEID=0 and IP=0.0.0.0, i.e. eight consecutive zero bytes inside an
    // otherwise small structure. An all-zero payload would mean the fabrication came back.
    const bool all_zero =
        std::all_of(bin->body.begin(), bin->body.end(), [](char c) { return c == '\0'; });
    EXPECT_FALSE(all_zero) << "transfer is entirely zero bytes -- that is the placeholder shape";
}

TEST(SmfHandoverN2SmInfoIntegration, HandoverRequiredOnUnknownContextIs404) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    nf_test::SpawnedProcess smf(SMF_PATH);
    ASSERT_GT(smf.pid(), 0) << "failed to fork smf";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts/nonexistent/retrieve", 80))
        << "smf never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    sbi_core::http2::ClientRequest ho_req;
    ho_req.method = "POST";
    ho_req.url = "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts/no-such-ref/modify";
    ho_req.headers.emplace("content-type", "application/json");
    ho_req.headers.emplace("authorization", "Bearer " + token);
    ho_req.body = json{{"n2SmInfoType", "HANDOVER_REQUIRED"}}.dump();
    auto ho_resp = client.send(ho_req);
    ASSERT_TRUE(ho_resp.has_value());
    // AMF treats any non-200 as "this session cannot be prepared" and skips it rather than
    // fabricating a tunnel -- so a clean 404 here is load-bearing, not cosmetic.
    EXPECT_EQ(ho_resp->status, 404) << ho_resp->body;
}
