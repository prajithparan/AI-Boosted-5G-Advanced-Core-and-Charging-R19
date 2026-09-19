// The LI_HI2 Delivery Function of ETSI TS 102 232-1 clause 6.3 against a loopback LEMF (ADR-0376).
// A real TLS 1.3 server stands in for the LEMF: it accepts the DF's connection, reads the BER
// PS-PDU stream, answers keep-alives, and can drop the connection to exercise clause 6.3.2's
// reopen and clause 6.3.3's resynchronisation.

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "li_core/hi2.hpp"
#include "li_core/hi2_client.hpp"

#include <gtest/gtest.h>

namespace {

using namespace li_core;

hi2::PsHeader lab_header() {
    hi2::PsHeader header;
    header.liid = "LIID-2026-0001";
    header.communication_identifier.operator_identifier = "5GC-R19-OP";
    header.communication_identifier.network_element_identifier = "mdf2-01";
    header.sequence_number = 0;
    header.timestamp = hi2::Timestamp{1789000000, 0};
    return header;
}

std::vector<std::uint8_t> sample_iri_pdu(std::uint32_t sequence) {
    hi2::IriMessage message;
    message.header = lab_header();
    message.header.sequence_number = sequence;
    message.iri_type = hi2::IriType::Report;
    // Opaque bytes stand in for a BER IRIPayload: this test is about the delivery function, and
    // the mediation that produces a real payload is covered by test_li_hi2.cpp.
    message.iri_payload = {0x30, 0x03, 0x02, 0x01, static_cast<std::uint8_t>(sequence)};
    auto encoded = hi2::encode_iri_message(message);
    EXPECT_TRUE(encoded.has_value()) << (encoded ? "" : encoded.error());
    return encoded.value_or(std::vector<std::uint8_t>{});
}

// A loopback LEMF: accepts connections in a loop, collects whole PS-PDUs, answers keep-alives.
class LoopbackLemf {
public:
    LoopbackLemf() {
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
        addr.sin_port = 0;
        ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
        socklen_t len = sizeof addr;
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        ::listen(listen_fd_, 4);
        running_.store(true);
        thread_ = std::thread([this] { serve(); });
    }
    ~LoopbackLemf() {
        running_.store(false);
        // shutdown() wakes the blocked accept(); listen_fd_ is only closed and cleared once the
        // serve thread has joined, so its read of listen_fd_ in accept() cannot race this write
        // (TSan, test_li_hi2_client.cpp:138 vs :83).
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
        }
        drop_connection();
        if (thread_.joinable()) {
            thread_.join();
        }
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (ctx_ != nullptr) {
            SSL_CTX_free(ctx_);
        }
    }

    std::uint16_t port() const { return port_; }

    // Close whatever connection is live, so the DF has to reopen it (clause 6.3.2).
    void drop_connection() {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (conn_fd_ >= 0) {
            ::shutdown(conn_fd_, SHUT_RDWR);
        }
    }

    bool wait_for_pdus(std::size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return pdus_.size() >= count; });
    }
    std::vector<std::vector<std::uint8_t>> pdus() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return pdus_;
    }
    std::uint32_t keepalives() const { return keepalives_.load(); }
    std::uint32_t connections() const { return connections_.load(); }

private:
    // A BER PS-PDU starts 30 <len>; read whole TLVs out of the stream.
    static std::size_t tlv_length(const std::vector<std::uint8_t>& buf) {
        if (buf.size() < 2) {
            return 0;
        }
        const std::uint8_t first = buf[1];
        if ((first & 0x80U) == 0) {
            return 2 + first;
        }
        const std::size_t count = first & 0x7FU;
        if (count == 0 || count > 4 || buf.size() < 2 + count) {
            return 0;
        }
        std::size_t len = 0;
        for (std::size_t i = 0; i < count; ++i) {
            len = (len << 8) | buf[2 + i];
        }
        return 2 + count + len;
    }

    void serve() {
        while (running_.load()) {
            const int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) {
                if (!running_.load()) {
                    return;
                }
                continue;
            }
            connections_.fetch_add(1);
            SSL* ssl = SSL_new(ctx_);
            SSL_set_fd(ssl, fd);
            if (SSL_accept(ssl) != 1) {
                SSL_free(ssl);
                ::close(fd);
                continue;
            }
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                conn_fd_ = fd;
            }
            std::vector<std::uint8_t> buffer;
            std::uint8_t chunk[4096];
            while (running_.load()) {
                const int n = SSL_read(ssl, chunk, static_cast<int>(sizeof chunk));
                if (n <= 0) {
                    break;
                }
                buffer.insert(buffer.end(), chunk, chunk + n);
                for (;;) {
                    const std::size_t len = tlv_length(buffer);
                    if (len == 0 || buffer.size() < len) {
                        break;
                    }
                    std::vector<std::uint8_t> pdu(buffer.begin(),
                                                  buffer.begin() + static_cast<long>(len));
                    buffer.erase(buffer.begin(), buffer.begin() + static_cast<long>(len));
                    const auto kind = hi2::payload_kind(pdu);
                    if (kind && *kind == hi2::PayloadKind::Tri) {
                        const auto tri = hi2::decode_tri_message(pdu);
                        if (tri && tri->type == hi2::TriType::KeepAlive) {
                            keepalives_.fetch_add(1);
                            // Clause 6.3.4: respond with the request's sequence number.
                            hi2::TriMessage response;
                            response.header = tri->header;
                            response.type = hi2::TriType::KeepAliveResponse;
                            const auto encoded = hi2::encode_tri_message(response);
                            if (encoded) {
                                SSL_write(ssl, encoded->data(), static_cast<int>(encoded->size()));
                            }
                        }
                        continue;
                    }
                    const std::lock_guard<std::mutex> lock(mutex_);
                    pdus_.push_back(std::move(pdu));
                    cv_.notify_all();
                }
            }
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                conn_fd_ = -1;
            }
            SSL_shutdown(ssl);
            SSL_free(ssl);
            ::close(fd);
        }
    }

    SSL_CTX* ctx_ = nullptr;
    int listen_fd_ = -1;
    int conn_fd_ = -1;
    std::uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<std::uint32_t> keepalives_{0};
    std::atomic<std::uint32_t> connections_{0};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::vector<std::uint8_t>> pdus_;
};

