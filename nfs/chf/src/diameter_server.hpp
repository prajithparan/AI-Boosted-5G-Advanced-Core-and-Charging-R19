#pragma once

#include "sbi_core/rate_limit.hpp"
#include "sbi_core/tls_config.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

#include "charging_engine.hpp"

// Private to nfs/chf -- not shared with any other NF, per CLAUDE.md's "no NF includes another NF's
// private headers" rule.
//
// P4.5/ADR-0059 Stage 2: CHF as a real Diameter server, accepting a real TCP connection (Diameter
// runs over TCP or SCTP, RFC 6733 -- this project uses plain TCP, matching pfcp_core's own
// UDP-not-SCTP choice for the same reason: Boost.Asio has no native SCTP support, and TCP is a
// fully spec-conformant transport option, not a simplification). Same "blocking I/O gets its own
// thread" discipline this project already uses for SMF's PfcpPeer and run_nrf_lifecycle
// (docs/DECISIONS.md ADR-0006/ADR-0019/ADR-0039): a dedicated accept thread, and one further
// dedicated thread per accepted connection (real Diameter deployments expect a small, stable
// number of long-lived peer connections, not per-request connections -- thread-per-connection is
// the right shape here, unlike per-request HTTP/2 handling).
//
// P4.5/ADR-0060 Stage 3: the per-connection loop now stays open after CER/CEA and dispatches real
// CCR-I/U/T (RFC 4006), calling the exact same chf::charge_one_usage/chf::finalize_subscriber_
// balance functions Nchf_ConvergedCharging's own HTTP Create/Update/Release handlers use
// (charging_engine.hpp) -- the single-code-path property CHARGING_PROMPT.md's P4.5 explicitly
// requires. Each connection thread builds its OWN dedicated product-catalog/balance-management
// http2::Client pair (sbi_core::http2::Client's own documented "one Client instance per thread"
// contract, libcurl's own real per-easy-handle single-thread requirement) -- NOT the HTTP route
// handlers' catalog_client/balance_client, which are confined to CHF's single io_context thread.
// The shared ChargingDataStore/CdrWriter/RatingDecisionStore ARE passed in and reused (Redis's own
// real connection pool is documented thread-safe already, ADR-0055; CdrWriter/RatingDecisionStore
// gained a real mutex this Stage specifically because this new concurrent caller now exists, see
// their own header comments).
//
// P4.5/ADR-0059 Stage 4 (Rf half): the same per-connection loop also dispatches real Diameter Base
// Accounting ACR/ACA (RFC 6733 §9, command-code 271, Application-Id 3 -- TS 32.299's Rf reference
// point runs this same real base-protocol application, not a 3GPP-specific one), normalized onto
// `Nchf_OfflineOnlyCharging`'s own real Create/Update/Release (`offline_charging_data_store`) --
// the same shared-HTTP-handler-state normalize pattern Stage 3 established for Gy/CCR, just with no
// rating engine involved (`Nchf_OfflineOnlyCharging` never had one, see main.cpp's own header).
//
// P4.5/ADR-0059 Stage 4 (Sy half): the same per-connection loop also dispatches real TS 29.219 SLR/
// STR (command-codes 8388635/275, real vendor-specific Application-Id 16777302 -- spec text read
// directly from the user-provided ETSI TS 129 219 V19.0.0 PDF, `specs/ts_129219v190000p.pdf`, since
// no dict_sy exists anywhere in the vendored freeDiameter tree, disclosed in dictionary.hpp's own
// header), normalized onto `Nchf_SpendingLimitControl`'s own real Subscribe/Update/Unsubscribe
// (`spending_limit_store`) -- CHF is the real OCS/server role on Sy, exactly matching its already-
// real SERVER role on the HTTP `Nchf_SpendingLimitControl` side (ADR-0055), so no direction
// mismatch to resolve. OCS-initiated SNR (policy-counter-change push notifications) is NOT
// implemented -- same real, disclosed gap as the HTTP side's own statusNotification callback (no
// policy-counter-breach-detection engine exists in this codebase to trigger either one from).

