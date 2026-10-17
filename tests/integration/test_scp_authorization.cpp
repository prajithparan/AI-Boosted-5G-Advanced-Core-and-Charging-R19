// ADR-0499 phase 4: the SCP enforces access tokens (oauth2_required, default true) on
// Nscp_EventExposure (TS29570_Nscp_EventExposure.yaml, scope nscp-ee). Real nrf + scp processes.

#include "oauth2_enforcement.hpp"
#include "spawn_guard.hpp"

#include <gtest/gtest.h>

TEST(ScpAuthorization, NscpEeNeedsATokenForTheScpAndItsService) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    nf_test::SpawnedProcess scp(SCP_PATH);
    ASSERT_GT(scp.pid(), 0) << "failed to fork scp";

    // An empty body: once authorized, the handler's own validation answers 400.
    nf_test::expect_oauth2_enforced({
        .method = "POST",
        .url = "https://127.0.0.1:7791/nscp-ee/v1/subscriptions",
        .body = "{}",
        .scope = "nscp-ee",
        .nf_type = "SCP",
        .other_scope = "nnrf-disc",
        .authorized_status = 400,
    });
}
