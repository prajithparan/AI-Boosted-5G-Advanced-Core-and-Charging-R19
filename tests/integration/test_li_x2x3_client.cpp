// ETSI TS 103 221-2 X2/X3 delivery client over mTLS (ADR-0372). A loopback TLS 1.3 server stands
// in for an MDF2/MDF3 Destination: it presents the lab AMF certificate (SAN localhost), accepts
// one connection, and reads the framed PDUs the client streams. The test asserts the bytes decode
// back through li_core::decode to exactly the PDUs sent -- so this exercises the real OpenSSL mTLS
// path, the stream framing, and the encode/decode round trip end to end, not just the codec.

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <cstdint>
#include <netinet/in.h>
#include <span>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "li_core/x2x3_client.hpp"
#include "li_core/x2x3_pdu.hpp"

#include <gtest/gtest.h>

namespace {

using namespace li_core;

// A minimal TLS 1.3 server on 127.0.0.1:<ephemeral>, presenting the lab AMF cert. serve_once()
// accepts one connection on a background thread and reads until `want` octets arrive or the peer
// closes; join() then hands back what it read.
class LoopbackMdf {
public:
    LoopbackMdf() {
        ctx_ = SSL_CTX_new(TLS_server_method());
        SSL_CTX_set_min_proto_version(ctx_, TLS1_3_VERSION);
        SSL_CTX_use_certificate_file(ctx_, CERTS_DIR "/amf/cert.pem", SSL_FILETYPE_PEM);
        SSL_CTX_use_PrivateKey_file(ctx_, CERTS_DIR "/amf/key.pem", SSL_FILETYPE_PEM);

        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0; // ephemeral -- no fixed-port collision with CI's Test step
        ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
        socklen_t len = sizeof addr;
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        ::listen(listen_fd_, 1);
    }
    ~LoopbackMdf() {
        join();
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
        }
        if (ctx_ != nullptr) {
            SSL_CTX_free(ctx_);
        }
    }
    LoopbackMdf(const LoopbackMdf&) = delete;
    LoopbackMdf& operator=(const LoopbackMdf&) = delete;

    std::uint16_t port() const { return port_; }

    void serve_once(std::size_t want) {
        th_ = std::thread([this, want] {
            const int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) {
                return;
            }
            SSL* ssl = SSL_new(ctx_);
            SSL_set_fd(ssl, fd);
            if (SSL_accept(ssl) == 1) {
                std::uint8_t buf[4096];
                while (received_.size() < want) {
                    const int n = SSL_read(ssl, buf, sizeof buf);
                    if (n <= 0) {
                        break;
                    }
                    received_.insert(received_.end(), buf, buf + n);
                }
                SSL_shutdown(ssl);
            }
            SSL_free(ssl);
            ::close(fd);
        });
    }
    void join() {
        if (th_.joinable()) {
            th_.join();
        }
    }
    const std::vector<std::uint8_t>& received() const { return received_; }

private:
    SSL_CTX* ctx_ = nullptr;
    int listen_fd_ = -1;
    std::uint16_t port_ = 0;
    std::thread th_;
    std::vector<std::uint8_t> received_;
};

Pdu sample_x2() {
    Pdu pdu;
    pdu.type = PduType::X2;
    pdu.payload_format = PayloadFormat::Tgpp33128Payload;
    pdu.payload_direction = PayloadDirection::FromTarget;
    pdu.xid = {0x3f,
               0xa8,
               0x5f,
               0x64,
               0x57,
               0x17,
               0x45,
               0x62,
               0xb3,
               0xfc,
               0x2c,
               0x96,
               0x3f,
               0x66,
               0xaf,
               0xa6};
    pdu.correlation_id = 0x0102030405060708ULL;
    pdu.payload = {0x30, 0x03, 0x02, 0x01, 0x2A}; // a small BER blob
    return pdu;
}

TEST(LiX2X3Client, DeliversPdusOverMtlsAndTheyDecodeBack) {
    const Pdu data = sample_x2();
    const Pdu keepalive = make_keepalive(1);
    const auto data_wire = encode(data);
    const auto ka_wire = encode(keepalive);

    LoopbackMdf mdf;
    mdf.serve_once(data_wire.size() + ka_wire.size());

    X2X3ClientConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = mdf.port();
    cfg.sni = "localhost"; // the AMF cert's SAN; SSL_set1_host verifies the server against it
    cfg.client_cert_path = CERTS_DIR "/amf/cert.pem";
    cfg.client_key_path = CERTS_DIR "/amf/key.pem";
    cfg.ca_path = CERTS_DIR "/ca/ca.crt";

    X2X3Client client(std::move(cfg));
    auto r1 = client.send(data);
    ASSERT_TRUE(r1.has_value()) << r1.error();
    EXPECT_TRUE(client.connected());
    auto r2 = client.send_keepalive();
    ASSERT_TRUE(r2.has_value()) << r2.error();
    client.disconnect();
    EXPECT_FALSE(client.connected());

    mdf.join();

    // Two PDUs framed on the wire; walk them with the codec's own framing and decode each.
    std::span<const std::uint8_t> rest(mdf.received());
    std::size_t consumed = 0;
    auto first = decode(rest, &consumed);
    ASSERT_TRUE(first.has_value()) << to_string(first.error());
    EXPECT_EQ(*first, data);
    rest = rest.subspan(consumed);

    auto second = decode(rest, &consumed);
    ASSERT_TRUE(second.has_value()) << to_string(second.error());
    EXPECT_EQ(second->type, PduType::Keepalive);
    EXPECT_EQ(sequence_number(*second), 1u);
    rest = rest.subspan(consumed);
    EXPECT_TRUE(rest.empty());
}

TEST(LiX2X3Client, RejectsANonConformantPduBeforeConnecting) {
    // A keepalive PDU carrying a payload is invalid (clause 5.1); the client must refuse it via
    // validate() and never open a connection.
    Pdu bad = make_keepalive(7);
    bad.payload = {0x01};

    X2X3ClientConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 1; // never contacted -- validate() fails first
    cfg.client_cert_path = CERTS_DIR "/amf/cert.pem";
    cfg.client_key_path = CERTS_DIR "/amf/key.pem";
    cfg.ca_path = CERTS_DIR "/ca/ca.crt";

    X2X3Client client(std::move(cfg));
    auto r = client.send(bad);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("not conformant"), std::string::npos);
    EXPECT_FALSE(client.connected());
}

} // namespace