namespace chf {

class DiameterServer {
public:
    // Binds 0.0.0.0:port and starts the accept thread immediately. origin_host/origin_realm are
    // this CHF's own real Diameter identity (echoed in every CEA's Origin-Host/Origin-Realm AVPs).
    // client_tls is the mTLS config used to build a fresh, dedicated product-catalog/balance-
    // management http2::Client pair per Diameter connection thread (same CERTS_DIR "/chf/..."
    // identity the HTTP route handlers' own clients already use, ADR-0047/ADR-0056). The
    // charging_data_store/cdr_writer/rating_decision_store references and counters are the SAME
    // instances main() passes to the HTTP server -- real shared state, not a Diameter-only copy.
    DiameterServer(std::uint16_t port,
                   std::string origin_host,
                   std::string origin_realm,
                   sbi_core::http2::TlsConfig client_tls,
                   ChargingDataStore& charging_data_store,
                   CdrWriter& cdr_writer,
                   RatingDecisionStore& rating_decision_store,
                   OfflineChargingDataStore& offline_charging_data_store,
                   SpendingLimitSubscriptionStore& spending_limit_store,
                   PolicyCounterConfigStore& policy_counter_config_store,
                   // ADR-0446: the same CatalogSnapshot instance main() passes to the HTTP
                   // handlers -- Gy shares build_rating_grant's rating path (CHARGING_PROMPT P4.5's
                   // single-code-path property), so it shares the snapshot that path now consults.
                   CatalogSnapshot& catalog_snapshot,
                   opentelemetry::metrics::Counter<std::uint64_t>* grant_counter,
                   opentelemetry::metrics::Counter<std::uint64_t>* reserve_rejected_counter,
                   opentelemetry::metrics::Counter<std::uint64_t>* ccr_initial_counter,
                   opentelemetry::metrics::Counter<std::uint64_t>* ccr_update_counter,
                   opentelemetry::metrics::Counter<std::uint64_t>* ccr_termination_counter,
                   opentelemetry::metrics::Counter<std::uint64_t>* acr_event_counter,
                   opentelemetry::metrics::Counter<std::uint64_t>* acr_start_counter,
                   opentelemetry::metrics::Counter<std::uint64_t>* acr_interim_counter,
                   opentelemetry::metrics::Counter<std::uint64_t>* acr_stop_counter,
                   opentelemetry::metrics::Counter<std::uint64_t>* slr_initial_counter,
                   opentelemetry::metrics::Counter<std::uint64_t>* slr_intermediate_counter,
                   opentelemetry::metrics::Counter<std::uint64_t>* str_counter);
    ~DiameterServer();

    DiameterServer(const DiameterServer&) = delete;
    DiameterServer& operator=(const DiameterServer&) = delete;

