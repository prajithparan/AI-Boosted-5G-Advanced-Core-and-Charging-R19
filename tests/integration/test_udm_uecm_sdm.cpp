// Drives nrf and udm as real, separate OS processes: nrf issues real signed OAuth2 tokens, udm
// registers with nrf on its own background thread (see nfs/udm/src/main.cpp's run_nrf_lifecycle,
// docs/DECISIONS.md ADR-0006/ADR-0019), then this test acts as an SBI client exercising udm's
// Nudm_UECM (AMF/SMF registration groups) and Nudm_SDM (subscriber data + subscriptions) routes
// over real TLS 1.3 + mTLS HTTP/2.

#include "sbi_core/http2_client.hpp"

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

} // namespace

TEST(UdmIntegration, AmfRegistrationLifecycle) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess udm(UDM_PATH);
    ASSERT_GT(udm.pid(), 0) << "failed to fork udm";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client,
        "https://127.0.0.1:7780/nudm-uecm/v1/nonexistent/registrations/amf-3gpp-access",
        50))
        << "udm never became reachable";

    const std::string token = fetch_token(client, "nudm-uecm");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const std::string ue_id = "imsi-999700000000001";

    sbi_gen::Amf3GppAccessRegistration create_data{};
    create_data.amfInstanceId = "00000000-0000-4000-8000-000000000aaa";
    create_data.deregCallbackUri = "https://example.com/dereg";
    create_data.guami.plmnId.mcc = "999";
    create_data.guami.plmnId.mnc = "70";
    create_data.guami.amfId = "ABCDEF";
    create_data.ratType = "NR"; // RatType is an opaque nlohmann::json fallback, not a struct

    sbi_core::http2::ClientRequest create_req;
    create_req.method = "PUT";
    create_req.url =
        "https://127.0.0.1:7780/nudm-uecm/v1/" + ue_id + "/registrations/amf-3gpp-access";
    create_req.headers.emplace("content-type", "application/json");
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = json(create_data).dump();
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    EXPECT_EQ(create_resp->status, 201);
    const auto created = json::parse(create_resp->body).get<sbi_gen::Amf3GppAccessRegistration>();
    EXPECT_EQ(created.amfInstanceId, create_data.amfInstanceId);

    // Re-PUT the same ueId: real replace semantics, 200 not 201.
    auto replace_resp = client.send(create_req);
    ASSERT_TRUE(replace_resp.has_value());
    EXPECT_EQ(replace_resp->status, 200);

    sbi_core::http2::ClientRequest get_req;
    get_req.method = "GET";
    get_req.url = create_req.url;
    get_req.headers.emplace("authorization", "Bearer " + token);
    auto get_resp = client.send(get_req);
    ASSERT_TRUE(get_resp.has_value());
    EXPECT_EQ(get_resp->status, 200);

    // Update3GppRegistration: RFC 7396 merge-patch -- only guami changes, rest is untouched.
    sbi_core::http2::ClientRequest patch_req;
    patch_req.method = "PATCH";
    patch_req.url = create_req.url;
    patch_req.headers.emplace("content-type", "application/merge-patch+json");
    patch_req.headers.emplace("authorization", "Bearer " + token);
    patch_req.body =
        json{{"guami", json{{"plmnId", json{{"mcc", "999"}, {"mnc", "70"}}}, {"amfId", "FEDCBA"}}}}
            .dump();
    auto patch_resp = client.send(patch_req);
    ASSERT_TRUE(patch_resp.has_value());
    EXPECT_EQ(patch_resp->status, 200);
    const auto patched = json::parse(patch_resp->body).get<sbi_gen::Amf3GppAccessRegistration>();
    EXPECT_EQ(patched.guami.amfId, "FEDCBA");
    // Merge-patch semantics: fields not mentioned in the patch survive unchanged.
    EXPECT_EQ(patched.amfInstanceId, create_data.amfInstanceId);
    EXPECT_EQ(patched.deregCallbackUri, create_data.deregCallbackUri);

    // deregAMF.
    sbi_core::http2::ClientRequest dereg_req;
    dereg_req.method = "POST";
    dereg_req.url = create_req.url + "/dereg-amf";
    dereg_req.headers.emplace("content-type", "application/json");
    dereg_req.headers.emplace("authorization", "Bearer " + token);
    dereg_req.body = json{{"deregReason", "UE_INITIAL_REGISTRATION"}}.dump();
    auto dereg_resp = client.send(dereg_req);
    ASSERT_TRUE(dereg_resp.has_value());
    EXPECT_EQ(dereg_resp->status, 204);

    // Registration is really gone -- GET must now 404.
    auto get_again = client.send(get_req);
    ASSERT_TRUE(get_again.has_value());
    EXPECT_EQ(get_again->status, 404);
}

