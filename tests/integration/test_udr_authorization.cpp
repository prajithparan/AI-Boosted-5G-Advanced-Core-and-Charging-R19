// ADR-0499 phase 4: the UDR enforces access tokens (oauth2_required, default true) on Nudr_DR
// (scope nudr-dr) and Nudr_GroupIDmap (TS29504_Nudr_GroupIDmap.yaml, scope nudr-group-id-map).
// The project-owned OAM provisioning API is not an SBI API and keeps its mTLS allow-list instead
// (ADR-0382). Real nrf + udr processes.

#include "oauth2_enforcement.hpp"
#include "spawn_guard.hpp"

#include <gtest/gtest.h>

TEST(UdrAuthorization, NudrNeedsATokenForTheUdrAndOamProvisioningDoesNot) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    nf_test::SpawnedProcess udr(UDR_PATH);
    ASSERT_GT(udr.pid(), 0) << "failed to fork udr";

    // The UDR's own startup seed (ADR-0069) provisions this subscriber.
    nf_test::expect_oauth2_enforced({
        .method = "GET",
        .url = "https://127.0.0.1:7781/nudr-dr/v2/subscription-data/imsi-999700000000001/"
               "authentication-data/authentication-subscription",
        .scope = "nudr-dr",
        .nf_type = "UDR",
        .other_scope = "nudr-group-id-map",
        .authorized_status = 200,
    });

    auto client = nf_test::oauth2_test_client();
    const auto resp = nf_test::send_to(
        client,
        {.method = "PUT",
         .url = "https://127.0.0.1:7781/oam-provisioning/v1/subscribers/imsi-999700000000001",
         .body = "{}"},
        "");
    EXPECT_NE(resp.status, 0);
    EXPECT_NE(resp.status, 401) << "OAM provisioning is decided by its mTLS allow-list: "
                                << resp.body;
}
