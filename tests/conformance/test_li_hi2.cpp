// Conformance tests for the LI_HI2 half of the MDF2 (ADR-0374): TS 33.128 clause 5.5 mediation
// (tables 5.5.1-1, 5.5.2-1, 5.5.2-2, clause 5.5.5) and the ETSI TS 102 232-1 PS-PDU envelope of
// ADR-0373. The envelope tests are what ADR-0373 deferred: until now the PS-PDU codec was
// generated and linked but never exercised.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "li_core/hi2.hpp"
#include "li_core/x1.hpp"
#include "li_core/x2x3_pdu.hpp"
#include "li_core/xiri.hpp"

#include <gtest/gtest.h>

using namespace li_core;

namespace {

// A real xIRI: the AMF Registration event of TS 33.128 6.2.2.2, BER-encoded by li_core::xiri.
std::vector<std::uint8_t> sample_xiri() {
    xiri::AmfRegistration registration;
    registration.registration_type = xiri::AmfRegistrationType::Initial;
    registration.registration_result = xiri::AmfRegistrationResult::ThreeGppAccess;
    registration.supi = xiri::Imsi{"204081234567890"};
    registration.guti = {"204", "08", 7, 8, 9, 0x0A0B0C0D};
    auto encoded = xiri::encode_xiri_payload(registration);
    EXPECT_TRUE(encoded.has_value()) << (encoded ? "" : encoded.error());
    return encoded.value_or(std::vector<std::uint8_t>{});
}

hi2::IriMessage sample_message(std::vector<std::uint8_t> iri_payload) {
    hi2::IriMessage message;
    message.header.liid = "LIID-2026-0001";
    message.header.communication_identifier.operator_identifier = "5GC-R19-OP";
    message.header.communication_identifier.network_element_identifier = "amf-01";
    message.header.communication_identifier.communication_identity_number = 4242;
    message.header.communication_identifier.delivery_country_code = "GB";
    message.header.sequence_number = 7;
    message.header.authorization_country_code = "GB";
    message.header.interception_point_id = "AMF-IRI";
    message.header.extended_interception_point_id = "amf-01.5gc.example.net";
    message.header.network_function_identifier = "amf-01.5gc.example.net";
    message.header.timestamp = hi2::Timestamp{1789000000, 123456};
    message.header.timestamp_qualifier = hi2::TimestampQualifier::TimeOfInterception;
    message.iri_type = hi2::IriType::Report;
    message.iri_payload = std::move(iri_payload);
    return message;
}

} // namespace

// ADR-0373 left the PS-PDU codec unexercised. Every PSHeader field of table 5.5.1-1 goes in and
// comes back.
TEST(LiHi2PsPdu, EncodesAndDecodesEveryHeaderField) {
    const auto mediated = hi2::mediate_xiri(sample_xiri(), {});
    ASSERT_TRUE(mediated.has_value()) << (mediated ? "" : mediated.error());
    const hi2::IriMessage sent = sample_message(mediated->iri_payload);

    const auto bytes = hi2::encode_iri_message(sent);
    ASSERT_TRUE(bytes.has_value()) << (bytes ? "" : bytes.error());
    const auto received = hi2::decode_iri_message(*bytes);
    ASSERT_TRUE(received.has_value()) << (received ? "" : received.error());

    EXPECT_EQ(received->header.liid, sent.header.liid);
    EXPECT_EQ(received->header.communication_identifier, sent.header.communication_identifier);
    EXPECT_EQ(received->header.sequence_number, sent.header.sequence_number);
    EXPECT_EQ(received->header.authorization_country_code, sent.header.authorization_country_code);
    EXPECT_EQ(received->header.interception_point_id, sent.header.interception_point_id);
    EXPECT_EQ(received->header.extended_interception_point_id,
              sent.header.extended_interception_point_id);
    EXPECT_EQ(received->header.network_function_identifier,
              sent.header.network_function_identifier);
    EXPECT_EQ(received->iri_type, sent.iri_type);
    EXPECT_EQ(received->iri_payload, sent.iri_payload);
    // PSHeader.timeStamp is a GeneralizedTime: seconds survive, the microsecond part rides in the
    // MicroSecondTimeStamp extension.
    ASSERT_TRUE(received->header.timestamp.has_value());
    EXPECT_EQ(received->header.timestamp->seconds, sent.header.timestamp->seconds);
    EXPECT_EQ(received->header.timestamp->microseconds, sent.header.timestamp->microseconds);
    EXPECT_EQ(received->header.timestamp_qualifier, hi2::TimestampQualifier::TimeOfInterception);
}

