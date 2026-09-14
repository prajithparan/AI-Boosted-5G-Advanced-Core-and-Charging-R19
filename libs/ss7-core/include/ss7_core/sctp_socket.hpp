#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Thin RAII wrapper around a kernel one-to-one style SCTP socket (AF_INET, IPPROTO_SCTP), used for
// M3UA transport (RFC 4666 real SCTP port 2905, real PPID 3 -- see m3ua_dictionary.hpp). Same real
// kernel-SCTP-API usage pattern this project's own `libs/ngap-core/src/sctp_socket.cpp` already
// established and has running against a real peer (UERANSIM's `nr-gnb`, ADR-0030) -- not a new
// invention, just M3UA's own real PPID/port in place of NGAP's. Boost.Asio has no native SCTP
// support, same reason ngap_core's own class exists; this class is meant to be used from a
// dedicated thread doing blocking I/O, the same "blocking transport gets its own thread"
// discipline ADR-0006/ADR-0030 already established.
//
// Real, disclosed scope: this is a pure transport primitive, not a live listener bound into any
// NF's `main()` -- which NF, if any, should own a real M3UA/SCTP association in this project is an
// architectural decision not made by this class (no NF in CLAUDE.md's own Tier 1-3 list is
// explicitly an SS7 gateway; MAP/CAP's own real operations, the actual reason to open one, are
// still blocked on spec material, see docs/DECISIONS.md's own Stage 5b disclosure).

namespace ss7_core {

class SctpSocket {
public:
    // Throws std::runtime_error on any socket()/setsockopt() failure.
    SctpSocket();
    ~SctpSocket();

    SctpSocket(const SctpSocket&) = delete;
    SctpSocket& operator=(const SctpSocket&) = delete;
    SctpSocket(SctpSocket&& other) noexcept;
    SctpSocket& operator=(SctpSocket&& other) noexcept;

    void bind_and_listen(const std::string& address, std::uint16_t port, int backlog = 8);

    // Blocks until a peer establishes an association; returns a new connected socket for it.
    // Throws std::runtime_error on failure.
    SctpSocket accept();

    // Client-side association establishment (real kernel `connect()` on a one-to-one style SCTP
    // socket -- same real syscall a TCP client would use; SCTP's own INIT/INIT-ACK/COOKIE-ECHO/
    // COOKIE-ACK four-way handshake, RFC 4960 3.1, happens inside the kernel during this call, not
    // modeled at this layer). Added for UDM's real MAP client role (ADR-0061) -- every prior use of
    // this class and its ngap_core precedent was server-side (bind_and_listen/accept) only. Throws
    // std::runtime_error on failure.
    void connect(const std::string& address, std::uint16_t port);

    // One send() call = one SCTP message = one real M3UA message (SCTP preserves message
    // boundaries, unlike a TCP stream -- no length-prefixing needed). Throws std::runtime_error on
    // failure.
    void send(const std::vector<std::uint8_t>& data, std::uint16_t stream = 0);

    // Blocking receive of one SCTP message. Returns an empty vector on graceful peer shutdown
    // (ECONNRESET) rather than throwing, since that's an expected, routine event, not an error
    // condition. Throws std::runtime_error on any other failure.
    std::vector<std::uint8_t> receive();

    bool valid() const { return fd_ >= 0; }

    // Closes the underlying fd early (idempotent -- safe to call more than once, and the
    // destructor no-ops if already closed). Added for CHF's real CAP listener (ADR-0061): closing
    // the listening socket from another thread is the same real, standard technique
    // DiameterServer's own shutdown already uses (closing its Boost.Asio acceptor) to unblock a
    // thread currently parked in a blocking accept() call.
    void close();

private:
    explicit SctpSocket(int fd);
    void close_if_open();

    int fd_ = -1;
};

} // namespace ss7_core
