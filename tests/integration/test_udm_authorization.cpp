// ADR-0499 phase 4: the UDM enforces access tokens (oauth2_required, default true) on its nudm-*
// APIs (TS29503_Nudm_*.yaml), each with its own scope. Real nrf + udr + udm processes: the
// authorized read goes on to the (also enforcing) UDR with the UDM's own token.

#include "oauth2_enforcement.hpp"
#include "spawn_guard.hpp"

#include <gtest/gtest.h>

TEST(UdmAuthorization, EachNudmApiNeedsItsOwnScopeAndTheUdmsNfType) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    nf_test::SpawnedProcess udr(UDR_PATH);
    ASSERT_GT(udr.pid(), 0) << "failed to fork udr";
    nf_test::SpawnedProcess udm(UDM_PATH);
    ASSERT_GT(udm.pid(), 0) << "failed to fork udm";

    auto probe = nf_test::oauth2_test_client();
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (nf_test::send_to(probe,
                             {.method = "GET",
                              .url = "https://127.0.0.1:7781/nudr-dr/v2/subscription-data/none"},
                             "")
                .status != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // The UDR's startup seed (ADR-0069) provisions this subscriber's am-data.
    nf_test::expect_oauth2_enforced({
        .method = "GET",
        .url = "https://127.0.0.1:7780/nudm-sdm/v2/imsi-999700000000001/am-data",
        .scope = "nudm-sdm",
        .nf_type = "UDM",
        .other_scope = "nudm-uecm",
        .authorized_status = 200,
    });
    // An empty body: once authorized, the handler's own validation answers 400.
    nf_test::expect_oauth2_enforced({
        .method = "POST",
        .url = "https://127.0.0.1:7780/nudm-ueau/v1/imsi-999700000000001/security-information/"
               "generate-auth-data",
        .body = "{}",
        .scope = "nudm-ueau",
        .nf_type = "UDM",
        .other_scope = "nudm-sdm",
        .authorized_status = 400,
    });
}
