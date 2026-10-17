// Drives nrf and udr as real, separate OS processes to exercise UDR's structured-data-for-exposure
// group (ADR-0255) over real TLS 1.3 + mTLS HTTP/2 with a real signed OAuth2 token, per
// TS29519_Exposure_Data.yaml (which TS29504_Nudr_DR.yaml $refs).
//
// This is also the live verification that ADR-0253/0254/0255 each disclosed as outstanding: these
// routes are backed by tables that only exist once nfs/udr/schema.postgres.sql has been re-applied,
// so a green run here proves the schema and the routes actually agree against a real PostgreSQL.
//
// The asymmetries this family has against its application-data sibling are asserted, not assumed:
// session-management-data has no PATCH, and the individual subscription PUT is modify-only (404 on
// an unknown subId, no upsert) -- both read from the YAML's own per-path response sets.

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

struct Duo {
    nf_test::SpawnedProcess nrf;
    nf_test::SpawnedProcess udr;
};

Duo spawn_nrf_udr() {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    nf_test::SpawnedProcess udr(UDR_PATH);
    return Duo{std::move(nrf), std::move(udr)};
}

sbi_core::http2::ClientRequest
authed(const std::string& method, const std::string& url, const std::string& token) {
    sbi_core::http2::ClientRequest req;
    req.method = method;
    req.url = url;
    req.headers.emplace("authorization", "Bearer " + token);
    return req;
}

} // namespace

// PUT/GET/PATCH/DELETE, and the real 201-vs-200 distinction on PUT. PATCH here is RFC 7396
// merge-patch and its only documented success is 204 -- no 200-with-body alternative.
TEST(UdrExposureDataIntegration, AccessAndMobilityDataLifecycle) {
    auto d = spawn_nrf_udr();
    auto client = make_client();
    const std::string url = "https://127.0.0.1:7781/nudr-dr/v2/exposure-data/"
                            "imsi-999700000000501/access-and-mobility-data";
    ASSERT_TRUE(wait_reachable(client, url, 200)) << "udr never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // Leftovers from an earlier run would mask the 201, so start from a known-absent resource.
    client.send(authed("DELETE", url, token));

    auto get_before = client.send(authed("GET", url, token));
    ASSERT_TRUE(get_before.has_value());
    EXPECT_EQ(get_before->status, 404);

    auto put_req = authed("PUT", url, token);
    put_req.headers.emplace("content-type", "application/json");
    put_req.body = json{{"roamingStatus", true}, {"timeZone", "+05:30"}}.dump();
    auto put_resp = client.send(put_req);
    ASSERT_TRUE(put_resp.has_value());
    EXPECT_EQ(put_resp->status, 201) << put_resp->body;
    EXPECT_NE(put_resp->headers.find("location"), put_resp->headers.end());

    // Second PUT on an existing resource is an update: 200, not 201.
    auto put_again = client.send(put_req);
    ASSERT_TRUE(put_again.has_value());
    EXPECT_EQ(put_again->status, 200) << put_again->body;

    auto get_resp = client.send(authed("GET", url, token));
    ASSERT_TRUE(get_resp.has_value());
    ASSERT_EQ(get_resp->status, 200) << get_resp->body;
    EXPECT_EQ(json::parse(get_resp->body).at("roamingStatus").get<bool>(), true);

    auto patch_req = authed("PATCH", url, token);
    patch_req.headers.emplace("content-type", "application/merge-patch+json");
    patch_req.body = json{{"roamingStatus", false}}.dump();
    auto patch_resp = client.send(patch_req);
    ASSERT_TRUE(patch_resp.has_value());
    EXPECT_EQ(patch_resp->status, 204) << patch_resp->body;

    auto get_patched = client.send(authed("GET", url, token));
    ASSERT_TRUE(get_patched.has_value());
    ASSERT_EQ(get_patched->status, 200) << get_patched->body;
    const auto patched = json::parse(get_patched->body);
    EXPECT_EQ(patched.at("roamingStatus").get<bool>(), false);
    // RFC 7396 merges: the untouched field must survive.
    EXPECT_EQ(patched.at("timeZone").get<std::string>(), "+05:30");

    auto del_resp = client.send(authed("DELETE", url, token));
    ASSERT_TRUE(del_resp.has_value());
    EXPECT_EQ(del_resp->status, 204);

    auto get_after = client.send(authed("GET", url, token));
    ASSERT_TRUE(get_after.has_value());
    EXPECT_EQ(get_after->status, 404);

    // PATCH deliberately does not upsert -- the spec documents 404 for this operation.
    auto patch_missing = client.send(patch_req);
    ASSERT_TRUE(patch_missing.has_value());
    EXPECT_EQ(patch_missing->status, 404) << patch_missing->body;
}