TEST(UdmIntegration, SmfRegistrationLifecycle) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess udm(UDM_PATH);
    ASSERT_GT(udm.pid(), 0) << "failed to fork udm";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client,
        "https://127.0.0.1:7780/nudm-uecm/v1/nonexistent/registrations/amf-3gpp-access",
        50))
        << "udm never became reachable";

    const std::string token = fetch_token(client, "nudm-uecm");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const std::string ue_id = "imsi-999700000000002";
    const std::string pdu_session_id = "5";

    sbi_gen::SmfRegistration create_data{};
    create_data.smfInstanceId = "00000000-0000-4000-8000-000000000bbb";
    create_data.pduSessionId = 5;
    create_data.singleNssai.sst = 1;
    create_data.plmnId.mcc = "999";
    create_data.plmnId.mnc = "70";

    sbi_core::http2::ClientRequest create_req;
    create_req.method = "PUT";
    create_req.url = "https://127.0.0.1:7780/nudm-uecm/v1/" + ue_id +
                     "/registrations/smf-registrations/" + pdu_session_id;
    create_req.headers.emplace("content-type", "application/json");
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = json(create_data).dump();
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    EXPECT_EQ(create_resp->status, 201);

    // GetSmfRegistration: the collection GET across all PDU sessions for this ueId.
    sbi_core::http2::ClientRequest list_req;
    list_req.method = "GET";
    list_req.url =
        "https://127.0.0.1:7780/nudm-uecm/v1/" + ue_id + "/registrations/smf-registrations";
    list_req.headers.emplace("authorization", "Bearer " + token);
    auto list_resp = client.send(list_req);
    ASSERT_TRUE(list_resp.has_value());
    EXPECT_EQ(list_resp->status, 200);
    const auto info = json::parse(list_resp->body).get<sbi_gen::SmfRegistrationInfo>();
    ASSERT_EQ(info.smfRegistrationList.size(), 1U);
    EXPECT_EQ(info.smfRegistrationList[0].smfInstanceId, create_data.smfInstanceId);

    // RetrieveSmfRegistration (individual).
    sbi_core::http2::ClientRequest get_req;
    get_req.method = "GET";
    get_req.url = create_req.url;
    get_req.headers.emplace("authorization", "Bearer " + token);
    auto get_resp = client.send(get_req);
    ASSERT_TRUE(get_resp.has_value());
    EXPECT_EQ(get_resp->status, 200);

    // UpdateSmfRegistration: merge-patch.
    sbi_core::http2::ClientRequest patch_req;
    patch_req.method = "PATCH";
    patch_req.url = create_req.url;
    patch_req.headers.emplace("content-type", "application/merge-patch+json");
    patch_req.headers.emplace("authorization", "Bearer " + token);
    patch_req.body =
        json{{"smfInstanceId", create_data.smfInstanceId}, {"smfSetId", "set1"}}.dump();
    auto patch_resp = client.send(patch_req);
    ASSERT_TRUE(patch_resp.has_value());
    EXPECT_EQ(patch_resp->status, 200);
    const auto patched = json::parse(patch_resp->body).get<sbi_gen::SmfRegistration>();
    ASSERT_TRUE(patched.smfSetId.has_value());
    EXPECT_EQ(*patched.smfSetId, "set1");
    EXPECT_EQ(patched.pduSessionId, 5); // merge-patch: untouched field survives

    // SmfDeregistration, then confirm it's really gone.
    sbi_core::http2::ClientRequest del_req;
    del_req.method = "DELETE";
    del_req.url = create_req.url;
    del_req.headers.emplace("authorization", "Bearer " + token);
    auto del_resp = client.send(del_req);
    ASSERT_TRUE(del_resp.has_value());
    EXPECT_EQ(del_resp->status, 204);

    auto get_again = client.send(get_req);
    ASSERT_TRUE(get_again.has_value());
    EXPECT_EQ(get_again->status, 404);
}