Hi2ClientConfig lab_config(std::uint16_t port) {
    Hi2ClientConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = port;
    cfg.client_cert_path = CERTS_DIR "/amf/cert.pem";
    cfg.client_key_path = CERTS_DIR "/amf/key.pem";
    cfg.ca_path = CERTS_DIR "/ca/ca.crt";
    cfg.sni = "localhost";
    cfg.reconnect_interval = std::chrono::seconds(1);
    cfg.keepalive_time1 = std::chrono::seconds(1);
    cfg.keepalive_time3 = std::chrono::seconds(5);
    return cfg;
}

} // namespace

TEST(LiHi2Client, DeliversPsPdusToTheLemf) {
    LoopbackLemf lemf;
    Hi2Client client(lab_config(lemf.port()), lab_header(), nullptr);
    client.start();

    const auto first = sample_iri_pdu(1);
    const auto second = sample_iri_pdu(2);
    ASSERT_TRUE(client.send(first).has_value());
    ASSERT_TRUE(client.send(second).has_value());

    ASSERT_TRUE(lemf.wait_for_pdus(2, std::chrono::seconds(10)));
    const auto received = lemf.pdus();
    ASSERT_GE(received.size(), 2u);
    EXPECT_EQ(received[0], first);
    EXPECT_EQ(received[1], second);
    EXPECT_GE(client.pdus_sent(), 2u);

    client.stop();
}

// Clause 6.3.4: a keep-alive TRI after TIME1 of silence, and the LEMF's response clears the wait.
TEST(LiHi2Client, SendsSessionLayerKeepalives) {
    LoopbackLemf lemf;
    Hi2Client client(lab_config(lemf.port()), lab_header(), nullptr);
    client.start();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (lemf.keepalives() < 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    EXPECT_GE(lemf.keepalives(), 2u) << "the DF sent no keep-alives after TIME1";
    EXPECT_GE(client.keepalives_sent(), 2u);
    // The connection survived: a missing response would have torn it down after TIME3.
    EXPECT_TRUE(client.connected());

    client.stop();
}

// Clause 6.3.2 (reopen immediately) and 6.3.3 (re-send what is still buffered before new data).
TEST(LiHi2Client, ReopensTheConnectionAndResynchronisesAfterADrop) {
    LoopbackLemf lemf;
    std::atomic<int> resynchronised{0};
    Hi2Client client(lab_config(lemf.port()), lab_header(), [&](Hi2Event event, std::string_view) {
        if (event == Hi2Event::Resynchronised) {
            resynchronised.fetch_add(1);
        }
    });
    client.start();

    ASSERT_TRUE(client.send(sample_iri_pdu(1)).has_value());
    ASSERT_TRUE(lemf.wait_for_pdus(1, std::chrono::seconds(10)));

    lemf.drop_connection();
    ASSERT_TRUE(client.send(sample_iri_pdu(2)).has_value());

    // The DF must reopen and deliver: the second PDU arrives on the new connection, and the first
    // is re-sent with it because it is still in the cyclic buffer.
    ASSERT_TRUE(lemf.wait_for_pdus(3, std::chrono::seconds(20)))
        << "the DF did not resynchronise after the connection was dropped";
    EXPECT_GE(lemf.connections(), 2u);
    EXPECT_GE(client.reconnects(), 2u);
    EXPECT_GE(resynchronised.load(), 1);

    client.stop();
}

// Clause 6.3.3: a full buffer is reported and the oldest data is overwritten, not an error.
TEST(LiHi2Client, OverwritesTheOldestPduWhenTheBufferIsFull) {
    LoopbackLemf lemf;
    Hi2ClientConfig cfg = lab_config(lemf.port());
    cfg.buffer_capacity_bytes = 300; // a few PS-PDUs
    std::atomic<int> buffer_full{0};
    Hi2Client client(cfg, lab_header(), [&](Hi2Event event, std::string_view) {
        if (event == Hi2Event::BufferFull) {
            buffer_full.fetch_add(1);
        }
    });
    // Deliberately NOT started: with no worker draining it, the buffer must fill.
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(client.send(sample_iri_pdu(static_cast<std::uint32_t>(i))).has_value());
    }
    EXPECT_GT(buffer_full.load(), 0) << "the buffer never reported itself full";
    EXPECT_GT(client.pdus_discarded(), 0u);
    EXPECT_LE(client.buffered_bytes(), cfg.buffer_capacity_bytes);

    // A PDU larger than the whole buffer is refused outright rather than silently dropped.
    std::vector<std::uint8_t> huge(cfg.buffer_capacity_bytes + 1, 0x00);
    EXPECT_FALSE(client.send(std::move(huge)).has_value());
}
