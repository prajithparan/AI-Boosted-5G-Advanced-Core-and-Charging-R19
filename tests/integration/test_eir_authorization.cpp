// ADR-0499 phase 4: the 5G-EIR enforces access tokens (oauth2_required, default true) on
// N5g-eir_EquipmentIdentityCheck (TS29511_N5g-eir_EquipmentIdentityCheck.yaml, scope
// n5g-eir-eic). Real nrf + eir processes.

#include "oauth2_enforcement.hpp"
#include "spawn_guard.hpp"

#include <gtest/gtest.h>

TEST(EirAuthorization, RequestsNeedATokenForTheEirsOwnServiceAndNfType) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    nf_test::SpawnedProcess eir(EIR_PATH);
    ASSERT_GT(eir.pid(), 0) << "failed to fork eir";

    nf_test::expect_oauth2_enforced({
        .method = "GET",
        .url = "https://127.0.0.1:7792/n5g-eir-eic/v1/equipment-status?pei=imeisv-3510000000000010",
        .scope = "n5g-eir-eic",
        .nf_type = "5G_EIR",
        .other_scope = "nudm-sdm",
        .authorized_status = 200,
    });
}
