#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Thin RAII wrapper around a kernel one-to-one style SCTP socket (AF_INET, IPPROTO_SCTP, via the
// system libsctp-dev headers/library), used for AMF's N2/NGAP transport (TS 38.412 specifies port
// 38412; TS 38.413 is the NGAP protocol carried over it). Boost.Asio, already this project's
// event-loop library for SBI/HTTP2, has no native SCTP support -- this class is meant to be used
// from a dedicated thread doing blocking I/O instead, the same "blocking transport gets its own
// thread" discipline docs/DECISIONS.md ADR-0006 already established for run_nrf_lifecycle. See
// ADR-0030.
//
// One-to-one style (SOCK_STREAM, not SOCK_SEQPACKET/one-to-many): accept() returns a fully
// connected per-association socket directly, the same accept() model as a TCP listening socket --
// no SCTP_ASSOC_CHANGE notification handling needed, unlike a one-to-many socket multiplexing
// several associations over one fd.
//
// PPID (Payload Protocol Identifier) for NGAP is 60 -- confirmed against UERANSIM's own real,
// already-building implementation (simulators/ransim/vendor/UERANSIM/src/lib/sctp/types.hpp),
// not from memory or general knowledge.
//
// Disclosed simplification: every message in this build is sent/received on SCTP stream 0. Real
// NGAP deployments reserve stream 0 for non-UE-associated signaling (e.g. NG Setup) and assign
// dynamic per-UE streams for UE-associated signaling; this build's scope (sequential procedures
// for one UE at a time) doesn't need that yet. `send`'s `stream` parameter exists so a future turn
// can add real per-association stream assignment without changing this class's shape.

namespace ngap_core {

constexpr std::uint32_t kNgapPpid = 60;

class SctpSocket {
public:
    // Throws std::runtime_error on any socket()/bind()/listen() failure.
    SctpSocket();
    ~SctpSocket();

    SctpSocket(const SctpSocket&) = delete;
    SctpSocket& operator=(const SctpSocket&) = delete;
    SctpSocket(SctpSocket&& other) noexcept;
    SctpSocket& operator=(SctpSocket&& other) noexcept;

    void bind_and_listen(const std::string& address, std::uint16_t port, int backlog = 8);

    // Blocks until a peer (e.g. a real gNB) establishes an association; returns a new connected
    // socket for it. Throws std::runtime_error on failure.
    SctpSocket accept();

    // ADR-0394. Same connection-accepting behaviour as accept(), but returns std::nullopt instead
    // of throwing when set_receive_timeout's own deadline expires with nothing pending -- lets a
    // caller poll a stop flag between attempts instead of blocking indefinitely (this is how
    // run_ngap_lifecycle's own accept loop learns to stop accepting new associations on shutdown,
    // since there is no portable, safe way to interrupt a blocked accept() on a LISTENING socket
    // from another thread the way shutdown_now() below does for a CONNECTED one). Still throws on
    // any other real accept() failure. set_receive_timeout affects this call the same way it
    // affects receive() -- SO_RCVTIMEO is a plain socket option the kernel honours for both.
    std::optional<SctpSocket> accept_or_timeout();

    // Client-role counterpart to bind_and_listen/accept -- establishes a new SCTP association to
    // a real listening peer (e.g. this AMF's own NGAP port, from a test acting as a second, real
    // gNB). Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #100, ADR-0090): added when a real
    // hand-crafted NGAP test client needed it -- UERANSIM's own gNB has no CLI-triggerable
    // handover/path-switch scenario, so this project's own strongest verification tier (a second,
    // independently-built real process) needed a real client-role SCTP capability this library
    // never had before (every prior stage only ever needed the server/gNB-facing accept() role).
    // Throws std::runtime_error on failure.
    void connect(const std::string& address, std::uint16_t port);

    // One send() call = one SCTP message = one NGAP PDU (SCTP preserves message boundaries,
    // unlike a TCP stream -- no length-prefixing needed). Throws std::runtime_error on failure.
    void send(const std::vector<std::uint8_t>& data, std::uint16_t stream = 0);

    // Bounds how long receive() blocks. Zero -- the default -- means block indefinitely, which is
    // what every production caller relies on (each association gets its own thread, ADR-0030, and
    // an idle gNB legitimately sends nothing for hours). Set by tests that must fail rather than
    // hang when a peer never answers: an unbounded wait there is indistinguishable from a CI
    // runner dying mid-job. Throws std::runtime_error if the socket rejects the option.
    void set_receive_timeout(std::chrono::milliseconds timeout);

    // Blocking receive of one SCTP message. Returns an empty vector on graceful peer shutdown
    // (ECONNRESET) rather than throwing, since that's an expected, routine event (a gNB
    // disconnecting), not an error condition -- and, when set_receive_timeout is in effect, on
    // that timeout expiring, which is the same "nothing came back" answer callers already handle.
    // Throws std::runtime_error on any other failure.
    std::vector<std::uint8_t> receive();

    bool valid() const { return fd_ >= 0; }

    // ADR-0394. Forces a blocking receive() on THIS association's socket, from ANY thread, to
    // return promptly -- shutdown(), not close(): calling shutdown() on a socket another thread
    // is concurrently blocked reading from is well-defined POSIX behaviour (the blocked call
    // returns, here exactly like a graceful peer disconnect, receive()'s own existing ECONNRESET
    // path already handles it correctly); a concurrent close() from another thread risks the
    // classic fd-reuse race instead. This is what closes the real gap ADR-0394 found: this
    // project's per-association NGAP threads (ADR-0030/ADR-0095) had no way to be asked to stop
    // before this existed, so process shutdown could race them destructing shared clients/stores
    // they were still using. Safe to call even after this object's own owning thread has already
    // closed it (a no-op: fd_ is atomic, read once, and ::shutdown on an fd number this object no
    // longer holds is simply never reached).
    void shutdown_now();

private:
    explicit SctpSocket(int fd);
    void close_if_open();

    std::atomic<int> fd_{-1};
};

} // namespace ngap_core