TEST(UdmIntegration, SdmDataRetrievalAndSubscriptions) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Real UDR (ADR-0069, gap-closure Tier 1b): GetAmData/GetSmfSelData/GetSmData now make real
    // Nudr_DataRepository GET calls, so this test needs a real udr process to reach, same as any
    // other real inter-NF dependency -- UDR_DATABASE_URL is inherited from this test binary's own
    // environment (set by ctest in CI, or by whoever runs this test locally), same as
    // test_udr_context_data.cpp's own real-Postgres requirement.
    nf_test::SpawnedProcess udr(UDR_PATH);
    ASSERT_GT(udr.pid(), 0) << "failed to fork udr";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto client = make_client();
    // Real, disclosed timing fix (task #166, confirmed by direct evidence from ADR-0228/ADR-0230's
    // own new tests): udr's own startup (TLS + real Postgres seeding) genuinely takes longer than
    // udm's port coming up, so waiting on udm reachability alone isn't enough here -- this test's
    // very first real request needs udr already listening (udm proxies to it). Waiting on udr
    // explicitly before spawning udm removes the race that made this test CI-flaky enough to need
    // excluding from ctest (see .github/workflows/ci.yml's own `-E` exclusion, ADR-0113/ADR-0212).
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7781/nudr-dr/v2/subscription-data/nonexistent", 50))
        << "udr never became reachable";

    nf_test::SpawnedProcess udm(UDM_PATH);
    ASSERT_GT(udm.pid(), 0) << "failed to fork udm";
    ASSERT_TRUE(wait_reachable(
        client,
        "https://127.0.0.1:7780/nudm-uecm/v1/nonexistent/registrations/amf-3gpp-access",
        50))
        << "udm never became reachable";

    const std::string token = fetch_token(client, "nudm-sdm");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // Real seeded test subscriber (nfs/udr/src/main.cpp's own startup seed, ADR-0069) -- proves
    // the real UDM->UDR SBI call round-trips real, non-empty data, not just a 2xx status.
    const std::string supi = "imsi-999700000000001";

    for (const std::string path : {"am-data", "smf-select-data", "sm-data"}) {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = "https://127.0.0.1:7780/nudm-sdm/v2/" + supi + "/" + path;
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value()) << path;
        EXPECT_EQ(resp->status, 200) << path;
    }
    const auto am_data = [&] {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = "https://127.0.0.1:7780/nudm-sdm/v2/" + supi + "/am-data";
        req.headers.emplace("authorization", "Bearer " + token);
        return client.send(req);
    }();
    ASSERT_TRUE(am_data.has_value());
    const auto am_dto =
        json::parse(am_data->body).get<sbi_gen::AccessAndMobilitySubscriptionData>();
    // Real, non-empty data from UDR's own seed (ADR-0069): sst=1/sd="000001", this project's own
    // real lab S-NSSAI (ADR-0016) -- proves this is real cross-NF data, not the old stub's
    // always-default-constructed response.
    ASSERT_TRUE(am_dto.nssai.has_value());
    ASSERT_EQ(am_dto.nssai->defaultSingleNssais.size(), 1u);
    EXPECT_EQ(am_dto.nssai->defaultSingleNssais[0].sst, 1);
    ASSERT_TRUE(am_dto.nssai->defaultSingleNssais[0].sd.has_value());
    EXPECT_EQ(*am_dto.nssai->defaultSingleNssais[0].sd, "000001");

    // A genuinely unseeded SUPI now correctly 404s (real UDR miss), instead of the old stub's
    // always-200-empty-body behavior.
    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = "https://127.0.0.1:7780/nudm-sdm/v2/imsi-999999999999999/am-data";
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 404);
    }

    // Subscribe.
    sbi_gen::SdmSubscription sub_data{};
    sub_data.nfInstanceId = "00000000-0000-4000-8000-000000000ccc";
    sub_data.callbackReference = "https://example.com/cb";
    sub_data.monitoredResourceUris = {"https://example.com/res"};

    sbi_core::http2::ClientRequest sub_req;
    sub_req.method = "POST";
    sub_req.url = "https://127.0.0.1:7780/nudm-sdm/v2/" + supi + "/sdm-subscriptions";
    sub_req.headers.emplace("content-type", "application/json");
    sub_req.headers.emplace("authorization", "Bearer " + token);
    sub_req.body = json(sub_data).dump();
    auto sub_resp = client.send(sub_req);
    ASSERT_TRUE(sub_resp.has_value());
    EXPECT_EQ(sub_resp->status, 201);
    const auto created = json::parse(sub_resp->body).get<sbi_gen::SdmSubscription>();
    ASSERT_TRUE(created.subscriptionId.has_value());
    const std::string subscription_id = *created.subscriptionId;
    EXPECT_FALSE(subscription_id.empty());

    // Unsubscribe, then confirm double-unsubscribe correctly 404s.
    sbi_core::http2::ClientRequest unsub_req;
    unsub_req.method = "DELETE";
    unsub_req.url =
        "https://127.0.0.1:7780/nudm-sdm/v2/" + supi + "/sdm-subscriptions/" + subscription_id;
    unsub_req.headers.emplace("authorization", "Bearer " + token);
    auto unsub_resp = client.send(unsub_req);
    ASSERT_TRUE(unsub_resp.has_value());
    EXPECT_EQ(unsub_resp->status, 204);

    auto unsub_again = client.send(unsub_req);
    ASSERT_TRUE(unsub_again.has_value());
    EXPECT_EQ(unsub_again->status, 404);
}

TEST(UdmIntegration, MissingRegistrationIs404AndTamperedTokenIs401) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess udm(UDM_PATH);
    ASSERT_GT(udm.pid(), 0) << "failed to fork udm";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client,
        "https://127.0.0.1:7780/nudm-uecm/v1/nonexistent/registrations/amf-3gpp-access",
        50))
        << "udm never became reachable";

    const std::string token = fetch_token(client, "nudm-uecm");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    sbi_core::http2::ClientRequest get_req;
    get_req.method = "GET";
    get_req.url =
        "https://127.0.0.1:7780/nudm-uecm/v1/imsi-999700000000099/registrations/amf-3gpp-access";
    get_req.headers.emplace("authorization", "Bearer " + token);
    auto get_resp = client.send(get_req);
    ASSERT_TRUE(get_resp.has_value());
    EXPECT_EQ(get_resp->status, 404);

    sbi_core::http2::ClientRequest tampered_req;
    tampered_req.method = "GET";
    tampered_req.url = get_req.url;
    tampered_req.headers.emplace("authorization", "Bearer " + token + "tampered");
    auto tampered_resp = client.send(tampered_req);
    ASSERT_TRUE(tampered_resp.has_value());
    EXPECT_EQ(tampered_resp->status, 401);
}
