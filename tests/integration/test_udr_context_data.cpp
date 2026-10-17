// Drives nrf and udr as real, separate OS processes: nrf issues real signed OAuth2 tokens, udr
// registers with nrf on its own background thread (see nfs/udr/src/main.cpp's run_nrf_lifecycle,
// docs/DECISIONS.md ADR-0006/ADR-0019), then this test acts as an SBI client exercising udr's
// Nudr_DataRepository context-data group over real TLS 1.3 + mTLS HTTP/2, including real RFC 6902
// JSON Patch application (a different standard than UDM's RFC 7396 merge-patch -- ADR-0025).

#include "sbi_core/http2_client.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

// TS29505_Subscription_Data's own types now live in TS26510_CommonData_grp.hpp -- see
// nfs/chf/src/stores.hpp's own comment (ADR-0072).
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

std::string fetch_token(sbi_core::http2::Client& client) {
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = "https://127.0.0.1:7777/oauth2/token";
    req.headers.emplace("content-type", "application/x-www-form-urlencoded");
    req.body =
        "grant_type=client_credentials&nfInstanceId=" + std::string(nf_test::kTestAfInstanceId) +
        "&scope=nudr-dr&"
        "targetNfType=UDR";
    auto resp = nf_test::send_token_request(req);
    if (!resp.has_value() || resp->status != 200) {
        return "";
    }
    return json::parse(resp->body).at("access_token").get<std::string>();
}

} // namespace

TEST(UdrIntegration, AmfContextLifecycle) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess udr(UDR_PATH);
    ASSERT_GT(udr.pid(), 0) << "failed to fork udr";

    auto client = make_client();
    const std::string base_url =
        "https://127.0.0.1:7781/nudr-dr/v2/subscription-data/imsi-999700000000001/"
        "context-data/amf-3gpp-access";
    ASSERT_TRUE(wait_reachable(client, base_url, 200)) << "udr never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    sbi_gen::Amf3GppAccessRegistration create_data{};
    create_data.amfInstanceId = "00000000-0000-4000-8000-000000000aaa";
    create_data.deregCallbackUri = "https://example.com/dereg";
    create_data.guami.plmnId.mcc = "999";
    create_data.guami.plmnId.mnc = "70";
    create_data.guami.amfId = "ABCDEF";
    create_data.ratType = "NR"; // RatType is an opaque nlohmann::json fallback, not a struct

    sbi_core::http2::ClientRequest create_req;
    create_req.method = "PUT";
    create_req.url = base_url;
    create_req.headers.emplace("content-type", "application/json");
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = json(create_data).dump();
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    EXPECT_EQ(create_resp->status, 201);
    const auto created = json::parse(create_resp->body).get<sbi_gen::Amf3GppAccessRegistration>();
    EXPECT_EQ(created.amfInstanceId, create_data.amfInstanceId);

    // Re-PUT the same ueId: real replace semantics, 204 (no body) not 201 -- per the spec, a
    // second PUT replaces the existing resource rather than creating a new one.
    auto replace_resp = client.send(create_req);
    ASSERT_TRUE(replace_resp.has_value());
    EXPECT_EQ(replace_resp->status, 204);

    sbi_core::http2::ClientRequest get_req;
    get_req.method = "GET";
    get_req.url = base_url;
    get_req.headers.emplace("authorization", "Bearer " + token);
    auto get_resp = client.send(get_req);
    ASSERT_TRUE(get_resp.has_value());
    EXPECT_EQ(get_resp->status, 200);

    // AmfContext3gpp: real RFC 6902 JSON Patch (application/json-patch+json), NOT UDM's RFC 7396
    // merge-patch -- see docs/DECISIONS.md ADR-0025.
    sbi_core::http2::ClientRequest patch_req;
    patch_req.method = "PATCH";
    patch_req.url = base_url;
    patch_req.headers.emplace("content-type", "application/json-patch+json");
    patch_req.headers.emplace("authorization", "Bearer " + token);
    patch_req.body =
        json::array({json{{"op", "replace"}, {"path", "/guami/amfId"}, {"value", "FEDCBA"}}})
            .dump();
    auto patch_resp = client.send(patch_req);
    ASSERT_TRUE(patch_resp.has_value());
    EXPECT_EQ(patch_resp->status, 204);

    auto get_after_patch = client.send(get_req);
    ASSERT_TRUE(get_after_patch.has_value());
    EXPECT_EQ(get_after_patch->status, 200);
    const auto patched =
        json::parse(get_after_patch->body).get<sbi_gen::Amf3GppAccessRegistration>();
    EXPECT_EQ(patched.guami.amfId, "FEDCBA");
    // JSON Patch only touched /guami/amfId -- everything else survives.
    EXPECT_EQ(patched.amfInstanceId, create_data.amfInstanceId);
    EXPECT_EQ(patched.deregCallbackUri, create_data.deregCallbackUri);
}

