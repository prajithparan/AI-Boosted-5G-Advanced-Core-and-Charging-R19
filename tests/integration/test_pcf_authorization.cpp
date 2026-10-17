// ADR-0499 phase 4: the PCF enforces access tokens (oauth2_required, default true) on its npcf-*
// APIs (TS29512_Npcf_SMPolicyControl.yaml, TS29514_Npcf_PolicyAuthorization.yaml, ...), each with
// its own scope; the CHF's spending-limit notification (TS 33.501 13.4.1.0) needs none. Real nrf +
// pcf processes.

#include "oauth2_enforcement.hpp"
#include "spawn_guard.hpp"

#include <gtest/gtest.h>

TEST(PcfAuthorization, EachNpcfApiNeedsItsOwnScopeAndTheSpendingLimitNotifyDoesNot) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    nf_test::SpawnedProcess pcf(PCF_PATH);
    ASSERT_GT(pcf.pid(), 0) << "failed to fork pcf";

    // Empty bodies: once authorized, the handlers' own validation answers 400.
    nf_test::expect_oauth2_enforced({
        .method = "POST",
        .url = "https://127.0.0.1:7783/npcf-smpolicycontrol/v1/sm-policies",
        .body = "{}",
        .scope = "npcf-smpolicycontrol",
        .nf_type = "PCF",
        .other_scope = "npcf-policyauthorization",
        .authorized_status = 400,
    });
    nf_test::expect_oauth2_enforced({
        .method = "POST",
        .url = "https://127.0.0.1:7783/npcf-policyauthorization/v1/app-sessions",
        .body = "{}",
        .scope = "npcf-policyauthorization",
        .nf_type = "PCF",
        .other_scope = "npcf-smpolicycontrol",
        .authorized_status = 400,
    });

    auto client = nf_test::oauth2_test_client();
    const auto resp = nf_test::send_to(
        client,
        {.method = "POST",
         .url = "https://127.0.0.1:7783/npcf-smpolicycontrol/v1/sm-policies/unknown/"
                "spending-limit-notify/notify",
         .body = "{}"},
        "");
    EXPECT_NE(resp.status, 0);
    EXPECT_NE(resp.status, 401) << "the notification reaches its handler without a token: "
                                << resp.body;
}
