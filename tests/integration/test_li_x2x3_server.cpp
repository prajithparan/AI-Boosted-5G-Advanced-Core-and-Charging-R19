// The MDF2 receiving side of LI_X2 over real mTLS (ADR-0375), driven by the real sending client
// of ADR-0372: li_core::X2X3Server binds an ephemeral loopback port with the lab AMF certificate,
// li_core::X2X3Client connects to it with the same certificate as its client credential, and the
// PDUs the POI streams arrive at the server's handler decoded. Two PDUs in one write exercise the
// stream reframing (clause 5.2.3), and a keepalive exercises the clause-6.2.4 acknowledgement.
//
// This is the first end-to-end LI path in the repository: an xIRI leaves a sender, crosses a real
// TLS 1.3 socket, and is mediated into an LI_HI2 PS-PDU on the far side.

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
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
#include "li_core/x2x3_client.hpp"
#include "li_core/x2x3_pdu.hpp"
#include "li_core/x2x3_server.hpp"
#include "li_core/xiri.hpp"

#include <gtest/gtest.h>

namespace {

using namespace li_core;

std::vector<std::uint8_t> sample_xiri() {
    xiri::AmfRegistration registration;
    registration.registration_type = xiri::AmfRegistrationType::Initial;
    registration.registration_result = xiri::AmfRegistrationResult::ThreeGppAccess;
    registration.supi = xiri::Imsi{"204081234567890"};
    registration.guti = {"204", "08", 7, 8, 9, 0x0A0B0C0D};
    auto encoded = xiri::encode_xiri_payload(registration);
    EXPECT_TRUE(encoded.has_value());
    return encoded.value_or(std::vector<std::uint8_t>{});
}

Pdu sample_x2(std::uint32_t sequence) {
    Pdu pdu;
    pdu.type = PduType::X2;
    pdu.payload_format = PayloadFormat::Tgpp33128Payload;
    pdu.payload_direction = PayloadDirection::FromTarget;
    pdu.xid = {0xA0,
               0xA1,
               0xA2,
               0xA3,
               0xA4,
               0xA5,
               0xA6,
               0xA7,
               0xA8,
               0xA9,
               0xAA,
               0xAB,
               0xAC,
               0xAD,
               0xAE,
               0xAF};
    pdu.correlation_id = 0x1122334455667788ULL;
    pdu.attributes.push_back(attr_sequence_number(sequence));
    pdu.attributes.push_back(attr_network_function_id("amf-01.5gc.example.net"));
    pdu.attributes.push_back(attr_interception_point_id("AMF-IRI-POI-1"));
    pdu.attributes.push_back(attr_timestamp(1789000000, 123456000));
    pdu.attributes.push_back(attr_matched_target_identifier("<imsi>204081234567890</imsi>"));
    pdu.payload = sample_xiri();
    return pdu;
}

// Collects what the server hands up, so the test can wait for a known count.
class Collector {
public:
    void operator()(const Pdu& pdu, std::string_view) {
        const std::lock_guard<std::mutex> lock(mutex_);
        received_.push_back(pdu);
        cv_.notify_all();
    }
    bool wait_for(std::size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return received_.size() >= count; });
    }
    std::vector<Pdu> take() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return received_;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Pdu> received_;
};

X2X3ServerConfig lab_server_config() {
    X2X3ServerConfig cfg;
    cfg.bind_address = "127.0.0.1";
    cfg.port = 0; // ephemeral -- no collision with CI's fixed test ports
    cfg.server_cert_path = CERTS_DIR "/amf/cert.pem";
    cfg.server_key_path = CERTS_DIR "/amf/key.pem";
    cfg.ca_path = CERTS_DIR "/ca/ca.crt";
    return cfg;
}

X2X3ClientConfig lab_client_config(std::uint16_t port) {
    X2X3ClientConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = port;
    cfg.client_cert_path = CERTS_DIR "/amf/cert.pem";
    cfg.client_key_path = CERTS_DIR "/amf/key.pem";
    cfg.ca_path = CERTS_DIR "/ca/ca.crt";
    cfg.sni = "localhost"; // the lab AMF cert's SAN
    return cfg;
}

} // namespace

TEST(LiX2X3Server, ReceivesStreamedPdusOverMtls) {
    Collector collector;
    X2X3Server server(lab_server_config(), std::ref(collector));
    const auto started = server.start();
    ASSERT_TRUE(started.has_value()) << (started ? "" : started.error());
    ASSERT_NE(server.bound_port(), 0);

    X2X3Client client(lab_client_config(server.bound_port()));
    const Pdu first = sample_x2(1);
    const Pdu second = sample_x2(2);
    ASSERT_TRUE(client.send(first).has_value());
    ASSERT_TRUE(client.send(second).has_value());

    ASSERT_TRUE(collector.wait_for(2, std::chrono::seconds(5)))
        << "the MDF did not receive both PDUs";
    const auto received = collector.take();
    ASSERT_EQ(received.size(), 2u);
    EXPECT_EQ(received[0], first);
    EXPECT_EQ(received[1], second);
    EXPECT_EQ(server.pdus_received(), 2u);

    client.disconnect();
    server.stop();
}

