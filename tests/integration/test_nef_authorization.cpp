// ADR-0499 phase 4: the NEF enforces access tokens (oauth2_required, default true). Its nnef-*
// SBI APIs need their YAML scope; its northbound APIs (e.g. TS29522_TrafficInfluence.yaml) declare
// OAuth2 with no scope, so a token for the NEF is enough; its callback routes take none
// (TS 33.501 13.4.1.0). Real nrf + nef processes.

#include "oauth2_enforcement.hpp"
#include "spawn_guard.hpp"

#include <gtest/gtest.h>

TEST(NefAuthorization, SbiAndNorthboundApisNeedATokenForTheNefAndCallbacksDoNot) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    nf_test::SpawnedProcess nef(NEF_PATH);
    ASSERT_GT(nef.pid(), 0) << "failed to fork nef";

    // An empty body: once authorized, the handler's own validation answers 400.
    nf_test::expect_oauth2_enforced({
        .method = "POST",
        .url = "https://127.0.0.1:7790/nnef-eventexposure/v1/subscriptions",
        .body = "{}",
        .scope = "nnef-eventexposure",
        .nf_type = "NEF",
        .other_scope = "nnef-pfdmanagement",
        .authorized_status = 400,
    });

    auto client = nf_test::oauth2_test_client();
    const nf_test::EnforcedRoute northbound{
        .method = "POST",
        .url = "https://127.0.0.1:7790/3gpp-traffic-influence/v1/af-1/subscriptions",
        .body = "{}"};
    auto resp = nf_test::send_to(client, northbound, "");
    EXPECT_EQ(resp.status, 401) << "no token: " << resp.body;
    resp = nf_test::send_to(
        client, northbound, nf_test::oauth2_test_token(client, "nnef-eventexposure", "AF"));
    EXPECT_EQ(resp.status, 401) << "a token for another NF type: " << resp.body;
    resp = nf_test::send_to(
        client, northbound, nf_test::oauth2_test_token(client, "nnef-eventexposure", "NEF"));
    EXPECT_EQ(resp.status, 400) << "any scope will do -- the YAML declares none: " << resp.body;

    resp = nf_test::send_to(
        client,
        {.method = "POST",
         .url = "https://127.0.0.1:7790/nnef-callback/v1/qos-notify/af-1/sub-1/notify",
         .body = "{}"},
        "");
    EXPECT_NE(resp.status, 0);
    EXPECT_NE(resp.status, 401) << "the notification reaches its handler without a token: "
                                << resp.body;
}
