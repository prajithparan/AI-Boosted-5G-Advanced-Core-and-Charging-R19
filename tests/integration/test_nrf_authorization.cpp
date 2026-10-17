// ADR-0499 phase 4: the NRF enforces access tokens (oauth2_required, default true) on
// Nnrf_NFManagement and Nnrf_NFDiscovery, except NFRegister -- registration is the pre-requisite
// of a token (TS 33.501 13.4.1.1.2) -- and the token/bootstrapping endpoints, whose YAMLs declare
// no security scheme. Replacing a registered profile (NFUpdate) needs a token. Real nrf process.

#include "oauth2_enforcement.hpp"
#include "spawn_guard.hpp"

#include <gtest/gtest.h>

TEST(NrfAuthorization, DiscoveryNeedsATokenRegisterDoesNotAndUpdateDoes) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";

    nf_test::expect_oauth2_enforced({
        .method = "GET",
        .url = "https://127.0.0.1:7777/nnrf-disc/v1/nf-instances?target-nf-type=UDM&"
               "requester-nf-type=AMF",
        .scope = "nnrf-disc",
        .nf_type = "NRF",
        .other_scope = "nnrf-nfm",
        .authorized_status = 200,
    });

    auto client = nf_test::oauth2_test_client();
    const nf_test::EnforcedRoute put{
        .method = "PUT",
        .url =
            "https://127.0.0.1:7777/nnrf-nfm/v1/nf-instances/4c1f2a3b-0000-4000-8000-0000000000a1",
        .body = R"({"nfInstanceId":"4c1f2a3b-0000-4000-8000-0000000000a1","nfType":"AMF",)"
                R"("nfStatus":"REGISTERED","ipv4Addresses":["127.0.0.1"]})"};
    auto resp = nf_test::send_to(client, put, "");
    EXPECT_EQ(resp.status, 201) << "NFRegister needs no token: " << resp.body;
    resp = nf_test::send_to(client, put, "");
    EXPECT_EQ(resp.status, 401) << "NFUpdate of a registered profile needs one: " << resp.body;
    resp = nf_test::send_to(client, put, nf_test::oauth2_test_token(client, "nnrf-nfm", "NRF"));
    EXPECT_EQ(resp.status, 200) << resp.body;
}
