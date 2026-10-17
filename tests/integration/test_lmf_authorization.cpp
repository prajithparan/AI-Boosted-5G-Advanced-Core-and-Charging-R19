// ADR-0499 phase 4: the LMF enforces access tokens (oauth2_required, default true) on Nlmf_Location
// and Nlmf_DataExposure (TS29572_Nlmf_Location.yaml / _DataExposure.yaml, scopes nlmf-loc /
// nlmf-dataexposure). Nlmf_Broadcast declares no security scheme and stays mTLS only. Real nrf +
// lmf processes.

#include "oauth2_enforcement.hpp"
#include "spawn_guard.hpp"

#include <gtest/gtest.h>

TEST(LmfAuthorization, LocationAndDataExposureNeedATokenAndBroadcastDoesNot) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    nf_test::SpawnedProcess lmf(LMF_PATH);
    ASSERT_GT(lmf.pid(), 0) << "failed to fork lmf";

    // Empty bodies: once authorized, the handlers' own validation answers 400.
    nf_test::expect_oauth2_enforced({
        .method = "POST",
        .url = "https://127.0.0.1:7795/nlmf-loc/v1/cancel-location",
        .body = "{}",
        .scope = "nlmf-loc",
        .nf_type = "LMF",
        .other_scope = "nlmf-dataexposure",
        .authorized_status = 400,
    });
    nf_test::expect_oauth2_enforced({
        .method = "POST",
        .url = "https://127.0.0.1:7795/nlmf-dataexposure/v1/subscriptions",
        .body = "{}",
        .scope = "nlmf-dataexposure",
        .nf_type = "LMF",
        .other_scope = "nlmf-loc",
        .authorized_status = 400,
    });

    auto client = nf_test::oauth2_test_client();
    const auto resp =
        nf_test::send_to(client,
                         {.method = "POST",
                          .url = "https://127.0.0.1:7795/nlmf-broadcast/v1/cipher-key-data",
                          .body = "{}"},
                         "");
    EXPECT_EQ(resp.status, 400) << "Nlmf_Broadcast reaches its handler without a token: "
                                << resp.body;
}
