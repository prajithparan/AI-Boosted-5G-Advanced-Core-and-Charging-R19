// The AMF IRI-POI (nfs/amf/src/li_poi.cpp, ADR-0377) exercised end to end without the rest of the
// AMF: a fake ADMF provisions a warrant over real LI_X1 (mTLS HTTP/2), the POI's
// report_registration builds an AMFRegistration xIRI and streams it over real LI_X2, and a fake
// MDF2 (li_core::X2X3Server) receives and decodes it. This is the POI's own logic -- X1
// provisioning -> target store -> is_target -> xIRI build -> X2 emit -- with the network functions
// it talks to stood in for by the already-tested li_core server/client. Compiling li_poi.cpp
// directly (it has no standalone process and pulls in no other AMF source) keeps this a unit-scope
// integration test.

#include "sbi_core/http2_client.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "li_core/x2x3_pdu.hpp"
#include "li_core/x2x3_server.hpp"
#include "li_core/xiri.hpp"
#include "li_poi.hpp"

#include <gtest/gtest.h>

namespace {

using namespace std::chrono_literals;

// A fixed, unique loopback port for the POI's LI_X1 listener (the sbi HTTP/2 server binds a fixed
// port); the fake MDF2 uses an ephemeral one. Chosen clear of every other test's fixed ports.
constexpr std::uint16_t kX1Port = 19807;
constexpr const char* kX1Url = "https://127.0.0.1:19807/X1/NE";
constexpr const char* kXid = "b0b1b2b3-b4b5-b6b7-b8b9-babbbcbdbebf";
constexpr const char* kTargetImsi = "999070000000001";
constexpr const char* kOtherImsi = "999070000000002";

// Collects the PDUs the fake MDF2 receives, so the test can wait for one.
class Collector {
public:
    void operator()(const li_core::Pdu& pdu, std::string_view) {
        const std::lock_guard<std::mutex> lock(mutex_);
        received_.push_back(pdu);
        cv_.notify_all();
    }
    bool wait_for(std::size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return received_.size() >= count; });
    }
    std::vector<li_core::Pdu> take() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return received_;
    }
    std::size_t size() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return received_.size();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<li_core::Pdu> received_;
};

li_core::X2X3ServerConfig fake_mdf2_config() {
    li_core::X2X3ServerConfig cfg;
    cfg.bind_address = "127.0.0.1";
    cfg.port = 0; // ephemeral
    cfg.server_cert_path = CERTS_DIR "/amf/cert.pem";
    cfg.server_key_path = CERTS_DIR "/amf/key.pem";
    cfg.ca_path = CERTS_DIR "/ca/ca.crt";
    return cfg;
}

amf::LiPoi::Config poi_config(std::uint16_t mdf2_port) {
    amf::LiPoi::Config cfg;
    cfg.x1_bind_address = "127.0.0.1";
    cfg.x1_port = kX1Port;
    cfg.ne_identifier = "amf-poi-test";
    cfg.network_function_id = "amf-01.5gc.example.net";
    cfg.interception_point_id = "AMF-IRI-POI-1";
    cfg.mdf2_host = "127.0.0.1";
    cfg.mdf2_port = mdf2_port;
    cfg.mdf2_sni = "localhost"; // the lab amf cert's SAN
    cfg.cert_path = CERTS_DIR "/amf/cert.pem";
    cfg.key_path = CERTS_DIR "/amf/key.pem";
    cfg.ca_path = CERTS_DIR "/ca/ca.crt";
    return cfg;
}

sbi_core::http2::Client make_admf_client() {
    sbi_core::http2::TlsConfig tls{
        .cert_path = CERTS_DIR "/amf/cert.pem",
        .key_path = CERTS_DIR "/amf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    return sbi_core::http2::Client(std::move(tls));
}

std::string x1_activate(const std::string& xid, const std::string& imsi) {
    return std::string(
               R"(<?xml version="1.0" encoding="UTF-8"?>
<X1Request xmlns="http://uri.etsi.org/03221/X1/2017/10" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xmlns:c="http://uri.etsi.org/03280/common/2017/07">
  <x1RequestMessage xsi:type="ActivateTaskRequest">
    <admfIdentifier>admf-test</admfIdentifier>
    <neIdentifier>amf-poi-test</neIdentifier>
    <messageTimestamp>2026-09-19T00:00:00.000000Z</messageTimestamp>
    <version>v1.23.1</version>
    <x1TransactionId>00000000-0000-4000-8000-000000000001</x1TransactionId>
    <taskDetails>
      <xId>)") +
           xid + R"(</xId>
      <targetIdentifiers>
        <targetIdentifier><supiimsi>)" +
           imsi + R"(</supiimsi></targetIdentifier>
      </targetIdentifiers>
      <deliveryType>X2Only</deliveryType>
      <listOfDIDs><dId>22222222-2222-4222-8222-222222222222</dId></listOfDIDs>
    </taskDetails>
  </x1RequestMessage>
</X1Request>)";
}