// Keyed by (ueId, pduSessionId), and -- unlike its access-and-mobility-data sibling -- the real
// spec gives this resource NO PATCH. Both facts are asserted here.
TEST(UdrExposureDataIntegration, SessionManagementDataLifecycle) {
    auto d = spawn_nrf_udr();
    auto client = make_client();
    const std::string base = "https://127.0.0.1:7781/nudr-dr/v2/exposure-data/"
                             "imsi-999700000000502/session-management-data/";
    const std::string url_1 = base + "1";
    const std::string url_2 = base + "2";
    ASSERT_TRUE(wait_reachable(client, url_1, 200)) << "udr never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    client.send(authed("DELETE", url_1, token));
    client.send(authed("DELETE", url_2, token));

    auto get_before = client.send(authed("GET", url_1, token));
    ASSERT_TRUE(get_before.has_value());
    EXPECT_EQ(get_before->status, 404);

    auto put_1 = authed("PUT", url_1, token);
    put_1.headers.emplace("content-type", "application/json");
    put_1.body = json{{"pduSessionId", 1}, {"dnn", "internet"}}.dump();
    auto put_1_resp = client.send(put_1);
    ASSERT_TRUE(put_1_resp.has_value());
    EXPECT_EQ(put_1_resp->status, 201) << put_1_resp->body;

    // Same UE, different PDU session: a genuinely separate resource, not an overwrite.
    auto put_2 = authed("PUT", url_2, token);
    put_2.headers.emplace("content-type", "application/json");
    put_2.body = json{{"pduSessionId", 2}, {"dnn", "ims"}}.dump();
    auto put_2_resp = client.send(put_2);
    ASSERT_TRUE(put_2_resp.has_value());
    EXPECT_EQ(put_2_resp->status, 201) << put_2_resp->body;

    auto get_1 = client.send(authed("GET", url_1, token));
    ASSERT_TRUE(get_1.has_value());
    ASSERT_EQ(get_1->status, 200) << get_1->body;
    EXPECT_EQ(json::parse(get_1->body).at("dnn").get<std::string>(), "internet");

    auto get_2 = client.send(authed("GET", url_2, token));
    ASSERT_TRUE(get_2.has_value());
    ASSERT_EQ(get_2->status, 200) << get_2->body;
    EXPECT_EQ(json::parse(get_2->body).at("dnn").get<std::string>(), "ims");

    auto put_1_again = client.send(put_1);
    ASSERT_TRUE(put_1_again.has_value());
    EXPECT_EQ(put_1_again->status, 200) << put_1_again->body;

    // No PATCH exists for this resource in the spec, so none is registered here.
    auto patch_req = authed("PATCH", url_1, token);
    patch_req.headers.emplace("content-type", "application/merge-patch+json");
    patch_req.body = json{{"dnn", "should-not-apply"}}.dump();
    auto patch_resp = client.send(patch_req);
    ASSERT_TRUE(patch_resp.has_value());
    EXPECT_GE(patch_resp->status, 400) << "PATCH must not be routed on this resource";

    auto get_unpatched = client.send(authed("GET", url_1, token));
    ASSERT_TRUE(get_unpatched.has_value());
    ASSERT_EQ(get_unpatched->status, 200) << get_unpatched->body;
    EXPECT_EQ(json::parse(get_unpatched->body).at("dnn").get<std::string>(), "internet");

    EXPECT_EQ(client.send(authed("DELETE", url_1, token))->status, 204);
    EXPECT_EQ(client.send(authed("DELETE", url_2, token))->status, 204);
    EXPECT_EQ(client.send(authed("DELETE", url_1, token))->status, 404);
}