    // ADR-0447 (increment 2 of ADR-0445/0446): sends a real RAR (RFC 6733 §8.3.1) for an active Gy
    // session and blocks (bounded by `timeout`) for the matching RAA, returning its real
    // Result-Code. std::nullopt means "could not get an answer" -- the session_id is unknown (no
    // live connection currently holds it; already disconnected, or never existed), the write
    // itself failed (peer gone), or no RAA arrived within `timeout` -- three real, valid "can't
    // reach this peer" outcomes this function does not try to distinguish further, rather than
    // inventing a code for a distinction the base protocol doesn't make either.
    //
    // Real, disclosed simplification: only ONE RAR may be outstanding per CONNECTION (not per
    // session) at a time -- a second call targeting a different session multiplexed on the same
    // connection while one is already in flight waits behind it rather than being independently
    // correlated. Real Diameter deployments support concurrent in-flight requests per connection;
    // this project's first RAR implementation does not yet need that generality, and building the
    // full per-request correlation table it would take is deferred, not silently assumed away.
    std::optional<std::int32_t>
    send_reauth_request(const std::string& session_id,
                        std::chrono::milliseconds timeout = std::chrono::seconds(5));

private:
    // ADR-0447: one per live connection that has learned at least one Gy session, so
    // send_reauth_request (called from ANY thread -- the eventual reconciliation sweep, a manual
    // trigger, a test) can locate the right socket and inject a real RAR onto it without racing
    // handle_connection's own CCA/ACA/STA writes on that same socket (two threads writing one TCP
    // socket concurrently can interleave bytes -- POSIX does not guarantee write() atomicity across
    // arbitrary sizes). `write_mutex` is held only for the duration of an actual write (by either
    // side); `result_mutex`/`result_cv` are separate so send_reauth_request's wait for the RAA does
    // not block handle_connection's own unrelated response writes for the whole RAR round trip.
    struct PeerConnection {
        boost::asio::ip::tcp::socket* socket = nullptr;
        std::string destination_host;  // the peer's own real Origin-Host, learned at CER
        std::string destination_realm; // the peer's own real Origin-Realm, learned at CER
        std::mutex write_mutex;
        std::mutex result_mutex;
        std::condition_variable result_cv;
        std::optional<std::int32_t> pending_raa_result;
    };
    std::mutex sessions_mutex_;
    std::unordered_map<std::string, std::shared_ptr<PeerConnection>> session_to_connection_;
    // RFC 6733 §3: Hop-by-Hop/End-to-End Identifiers are "set by the originator of a Diameter
    // request" -- every OTHER message CHF builds is an Answer echoing a request's own identifiers;
    // RAR is CHF's first outgoing Diameter REQUEST, so it is the first time CHF must generate its
    // own. A monotonic counter seeded from real wall-clock time at construction is a disclosed,
    // lab-adequate simplification of RFC 6733's own fuller "guaranteed unique at start-up" scheme
    // (which recommends a wider construction combining boot time with a counter) -- adequate for
    // this project's own single-process-per-instance deployment, not claimed to be the full scheme.
    std::atomic<std::uint32_t> next_request_id_;

private:
    // P15 (ADR-0285): a TPS ceiling for the Diameter front door, mirroring the SBI one
    // (ADR-0280). OFF unless configured -- and off by default for a second reason beyond caution,
    // see the .cpp: the shed answer's Result-Code semantics are not fully verifiable from spec
    // material in this repository.
public:
    void set_tps_limit(double sustained_tps, double burst_capacity);

private:
    // ADR-0290: this server starts its accept thread in its own CONSTRUCTOR, so main() calls
    // set_tps_limit() while connection threads are already running and reading this. The bucket
    // itself is internally locked; the POINTER to it was the race. Owner + atomic view: the owner
    // keeps it alive, the atomic is what the message loop reads.
    std::unique_ptr<sbi_core::TokenBucket> rate_limit_owner_;
    std::atomic<sbi_core::TokenBucket*> rate_limit_{nullptr};

    void accept_loop();
    void handle_connection(boost::asio::ip::tcp::socket socket);

    boost::asio::io_context ioc_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::string origin_host_;
    std::string origin_realm_;
    sbi_core::http2::TlsConfig client_tls_;
    ChargingDataStore& charging_data_store_;
    CdrWriter& cdr_writer_;
    RatingDecisionStore& rating_decision_store_;
    OfflineChargingDataStore& offline_charging_data_store_;
    SpendingLimitSubscriptionStore& spending_limit_store_;
    PolicyCounterConfigStore& policy_counter_config_store_;
    CatalogSnapshot& catalog_snapshot_;
    opentelemetry::metrics::Counter<std::uint64_t>* grant_counter_;
    opentelemetry::metrics::Counter<std::uint64_t>* reserve_rejected_counter_;
    opentelemetry::metrics::Counter<std::uint64_t>* ccr_initial_counter_;
    opentelemetry::metrics::Counter<std::uint64_t>* ccr_update_counter_;
    opentelemetry::metrics::Counter<std::uint64_t>* ccr_termination_counter_;
    opentelemetry::metrics::Counter<std::uint64_t>* acr_event_counter_;
    opentelemetry::metrics::Counter<std::uint64_t>* acr_start_counter_;
    opentelemetry::metrics::Counter<std::uint64_t>* acr_interim_counter_;
    opentelemetry::metrics::Counter<std::uint64_t>* acr_stop_counter_;
    opentelemetry::metrics::Counter<std::uint64_t>* slr_initial_counter_;
    opentelemetry::metrics::Counter<std::uint64_t>* slr_intermediate_counter_;
    opentelemetry::metrics::Counter<std::uint64_t>* str_counter_;
    std::thread accept_thread_;
    std::atomic<bool> stop_{false};
};

} // namespace chf