// Clause 6.2.4: the receiver answers a Keepalive with a Keepalive Acknowledgement carrying the
// same Sequence Number, and the keepalive is not delivered to the application.
TEST(LiX2X3Server, AnswersKeepalivesAndDoesNotDeliverThem) {
    Collector collector;
    X2X3Server server(lab_server_config(), std::ref(collector));
    ASSERT_TRUE(server.start().has_value());

    X2X3Client client(lab_client_config(server.bound_port()));
    ASSERT_TRUE(client.send_keepalive().has_value());
    ASSERT_TRUE(client.send(sample_x2(1)).has_value());

    ASSERT_TRUE(collector.wait_for(1, std::chrono::seconds(5)));
    EXPECT_EQ(collector.take().size(), 1u) << "the keepalive was delivered to the handler";
    EXPECT_GE(server.keepalives_answered(), 1u);

    client.disconnect();
    server.stop();
}

// The whole MDF2 path in one test: a POI sends an xIRI over LI_X2, the MDF receives it and
// mediates it into the LI_HI2 PS-PDU the LEMF would get (TS 33.128 clause 5.5).
TEST(LiX2X3Server, MediatesAReceivedXiriIntoAnHi2PsPdu) {
    Collector collector;
    X2X3Server server(lab_server_config(), std::ref(collector));
    ASSERT_TRUE(server.start().has_value());

    X2X3Client client(lab_client_config(server.bound_port()));
    ASSERT_TRUE(client.send(sample_x2(1)).has_value());
    ASSERT_TRUE(collector.wait_for(1, std::chrono::seconds(5)));
    const auto received = collector.take();
    ASSERT_EQ(received.size(), 1u);

    hi2::MediationContext context;
    context.liid = "LIID-2026-0001";
    context.communication_identifier.operator_identifier = "5GC-R19-OP";
    context.sequence_number = 1;
    context.iri_type = hi2::IriType::Report;

    const auto hi2_bytes = hi2::mediate_x2_pdu(received[0], context);
    ASSERT_TRUE(hi2_bytes.has_value()) << (hi2_bytes ? "" : hi2_bytes.error());
    const auto message = hi2::decode_iri_message(*hi2_bytes);
    ASSERT_TRUE(message.has_value()) << (message ? "" : message.error());
    EXPECT_EQ(message->header.liid, "LIID-2026-0001");
    EXPECT_EQ(message->header.network_function_identifier, "amf-01.5gc.example.net");
    EXPECT_EQ(message->header.extended_interception_point_id, "AMF-IRI-POI-1");
    EXPECT_EQ(message->header.timestamp_qualifier, hi2::TimestampQualifier::TimeOfInterception);
    const std::string payload(message->iri_payload.begin(), message->iri_payload.end());
    EXPECT_NE(payload.find("204081234567890"), std::string::npos);

    client.disconnect();
    server.stop();
}

// A peer that cannot present a certificate the MDF's CA signed never gets to send anything: the
// TLS handshake fails and the connection is counted as rejected (TS 33.128 clause 5.3).
TEST(LiX2X3Server, RefusesAPeerWithoutAClientCertificate) {
    Collector collector;
    X2X3Server server(lab_server_config(), std::ref(collector));
    ASSERT_TRUE(server.start().has_value());

    X2X3ClientConfig cfg = lab_client_config(server.bound_port());
    cfg.client_cert_path.clear(); // no client credential at all
    cfg.client_key_path.clear();
    X2X3Client client(cfg);
    EXPECT_FALSE(client.send(sample_x2(1)).has_value());
    EXPECT_FALSE(collector.wait_for(1, std::chrono::milliseconds(500)));

    server.stop();
}

// The reframing path the client cannot exercise: X2X3Client writes one whole PDU per SSL_write,
// so on loopback a frame never arrives split. This raw TLS client writes the first 20 octets of a
// PDU, pauses, then the rest -- the server must hold the partial frame and deliver one PDU.
TEST(LiX2X3Server, ReassemblesAFrameSplitAcrossTwoWrites) {
    Collector collector;
    X2X3Server server(lab_server_config(), std::ref(collector));
    ASSERT_TRUE(server.start().has_value());

    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    ASSERT_NE(ctx, nullptr);
    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
    ASSERT_EQ(SSL_CTX_use_certificate_file(ctx, CERTS_DIR "/amf/cert.pem", SSL_FILETYPE_PEM), 1);
    ASSERT_EQ(SSL_CTX_use_PrivateKey_file(ctx, CERTS_DIR "/amf/key.pem", SSL_FILETYPE_PEM), 1);
    ASSERT_EQ(SSL_CTX_load_verify_locations(ctx, CERTS_DIR "/ca/ca.crt", nullptr), 1);

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(server.bound_port());
    ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr), 0);
    SSL* ssl = SSL_new(ctx);
    ASSERT_NE(ssl, nullptr);
    SSL_set_fd(ssl, fd);
    ASSERT_EQ(SSL_connect(ssl), 1);

    const Pdu pdu = sample_x2(1);
    const auto bytes = encode(pdu);
    ASSERT_GT(bytes.size(), 40u);
    const int head = SSL_write(ssl, bytes.data(), 20); // less than the 12 + lengths a frame needs
    ASSERT_EQ(head, 20);
    // Give the server a chance to read the partial frame and (correctly) do nothing with it.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(collector.wait_for(1, std::chrono::milliseconds(100)))
        << "the server delivered a PDU before the whole frame arrived";
    const int tail = SSL_write(ssl, bytes.data() + 20, static_cast<int>(bytes.size() - 20));
    ASSERT_EQ(tail, static_cast<int>(bytes.size() - 20));

    ASSERT_TRUE(collector.wait_for(1, std::chrono::seconds(5)))
        << "the server did not reassemble the split frame";
    const auto received = collector.take();
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0], pdu);

    SSL_shutdown(ssl);
    SSL_free(ssl);
    ::close(fd);
    SSL_CTX_free(ctx);
    server.stop();
}
