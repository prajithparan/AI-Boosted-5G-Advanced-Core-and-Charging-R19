// The MDF2 end to end (ADR-0377): a real li-mdf process, provisioned over real LI_X1, fed a real
// xIRI over real LI_X2, delivering a real LI_HI2 PS-PDU to a loopback LEMF.
//
// This is the first test in which intercepted material travels the whole path a warrant takes:
//
//   ADMF --X1(HTTP/2+mTLS, XML)--> MDF2 <--X2(TLS, TS 103 221-2)-- IRI-POI
//                                    |
//                                    +--HI2(TLS, TS 102 232-1 PS-PDU)--> LEMF
//
// Needs Valkey (the MDF2's warrant store); skipped, not failed, when it is absent.

#include "sbi_core/http2_client.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
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
#include "li_core/xiri.hpp"
#include "spawn_guard.hpp"

#include <gtest/gtest.h>

namespace {

using namespace std::chrono_literals;
using namespace li_core;

constexpr const char* kX1Url = "https://127.0.0.1:7805/X1/NE";
constexpr std::uint16_t kX2Port = 7806;
// The XID the task is provisioned with, and the same 16 octets as an X2 PDU carries them.
constexpr const char* kXid = "a0a1a2a3-a4a5-a6a7-a8a9-aaabacadaeaf";
constexpr const char* kDid = "22222222-2222-4222-8222-222222222222";
constexpr const char* kLiid = "LIID-2026-0001";

sbi_core::http2::Client make_client() {
    sbi_core::http2::TlsConfig tls{
        .cert_path = CERTS_DIR "/hello-nf/cert.pem",
        .key_path = CERTS_DIR "/hello-nf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    return sbi_core::http2::Client(std::move(tls));
}

std::string x1_envelope(const std::string& type, const std::string& body) {
    return R"(<?xml version="1.0" encoding="UTF-8"?>
<X1Request xmlns="http://uri.etsi.org/03221/X1/2017/10" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xmlns:c="http://uri.etsi.org/03280/common/2017/07">
  <x1RequestMessage xsi:type=")" +
           type + R"(">
    <admfIdentifier>admf-1</admfIdentifier>
    <neIdentifier>mdf2-01</neIdentifier>
    <messageTimestamp>2026-09-18T00:00:00.000000Z</messageTimestamp>
    <version>v1.23.1</version>
    <x1TransactionId>2b1e4f6a-0000-4000-8000-000000000042</x1TransactionId>)" +
           body + R"(
  </x1RequestMessage>
</X1Request>)";
}

tl::expected<sbi_core::http2::ClientResponse, std::string> post_x1(sbi_core::http2::Client& client,
                                                                   const std::string& xml) {
    sbi_core::http2::ClientRequest request;
    request.method = "POST";
    request.url = kX1Url;
    request.headers.emplace("content-type", "application/xml");
    request.body = xml;
    return client.send(request);
}

// A loopback LEMF: one TLS 1.3 listener collecting whole BER PS-PDUs.
class LoopbackLemf {
public:
    LoopbackLemf() {
        ctx_ = SSL_CTX_new(TLS_server_method());
        SSL_CTX_set_min_proto_version(ctx_, TLS1_3_VERSION);
        SSL_CTX_use_certificate_file(ctx_, CERTS_DIR "/hello-nf/cert.pem", SSL_FILETYPE_PEM);
        SSL_CTX_use_PrivateKey_file(ctx_, CERTS_DIR "/hello-nf/key.pem", SSL_FILETYPE_PEM);
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
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        if (ctx_ != nullptr) {
            SSL_CTX_free(ctx_);
        }
    }
    std::uint16_t port() const { return port_; }
    bool wait_for_iri(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return !iri_.empty(); });
    }
    std::vector<std::vector<std::uint8_t>> iri() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return iri_;
    }

private:
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
            SSL* ssl = SSL_new(ctx_);
            SSL_set_fd(ssl, fd);
            if (SSL_accept(ssl) != 1) {
                SSL_free(ssl);
                ::close(fd);
                continue;
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
                            hi2::TriMessage response = *tri;
                            response.type = hi2::TriType::KeepAliveResponse;
                            if (const auto encoded = hi2::encode_tri_message(response)) {
                                SSL_write(ssl, encoded->data(), static_cast<int>(encoded->size()));
                            }
                        }
                        continue;
                    }
                    const std::lock_guard<std::mutex> lock(mutex_);
                    iri_.push_back(std::move(pdu));
                    cv_.notify_all();
                }
            }
            SSL_shutdown(ssl);
            SSL_free(ssl);
            ::close(fd);
        }
    }

    SSL_CTX* ctx_ = nullptr;
    int listen_fd_ = -1;
    std::uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::vector<std::uint8_t>> iri_;
};

