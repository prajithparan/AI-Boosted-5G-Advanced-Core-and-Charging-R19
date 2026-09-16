#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <tl/expected.hpp>

#include "li_core/x2x3_pdu.hpp"

// LI_X2 / LI_X3 delivery client (ADR-0372): a POI/MDF opens one persistent TLS 1.3 connection to
// a Destination (an MDF2/MDF3, TS 103 221-1 DeliveryAddress) and streams TS 103 221-2 PDUs
// (x2x3_pdu) over it, framed by the header's own Header/Payload Length (clause 5.2.3). The
// clause-5.1/6.2.4 keepalive PDU is sent when the link is idle. Blocking, one connection, a mutex
// so a POI can send from several threads; reconnects on the next send after a broken link. mTLS
// per TS 33.128 5.3 -- client cert, verified server cert.

#pragma GCC visibility push(default)
namespace li_core {

struct X2X3ClientConfig {
    std::string host;
    std::uint16_t port = 0;
    std::string client_cert_path; // PEM
    std::string client_key_path;  // PEM
    std::string ca_path;          // PEM bundle to verify the MDF
    std::string sni;              // server name to require in the cert; host if empty
    std::chrono::seconds keepalive_interval{60}; // idle interval before a keepalive PDU
    std::chrono::milliseconds connect_timeout{5000};
};

class X2X3Client {
public:
    explicit X2X3Client(X2X3ClientConfig config);
    ~X2X3Client();
    X2X3Client(const X2X3Client&) = delete;
    X2X3Client& operator=(const X2X3Client&) = delete;

    // Encode and send one PDU (validated with x2x3::validate first). Connects if needed. Returns
    // the error string on failure, having dropped the connection so the next call reconnects.
    tl::expected<void, std::string> send(const Pdu& pdu);
    // Send a keepalive PDU with the next sequence number.
    tl::expected<void, std::string> send_keepalive();
    // Close the connection (a graceful shutdown; the next send reconnects).
    void disconnect();
    [[nodiscard]] bool connected() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::mutex mutex_;
};

} // namespace li_core
#pragma GCC visibility pop
