// Conformance tests for libs/li-core -- ETSI TS 103 221-2 V1.10.1 X2/X3 PDU byte layout (table
// 5.1-1, table 5.3.1-1, clauses 5.2-5.4, 6.2.4) and the TS 33.128 V19.7.0 xIRI payload facade
// (table 5.3.2-1/-3: Payload Format 2, BER XIRIPayload, xIRIPayloadOID). ADR-0364.

#include <algorithm>
#include <cstring>

#include "li_core/x2x3_pdu.hpp"
#include "li_core/xiri.hpp"

#include <gtest/gtest.h>

using namespace li_core;

namespace {

std::array<std::uint8_t, 16> sample_xid() {
    std::array<std::uint8_t, 16> x{};
    for (std::size_t i = 0; i < x.size(); ++i)
        x[i] = static_cast<std::uint8_t>(0xA0 + i);
    return x;
}

} // namespace

// Table 5.1-1 byte-for-byte: 40-octet mandatory header, then TLV attributes, then payload.
TEST(X2X3Pdu, EncodesMandatoryHeaderPerTable511) {
    Pdu pdu;
    pdu.type = PduType::X2;
    pdu.payload_format = PayloadFormat::Tgpp33128Payload;
    pdu.payload_direction = PayloadDirection::FromTarget;
    pdu.xid = sample_xid();
    pdu.correlation_id = 0x0102030405060708ULL;
    pdu.payload = {0xDE, 0xAD};

    const auto bytes = encode(pdu);
    ASSERT_EQ(bytes.size(), 40u + 2u);
    EXPECT_EQ(bytes[0], 0x00);
    EXPECT_EQ(bytes[1], 0x06); // Version 6: major 0, minor 6 (5.2.1)
    EXPECT_EQ(bytes[2], 0x00);
    EXPECT_EQ(bytes[3], 0x01); // PDU Type 1 = X2 (table 5.2.2-1)
    EXPECT_EQ(bytes[4], 0x00);
    EXPECT_EQ(bytes[5], 0x00);
    EXPECT_EQ(bytes[6], 0x00);
    EXPECT_EQ(bytes[7], 40); // Header Length (5.2.3)
    EXPECT_EQ(bytes[8], 0x00);
    EXPECT_EQ(bytes[9], 0x00);
    EXPECT_EQ(bytes[10], 0x00);
    EXPECT_EQ(bytes[11], 2); // Payload Length (5.2.4)
    EXPECT_EQ(bytes[12], 0x00);
    EXPECT_EQ(bytes[13], 0x02); // Payload Format 2 = TS 33.128 (table 5.4.1-1)
    EXPECT_EQ(bytes[14], 0x00);
    EXPECT_EQ(bytes[15], 0x03); // Payload Direction 3 = from target (table 5.2.6-1)
    EXPECT_EQ(std::memcmp(bytes.data() + 16, sample_xid().data(), 16), 0); // XID (5.2.7)
    for (std::size_t i = 0; i < 8; ++i)
        EXPECT_EQ(bytes[32 + i], i + 1); // Correlation ID, big-endian (5.2.8)
    EXPECT_EQ(bytes[40], 0xDE);
    EXPECT_EQ(bytes[41], 0xAD);
}

// Table 5.3.1-1: 16-bit type, 16-bit length, contents -- counted into Header Length (5.2.3).
TEST(X2X3Pdu, ConditionalAttributesExtendHeaderLength) {
    Pdu pdu;
    pdu.attributes.push_back(attr_sequence_number(7));           // type 8, 4 octets (5.3.9)
    pdu.attributes.push_back(attr_timestamp(1700000000u, 500u)); // type 9, 2 x u32 (5.3.10)
    pdu.attributes.push_back(attr_network_function_id("amf1.5gc.example")); // type 6 (5.3.7)

    const auto bytes = encode(pdu);
    const std::size_t expected_header = 40 + (4 + 4) + (4 + 8) + (4 + 16);
    ASSERT_EQ(bytes.size(), expected_header);
    EXPECT_EQ(bytes[7], expected_header);
    // First TLV: type 0x0008, length 0x0004, value 0x00000007.
    EXPECT_EQ(bytes[40], 0x00);
    EXPECT_EQ(bytes[41], 0x08);
    EXPECT_EQ(bytes[42], 0x00);
    EXPECT_EQ(bytes[43], 0x04);
    EXPECT_EQ(bytes[47], 0x07);
    // Timestamp: type 9, seconds then nanoseconds, each network byte order.
    EXPECT_EQ(bytes[49], 0x09);
    EXPECT_EQ(bytes[51], 0x08);
    EXPECT_EQ(bytes[52], 0x65);
    EXPECT_EQ(bytes[53], 0x53);
    EXPECT_EQ(bytes[54], 0xF1);
    EXPECT_EQ(bytes[55], 0x00); // 1700000000
    EXPECT_EQ(bytes[59], 0xF4); // 500 = 0x1F4
    EXPECT_EQ(bytes[58], 0x01);
}