bool wait_for_x1(sbi_core::http2::Client& client, std::chrono::seconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        // A malformed body is answered with a TopLevelError -- which is proof the listener is up.
        if (post_x1(client, "<nonsense/>").has_value()) {
            return true;
        }
        std::this_thread::sleep_for(200ms);
    }
    return false;
}

// Probe whatever endpoint the MDF2 itself will use: config/li-mdf.json's 6379 for a local lab
// run, or whatever LI_MDF_REDIS_URL overrides it with (CI maps the service to 16379, ADR-0363).
std::uint16_t valkey_port() {
    const char* url = std::getenv("LI_MDF_REDIS_URL");
    if (url == nullptr) {
        return 6379;
    }
    const std::string text(url);
    const auto colon = text.rfind(':');
    if (colon == std::string::npos) {
        return 6379;
    }
    const auto port = std::strtoul(text.c_str() + colon + 1, nullptr, 10);
    return port > 0 && port <= 65535 ? static_cast<std::uint16_t>(port) : 6379;
}

bool valkey_available() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(valkey_port());
    const bool up = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0;
    ::close(fd);
    return up;
}

} // namespace

TEST(LiMdf, ProvisionsOverX1AndDeliversAMediatedIriToTheLemf) {
    if (!valkey_available()) {
        GTEST_SKIP() << "Valkey is not running on 127.0.0.1:" << valkey_port()
                     << " -- the MDF2's warrant store";
    }
    LoopbackLemf lemf;
    nf_test::SpawnedProcess mdf(LI_MDF_PATH);

    auto client = make_client();
    ASSERT_TRUE(wait_for_x1(client, 20s)) << "li-mdf did not open its LI_X1 listener";

    // The warrant store is Valkey and outlives the process, so start from a known-empty one --
    // otherwise a re-run meets its own XID from last time and is refused with error 2010.
    (void)post_x1(client, x1_envelope("DeactivateAllTasksRequest", ""));
    (void)post_x1(client, x1_envelope("RemoveAllDestinationsRequest", ""));

    // 1. The ADMF provisions where the records go (TS 103 221-1 6.3.1). IPAddressPort's own
    // children come from TS 103 280, whose schema is elementFormDefault="qualified" -- so
    // address/IPv4Address/port carry that namespace, not the X1 one.
    const std::string destination = std::string(R"(
    <destinationDetails>
      <dId>)") + kDid + R"(</dId>
      <deliveryType>X2andX3</deliveryType>
      <deliveryAddress><ipAddressAndPort>
        <c:address><c:IPv4Address>127.0.0.1</c:IPv4Address></c:address>
        <c:port><c:TCPPort>)" + std::to_string(lemf.port()) +
                                    R"(</c:TCPPort></c:port>
      </ipAddressAndPort></deliveryAddress>
    </destinationDetails>)";
    const auto create = post_x1(client, x1_envelope("CreateDestinationRequest", destination));
    ASSERT_TRUE(create.has_value()) << create.error();
    EXPECT_EQ(create->status, 200);
    EXPECT_NE(create->body.find("CreateDestinationResponse"), std::string::npos) << create->body;
    EXPECT_EQ(create->body.find("ErrorResponse"), std::string::npos) << create->body;

    // 2. ...and the warrant itself. The LIID rides in the Annex C.2.2 MediationDetails, which is
    // how clause 5.1.2's "XID to LIID(s) mapping" reaches an MDF.
    const std::string task = std::string(R"(
    <taskDetails>
      <xId>)") + kXid + R"(</xId>
      <targetIdentifiers>
        <targetIdentifier><supiimsi>204081234567890</supiimsi></targetIdentifier>
      </targetIdentifiers>
      <deliveryType>X2Only</deliveryType>
      <listOfDIDs><dId>)" + kDid +
                             R"(</dId></listOfDIDs>
      <listOfMediationDetails>
        <mediationDetails>
          <LIID>)" + kLiid + R"(</LIID>
          <deliveryType>HI2Only</deliveryType>
        </mediationDetails>
      </listOfMediationDetails>
    </taskDetails>)";
    const auto activate = post_x1(client, x1_envelope("ActivateTaskRequest", task));
    ASSERT_TRUE(activate.has_value()) << activate.error();
    EXPECT_EQ(activate->status, 200);
    EXPECT_NE(activate->body.find("ActivateTaskResponse"), std::string::npos) << activate->body;
    EXPECT_EQ(activate->body.find("ErrorResponse"), std::string::npos) << activate->body;

    // 3. An IRI-POI delivers an xIRI for that XID over LI_X2.
    X2X3ClientConfig poi_config;
    poi_config.host = "127.0.0.1";
    poi_config.port = kX2Port;
    poi_config.client_cert_path = CERTS_DIR "/amf/cert.pem";
    poi_config.client_key_path = CERTS_DIR "/amf/key.pem";
    poi_config.ca_path = CERTS_DIR "/ca/ca.crt";
    poi_config.sni = "localhost";
    X2X3Client poi(poi_config);

    xiri::AmfRegistration registration;
    registration.registration_type = xiri::AmfRegistrationType::Initial;
    registration.registration_result = xiri::AmfRegistrationResult::ThreeGppAccess;
    registration.supi = xiri::Imsi{"204081234567890"};
    registration.guti = {"204", "08", 7, 8, 9, 0x0A0B0C0D};
    const auto payload = xiri::encode_xiri_payload(registration);
    ASSERT_TRUE(payload.has_value()) << payload.error();

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
    pdu.attributes.push_back(attr_sequence_number(1));
    pdu.attributes.push_back(attr_network_function_id("amf-01.5gc.example.net"));
    pdu.attributes.push_back(attr_interception_point_id("AMF-IRI-POI-1"));
    pdu.attributes.push_back(attr_timestamp(1789000000, 123456000));
    pdu.attributes.push_back(attr_matched_target_identifier("<imsi>204081234567890</imsi>"));
    pdu.payload = *payload;
    ASSERT_TRUE(poi.send(pdu).has_value());

    // 4. The LEMF receives the mediated record.
    ASSERT_TRUE(lemf.wait_for_iri(20s)) << "no LI_HI2 record reached the LEMF";
    const auto received = lemf.iri();
    ASSERT_FALSE(received.empty());
    const auto message = hi2::decode_iri_message(received[0]);
    ASSERT_TRUE(message.has_value()) << (message ? "" : message.error());

    // The LIID the ADMF provisioned, not the XID and not an invented one.
    EXPECT_EQ(message->header.liid, kLiid);
    // Table 5.5.1-1's mapped fields, carried from the POI's own X2 attributes.
    EXPECT_EQ(message->header.network_function_identifier, "amf-01.5gc.example.net");
    EXPECT_EQ(message->header.extended_interception_point_id, "AMF-IRI-POI-1");
    EXPECT_EQ(message->header.timestamp_qualifier, hi2::TimestampQualifier::TimeOfInterception);
    EXPECT_EQ(message->header.communication_identifier.operator_identifier, "5GC-R19-OP");
    // The target's IMSI, and both provenances an MDF2 can source today: lEAProvided(1) from the
    // X1 task, matchedOn(3) from the PDU's Matched Target Identifier attribute (clause 5.5.5).
    const std::string payload_bytes(message->iri_payload.begin(), message->iri_payload.end());
    EXPECT_NE(payload_bytes.find("204081234567890"), std::string::npos);
    const std::vector<std::uint8_t> lea_provided{0x82, 0x01, 0x01};
    const std::vector<std::uint8_t> matched_on{0x82, 0x01, 0x03};
    EXPECT_NE(std::search(message->iri_payload.begin(),
                          message->iri_payload.end(),
                          lea_provided.begin(),
                          lea_provided.end()),
              message->iri_payload.end())
        << "no identifier carries provenance lEAProvided(1) from the X1 task";
    EXPECT_NE(std::search(message->iri_payload.begin(),
                          message->iri_payload.end(),
                          matched_on.begin(),
                          matched_on.end()),
              message->iri_payload.end())
        << "no identifier carries provenance matchedOn(3) from the X2 attribute";

    // 5. Deactivation stops it: the same xIRI now matches no task and is dropped.
    const auto deactivate = post_x1(
        client, x1_envelope("DeactivateTaskRequest", std::string("\n    <xId>") + kXid + "</xId>"));
    ASSERT_TRUE(deactivate.has_value()) << deactivate.error();
    EXPECT_NE(deactivate->body.find("DeactivateTaskResponse"), std::string::npos)
        << deactivate->body;
    const std::size_t before = lemf.iri().size();
    ASSERT_TRUE(poi.send(pdu).has_value());
    std::this_thread::sleep_for(1s);
    EXPECT_EQ(lemf.iri().size(), before)
        << "the MDF2 delivered a record for a task that was deactivated";

    poi.disconnect();
}