// The two identifier fields that are easy to cross: PSHeader.li-psDomainId is the ETSI TS 102
// 232-1 version43 OBJECT IDENTIFIER, IRIPayload.iRIPayloadOID the TS 33.128 RELATIVE-OID.
TEST(LiHi2PsPdu, CarriesTheTs102232DomainIdAndTheTs33128PayloadOid) {
    const auto mediated = hi2::mediate_xiri(sample_xiri(), {});
    ASSERT_TRUE(mediated.has_value());
    const auto bytes = hi2::encode_iri_message(sample_message(mediated->iri_payload));
    ASSERT_TRUE(bytes.has_value());

    // {0 4 0 2 2 5 1 43}: the first two arcs collapse to 0*40+4 = 0x04, the rest are one octet
    // each. The module is DEFINITIONS IMPLICIT TAGS and the field is li-psDomainId [0], so the
    // universal OBJECT IDENTIFIER tag 0x06 is replaced by the context tag 0x80.
    const std::vector<std::uint8_t> domain_id_tlv{
        0x80, 0x07, 0x04, 0x00, 0x02, 0x02, 0x05, 0x01, 0x2B};
    EXPECT_NE(std::search(bytes->begin(), bytes->end(), domain_id_tlv.begin(), domain_id_tlv.end()),
              bytes->end())
        << "PSHeader.li-psDomainId {0 4 0 2 2 5 1 43} not found in the PS-PDU";

    // The RELATIVE-OID {4 19 19 7 3} is five single-octet arcs; iRIPayloadOID is [1] IMPLICIT,
    // so the universal RELATIVE-OID tag 0x0D is replaced by 0x81.
    const std::vector<std::uint8_t> iri_oid_tlv{0x81, 0x05, 0x04, 0x13, 0x13, 0x07, 0x03};
    EXPECT_NE(std::search(mediated->iri_payload.begin(),
                          mediated->iri_payload.end(),
                          iri_oid_tlv.begin(),
                          iri_oid_tlv.end()),
              mediated->iri_payload.end())
        << "IRIPayload.iRIPayloadOID {4 19 19 7 3} not found in the mediated payload";
}

TEST(LiHi2PsPdu, RejectsAnOversizedLiid) {
    const auto mediated = hi2::mediate_xiri(sample_xiri(), {});
    ASSERT_TRUE(mediated.has_value());
    hi2::IriMessage message = sample_message(mediated->iri_payload);
    message.header.liid = std::string(26, 'X'); // TS 103 280 LIID is 1..25 octets
    const auto bytes = hi2::encode_iri_message(message);
    ASSERT_FALSE(bytes.has_value());
    EXPECT_NE(bytes.error().find("1..25"), std::string::npos) << bytes.error();
}

// Table 5.5.2-2: the MDF2 keeps the event CHOICE the POI chose and swaps the payload OID from
// xIRI(1) to iRI(3). Same release and version, so no mediatedFromIndicator.
TEST(LiHi2Mediation, KeepsTheEventChoiceAndSwapsThePayloadOid) {
    const auto xiri_bytes = sample_xiri();
    const auto mediated = hi2::mediate_xiri(xiri_bytes, {});
    ASSERT_TRUE(mediated.has_value()) << (mediated ? "" : mediated.error());
    EXPECT_FALSE(mediated->mediated_from_indicator);

    // The event TLV is identical in both payloads: only the OID and the wrapper differ.
    ASSERT_GT(xiri_bytes.size(), 8u);
    const auto event_start = std::search(xiri_bytes.begin() + 2,
                                         xiri_bytes.end(),
                                         mediated->iri_payload.begin() + 9,
                                         mediated->iri_payload.begin() + 20);
    EXPECT_NE(event_start, xiri_bytes.end())
        << "the mediated IRIPayload does not carry the xIRI's own event bytes";
}

