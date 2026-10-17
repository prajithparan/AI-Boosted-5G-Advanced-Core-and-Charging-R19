// ADR-0499 phase 4: the BSF enforces access tokens (oauth2_required, default true) on
// Nbsf_Management (TS29521_Nbsf_Management.yaml, scope nbsf-management). Real nrf + bsf processes.

#include "oauth2_enforcement.hpp"
#include "spawn_guard.hpp"

#include <gtest/gtest.h>

TEST(BsfAuthorization, NbsfManagementNeedsATokenForTheBsfAndItsService) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    nf_test::SpawnedProcess bsf(BSF_PATH);
    ASSERT_GT(bsf.pid(), 0) << "failed to fork bsf";

    // An empty body: once authorized, the handler's own validation answers 400.
    nf_test::expect_oauth2_enforced({
        .method = "POST",
        .url = "https://127.0.0.1:7789/nbsf-management/v1/pcfBindings",
        .body = "{}",
        .scope = "nbsf-management",
        .nf_type = "BSF",
        .other_scope = "npcf-policyauthorization",
        .authorized_status = 400,
    });
}
