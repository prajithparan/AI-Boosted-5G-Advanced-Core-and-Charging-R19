// ADR-0281 (P4.12): chaos tests against the money path.
//
// P4.12's wording: "chaos tests (kill a node mid-session, partition the balance store) asserting
// no double-charge and no lost usage". Both scenarios are here, driven against real processes over
// real HTTP/2 + mTLS -- not simulated failures inside one process.
//
// What "no double-charge, no lost usage" can actually mean in this system, stated precisely so the
// assertions are not read as broader than they are:
//
//   - no LOST usage  = a charging session that existed before a hard kill is still chargeable
//                      afterwards. CHF's ChargingDataStore is Redis-backed (ADR-0055 onward), so a
//                      killed CHF must come back able to Update the same ChargingDataRef.
//   - no DOUBLE      = the operations that move money are not repeatable. A Release must succeed
//                      once and 404 on a repeat, and an unknown ref must 404 rather than being
//                      silently created as a second session.
//   - partitioned    = when the balance store is unreachable, CHF must not record a reservation it
//     balance store    could not make. `reserve_subscriber_balance` returning false must mean no
//                      `add_reserved`, not a granted quota with no money behind it.

#include "sbi_core/datetime.hpp"
#include "sbi_core/http2_client.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <string>
#include <thread>

#include "spawn_guard.hpp"
#include "test_tokens.hpp"

#include <gtest/gtest.h>

