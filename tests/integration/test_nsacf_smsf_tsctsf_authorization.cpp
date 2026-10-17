// ADR-0499 phase 4: the NSACF (TS29536_Nnsacf_NSAC.yaml / _SliceEventExposure.yaml), SMSF
// (TS29540_Nsmsf_SMService.yaml) and TSCTSF (TS29565_Ntsctsf_QoSandTSCAssistance.yaml) enforce
// access tokens (oauth2_required, default true). Real nrf + NF processes.

#include "oauth2_enforcement.hpp"
#include "spawn_guard.hpp"

#include <gtest/gtest.h>

// Empty bodies: once authorized, the handlers' own validation answers 400.
TEST(NsacfAuthorization, BothServicesNeedATokenForTheNsacfAndTheirOwnScope) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    nf_test::SpawnedProcess nsacf(NSACF_PATH);
    ASSERT_GT(nsacf.pid(), 0) << "failed to fork nsacf";

    nf_test::expect_oauth2_enforced({
        .method = "POST",
        .url = "https://127.0.0.1:7797/nnsacf-nsac/v1/slices/ues",
        .body = "{}",
        .scope = "nnsacf-nsac",
        .nf_type = "NSACF",
        .other_scope = "nnsacf-slice-ee",
        .authorized_status = 400,
    });
    nf_test::expect_oauth2_enforced({
        .method = "POST",
        .url = "https://127.0.0.1:7797/nnsacf-slice-ee/v1/subscriptions",
        .body = "{}",
        .scope = "nnsacf-slice-ee",
        .nf_type = "NSACF",
        .other_scope = "nnsacf-nsac",
        .authorized_status = 400,
    });
}

TEST(SmsfAuthorization, NsmsfSmsNeedsATokenForTheSmsfAndItsService) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    nf_test::SpawnedProcess smsf(SMSF_PATH);
    ASSERT_GT(smsf.pid(), 0) << "failed to fork smsf";

    nf_test::expect_oauth2_enforced({
        .method = "PUT",
        .url = "https://127.0.0.1:7793/nsmsf-sms/v2/ue-contexts/imsi-999700000000001",
        .body = "{}",
        .scope = "nsmsf-sms",
        .nf_type = "SMSF",
        .other_scope = "namf-comm",
        .authorized_status = 400,
    });
}

TEST(TsctsfAuthorization, QosTscaiNeedsATokenAndThePcfNotificationDoesNot) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    nf_test::SpawnedProcess tsctsf(TSCTSF_PATH);
    ASSERT_GT(tsctsf.pid(), 0) << "failed to fork tsctsf";

    nf_test::expect_oauth2_enforced({
        .method = "POST",
        .url = "https://127.0.0.1:7825/ntsctsf-qos-tscai/v1/tsc-app-sessions",
        .body = "{}",
        .scope = "ntsctsf-qos-tscai",
        .nf_type = "TSCTSF",
        .other_scope = "npcf-policyauthorization",
        .authorized_status = 400,
    });

    // TS 33.501 13.4.1.0: the notification operation carries no token.
    auto client = nf_test::oauth2_test_client();
    const auto resp =
        nf_test::send_to(client,
                         {.method = "POST",
                          .url = "https://127.0.0.1:7825/ntsctsf-qos-tscai/v1/pcf-notify/app-1",
                          .body = "{}"},
                         "");
    EXPECT_EQ(resp.status, 204) << resp.body;
}
