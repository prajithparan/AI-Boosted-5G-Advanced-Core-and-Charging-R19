// Drives nrf and udr as real, separate OS processes to exercise UDR's AIoT data group and the
// data-restoration subscription resource (ADR-0256) over real TLS 1.3 + mTLS HTTP/2 with a real
// signed OAuth2 token, per TS29506_Aiot_Data.yaml (paths, TS 29.506 V19.2.0) and
// TS29369_Nadm_DM.yaml (schemas, TS 29.369 V19.2.0).
//
// Two behaviours here are deliberate non-implementations and are asserted as such, so that a later
// change cannot quietly turn them into something else:
//   * UpdateBundledAiotDeviceProfileData returns 501 -- the spec defines no device selector for
//     it, so no scope can be implemented without inventing one (ADR-0256).
//   * /data-restoration-events returns 204, not 201 -- the YAML defines no 2xx at all and no
//     individual-subscription resource, so a 201's Location would point at nothing.

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

// The seeded lab device (see nfs/udr/src/main.cpp). The resource has no create operation in the
// spec, so seeding is the only way a row can exist -- and every udr start re-seeds it, which makes
// this test idempotent against a persistent database.
// Base64 ("aiot-dev-1"): AiotDevPermId is `Bytes`/`format: byte`, not a free-form
// label. The generated type is an opaque fallback and would accept anything, so this
// is pinned to what the YAML actually specifies.
constexpr const char* kSeededDevice = "YWlvdC1kZXYtMQ==";

const std::string kProfileColl = "https://127.0.0.1:7781/nudr-dr/v2/aiot-data/"
                                 "aiot-device-profile-data";

} // namespace

TEST(UdrAiotDataIntegration, DeviceProfileReadAndRfc6902Patch) {
    auto d = spawn_nrf_udr();
    auto client = make_client();
    const std::string item_url = kProfileColl + "/" + kSeededDevice;
    ASSERT_TRUE(wait_reachable(client, item_url, 200)) << "udr never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    auto get_resp = client.send(authed("GET", item_url, token));
    ASSERT_TRUE(get_resp.has_value());
    ASSERT_EQ(get_resp->status, 200) << get_resp->body;
    const auto profile = json::parse(get_resp->body);
    EXPECT_EQ(profile.at("aiotDevPermId").get<std::string>(), kSeededDevice);
    // Both fields the AiotDevProfileData schema marks required must be present.
    EXPECT_TRUE(profile.contains("lastKnownAiotfInfo"));

    auto unknown = client.send(authed("GET", kProfileColl + "/no-such-device", token));
    ASSERT_TRUE(unknown.has_value());
    EXPECT_EQ(unknown->status, 404);

    // RFC 6902, array body, 204 with no PatchResult report -- this file's established choice.
    auto patch_req = authed("PATCH", item_url, token);
    patch_req.headers.emplace("content-type", "application/json-patch+json");
    patch_req.body =
        json::array({json{{"op", "add"}, {"path", "/tidCurrent"}, {"value", "tid-1"}}}).dump();
    auto patch_resp = client.send(patch_req);
    ASSERT_TRUE(patch_resp.has_value());
    EXPECT_EQ(patch_resp->status, 204) << patch_resp->body;

    auto get_patched = client.send(authed("GET", item_url, token));
    ASSERT_TRUE(get_patched.has_value());
    ASSERT_EQ(get_patched->status, 200) << get_patched->body;
    EXPECT_EQ(json::parse(get_patched->body).at("tidCurrent").get<std::string>(), "tid-1");

    // The YAML $refs a BARE PatchItem here; the array reading is what is implemented, and a
    // non-array body is rejected rather than silently coerced.
    auto bare_req = authed("PATCH", item_url, token);
    bare_req.headers.emplace("content-type", "application/json-patch+json");
    bare_req.body = json{{"op", "add"}, {"path", "/tidPrevious"}, {"value", "tid-0"}}.dump();
    auto bare_resp = client.send(bare_req);
    ASSERT_TRUE(bare_resp.has_value());
    EXPECT_EQ(bare_resp->status, 400) << bare_resp->body;

    // A well-formed patch that cannot apply (missing parent) is a 400, not a 500.
    auto bad_req = authed("PATCH", item_url, token);
    bad_req.headers.emplace("content-type", "application/json-patch+json");
    bad_req.body =
        json::array({json{{"op", "replace"}, {"path", "/nope/deeper"}, {"value", 1}}}).dump();
    auto bad_resp = client.send(bad_req);
    ASSERT_TRUE(bad_resp.has_value());
    EXPECT_EQ(bad_resp->status, 400) << bad_resp->body;

    auto patch_missing = client.send([&] {
        auto r = authed("PATCH", kProfileColl + "/no-such-device", token);
        r.headers.emplace("content-type", "application/json-patch+json");
        r.body = json::array({json{{"op", "add"}, {"path", "/tidCurrent"}, {"value", "x"}}}).dump();
        return r;
    }());
    ASSERT_TRUE(patch_missing.has_value());
    EXPECT_EQ(patch_missing->status, 404) << patch_missing->body;
}