TEST(X2X3Pdu, RoundTripsEveryFieldAndAttribute) {
    Pdu pdu;
    pdu.type = PduType::X3;
    pdu.payload_format = PayloadFormat::Ipv4Packet;
    pdu.payload_direction = PayloadDirection::ToTarget;
    pdu.xid = sample_xid();
    pdu.correlation_id = 42;
    pdu.attributes = {
        attr_domain_id("csp-a"),
        attr_network_function_id("upf1"),
        attr_interception_point_id("cc-poi-1"),
        attr_sequence_number(0xFFFFFFFFu),
        attr_timestamp(1, 2),
        attr_source_ipv4({10, 0, 0, 1}),
        attr_destination_ipv4({10, 0, 0, 2}),
        attr_source_ipv6({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}),
        attr_destination_ipv6({}),
        attr_source_port(1234),
        attr_destination_port(80),
        attr_ip_protocol(17),
        attr_matched_target_identifier("<imsi>204081234567890</imsi>"),
        attr_other_target_identifier("<msisdn>31612345678</msisdn>"),
        attr_other_target_identifier("<pei>3512340000000012</pei>"),
        attr_mime_content_type("text/plain"),
        attr_mime_content_transfer_encoding("binary"),
        attr_additional_xid_related_information(sample_xid(), 43, 9),
        attr_additional_xid_related_information(sample_xid(), 44, std::nullopt),
        attr_sdp_session_description("v=0\r\n"),
    };
    pdu.payload.assign(1500, 0x5A);

    std::size_t consumed = 0;
    const auto bytes = encode(pdu);
    const auto decoded = decode(bytes, &consumed);
    ASSERT_TRUE(decoded.has_value()) << to_string(decoded.error());
    EXPECT_EQ(consumed, bytes.size());
    EXPECT_EQ(*decoded, pdu);
    EXPECT_EQ(sequence_number(*decoded), 0xFFFFFFFFu);
    EXPECT_EQ(timestamp(*decoded), (Timespec{1, 2}));
    EXPECT_EQ(text_attribute(*decoded, AttributeType::MatchedTargetIdentifier),
              "<imsi>204081234567890</imsi>");
    EXPECT_EQ(text_attributes(*decoded, AttributeType::OtherTargetIdentifier).size(), 2u);
    // 5.3.22: 16 + 8 + 4 with a sequence number, 24 without.
    EXPECT_EQ(pdu.attributes[17].contents.size(), 28u);
    EXPECT_EQ(pdu.attributes[18].contents.size(), 24u);
    EXPECT_EQ(validate(pdu), std::nullopt);
}

// Clause 5.1 + 6.2.4: keepalive is the mandatory header with everything but Version, PDU Type
// and Header Length zero, plus a Sequence Number; the ack echoes the number.
TEST(X2X3Pdu, KeepaliveShapeAndAck) {
    const Pdu ka = make_keepalive(99);
    const auto bytes = encode(ka);
    ASSERT_EQ(bytes.size(), 48u);
    EXPECT_EQ(bytes[3], 3);  // PDU Type 3 = Keepalive
    EXPECT_EQ(bytes[7], 48); // Header Length includes the Sequence Number TLV
    for (std::size_t i = 8; i < 40; ++i)
        EXPECT_EQ(bytes[i], 0) << "octet " << i; // Payload Length..Correlation ID zero
    EXPECT_EQ(validate(ka), std::nullopt);

    const Pdu ack = make_keepalive_ack(99);
    EXPECT_EQ(ack.type, PduType::KeepaliveAck);
    EXPECT_EQ(sequence_number(ack), 99u);
    EXPECT_EQ(validate(ack), std::nullopt);

    Pdu bad = ka;
    bad.payload = {1};
    EXPECT_TRUE(validate(bad).has_value());
}

