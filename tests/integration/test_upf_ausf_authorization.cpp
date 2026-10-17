// ADR-0499 phase 4: the UPF (TS29564_Nupf_EventExposure.yaml,
// TS29564_Nupf_GetUEPrivateIPaddrAndIdentifiers.yaml) and AUSF
// (TS29509_Nausf_UEAuthentication.yaml, TS29509_Nausf_UPUProtection.yaml) enforce access tokens
// (oauth2_required, default true). Real nrf + NF processes.

#include "oauth2_enforcement.hpp"
#include "spawn_guard.hpp"

#include <gtest/gtest.h>

// Empty bodies: once authorized, the handlers' own validation answers 400.
TEST(UpfAuthorization, EventExposureNeedsATokenForTheUpfAndItsService) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    nf_test::SpawnedProcess upf(UPF_PATH);
    ASSERT_GT(upf.pid(), 0) << "failed to fork upf";

    nf_test::expect_oauth2_enforced({
        .method = "POST",
        .url = "https://127.0.0.1:7796/nupf-ee/v1/ee-subscriptions",
        .body = "{}",
        .scope = "nupf-ee",
        .nf_type = "UPF",
        .other_scope = "nupf-gueip",
        .authorized_status = 400,
    });
}

TEST(AusfAuthorization, UeAuthenticationAndUpuProtectionNeedTheirOwnScope) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    nf_test::SpawnedProcess ausf(AUSF_PATH);
    ASSERT_GT(ausf.pid(), 0) << "failed to fork ausf";

    nf_test::expect_oauth2_enforced({
        .method = "POST",
        .url = "https://127.0.0.1:7782/nausf-auth/v1/ue-authentications",
        .body = "{}",
        .scope = "nausf-auth",
        .nf_type = "AUSF",
        .other_scope = "nausf-upuprotection",
        .authorized_status = 400,
    });
    nf_test::expect_oauth2_enforced({
        .method = "POST",
        .url = "https://127.0.0.1:7782/nausf-upuprotection/v1/imsi-999700000000001/ue-upu",
        .body = "{}",
        .scope = "nausf-upuprotection",
        .nf_type = "AUSF",
        .other_scope = "nausf-auth",
        .authorized_status = 400,
    });
}