std::string x1_deactivate(const std::string& xid) {
    return std::string(
               R"(<?xml version="1.0" encoding="UTF-8"?>
<X1Request xmlns="http://uri.etsi.org/03221/X1/2017/10" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
  <x1RequestMessage xsi:type="DeactivateTaskRequest">
    <admfIdentifier>admf-test</admfIdentifier>
    <neIdentifier>amf-poi-test</neIdentifier>
    <messageTimestamp>2026-09-19T00:00:00.000000Z</messageTimestamp>
    <version>v1.23.1</version>
    <x1TransactionId>00000000-0000-4000-8000-000000000002</x1TransactionId>
    <xId>)") +
           xid + R"(</xId>
  </x1RequestMessage>
</X1Request>)";
}

// POST an X1 document to the POI's LI_X1 listener, retrying briefly while the listener comes up.
sbi_core::http2::ClientResponse post_x1(sbi_core::http2::Client& client, const std::string& xml) {
    sbi_core::http2::ClientRequest request;
    request.method = "POST";
    request.url = kX1Url;
    request.headers.emplace("content-type", "application/xml");
    request.body = xml;
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (auto r = client.send(request); r.has_value()) {
            return *r;
        }
        std::this_thread::sleep_for(100ms);
    }
    ADD_FAILURE() << "LI_X1 listener never answered";
    return {};
}

amf::GutiParts sample_guti() {
    amf::GutiParts guti;
    guti.mcc = "999";
    guti.mnc = "70";
    guti.amf_region_id = 1;
    guti.amf_set_id = 2;
    guti.amf_pointer = 3;
    guti.five_g_tmsi = 0x0A0B0C0D;
    return guti;
}

} // namespace

TEST(LiAmfPoi, ProvisionsOverX1AndEmitsRegistrationXiriForATarget) {
    Collector mdf2;
    li_core::X2X3Server mdf2_server(fake_mdf2_config(), std::ref(mdf2));
    ASSERT_TRUE(mdf2_server.start().has_value());
    ASSERT_NE(mdf2_server.bound_port(), 0);

    amf::LiPoi poi(poi_config(mdf2_server.bound_port()));
    poi.start();

    // 1. The ADMF provisions a warrant for the target IMSI over LI_X1.
    auto client = make_admf_client();
    const auto activate = post_x1(client, x1_activate(kXid, kTargetImsi));
    EXPECT_EQ(activate.status, 200);
    EXPECT_NE(activate.body.find("ActivateTaskResponse"), std::string::npos) << activate.body;
    EXPECT_EQ(activate.body.find("ErrorResponse"), std::string::npos) << activate.body;

    // 2. The target is now recognised; a non-target is not.
    EXPECT_TRUE(poi.is_target(std::string("imsi-") + kTargetImsi));
    EXPECT_TRUE(poi.is_target(kTargetImsi)); // bare digits also match
    EXPECT_FALSE(poi.is_target(std::string("imsi-") + kOtherImsi));

    // 3. A non-target registration emits nothing.
    poi.report_registration(std::string("imsi-") + kOtherImsi, sample_guti());
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(mdf2.size(), 0u);

    // 4. The target's registration emits one AMFRegistration xIRI to the MDF2.
    poi.report_registration(std::string("imsi-") + kTargetImsi, sample_guti());
    ASSERT_TRUE(mdf2.wait_for(1, 5s)) << "no xIRI reached the MDF2";

    const auto received = mdf2.take();
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].type, li_core::PduType::X2);
    EXPECT_EQ(received[0].payload_format, li_core::PayloadFormat::Tgpp33128Payload);
    // The Matched Target Identifier attribute carries the provisioned identity (clause 5.3.18).
    const auto matched =
        li_core::text_attribute(received[0], li_core::AttributeType::MatchedTargetIdentifier);
    ASSERT_TRUE(matched.has_value());
    EXPECT_NE(matched->find(kTargetImsi), std::string::npos) << *matched;

    const auto decoded = li_core::xiri::decode_xiri_payload(received[0].payload);
    ASSERT_TRUE(decoded.has_value()) << decoded.error();
    ASSERT_TRUE(std::holds_alternative<li_core::xiri::AmfRegistration>(decoded->event));
    const auto& reg = std::get<li_core::xiri::AmfRegistration>(decoded->event);
    ASSERT_TRUE(std::holds_alternative<li_core::xiri::Imsi>(reg.supi));
    EXPECT_EQ(std::get<li_core::xiri::Imsi>(reg.supi).digits, kTargetImsi);
    EXPECT_EQ(reg.guti.mcc, "999");
    EXPECT_EQ(reg.guti.mnc, "70");
    EXPECT_EQ(reg.guti.amf_region_id, 1);
    EXPECT_EQ(reg.guti.five_g_tmsi, 0x0A0B0C0Du);

    // 5. After deactivation the same UE is no longer a target and nothing more is emitted.
    const auto deactivate = post_x1(client, x1_deactivate(kXid));
    EXPECT_EQ(deactivate.status, 200);
    EXPECT_NE(deactivate.body.find("DeactivateTaskResponse"), std::string::npos) << deactivate.body;
    EXPECT_FALSE(poi.is_target(std::string("imsi-") + kTargetImsi));

    const std::size_t before = mdf2.size();
    poi.report_registration(std::string("imsi-") + kTargetImsi, sample_guti());
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(mdf2.size(), before) << "a deactivated target still emitted an xIRI";

    poi.stop();
    mdf2_server.stop();
}