// Clause 5.5.5 provenance, sourced from the LI_X2 conditional attributes of table 5.3.1-2.
TEST(LiHi2Mediation, TagsMatchedAndOtherTargetIdentifiersWithTheirProvenance) {
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
    pdu.payload = sample_xiri();
    pdu.attributes.push_back(attr_network_function_id("amf-01.5gc.example.net"));
    pdu.attributes.push_back(attr_interception_point_id("AMF-IRI-POI-1"));
    pdu.attributes.push_back(attr_timestamp(1789000000, 123456000));
    pdu.attributes.push_back(attr_matched_target_identifier("<imsi>204081234567890</imsi>"));
    pdu.attributes.push_back(attr_other_target_identifier("<e164Number>447700900000</e164Number>"));

    hi2::MediationContext context;
    context.liid = "LIID-2026-0001";
    context.communication_identifier.operator_identifier = "5GC-R19-OP";
    context.sequence_number = 11;
    context.iri_type = hi2::IriType::Report;

    const auto bytes = hi2::mediate_x2_pdu(pdu, context);
    ASSERT_TRUE(bytes.has_value()) << (bytes ? "" : bytes.error());
    const auto message = hi2::decode_iri_message(*bytes);
    ASSERT_TRUE(message.has_value()) << (message ? "" : message.error());

    // Table 5.5.1-1: NFID -> networkFunctionIdentifier, IPID -> extendedInterceptionPointID, and
    // the PDU timestamp with qualifier timeOfInterception(1).
    EXPECT_EQ(message->header.network_function_identifier, "amf-01.5gc.example.net");
    EXPECT_EQ(message->header.extended_interception_point_id, "AMF-IRI-POI-1");
    ASSERT_TRUE(message->header.timestamp.has_value());
    EXPECT_EQ(message->header.timestamp->seconds, 1789000000u);
    EXPECT_EQ(message->header.timestamp->microseconds, 123456u);
    EXPECT_EQ(message->header.timestamp_qualifier, hi2::TimestampQualifier::TimeOfInterception);
    EXPECT_EQ(message->header.liid, "LIID-2026-0001");
    EXPECT_EQ(message->header.sequence_number, 11u);

    // The identifiers ride inside the BER IRIPayload: the IMSI digits and the E.164 digits are
    // both there, and provenance matchedOn(3)/other(4) are single-octet ENUMERATED values.
    const std::string payload(message->iri_payload.begin(), message->iri_payload.end());
    EXPECT_NE(payload.find("204081234567890"), std::string::npos);
    EXPECT_NE(payload.find("447700900000"), std::string::npos);
    // IRITargetIdentifier.provenance is [2] IMPLICIT on an ENUMERATED, so the universal tag 0x0A
    // is replaced by 0x82: matchedOn(3) and other(4) each encode as three octets.
    const std::vector<std::uint8_t> matched_on{0x82, 0x01, 0x03};
    const std::vector<std::uint8_t> other{0x82, 0x01, 0x04};
    EXPECT_NE(std::search(message->iri_payload.begin(),
                          message->iri_payload.end(),
                          matched_on.begin(),
                          matched_on.end()),
              message->iri_payload.end())
        << "no target identifier carries provenance matchedOn(3)";
    EXPECT_NE(
        std::search(
            message->iri_payload.begin(), message->iri_payload.end(), other.begin(), other.end()),
        message->iri_payload.end())
        << "no target identifier carries provenance other(4)";
}