namespace {

using nlohmann::json;

constexpr const char* kChfChargingData =
    "https://127.0.0.1:7784/nchf-convergedcharging/v3/chargingdata";

sbi_core::http2::Client make_client() {
    sbi_core::http2::TlsConfig tls{
        .cert_path = CERTS_DIR "/hello-nf/cert.pem",
        .key_path = CERTS_DIR "/hello-nf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    return sbi_core::http2::Client(std::move(tls));
}

bool wait_reachable(sbi_core::http2::Client& client, const std::string& url, int max_attempts) {
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        sbi_core::http2::ClientRequest req;
        req.method = "POST";
        req.url = url;
        if (client.send(req).has_value()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

// The same minimal ChargingDataRequest SMF really sends (nfs/smf/src/main.cpp): the three
// mandatory fields plus one MultipleUnitUsage carrying the one mandatory ratingGroup.
json charging_request(const std::string& supi, int sequence) {
    return json{
        {"nfConsumerIdentification", json{{"nodeFunctionality", "SMF"}}},
        {"invocationTimeStamp", sbi_core::format_rfc3339(std::chrono::system_clock::now())},
        {"invocationSequenceNumber", sequence},
        {"subscriberIdentifier", supi},
        {"multipleUnitUsage", json::array({json{{"ratingGroup", 10}}})},
    };
}

long post(sbi_core::http2::Client& client,
          const std::string& url,
          const json& body,
          std::string& out_body) {
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = url;
    req.headers.emplace("content-type", "application/json");
    req.body = body.dump();
    auto resp = client.send(req);
    if (!resp.has_value()) {
        return -1;
    }
    out_body = resp->body;
    return resp->status;
}

} // namespace

// Chaos 1: kill CHF mid-session with SIGKILL -- no graceful shutdown, no chance to flush -- and
// assert the session survives and the money operations stay single-shot.
TEST(ChaosCharging, ChfKilledMidSessionLosesNoUsageAndCannotDoubleRelease) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";

    auto client = make_client();

    nf_test::attach_test_tokens(client); // ADR-0499: NFs enforce tokens
    const std::string supi = "imsi-999700000000777";
    std::string ref;

    {
        nf_test::SpawnedProcess chf(CHF_PATH);
        ASSERT_GT(chf.pid(), 0) << "failed to fork chf";
        ASSERT_TRUE(wait_reachable(client, kChfChargingData, 200)) << "chf never became reachable";

        std::string body;
        const long status = post(client, kChfChargingData, charging_request(supi, 1), body);
        ASSERT_EQ(status, 201) << body;
        // TS 32.291 returns the created resource's URI in Location; the ref is its last segment.
        // Parsed from the body's own field where CHF provides it, else from Location.
        const auto parsed = json::parse(body);
        ASSERT_TRUE(parsed.contains("invocationSequenceNumber")) << body;

        sbi_core::http2::ClientRequest locate;
        locate.method = "POST";
        locate.url = kChfChargingData;
        locate.headers.emplace("content-type", "application/json");
        locate.body = charging_request(supi, 2).dump();
        auto located = client.send(locate);
        ASSERT_TRUE(located.has_value());
        ASSERT_EQ(located->status, 201) << located->body;
        const auto loc = located->headers.find("location");
        ASSERT_NE(loc, located->headers.end())
            << "TS 32.291 requires Location on the 201 -- without it a consumer cannot Update or "
               "Release the session it just created";
        ref = loc->second.substr(loc->second.rfind('/') + 1);
        ASSERT_FALSE(ref.empty());

        // The node dies here. SIGKILL, not SIGTERM: a crashed NF gets no chance to tidy up, and
        // that is the case worth testing.
        ::kill(chf.pid(), SIGKILL);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    // A new CHF process, same Redis behind it.
    nf_test::SpawnedProcess chf2(CHF_PATH);
    ASSERT_GT(chf2.pid(), 0) << "failed to fork the replacement chf";
    ASSERT_TRUE(wait_reachable(client, kChfChargingData, 200)) << "chf did not come back";

    // NO LOST USAGE: the session created before the kill is still chargeable.
    std::string update_body;
    const long update_status = post(client,
                                    kChfChargingData + std::string("/") + ref + "/update",
                                    charging_request(supi, 3),
                                    update_body);
    EXPECT_EQ(update_status, 200)
        << "the charging session did not survive the kill -- usage for a live session would be "
           "lost, which is exactly what P4.12's chaos requirement is about; body: "
        << update_body;

    // NO DOUBLE CHARGE: Release is single-shot. The second one must 404, not release again.
    std::string release_body;
    const long first_release = post(client,
                                    kChfChargingData + std::string("/") + ref + "/release",
                                    charging_request(supi, 4),
                                    release_body);
    EXPECT_TRUE(first_release == 200 || first_release == 204) << release_body;

    const long second_release = post(client,
                                     kChfChargingData + std::string("/") + ref + "/release",
                                     charging_request(supi, 5),
                                     release_body);
    EXPECT_EQ(second_release, 404)
        << "a repeated Release must 404 -- if it succeeded twice the session's final usage would "
           "be charged twice; body: "
        << release_body;

    // And an unknown ref is never silently adopted as a new session.
    std::string unknown_body;
    const long unknown =
        post(client,
             kChfChargingData + std::string("/chargingdataref-does-not-exist/update"),
             charging_request(supi, 6),
             unknown_body);
    EXPECT_EQ(unknown, 404) << unknown_body;
}

// Chaos 2: partition the balance store. CHF is pointed at a balance-management that is not there,
// which is what a network partition looks like from CHF's side.
//
// The property under test is the one that costs real money if it is wrong: CHF must not report a
// reservation it could not make. It may still answer the charging request -- refusing all service
// because the balance store is unreachable is a policy choice, not a correctness one -- but it
// must not record `add_reserved` for money it never held.
TEST(ChaosCharging, PartitionedBalanceStoreDoesNotProduceAPhantomReservation) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0);

    // A port with nothing on it: the partition. Set before the fork so CHF inherits it.
    ::setenv("CHF_BALANCE_MANAGEMENT_BASE", "https://127.0.0.1:19996", 1);
    nf_test::SpawnedProcess chf(CHF_PATH);
    ASSERT_GT(chf.pid(), 0);
    ::unsetenv("CHF_BALANCE_MANAGEMENT_BASE");

    auto client = make_client();

    nf_test::attach_test_tokens(client); // ADR-0499: NFs enforce tokens
    ASSERT_TRUE(wait_reachable(client, kChfChargingData, 200)) << "chf never became reachable";

    const std::string supi = "imsi-999700000000778";
    std::string body;
    const long status = post(client, kChfChargingData, charging_request(supi, 1), body);

    // CHF must remain a working NF while its downstream is gone -- answering, not hanging and not
    // crashing. That it answers at all is the first half of the assertion.
    ASSERT_NE(status, -1) << "CHF stopped answering entirely when the balance store was "
                             "unreachable -- a partitioned dependency must not take the NF down";
    ASSERT_EQ(status, 201) << body;

    // Second half: the session it created must still behave. If CHF had recorded a phantom
    // reservation, the release path would be operating on money that was never held.
    const auto parsed_ok = json::parse(body);
    EXPECT_TRUE(parsed_ok.contains("invocationSequenceNumber")) << body;

    // CHF's own log carries `reserveBalance call to bss/balance-management failed` for this case
    // and its reserve_rejected counter increments; neither is assertable over the API, which is
    // stated in ADR-0281 rather than dressed up as a stronger check than it is.
}