// Table 5.4.1-1 "Permitted in X2 / X3" columns.
TEST(X2X3Pdu, ValidateEnforcesPayloadFormatPermissions) {
    Pdu pdu;
    pdu.type = PduType::X2;
    pdu.payload_direction = PayloadDirection::Unknown;
    for (auto fmt : {PayloadFormat::EthernetFrame,
                     PayloadFormat::RtpPacket,
                     PayloadFormat::GtpUMessage,
                     PayloadFormat::MsrpMessage,
                     PayloadFormat::TgppUnstructuredPdu}) {
        pdu.payload_format = fmt;
        EXPECT_TRUE(validate(pdu).has_value())
            << static_cast<int>(fmt) << " is not permitted in X2";
    }
    pdu.type = PduType::X3;
    for (auto fmt : {PayloadFormat::SipMessage,
                     PayloadFormat::DhcpMessage,
                     PayloadFormat::RadiusPacket,
                     PayloadFormat::Tgpp33108EpsIriContent}) {
        pdu.payload_format = fmt;
        EXPECT_TRUE(validate(pdu).has_value())
            << static_cast<int>(fmt) << " is not permitted in X3";
    }
    pdu.payload_format = PayloadFormat::Tgpp33128Payload; // permitted in both
    EXPECT_EQ(validate(pdu), std::nullopt);
    pdu.type = PduType::X2;
    EXPECT_EQ(validate(pdu), std::nullopt);
    pdu.payload_direction = PayloadDirection::KeepaliveReserved; // 0 is reserved for keepalive
    EXPECT_TRUE(validate(pdu).has_value());
}

TEST(X2X3Pdu, DecodeRejectsMalformedInput) {
    const auto good = encode(make_keepalive(1));
    EXPECT_EQ(decode(std::span(good).first(39)).error(), DecodeError::Truncated);
    EXPECT_EQ(decode(std::span(good).first(44)).error(), DecodeError::Truncated); // inside the TLV

    auto short_header = good;
    short_header[7] = 39; // Header Length below the 40-octet minimum (5.2.3)
    EXPECT_EQ(decode(short_header).error(), DecodeError::HeaderLengthTooSmall);

    auto overrun = good;
    overrun[43] = 200; // attribute length past Header Length
    EXPECT_EQ(decode(overrun).error(), DecodeError::AttributeOverrun);

    auto major = good;
    major[0] = 0x01; // major version 1 -- backwards-incompatible (5.2.1)
    EXPECT_EQ(decode(major).error(), DecodeError::UnsupportedMajorVersion);

    auto minor = good;
    minor[1] = 0x09; // minor 9: backwards-compatible extension, must still decode
    EXPECT_TRUE(decode(minor).has_value());

    // frame_length needs the first 12 octets and reports header + payload.
    EXPECT_EQ(frame_length(std::span(good).first(11)), std::nullopt);
    EXPECT_EQ(frame_length(good), 48u);
}

// Stream framing: two PDUs back to back, decode() consumes exactly one.
TEST(X2X3Pdu, DecodeConsumesOnePduFromAStream) {
    auto stream = encode(make_keepalive(5));
    const auto second = encode(make_keepalive_ack(5));
    stream.insert(stream.end(), second.begin(), second.end());
    std::size_t consumed = 0;
    const auto a = decode(stream, &consumed);
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->type, PduType::Keepalive);
    const auto b = decode(std::span(stream).subspan(consumed), &consumed);
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->type, PduType::KeepaliveAck);
    EXPECT_EQ(sequence_number(*b), 5u);
}