TEST(UdrIntegration, SmfRegistrationLifecycle) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess udr(UDR_PATH);
    ASSERT_GT(udr.pid(), 0) << "failed to fork udr";

    auto client = make_client();
    const std::string base_url =
        "https://127.0.0.1:7781/nudr-dr/v2/subscription-data/imsi-999700000000002/"
        "context-data/smf-registrations";
    ASSERT_TRUE(wait_reachable(client, base_url, 200)) << "udr never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const std::string individual_url = base_url + "/5";

    sbi_gen::SmfRegistration create_data{};
    create_data.smfInstanceId = "00000000-0000-4000-8000-000000000bbb";
    create_data.pduSessionId = 5;
    create_data.singleNssai.sst = 1;
    create_data.plmnId.mcc = "999";
    create_data.plmnId.mnc = "70";

    sbi_core::http2::ClientRequest create_req;
    create_req.method = "PUT";
    create_req.url = individual_url;
    create_req.headers.emplace("content-type", "application/json");
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = json(create_data).dump();
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    EXPECT_EQ(create_resp->status, 201);

    // QuerySmfRegList: the collection GET, using the real SmfRegList generated type
    // (using SmfRegList = std::vector<SmfRegistration>).
    sbi_core::http2::ClientRequest list_req;
    list_req.method = "GET";
    list_req.url = base_url;
    list_req.headers.emplace("authorization", "Bearer " + token);
    auto list_resp = client.send(list_req);
    ASSERT_TRUE(list_resp.has_value());
    EXPECT_EQ(list_resp->status, 200);
    const auto list = json::parse(list_resp->body).get<sbi_gen::SmfRegList>();
    ASSERT_EQ(list.size(), 1U);
    EXPECT_EQ(list[0].smfInstanceId, create_data.smfInstanceId);

    sbi_core::http2::ClientRequest get_req;
    get_req.method = "GET";
    get_req.url = individual_url;
    get_req.headers.emplace("authorization", "Bearer " + token);
    auto get_resp = client.send(get_req);
    ASSERT_TRUE(get_resp.has_value());
    EXPECT_EQ(get_resp->status, 200);

    // UpdateSmfContext: real RFC 6902 JSON Patch.
    sbi_core::http2::ClientRequest patch_req;
    patch_req.method = "PATCH";
    patch_req.url = individual_url;
    patch_req.headers.emplace("content-type", "application/json-patch+json");
    patch_req.headers.emplace("authorization", "Bearer " + token);
    patch_req.body =
        json::array({json{{"op", "add"}, {"path", "/smfSetId"}, {"value", "set1"}}}).dump();
    auto patch_resp = client.send(patch_req);
    ASSERT_TRUE(patch_resp.has_value());
    EXPECT_EQ(patch_resp->status, 204);

    auto get_after_patch = client.send(get_req);
    ASSERT_TRUE(get_after_patch.has_value());
    const auto patched = json::parse(get_after_patch->body).get<sbi_gen::SmfRegistration>();
    ASSERT_TRUE(patched.smfSetId.has_value());
    EXPECT_EQ(*patched.smfSetId, "set1");
    EXPECT_EQ(patched.pduSessionId, 5); // untouched by the patch, survives

    // DeleteSmfRegistration, then confirm it's really gone (second delete -> 404).
    sbi_core::http2::ClientRequest del_req;
    del_req.method = "DELETE";
    del_req.url = individual_url;
    del_req.headers.emplace("authorization", "Bearer " + token);
    auto del_resp = client.send(del_req);
    ASSERT_TRUE(del_resp.has_value());
    EXPECT_EQ(del_resp->status, 204);

    auto del_again = client.send(del_req);
    ASSERT_TRUE(del_again.has_value());
    EXPECT_EQ(del_again->status, 404);
}

TEST(UdrIntegration, MissingResourceIs404AndTamperedTokenIs401) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess udr(UDR_PATH);
    ASSERT_GT(udr.pid(), 0) << "failed to fork udr";

    auto client = make_client();
    const std::string base_url =
        "https://127.0.0.1:7781/nudr-dr/v2/subscription-data/imsi-999700000000099/"
        "context-data/amf-3gpp-access";
    ASSERT_TRUE(wait_reachable(client, base_url, 200)) << "udr never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    sbi_core::http2::ClientRequest get_req;
    get_req.method = "GET";
    get_req.url = base_url;
    get_req.headers.emplace("authorization", "Bearer " + token);
    auto get_resp = client.send(get_req);
    ASSERT_TRUE(get_resp.has_value());
    EXPECT_EQ(get_resp->status, 404);

    sbi_core::http2::ClientRequest tampered_req;
    tampered_req.method = "GET";
    tampered_req.url = base_url;
    tampered_req.headers.emplace("authorization", "Bearer " + token + "tampered");
    auto tampered_resp = client.send(tampered_req);
    ASSERT_TRUE(tampered_resp.has_value());
    EXPECT_EQ(tampered_resp->status, 401);
}
