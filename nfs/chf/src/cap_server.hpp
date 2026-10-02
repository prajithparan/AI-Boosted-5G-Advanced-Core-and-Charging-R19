#pragma once

#include "sbi_core/rate_limit.hpp"
#include "sbi_core/tls_config.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include "charging_engine.hpp"
#include "ss7_core/sctp_socket.hpp"

// Private to nfs/chf -- not shared with any other NF, per CLAUDE.md's "no NF includes another NF's
// private headers" rule.
//
// P4.5/ADR-0061: CHF as a real CAP (TS 29.078) server, playing the real gsmSCF role toward a real
// gsmSSF peer (a real MSC/switch). Real direction, confirmed from `capssf-scfGenericAC`
// (TS 29.078 clause 6.1.2): the gsmSSF opens the dialogue (sends the real InitialDP), the gsmSCF
// (CHF, here) is the real responder -- the opposite direction from UDM's own MAP client role
// (ADR-0061's own MAP-side update), where the HLR-equivalent is the initiator. Same real "blocking
// I/O gets its own thread" discipline as `DiameterServer` (accept thread + one thread per
// association) and the same real per-connection dedicated `catalog_client`/`balance_client`
// rationale (`sbi_core::http2::Client`'s own documented "one Client instance per thread" contract).
//
// Real, disclosed scope -- now the full real TS 29.078 clause 11 call flow's happy path, both
// halves: (1) "arm the charging" -- receive `InitialDP`, rate/reserve via the exact same
// `chf::charge_one_usage` shared code path Gy/Rf/Sy already use (CHARGING_PROMPT.md's own
// single-code-path requirement, now extended to a fourth real protocol), convert the resulting
// `GrantedUnit.time` into `ApplyChargingArg.max_call_period_duration`, and respond with a single
// real TC-Continue carrying `RequestReportBCSMEvent` (arming `oAnswer`/`oDisconnect`) and
// `ApplyCharging`; (2) "close the charging" -- on the same association's later real TC-Continue
// messages, `EventReportBCSM` is logged (Class 4, "ALWAYS RESPONDS FALSE" per TS 29.078 clause
// 6.1.1 -- no real response is defined, logging IS the whole real obligation) and
// `ApplyChargingReport` finalizes the reservation via `chf::finalize_subscriber_balance` (the same
// real "finalize the full reserved total" simplification already disclosed for Diameter Gy's own
// CCR-Termination path, ADR-0057 -- not a new one) and closes the dialogue with a real, empty
// TC-End (Class 2, "RESULT FALSE" -- no real successful RESULT payload exists to send back,
// confirmed from the real operation definition, not guessed). Any other opcode, on either a
// TC-Begin or TC-Continue, is logged and ignored, not silently mishandled. `ratingGroup` is set to
// CAP's own real `serviceKey` (both are real integer identifiers selecting a rating/service
// context -- a real, disclosed conceptual mapping, not an arbitrary placeholder). Subscriber
// identity: `InitialDPArg.imsi` (TBCD, TS 23.003 §2.2, decoded via the new `libs/tbcd-core`)
// becomes SUPI `"imsi-" + digits`, the real TS 23.003 SUPI format already used elsewhere in this
// codebase's own test fixtures -- if `imsi` is absent (a real, valid `InitialDPArg` state per its
// own OPTIONAL tag), the InitialDP is rejected with a real `ReturnError` (`missingParameter`,
// TS 29.078 clause 5.4) rather than guessing an identity.
//
// Real, disclosed gap still open: no periodic re-authorization when `maxCallPeriodDuration`
// expires mid-call (real CAMEL would expect another `ApplyChargingReport`/`ApplyCharging` exchange
// at that point, not just at call end) -- this dialogue only ever runs one InitialDP -> one
// ApplyChargingReport -> close, not a real multi-report call. No `ReleaseCall`/`Connect` handling.
// The finalize step uses the FULL reserved total regardless of `ApplyChargingReport`'s own real
// reported elapsed time (logged, not applied to a proportional refund) -- same disclosed
// simplification as the Diameter path, not a new gap introduced here.

namespace chf {

class CapServer {
public:
    // P15 (ADR-0288): sustained_tps <= 0 disables it, which is the default.
    void set_tps_limit(double sustained_tps, double burst_capacity);

    // grant_counter/reserve_rejected_counter are the SAME real counters main.cpp's HTTP handlers
    // and DiameterServer already share (a real grant is a real grant regardless of which real
    // protocol triggered it -- CHARGING_PROMPT.md's own single-code-path property). initial_dp_
    // counter/apply_charging_counter are new, CAP-specific message-volume counters, analogous to
    // DiameterServer's own ccr_initial_counter etc.
    CapServer(std::uint16_t port,
              sbi_core::http2::TlsConfig client_tls,
              ChargingDataStore& charging_data_store,
              CdrWriter& cdr_writer,
              RatingDecisionStore& rating_decision_store,
              // ADR-0446: the same CatalogSnapshot instance main() passes to the HTTP handlers --
              // CAP shares build_rating_grant's rating path too (CHARGING_PROMPT P4.5).
              CatalogSnapshot& catalog_snapshot,
              opentelemetry::metrics::Counter<std::uint64_t>* grant_counter,
              opentelemetry::metrics::Counter<std::uint64_t>* reserve_rejected_counter,
              opentelemetry::metrics::Counter<std::uint64_t>* initial_dp_counter,
              opentelemetry::metrics::Counter<std::uint64_t>* apply_charging_counter);
    ~CapServer();

    CapServer(const CapServer&) = delete;
    CapServer& operator=(const CapServer&) = delete;

private:
    void accept_loop();
    void handle_connection(ss7_core::SctpSocket socket);

    ss7_core::SctpSocket listener_;
    // P15 (ADR-0288): the SS7/M3UA front door's own ceiling, the third and last protocol. Null
    // unless configured -- see the .cpp for why a shed here drops rather than answers.
    // ADR-0290: this server starts its accept thread in its own CONSTRUCTOR, so main() calls
    // set_tps_limit() while connection threads are already running and reading this. The bucket
    // itself is internally locked; the POINTER to it was the race. Owner + atomic view: the owner
    // keeps it alive, the atomic is what the message loop reads.
    std::unique_ptr<sbi_core::TokenBucket> rate_limit_owner_;
    std::atomic<sbi_core::TokenBucket*> rate_limit_{nullptr};
    sbi_core::http2::TlsConfig client_tls_;
    ChargingDataStore& charging_data_store_;
    CdrWriter& cdr_writer_;
    RatingDecisionStore& rating_decision_store_;
    CatalogSnapshot& catalog_snapshot_;
    opentelemetry::metrics::Counter<std::uint64_t>* grant_counter_;
    opentelemetry::metrics::Counter<std::uint64_t>* reserve_rejected_counter_;
    opentelemetry::metrics::Counter<std::uint64_t>* initial_dp_counter_;
    opentelemetry::metrics::Counter<std::uint64_t>* apply_charging_counter_;
    std::thread accept_thread_;
    std::atomic<bool> stop_{false};
};

} // namespace chf