// An xIRI whose alternative has no IRIEvent counterpart must be refused, not silently emptied:
// TS33128Payloads is EXTENSIBILITY IMPLIED, so asn1c returns RC_OK with present == NOTHING for a
// tag it does not know. Tag [161] iPIRIPacketReport is one of the three xIRI-only alternatives.
TEST(LiHi2Mediation, RefusesAnXiriOnlyEventAlternative) {
    // A well-formed XIRIPayload whose event is iPIRIPacketReport [161] -- a valid XIRIEvent
    // alternative with no IRIEvent counterpart (the others are n9HRPDUSessionInfo [100] and
    // s8HRBearerInfo [101]). Hand-encoded because li_core models one event and the generated
    // headers are deliberately unreachable from a test (ADR-0364).
    //
    //   XIRIPayload ::= SEQUENCE { xIRIPayloadOID [1] RELATIVE-OID, event [2] XIRIEvent }
    //   [2] is explicit: XIRIEvent is a CHOICE, which cannot take an implicit tag.
    //   [161] holds an ETSI TS 102 232-3 IPIRIPacketReport { [0] RELATIVE-OID, [1] PacketReport },
    //   whose PacketReport is the header [1] alternative wrapping PacketReportHeader { [1] OCTET
    //   STRING }.
    const std::vector<std::uint8_t> xiri{
        0x30, 0x18,                               // SEQUENCE, 24 octets
        0x81, 0x05, 0x04, 0x13, 0x13, 0x07, 0x01, // [1] RELATIVE-OID {4 19 19 7 1}
        0xA2, 0x0F,                               // [2] XIRIEvent (explicit)
        0xBF, 0x81, 0x21, 0x0B,                   // [161] iPIRIPacketReport, 11 octets
        0x80, 0x01, 0x01,                         //   [0] RELATIVE-OID {1}
        0xA1, 0x06,                               //   [1] PacketReport (explicit CHOICE)
        0xA1, 0x04,                               //     [1] PacketReportHeader
        0x81, 0x02, 0xDE, 0xAD,                   //       [1] OCTET STRING 'DEAD'
    };
    // It is a valid xIRI: li_core::xiri decodes the wrapper and reports the alternative it does
    // not model, rather than failing to parse.
    const auto as_xiri = xiri::decode_xiri_payload(xiri);
    ASSERT_FALSE(as_xiri.has_value());
    EXPECT_NE(as_xiri.error().find("not modelled"), std::string::npos) << as_xiri.error();

    // TS33128Payloads is EXTENSIBILITY IMPLIED, so decoding this alternative's TLV against
    // IRIEvent returns RC_OK with an empty CHOICE rather than an error: mediate_xiri must check
    // the alternative, not the return code, or the LEMF would receive an IRI with no event.
    const auto mediated = hi2::mediate_xiri(xiri, {});
    ASSERT_FALSE(mediated.has_value());
    EXPECT_NE(mediated.error().find("no IRIEvent counterpart"), std::string::npos)
        << mediated.error();
}

TEST(LiHi2Mediation, RefusesANonTgpp33128PayloadFormat) {
    Pdu pdu;
    pdu.type = PduType::X2;
    pdu.payload_format = PayloadFormat::Ipv4Packet;
    pdu.payload = {0x45, 0x00};
    const auto bytes = hi2::mediate_x2_pdu(pdu, hi2::MediationContext{});
    ASSERT_FALSE(bytes.has_value());
    EXPECT_NE(bytes.error().find("Payload Format"), std::string::npos) << bytes.error();
}

// TS 103 221-2 clause 5.3.18: the attribute carries the TargetIdentifier element's children.
TEST(LiHi2TargetIdentifier, ParsesTheX2AttributeFragment) {
    const auto imsi = x1::parse_target_identifier_fragment("<imsi>204081234567890</imsi>");
    ASSERT_TRUE(imsi.has_value()) << (imsi ? "" : imsi.error());
    EXPECT_EQ(imsi->kind, x1::TargetIdentifierKind::Imsi);
    EXPECT_EQ(imsi->value, "204081234567890");

    const auto unknown = x1::parse_target_identifier_fragment("<somethingNew>x</somethingNew>");
    ASSERT_TRUE(unknown.has_value());
    EXPECT_EQ(unknown->kind, x1::TargetIdentifierKind::Other);
    EXPECT_EQ(unknown->element, "somethingNew");

    EXPECT_FALSE(x1::parse_target_identifier_fragment("<imsi>204</wrong>").has_value());
}

