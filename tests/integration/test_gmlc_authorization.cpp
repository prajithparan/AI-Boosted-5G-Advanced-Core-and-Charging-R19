// ADR-0499 phase 4: the GMLC enforces access tokens (oauth2_required, default true) on
// Ngmlc_Location (TS29515_Ngmlc_Location.yaml, scope ngmlc-loc). Real nrf + gmlc processes.

#include "oauth2_enforcement.hpp"
#include "spawn_guard.hpp"

#include <gtest/gtest.h>

TEST(GmlcAuthorization, NgmlcLocationNeedsATokenForTheGmlcAndItsService) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    nf_test::SpawnedProcess gmlc(GMLC_PATH);
    ASSERT_GT(gmlc.pid(), 0) << "failed to fork gmlc";

    // An empty body: once authorized, the handler's own validation answers 400.
    nf_test::expect_oauth2_enforced({
        .method = "POST",
        .url = "https://127.0.0.1:7794/ngmlc-loc/v1/cancel-location",
        .body = "{}",
        .scope = "ngmlc-loc",
        .nf_type = "GMLC",
        .other_scope = "nlmf-loc",
        .authorized_status = 400,
    });
}
