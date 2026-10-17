// Drives nrf and udm as real, separate OS processes to exercise UDM's own Nudm_UECM bare
// aggregate GET (ADR-0220, docs/CAPABILITY_GAP_ANALYSIS.md's own UDM audit): GetRegistrations,
// composing RegistrationDataSets from the real per-group stores built across ADR-0215 through
// ADR-0219 -- over real TLS 1.3 + mTLS HTTP/2 with a real signed OAuth2 token, per
// TS29503_Nudm_UECM.yaml. Seeds AMF_3GPP (single-object store), SMSF_3GPP (single-object store),
// and NWDAF (list store) real sub-resources, then verifies the aggregate GET both composes all
// three when requested and correctly narrows to a subset via the real `registration-dataset-
// names` query param -- not a fabricated success, an actual selection check. Also verifies the
// real spec's own `minItems: 2` on `registration-dataset-names` is enforced as a real `400`.

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

TEST(UdmGetRegistrationsGapClosureIntegration, ComposesAndFiltersRealRegistrationDataSets) {
    auto d = spawn_nrf_udm();
    auto client = make_client();
    const std::string ue_id = "imsi-999700000000001";
    const std::string base = "https://127.0.0.1:7780/nudm-uecm/v1/" + ue_id;
    ASSERT_TRUE(wait_reachable(client, base + "/registrations/amf-3gpp-access", 200))
        << "udm never became reachable";

    const std::string token = fetch_token(client, "nudm-uecm");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // Seed AMF_3GPP.
    sbi_gen::Amf3GppAccessRegistration amf_data{};
    amf_data.amfInstanceId = "00000000-0000-4000-8000-000000000aaa";
    amf_data.deregCallbackUri = "https://example.com/dereg";
    amf_data.guami.plmnId.mcc = "999";
    amf_data.guami.plmnId.mnc = "70";
    amf_data.guami.amfId = "ABCDEF";
    amf_data.ratType = "NR";
    sbi_core::http2::ClientRequest amf_put;
    amf_put.method = "PUT";
    amf_put.url = base + "/registrations/amf-3gpp-access";
    amf_put.headers.emplace("content-type", "application/json");
    amf_put.headers.emplace("authorization", "Bearer " + token);
    amf_put.body = json(amf_data).dump();
    auto amf_put_resp = client.send(amf_put);
    ASSERT_TRUE(amf_put_resp.has_value());
    ASSERT_EQ(amf_put_resp->status, 201) << amf_put_resp->body;

    // Seed SMSF_3GPP.
    sbi_gen::SmsfRegistration smsf_data{};
    smsf_data.smsfInstanceId = "00000000-0000-4000-8000-000000000ddd";
    smsf_data.plmnId.mcc = "999";
    smsf_data.plmnId.mnc = "70";
    sbi_core::http2::ClientRequest smsf_put;
    smsf_put.method = "PUT";
    smsf_put.url = base + "/registrations/smsf-3gpp-access";
    smsf_put.headers.emplace("content-type", "application/json");
    smsf_put.headers.emplace("authorization", "Bearer " + token);
    smsf_put.body = json(smsf_data).dump();
    auto smsf_put_resp = client.send(smsf_put);
    ASSERT_TRUE(smsf_put_resp.has_value());
    ASSERT_EQ(smsf_put_resp->status, 201) << smsf_put_resp->body;

    // Seed NWDAF.
    sbi_gen::NwdafRegistration nwdaf_data{};
    nwdaf_data.nwdafInstanceId = "00000000-0000-4000-8000-000000000eee";
    sbi_gen::EventId analytics_id{};
    analytics_id.value = sbi_gen::EventId::NF_LOAD;
    nwdaf_data.analyticsIds = {analytics_id};
    sbi_core::http2::ClientRequest nwdaf_put;
    nwdaf_put.method = "PUT";
    nwdaf_put.url = base + "/registrations/nwdaf-registrations/nwdaf-reg-1";
    nwdaf_put.headers.emplace("content-type", "application/json");
    nwdaf_put.headers.emplace("authorization", "Bearer " + token);
    nwdaf_put.body = json(nwdaf_data).dump();
    auto nwdaf_put_resp = client.send(nwdaf_put);
    ASSERT_TRUE(nwdaf_put_resp.has_value());
    ASSERT_EQ(nwdaf_put_resp->status, 201) << nwdaf_put_resp->body;

    // Real spec's own minItems: 2 -- a single dataset name is a real 400.
    sbi_core::http2::ClientRequest one_name_req;
    one_name_req.method = "GET";
    one_name_req.url = base + "/registrations?registration-dataset-names=AMF_3GPP";
    one_name_req.headers.emplace("authorization", "Bearer " + token);
    auto one_name_resp = client.send(one_name_req);
    ASSERT_TRUE(one_name_resp.has_value());
    EXPECT_EQ(one_name_resp->status, 400) << one_name_resp->body;

    // Request all three seeded datasets plus one unseeded (AMF_NON_3GPP) -- confirms composition
    // and that an unseeded dataset is simply omitted, not a hard failure.
    sbi_core::http2::ClientRequest all_req;
    all_req.method = "GET";
    all_req.url = base + "/registrations?registration-dataset-names=AMF_3GPP,SMSF_3GPP,NWDAF,"
                         "AMF_NON_3GPP";
    all_req.headers.emplace("authorization", "Bearer " + token);
    auto all_resp = client.send(all_req);
    ASSERT_TRUE(all_resp.has_value());
    ASSERT_EQ(all_resp->status, 200) << all_resp->body;
    auto all_body = json::parse(all_resp->body);
    EXPECT_TRUE(all_body.contains("amf3Gpp"));
    EXPECT_EQ(all_body.at("amf3Gpp").at("amfInstanceId").get<std::string>(),
              "00000000-0000-4000-8000-000000000aaa");
    EXPECT_TRUE(all_body.contains("smsf3Gpp"));
    EXPECT_EQ(all_body.at("smsf3Gpp").at("smsfInstanceId").get<std::string>(),
              "00000000-0000-4000-8000-000000000ddd");
    EXPECT_TRUE(all_body.contains("nwdafRegistration"));
    EXPECT_EQ(all_body.at("nwdafRegistration").at("nwdafRegistrationList").size(), 1u);
    EXPECT_FALSE(all_body.contains("amfNon3Gpp"));
    EXPECT_FALSE(all_body.contains("ipSmGw"));

    // Narrow the same request to just two of the three seeded datasets -- real filtering, not
    // just aggregation.
    sbi_core::http2::ClientRequest narrow_req;
    narrow_req.method = "GET";
    narrow_req.url = base + "/registrations?registration-dataset-names=SMSF_3GPP,NWDAF";
    narrow_req.headers.emplace("authorization", "Bearer " + token);
    auto narrow_resp = client.send(narrow_req);
    ASSERT_TRUE(narrow_resp.has_value());
    ASSERT_EQ(narrow_resp->status, 200) << narrow_resp->body;
    auto narrow_body = json::parse(narrow_resp->body);
    EXPECT_FALSE(narrow_body.contains("amf3Gpp"));
    EXPECT_TRUE(narrow_body.contains("smsf3Gpp"));
    EXPECT_TRUE(narrow_body.contains("nwdafRegistration"));
}
