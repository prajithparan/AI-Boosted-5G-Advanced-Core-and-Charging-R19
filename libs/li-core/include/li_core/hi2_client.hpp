#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tl/expected.hpp>
#include <vector>

#include "li_core/hi2.hpp"

// The LI_HI2 / LI_HI3 Delivery Function of ETSI TS 102 232-1 clause 6.3 (ADR-0376): the MDF's
// side of the handover link to the LEMF. Clause 6.4 profiles the transport over TCP, optionally
// inside TLS (clause 6.3.1 requires at least TLS 1.2 and says new implementations should support
// TLS 1.3 -- this one requires 1.3 and mTLS, as every other link in this project does).
//
// What the clause actually demands of a Delivery Function, and what this class therefore does:
//
//   6.3.2  Open the connection as soon as the DF exists; if it terminates for any reason, reopen
//          it immediately; if opening fails, keep retrying at a configurable interval (e.g. 30 s)
//          and report the failure to the Handover Manager.
//   6.3.3  Never lose data to an unexpected termination: keep a cyclic buffer, delete a PDU only
//          once the transport says it was sent, resend whatever is still buffered after a
//          reconnect *before* any new data, and when the buffer fills, report it and overwrite
//          the oldest. TRI keep-alives and acknowledgement PDUs are not buffered.
//   6.3.4  Session-layer keep-alives: a timer reset by every send, a keep-alive TRI at TIME1, and
//          if no keep-aliveResponse arrives within TIME3, terminate the connection and open a new
//          one.
//   6.4.3  Choose one of three definitions of "successfully sent". This implements option 3 (data
//          is sent once it is passed to an open socket) -- see the disclosure in ADR-0376.
//
// Everything runs on one worker thread: send() only enqueues, so an MDF2's X2 receive thread is
// never blocked by a slow or dead LEMF.

#pragma GCC visibility push(default)
namespace li_core {

struct Hi2ClientConfig {
    std::string host;
    std::uint16_t port = 0;
    std::string client_cert_path; // PEM
    std::string client_key_path;  // PEM
    std::string ca_path;          // PEM bundle to verify the LEMF
    std::string sni;              // name required in the LEMF's certificate; host if empty
    // 6.3.2: "a configurable time interval (e.g. 30 s) between attempts".
    std::chrono::seconds reconnect_interval{30};
    // 6.3.4: TIME1 (idle before a keep-alive; typical 120-360 s) and TIME3 (no response before
    // the connection is torn down; typical 60 s, and must exceed the LEMF's TIME2 of ~30 s).
    std::chrono::seconds keepalive_time1{180};
    std::chrono::seconds keepalive_time3{60};
    // 6.3.3: the cyclic buffer, bounded in octets rather than PDUs because that is what the
    // clause's "room for them" and the TCP-send-buffer guidance are about.
    std::size_t buffer_capacity_bytes = 8U * 1024U * 1024U;
    std::chrono::milliseconds connect_timeout{5000};
};

// Clause 6.3.2/6.3.3 require failures to be "reported to the Handover Manager". This project has
// no such component yet, so the DF reports through a callback the MDF2 wires to its logger and
// counters; nothing is swallowed.
enum class Hi2Event : std::uint8_t {
    Connected,
    ConnectFailed, // 6.3.2, reported to the Handover Manager
    Disconnected,
    BufferFull,       // 6.3.3, reported to the Handover Manager; oldest data overwritten
    KeepaliveTimeout, // 6.3.4, no keep-aliveResponse within TIME3
    Resynchronised,   // 6.3.3, buffered PDUs re-sent after a reconnect
};
using Hi2EventHandler = std::function<void(Hi2Event event, std::string_view detail)>;

class Hi2Client {
public:
    // `keepalive_header` is the PSHeader the clause-6.3.4 keep-alive TRIs carry. Clause 6.3.4
    // says only the timestamp and version must be "set appropriately" and all other header fields
    // may hold any value; this DF sends the operator's own identifiers so a LEMF sees a coherent
    // header, and stamps a fresh timestamp and an incrementing sequence number per keep-alive.
    Hi2Client(Hi2ClientConfig config, hi2::PsHeader keepalive_header, Hi2EventHandler on_event);
    ~Hi2Client();
    Hi2Client(const Hi2Client&) = delete;
    Hi2Client& operator=(const Hi2Client&) = delete;

    // Start the worker: it opens the connection immediately (6.3.2) and keeps it open.
    void start();
    // Stop the worker and close the connection. Anything still buffered is NOT delivered -- the
    // clause's buffer covers short outages, not process exit.
    void stop();

    // Hand one encoded PS-PDU to the Delivery Function. Returns an error only when the PDU cannot
    // be accepted at all; a full buffer is not an error, it is the clause-6.3.3 overwrite (and a
    // BufferFull event). Never blocks on the network.
    tl::expected<void, std::string> send(std::vector<std::uint8_t> ps_pdu);

    [[nodiscard]] bool connected() const;
    [[nodiscard]] std::uint64_t pdus_sent() const;
    [[nodiscard]] std::uint64_t pdus_discarded() const; // overwritten in a full buffer (6.3.3)
    [[nodiscard]] std::uint64_t reconnects() const;
    [[nodiscard]] std::uint64_t keepalives_sent() const;
    [[nodiscard]] std::size_t buffered_bytes() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace li_core
#pragma GCC visibility pop
