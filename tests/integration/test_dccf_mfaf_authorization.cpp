// ADR-0499 phase 4: the DCCF (TS29574_Ndccf_DataManagement.yaml / _ContextManagement.yaml) and
// MFAF (TS29576_Nmfaf_3daDataManagement.yaml / TS29576_Nmfaf_3caDataManagement.yaml) enforce access
// tokens (oauth2_required, default true); the MFAF's notification target and fetch callback do not.
// Real nrf + NF processes.

#include "oauth2_enforcement.hpp"
#include "spawn_guard.hpp"

#include <gtest/gtest.h>

// Empty bodies: once authorized, the handlers' own validation answers 400.
TEST(DccfAuthorization, DataManagementNeedsATokenForTheDccfAndItsService) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    nf_test::SpawnedProcess dccf(DCCF_PATH);
    ASSERT_GT(dccf.pid(), 0) << "failed to fork dccf";

    nf_test::expect_oauth2_enforced({
        .method = "POST",
        .url = "https://127.0.0.1:7803/ndccf-datamanagement/v1/data-subscriptions",
        .body = "{}",
        .scope = "ndccf-datamanagement",
        .nf_type = "DCCF",
        .other_scope = "ndccf-contextmanagement",
        .authorized_status = 400,
    });
}

TEST(MfafAuthorization, ConfigurationsNeedATokenAndTheMfafCallbacksDoNot) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    nf_test::SpawnedProcess mfaf(MFAF_PATH);
    ASSERT_GT(mfaf.pid(), 0) << "failed to fork mfaf";

    nf_test::expect_oauth2_enforced({
        .method = "POST",
        .url = "https://127.0.0.1:7800/nmfaf-3dadatamanagement/v1/configurations",
        .body = "{}",
        .scope = "nmfaf-3dadatamanagement",
        .nf_type = "MFAF",
        .other_scope = "nmfaf-contextmanagement",
        .authorized_status = 400,
    });

    // TS 33.501 13.4.1.0: the notification and its fetch callback carry no token.
    auto client = nf_test::oauth2_test_client();
    for (const std::string path :
         {"/mfaf-inbound/v1/notifications/unknown", "/mfaf-inbound/v1/fetch"}) {
        const auto resp = nf_test::send_to(
            client, {.method = "POST", .url = "https://127.0.0.1:7800" + path, .body = "{}"}, "");
        EXPECT_NE(resp.status, 0) << path;
        EXPECT_NE(resp.status, 401)
            << path << " reaches its handler without a token: " << resp.body;
    }
}