TEST(UdrAiotDataIntegration, BundledReadRequiresDeviceIdsAndBundledPatchIsNotImplemented) {
    auto d = spawn_nrf_udr();
    auto client = make_client();
    ASSERT_TRUE(wait_reachable(client, kProfileColl, 200)) << "udr never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // requester-aiot-devices-id is REQUIRED: its absence is a 400, deliberately not a full list.
    auto no_param = client.send(authed("GET", kProfileColl, token));
    ASSERT_TRUE(no_param.has_value());
    EXPECT_EQ(no_param->status, 400) << no_param->body;

    auto ok = client.send(authed(
        "GET", kProfileColl + "?requester-aiot-devices-id=" + std::string(kSeededDevice), token));
    ASSERT_TRUE(ok.has_value());
    ASSERT_EQ(ok->status, 200) << ok->body;
    const auto list = json::parse(ok->body);
    ASSERT_TRUE(list.contains("aiotDevProfileDataList"));
    ASSERT_EQ(list.at("aiotDevProfileDataList").size(), 1u);
    EXPECT_EQ(list.at("aiotDevProfileDataList")[0].at("aiotDevPermId").get<std::string>(),
              kSeededDevice);

    // Comma-separated form array: the known device plus an unknown one yields just the known.
    auto partial = client.send(authed("GET",
                                      kProfileColl + "?requester-aiot-devices-id=" +
                                          std::string(kSeededDevice) + ",no-such-device",
                                      token));
    ASSERT_TRUE(partial.has_value());
    ASSERT_EQ(partial->status, 200) << partial->body;
    EXPECT_EQ(json::parse(partial->body).at("aiotDevProfileDataList").size(), 1u);

    auto none = client.send(
        authed("GET", kProfileColl + "?requester-aiot-devices-id=no-such-device", token));
    ASSERT_TRUE(none.has_value());
    EXPECT_EQ(none->status, 404) << none->body;

    // Bundled PATCH is registered but deliberately not implemented: the spec gives it no device
    // selector, so no scope can be chosen without inventing one. A malformed body is still
    // answered as malformed, and a well-formed one gets 501 with ProblemDetails.
    auto malformed = client.send([&] {
        auto r = authed("PATCH", kProfileColl + "?supported-features=0", token);
        r.headers.emplace("content-type", "application/merge-patch+json");
        r.body = "{not json";
        return r;
    }());
    ASSERT_TRUE(malformed.has_value());
    EXPECT_EQ(malformed->status, 400) << malformed->body;

    auto not_impl = client.send([&] {
        auto r = authed("PATCH", kProfileColl + "?supported-features=0", token);
        r.headers.emplace("content-type", "application/merge-patch+json");
        r.body = json::array({json{{"op", "add"}, {"path", "/x"}, {"value", 1}}}).dump();
        return r;
    }());
    ASSERT_TRUE(not_impl.has_value());
    EXPECT_EQ(not_impl->status, 501) << not_impl->body;
    EXPECT_NE(json::parse(not_impl->body).at("detail").get<std::string>().find("device selector"),
              std::string::npos);
}

TEST(UdrAiotDataIntegration, AfAuthorizationDataReadAndFilter) {
    auto d = spawn_nrf_udr();
    auto client = make_client();
    const std::string url = "https://127.0.0.1:7781/nudr-dr/v2/aiot-data/af-authorization-data";
    ASSERT_TRUE(wait_reachable(client, url, 200)) << "udr never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    auto all = client.send(authed("GET", url, token));
    ASSERT_TRUE(all.has_value());
    ASSERT_EQ(all->status, 200) << all->body;
    const auto doc = json::parse(all->body);
    ASSERT_TRUE(doc.contains("afAuthData"));
    ASSERT_TRUE(doc.at("afAuthData").contains("af-app-1"));
    // afId is IndividualAfAuthorizationData's required field. Asserted because this route returns
    // the map opaquely -- nothing else would notice a stored entry that violates its own schema.
    EXPECT_EQ(doc.at("afAuthData").at("af-app-1").at("afId").get<std::string>(), "af-app-1");

    // af-id is an optional filter and IS honoured -- it keys the map this document is built on.
    // The envelope is kept rather than returning a bare entry.
    auto filtered = client.send(authed("GET", url + "?af-id=af-app-1", token));
    ASSERT_TRUE(filtered.has_value());
    ASSERT_EQ(filtered->status, 200) << filtered->body;
    const auto f = json::parse(filtered->body);
    ASSERT_TRUE(f.contains("afAuthData"));
    EXPECT_EQ(f.at("afAuthData").size(), 1u);
    EXPECT_TRUE(f.at("afAuthData").contains("af-app-1"));

    auto missing = client.send(authed("GET", url + "?af-id=no-such-af", token));
    ASSERT_TRUE(missing.has_value());
    EXPECT_EQ(missing->status, 404) << missing->body;
}

TEST(UdrAiotDataIntegration, DataRestorationSubscriptionReturns204NotCreated) {
    auto d = spawn_nrf_udr();
    auto client = make_client();
    const std::string url = "https://127.0.0.1:7781/nudr-dr/v2/data-restoration-events";
    ASSERT_TRUE(wait_reachable(client, url, 200)) << "udr never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // The real request body schema is literally `{}` -- no fields -- so any JSON object is a valid
    // request and is stored opaquely. Nothing is validated against a shape that does not exist.
    auto post_req = authed("POST", url, token);
    post_req.headers.emplace("content-type", "application/json");
    post_req.body = json{{"dataRestorationCallbackUri", "https://127.0.0.1:9999/restore"}}.dump();
    auto post_resp = client.send(post_req);
    ASSERT_TRUE(post_resp.has_value());
    EXPECT_EQ(post_resp->status, 204) << post_resp->body;
    // 204, so deliberately no Location: the spec defines no individual subscription resource for
    // one to point at. Asserted so a later "helpful" 201 has to be a conscious change.
    EXPECT_EQ(post_resp->headers.find("location"), post_resp->headers.end());

    auto malformed = authed("POST", url, token);
    malformed.headers.emplace("content-type", "application/json");
    malformed.body = "{not json";
    auto malformed_resp = client.send(malformed);
    ASSERT_TRUE(malformed_resp.has_value());
    EXPECT_EQ(malformed_resp->status, 400) << malformed_resp->body;
}
