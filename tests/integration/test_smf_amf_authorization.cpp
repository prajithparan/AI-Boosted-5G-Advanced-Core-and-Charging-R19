// ADR-0499 phase 4: the SMF (TS29502_Nsmf_PDUSession.yaml, ...) and AMF (TS29518_Namf_*.yaml)
// enforce access tokens (oauth2_required, default true), each API with its own scope; the SMF's
// PCF notification route (TS 33.501 13.4.1.0) needs none. Real nrf + NF processes.

#include "oauth2_enforcement.hpp"
#include "spawn_guard.hpp"

#include <gtest/gtest.h>

TEST(SmfAuthorization, NsmfApisNeedTheirOwnScopeAndThePcfNotifyDoesNot) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    nf_test::SpawnedProcess smf(SMF_PATH);
    ASSERT_GT(smf.pid(), 0) << "failed to fork smf";

    // An empty body: once authorized, the handler's own validation answers 400.
    nf_test::expect_oauth2_enforced({
        .method = "POST",
        .url = "https://127.0.0.1:7779/nsmf-event-exposure/v1/subscriptions",
        .body = "{}",
        .scope = "nsmf-event-exposure",
        .nf_type = "SMF",
        .other_scope = "nsmf-pdusession",
        .authorized_status = 400,
    });

    auto client = nf_test::oauth2_test_client();
    const auto resp = nf_test::send_to(
        client,
        {.method = "POST",
         .url = "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts/unknown/pcf-notify/update",
         .body = "{}"},
        "");
    EXPECT_NE(resp.status, 0);
    EXPECT_NE(resp.status, 401) << "the notification reaches its handler without a token: "
                                << resp.body;
}

TEST(AmfAuthorization, NamfApisNeedTheirOwnScopeAndTheAmfsNfType) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    nf_test::SpawnedProcess amf(AMF_PATH);
    ASSERT_GT(amf.pid(), 0) << "failed to fork amf";

    nf_test::expect_oauth2_enforced({
        .method = "POST",
        .url = "https://127.0.0.1:7778/namf-evts/v1/subscriptions",
        .body = "{}",
        .scope = "namf-evts",
        .nf_type = "AMF",
        .other_scope = "namf-comm",
        .authorized_status = 400,
    });
}