// TS 33.128 table 5.3.2-3: XIRIPayload { xIRIPayloadOID, event }, BER. The OID arcs are the
// module's xIRIPayloadOID = {tS33128PayloadsOID xIRI(1)} with tS33128PayloadsOID =
// {threeGPP(4) ts33128(19) r19(19) version7(7)} -- read from TS33128Payloads.asn r19 version7.
TEST(Xiri, EncodesBerXiriPayloadWithModuleOid) {
    xiri::AmfRegistration reg;
    reg.registration_type = xiri::AmfRegistrationType::Initial;
    reg.registration_result = xiri::AmfRegistrationResult::ThreeGppAccess;
    reg.supi = xiri::Imsi{"204081234567890"};
    reg.guti = {"204", "08", 1, 2, 3, 0xDEADBEEF};

    const auto bytes = xiri::encode_xiri_payload(reg);
    ASSERT_TRUE(bytes.has_value()) << bytes.error();
    // Outer: XIRIPayload SEQUENCE (universal 16, constructed) = 0x30. First member:
    // xIRIPayloadOID [1] IMPLICIT RELATIVE-OID -> context-specific primitive tag 1 = 0x81,
    // length 5, arcs 4 19 19 7 1 as single base-128 octets.
    ASSERT_GE(bytes->size(), 9u);
    EXPECT_EQ((*bytes)[0], 0x30);
    EXPECT_EQ((*bytes)[2], 0x81);
    EXPECT_EQ((*bytes)[3], 0x05);
    EXPECT_EQ((*bytes)[4], 4);
    EXPECT_EQ((*bytes)[5], 19);
    EXPECT_EQ((*bytes)[6], 19);
    EXPECT_EQ((*bytes)[7], 7);
    EXPECT_EQ((*bytes)[8], 1);
    // event [2] XIRIEvent -- CHOICE members are EXPLICITly tagged in an IMPLICIT-TAGS module,
    // so [2] is constructed: 0xA2. Inside: registration [1] AMFRegistration, SEQUENCE -> 0xA1.
    EXPECT_EQ((*bytes)[9], 0xA2);
    // Then the AMFRegistration: registrationType [1] ENUMERATED initial(1) -> 81 01 01.
    static constexpr std::uint8_t kRegistrationTypeInitial[] = {0x81, 0x01, 0x01};
    EXPECT_NE(std::search(bytes->begin(),
                          bytes->end(),
                          std::begin(kRegistrationTypeInitial),
                          std::end(kRegistrationTypeInitial)),
              bytes->end());

    const auto decoded = xiri::decode_xiri_payload(*bytes);
    ASSERT_TRUE(decoded.has_value()) << decoded.error();
    EXPECT_EQ(decoded->payload_oid, (std::vector<std::uint32_t>{4, 19, 19, 7, 1}));
    ASSERT_TRUE(std::holds_alternative<xiri::AmfRegistration>(decoded->event));
    EXPECT_EQ(std::get<xiri::AmfRegistration>(decoded->event), reg);
}

TEST(Xiri, NaiSupiRoundTripsAndConstraintsAreEnforced) {
    xiri::AmfRegistration reg;
    reg.registration_type = xiri::AmfRegistrationType::Emergency;
    reg.registration_result = xiri::AmfRegistrationResult::ThreeGppAndNonThreeGppAccess;
    reg.supi = xiri::Nai{"user@realm.example"};
    reg.guti = {"999", "999", 255, 1023, 63, 0xFFFFFFFF};
    const auto bytes = xiri::encode_xiri_payload(reg);
    ASSERT_TRUE(bytes.has_value()) << bytes.error();
    const auto decoded = xiri::decode_xiri_payload(*bytes);
    ASSERT_TRUE(decoded.has_value()) << decoded.error();
    EXPECT_EQ(std::get<xiri::AmfRegistration>(decoded->event), reg);

    reg.supi = xiri::Imsi{"12345"}; // SIZE(6..15)
    EXPECT_FALSE(xiri::encode_xiri_payload(reg).has_value());
    reg.supi = xiri::Imsi{"204081234567890"};
    reg.guti.amf_set_id = 1024; // INTEGER (0..1023)
    EXPECT_FALSE(xiri::encode_xiri_payload(reg).has_value());
    reg.guti.amf_set_id = 0;
    reg.guti.mnc = "1"; // SIZE(2..3)
    EXPECT_FALSE(xiri::encode_xiri_payload(reg).has_value());

    EXPECT_FALSE(xiri::decode_xiri_payload(std::span(*bytes).first(bytes->size() - 1)).has_value());
    auto trailing = *bytes;
    trailing.push_back(0);
    EXPECT_FALSE(xiri::decode_xiri_payload(trailing).has_value());
}

// The whole chain an IRI-POI runs: xIRI event -> BER payload -> X2 PDU with TS 33.128's
// mandatory attributes (table 5.3.2-2) -> bytes -> back out at the MDF2.
TEST(Xiri, X2PduCarriesXiriPayloadEndToEnd) {
    xiri::AmfRegistration reg;
    reg.supi = xiri::Imsi{"204081234567890"};
    reg.guti = {"204", "08", 0, 0, 0, 1};
    const auto payload = xiri::encode_xiri_payload(reg);
    ASSERT_TRUE(payload.has_value());

    Pdu pdu;
    pdu.type = PduType::X2;
    pdu.payload_format = PayloadFormat::Tgpp33128Payload;
    pdu.payload_direction = PayloadDirection::FromTarget; // initiator of the procedure (5.3.2-1)
    pdu.xid = sample_xid();
    pdu.attributes = {attr_network_function_id("amf1"),
                      attr_interception_point_id("iri-poi"),
                      attr_sequence_number(0),
                      attr_timestamp(1700000000u, 0),
                      attr_matched_target_identifier("<imsi>204081234567890</imsi>")};
    pdu.payload = *payload;
    EXPECT_EQ(validate(pdu), std::nullopt);

    const auto wire = encode(pdu);
    const auto back = decode(wire);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->payload_format, PayloadFormat::Tgpp33128Payload);
    const auto ev = xiri::decode_xiri_payload(back->payload);
    ASSERT_TRUE(ev.has_value()) << ev.error();
    EXPECT_EQ(std::get<xiri::AmfRegistration>(ev->event), reg);
}

