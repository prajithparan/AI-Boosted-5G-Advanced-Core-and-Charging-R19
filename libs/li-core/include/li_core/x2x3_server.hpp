#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <tl/expected.hpp>

#include "li_core/x2x3_pdu.hpp"

// LI_X2 / LI_X3 receiving server (ADR-0375): the MDF2/MDF3 side of the link whose sending half is
// x2x3_client. A POI opens one persistent mTLS connection and streams TS 103 221-2 PDUs; this
// server accepts those connections, reframes the stream by the header's own Header/Payload Length
// (clause 5.2.3), answers Keepalive with a Keepalive Acknowledgement carrying the same sequence
// number (clause 6.2.4), and hands every X2/X3 PDU to the handler.
//
// Blocking sockets, one thread per connection, as the client is: an MDF has a handful of POI
// connections, not a fan-in that needs an event loop. mTLS both ways -- the server presents its
// certificate and REQUIRES a verified client certificate, because an X2 connection is how
// intercepted material enters the MDF (TS 33.128 clause 5.3).

#pragma GCC visibility push(default)
namespace li_core {

struct X2X3ServerConfig {
    std::string bind_address = "127.0.0.1";
    std::uint16_t port = 0;       // 0 binds an ephemeral port; read it back with bound_port()
    std::string server_cert_path; // PEM
    std::string server_key_path;  // PEM
    std::string ca_path;          // PEM bundle the POI's client certificate must chain to
    // A PDU larger than this is refused and the connection dropped: Payload Length is a 32-bit
    // field on an attacker-reachable socket, so it is never trusted as an allocation size.
    std::uint32_t max_pdu_bytes = 8U * 1024U * 1024U;
    int backlog = 16;
};

// Called on the connection's own thread for every well-formed, validate()-clean X2/X3 PDU.
// Keepalives are answered by the server and are NOT passed to the handler.
using X2X3PduHandler = std::function<void(const Pdu& pdu, std::string_view peer)>;

class X2X3Server {
public:
    X2X3Server(X2X3ServerConfig config, X2X3PduHandler handler);
    ~X2X3Server();
    X2X3Server(const X2X3Server&) = delete;
    X2X3Server& operator=(const X2X3Server&) = delete;

    // Bind, listen and start accepting on a background thread. Returns the bind/TLS error on
    // failure; the server is not running in that case.
    tl::expected<void, std::string> start();
    // Stop accepting, close every live connection and join the threads. Idempotent.
    void stop();

    [[nodiscard]] std::uint16_t bound_port() const;
    [[nodiscard]] std::uint64_t pdus_received() const;
    [[nodiscard]] std::uint64_t keepalives_answered() const;
    [[nodiscard]] std::uint64_t connections_rejected() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace li_core
#pragma GCC visibility pop
