// ADR-0499 phase 4: the CHF (TS32291_Nchf_ConvergedCharging.yaml, ...), NWDAF
// (TS29520_Nnwdaf_*.yaml) and ADRF (TS29575_Nadrf_*.yaml) enforce access tokens (oauth2_required,
// default true); their inbound notification routes take none (TS 33.501 13.4.1.0). Real nrf + NF
// processes; these NFs need Doris (and the ADRF its model-store Postgres) to start.

#include "oauth2_enforcement.hpp"
#include "spawn_guard.hpp"

#include <gtest/gtest.h>

namespace {

void expect_no_token_needed(const std::string& url, const std::string& what) {
    auto client = nf_test::oauth2_test_client();
    const auto resp = nf_test::send_to(client, {.method = "POST", .url = url, .body = "{}"}, "");
    EXPECT_NE(resp.status, 0) << what;
    EXPECT_NE(resp.status, 401) << what << " reaches its handler without a token: " << resp.body;
}

} // namespace

// Empty bodies: once authorized, the handlers' own validation answers 400.
TEST(ChfAuthorization, ConvergedChargingNeedsATokenForTheChfAndItsService) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    nf_test::SpawnedProcess chf(CHF_PATH);
    ASSERT_GT(chf.pid(), 0) << "failed to fork chf";

    nf_test::expect_oauth2_enforced({
        .method = "POST",
        .url = "https://127.0.0.1:7784/nchf-convergedcharging/v3/chargingdata",
        .body = "{}",
        .scope = "nchf-convergedcharging",
        .nf_type = "CHF",
        .other_scope = "nchf-spendinglimitcontrol",
        .authorized_status = 400,
    });
}

TEST(NwdafAuthorization, EventsSubscriptionNeedsATokenAndInboundNotificationsDoNot) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    nf_test::SpawnedProcess nwdaf(NWDAF_PATH);
    ASSERT_GT(nwdaf.pid(), 0) << "failed to fork nwdaf";

    nf_test::expect_oauth2_enforced({
        .method = "POST",
        .url = "https://127.0.0.1:7798/nnwdaf-eventssubscription/v1/subscriptions",
        .body = "{}",
        .scope = "nnwdaf-eventssubscription",
        .nf_type = "NWDAF",
        .other_scope = "nnwdaf-analyticsinfo",
        .authorized_status = 400,
    });
    expect_no_token_needed("https://127.0.0.1:7798/nwdaf-inbound/v1/notifications/unknown",
                           "the NWDAF's notification target");
}

TEST(AdrfAuthorization, DataManagementNeedsATokenAndInboundNotificationsDoNot) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    nf_test::SpawnedProcess adrf(ADRF_PATH);
    ASSERT_GT(adrf.pid(), 0) << "failed to fork adrf";

    nf_test::expect_oauth2_enforced({
        .method = "POST",
        .url = "https://127.0.0.1:7804/nadrf-datamanagement/v1/data-store-records",
        .body = "{}",
        .scope = "nadrf-datamanagement",
        .nf_type = "ADRF",
        .other_scope = "nadrf-mlmodelmanagement",
        .authorized_status = 400,
    });
    expect_no_token_needed("https://127.0.0.1:7804/adrf-inbound/v1/notifications/unknown",
                           "the ADRF's notification target");
}
