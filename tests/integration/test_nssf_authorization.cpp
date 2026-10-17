// ADR-0499 phase 4: the NSSF enforces access tokens (oauth2_required, default true) on
// Nnssf_NSSelection and Nnssf_NSSAIAvailability (TS29531_Nnssf_NSSelection.yaml /
// _NSSAIAvailability.yaml). Real nrf + nssf processes.

#include "oauth2_enforcement.hpp"
#include "spawn_guard.hpp"

#include <gtest/gtest.h>

TEST(NssfAuthorization, BothServicesNeedATokenForTheNssfAndTheirOwnScope) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    nf_test::SpawnedProcess nssf(NSSF_PATH);
    ASSERT_GT(nssf.pid(), 0) << "failed to fork nssf";

    // An empty body: once authorized, the handler's own validation answers 400.
    nf_test::expect_oauth2_enforced({
        .method = "POST",
        .url = "https://127.0.0.1:7801/nnssf-nssaiavailability/v1/nssai-availability/subscriptions",
        .body = "{}",
        .scope = "nnssf-nssaiavailability",
        .nf_type = "NSSF",
        .other_scope = "nnssf-nsselection",
        .authorized_status = 400,
    });
    // No query parameters: once authorized, the handler answers 400 (nf-type/nf-id are required).
    nf_test::expect_oauth2_enforced({
        .method = "GET",
        .url = "https://127.0.0.1:7801/nnssf-nsselection/v2/network-slice-information",
        .scope = "nnssf-nsselection",
        .nf_type = "NSSF",
        .other_scope = "nnssf-nssaiavailability",
        .authorized_status = 400,
    });
}