// SUCI has no alternative in TS33128Payloads.TargetIdentifier: report it, never guess a field.
TEST(LiHi2TargetIdentifier, RefusesAnIdentifierWithNoAsn1Alternative) {
    hi2::IriTargetIdentifier suci;
    suci.identifier.kind = x1::TargetIdentifierKind::Suci;
    suci.identifier.element = "suci";
    suci.identifier.value = "suci-0-204-08-0-0-0-0123456789";
    suci.provenance = hi2::Provenance::LeaProvided;

    const auto mediated = hi2::mediate_xiri(sample_xiri(), {suci});
    ASSERT_FALSE(mediated.has_value());
    EXPECT_NE(mediated.error().find("no alternative"), std::string::npos) << mediated.error();
}

// ETSI TS 102 232-1 clause 6.3: the session-layer TRI messages ride in the same PS-PDU envelope.
// The subset of ADR-0373 originally dropped the tRIPayload alternative; ADR-0376 restored it,
// because clause 6.3.4's keep-alive IS a TRI and a Delivery Function cannot do without one.
TEST(LiHi2Tri, EncodesAndDecodesTheKeepalivePair) {
    hi2::TriMessage keepalive;
    keepalive.header.liid = "LIID-2026-0001";
    keepalive.header.communication_identifier.operator_identifier = "5GC-R19-OP";
    keepalive.header.sequence_number = 42;
    keepalive.header.timestamp = hi2::Timestamp{1789000000, 0};
    keepalive.type = hi2::TriType::KeepAlive;

    const auto bytes = hi2::encode_tri_message(keepalive);
    ASSERT_TRUE(bytes.has_value()) << (bytes ? "" : bytes.error());
    const auto decoded = hi2::decode_tri_message(*bytes);
    ASSERT_TRUE(decoded.has_value()) << (decoded ? "" : decoded.error());
    EXPECT_EQ(decoded->type, hi2::TriType::KeepAlive);
    // Clause 6.3.4: the response carries the sequence number of the keep-alive that caused it.
    EXPECT_EQ(decoded->header.sequence_number, 42u);
    EXPECT_EQ(decoded->header.liid, "LIID-2026-0001");

    hi2::TriMessage response = *decoded;
    response.type = hi2::TriType::KeepAliveResponse;
    const auto response_bytes = hi2::encode_tri_message(response);
    ASSERT_TRUE(response_bytes.has_value());
    const auto response_decoded = hi2::decode_tri_message(*response_bytes);
    ASSERT_TRUE(response_decoded.has_value());
    EXPECT_EQ(response_decoded->type, hi2::TriType::KeepAliveResponse);
    EXPECT_EQ(response_decoded->header.sequence_number, 42u);
}

// A reader has to route a received PS-PDU before it can decode it.
TEST(LiHi2Tri, PayloadKindDistinguishesIriFromTri) {
    const auto mediated = hi2::mediate_xiri(sample_xiri(), {});
    ASSERT_TRUE(mediated.has_value());
    const auto iri_bytes = hi2::encode_iri_message(sample_message(mediated->iri_payload));
    ASSERT_TRUE(iri_bytes.has_value());

    hi2::TriMessage keepalive;
    keepalive.header.liid = "LIID-2026-0001";
    keepalive.header.communication_identifier.operator_identifier = "5GC-R19-OP";
    keepalive.type = hi2::TriType::KeepAlive;
    const auto tri_bytes = hi2::encode_tri_message(keepalive);
    ASSERT_TRUE(tri_bytes.has_value());

    const auto iri_kind = hi2::payload_kind(*iri_bytes);
    ASSERT_TRUE(iri_kind.has_value());
    EXPECT_EQ(*iri_kind, hi2::PayloadKind::Iri);
    const auto tri_kind = hi2::payload_kind(*tri_bytes);
    ASSERT_TRUE(tri_kind.has_value());
    EXPECT_EQ(*tri_kind, hi2::PayloadKind::Tri);

    // And each decoder refuses the other's PDU rather than returning something empty.
    EXPECT_FALSE(hi2::decode_tri_message(*iri_bytes).has_value());
    EXPECT_FALSE(hi2::decode_iri_message(*tri_bytes).has_value());
}