// POST-only collection, and a modify-only PUT on the individual subscription: an unknown subId is
// a 404, deliberately unlike ADR-0254's application-data subs-to-notify PUT, which upserts.
TEST(UdrExposureDataIntegration, SubsToNotifyLifecycle) {
    auto d = spawn_nrf_udr();
    auto client = make_client();
    const std::string coll_url = "https://127.0.0.1:7781/nudr-dr/v2/exposure-data/subs-to-notify";
    ASSERT_TRUE(wait_reachable(client, coll_url, 200)) << "udr never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const json subscription = json{
        {"notificationUri", "https://127.0.0.1:9999/exposure-notify"},
        {"monitoredResourceUris",
         json::array({"/nudr-dr/v2/exposure-data/imsi-999700000000503/access-and-mobility-data"})}};

    auto post_req = authed("POST", coll_url, token);
    post_req.headers.emplace("content-type", "application/json");
    post_req.body = subscription.dump();
    auto post_resp = client.send(post_req);
    ASSERT_TRUE(post_resp.has_value());
    ASSERT_EQ(post_resp->status, 201) << post_resp->body;
    const auto location = post_resp->headers.find("location");
    ASSERT_NE(location, post_resp->headers.end()) << "201 must carry a Location header";
    // UDR has no configured external base URL, so every Location it emits is an API *path*, not
    // the absolute URI the Uri schema documents. That is a pre-existing, project-wide disclosed
    // limitation (see resolved_location / the onDataChange webhook comment in nfs/udr/src/
    // main.cpp), asserted here rather than worked around silently -- the subId is taken from the
    // path and the absolute URL rebuilt client-side.
    const std::string location_path = location->second;
    EXPECT_EQ(location_path.rfind("/nudr-dr/v2/exposure-data/subs-to-notify/", 0), 0u)
        << location_path;
    const std::string sub_id = location_path.substr(location_path.rfind('/') + 1);
    ASSERT_FALSE(sub_id.empty()) << location_path;
    const std::string item_url = coll_url + "/" + sub_id;

    json modified = subscription;
    modified["notificationUri"] = "https://127.0.0.1:9999/exposure-notify-v2";
    auto put_req = authed("PUT", item_url, token);
    put_req.headers.emplace("content-type", "application/json");
    put_req.body = modified.dump();
    auto put_resp = client.send(put_req);
    ASSERT_TRUE(put_resp.has_value());
    ASSERT_EQ(put_resp->status, 200) << put_resp->body;
    EXPECT_EQ(json::parse(put_resp->body).at("notificationUri").get<std::string>(),
              "https://127.0.0.1:9999/exposure-notify-v2");

    // Modify-only: PUT on an unknown subscription is a 404, not a create.
    auto put_unknown = authed("PUT", coll_url + "/does-not-exist", token);
    put_unknown.headers.emplace("content-type", "application/json");
    put_unknown.body = modified.dump();
    auto put_unknown_resp = client.send(put_unknown);
    ASSERT_TRUE(put_unknown_resp.has_value());
    EXPECT_EQ(put_unknown_resp->status, 404) << put_unknown_resp->body;

    auto del_resp = client.send(authed("DELETE", item_url, token));
    ASSERT_TRUE(del_resp.has_value());
    EXPECT_EQ(del_resp->status, 204);

    auto del_again = client.send(authed("DELETE", item_url, token));
    ASSERT_TRUE(del_again.has_value());
    EXPECT_EQ(del_again->status, 404);
}