// TS 33.128 clause 6.2.2.2.3 -- the AMFDeregistration xIRI. Only the two M members
// (deregistrationDirection, accessType) are modelled; identifiers are C and correlated by XID.
TEST(Xiri, AmfDeregistrationRoundTripsBothDirectionsAndAccessTypes) {
    xiri::AmfDeregistration dereg;
    dereg.deregistration_direction = xiri::AmfDirection::UeInitiated;
    dereg.access_type = xiri::AccessType::ThreeGppAndNonThreeGppAccess;

    const auto bytes = xiri::encode_xiri_payload(dereg);
    ASSERT_TRUE(bytes.has_value()) << bytes.error();

    const auto decoded = xiri::decode_xiri_payload(*bytes);
    ASSERT_TRUE(decoded.has_value()) << decoded.error();
    // The OID is the module's own {4 19 19 7 1}, same as the registration event.
    EXPECT_EQ(decoded->payload_oid, (std::vector<std::uint32_t>{4, 19, 19, 7, 1}));
    ASSERT_TRUE(std::holds_alternative<xiri::AmfDeregistration>(decoded->event));
    EXPECT_EQ(std::get<xiri::AmfDeregistration>(decoded->event), dereg);

    // The other direction/access combination encodes to different bytes and round-trips too.
    xiri::AmfDeregistration network_initiated;
    network_initiated.deregistration_direction = xiri::AmfDirection::NetworkInitiated;
    network_initiated.access_type = xiri::AccessType::NonThreeGppAccess;
    const auto bytes2 = xiri::encode_xiri_payload(network_initiated);
    ASSERT_TRUE(bytes2.has_value()) << bytes2.error();
    EXPECT_NE(*bytes, *bytes2);
    const auto decoded2 = xiri::decode_xiri_payload(*bytes2);
    ASSERT_TRUE(decoded2.has_value()) << decoded2.error();
    EXPECT_EQ(std::get<xiri::AmfDeregistration>(decoded2->event), network_initiated);
}

// TS 33.128 clause 6.2.2.2.5 -- the AMFStartOfInterceptionWithRegisteredUE xIRI. Only the M
// members (registrationResult, sUPI, gUTI) are modelled; registrationType and the rest are C/O.
TEST(Xiri, AmfStartOfInterceptionWithRegisteredUeRoundTrips) {
    xiri::AmfStartOfInterceptionWithRegisteredUE soi;
    soi.registration_result = xiri::AmfRegistrationResult::ThreeGppAndNonThreeGppAccess;
    soi.supi = xiri::Imsi{"310170123456789"};
    soi.guti = {"310", "170", 5, 300, 12, 0xDEADBEEF};

    const auto bytes = xiri::encode_xiri_payload(soi);
    ASSERT_TRUE(bytes.has_value()) << bytes.error();

    const auto decoded = xiri::decode_xiri_payload(*bytes);
    ASSERT_TRUE(decoded.has_value()) << decoded.error();
    EXPECT_EQ(decoded->payload_oid, (std::vector<std::uint32_t>{4, 19, 19, 7, 1}));
    ASSERT_TRUE(std::holds_alternative<xiri::AmfStartOfInterceptionWithRegisteredUE>(decoded->event));
    EXPECT_EQ(std::get<xiri::AmfStartOfInterceptionWithRegisteredUE>(decoded->event), soi);

    // An NAI SUPI works through the shared helper too.
    soi.supi = xiri::Nai{"user@operator.example"};
    const auto nai_bytes = xiri::encode_xiri_payload(soi);
    ASSERT_TRUE(nai_bytes.has_value()) << nai_bytes.error();
    const auto nai_decoded = xiri::decode_xiri_payload(*nai_bytes);
    ASSERT_TRUE(nai_decoded.has_value()) << nai_decoded.error();
    EXPECT_EQ(std::get<xiri::AmfStartOfInterceptionWithRegisteredUE>(nai_decoded->event), soi);

    // The shared GUTI validation still rejects a malformed MCC.
    soi.guti.mcc = "31";
    EXPECT_FALSE(xiri::encode_xiri_payload(soi).has_value());
}
