#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tl/expected.hpp>
#include <vector>

// ETSI TS 103 221-2 V1.10.1 (2026-03) X2/X3 PDU codec -- the wire format every POI uses to hand
// xIRI (LI_X2) and xCC (LI_X3) to the MDF2/MDF3 (TS 33.128 V19.7.0 clause 5.3: "Functions having
// an LI_X2 or LI_X3 interface shall support the use of ETSI TS 103 221-2 to realise the
// interface"). Byte layouts are table 5.1-1 (header), table 5.3.1-1 (conditional attribute TLV)
// and the per-attribute clauses 5.3.6-5.3.23, read from the ETSI text; every value below cites its
// clause. Nothing in this file is 3GPP-specific -- TS 33.128's additional requirements on which
// attributes an xIRI carries (table 5.3.2-2) are the POIs' business, expressed with the builders
// at the bottom. See docs/DECISIONS.md ADR-0364.
//
// Pure codec: no transport. The TLS transport profile (clause 6.2) and keepalive timers
// (clause 6.2.4, TIME_P1 = 60 s, TIME_P2 = 180 s) live in the X2/X3 client, not here.

#pragma GCC visibility push(default)
namespace li_core {

// Clause 5.2.1: "the Version field is set to the value 6, comprised of the major number 0 and
// minor number 6".
inline constexpr std::uint16_t kPduVersion = 6;
// Clause 5.2.3: "this value has a minimum value of 40 octets" -- the eight mandatory fields.
inline constexpr std::uint32_t kMandatoryHeaderLength = 40;

// Table 5.2.2-1.
enum class PduType : std::uint16_t {
    X2 = 1,
    X3 = 2,
    Keepalive = 3,
    KeepaliveAck = 4,
};

// Table 5.4.1-1. "Permitted in X2/X3" columns are enforced by validate() below.
enum class PayloadFormat : std::uint16_t {
    KeepaliveReserved = 0,
    Etsi102232Part1Payload = 1,
    Tgpp33128Payload = 2, // BER-encoded TS33128Payloads structure -- what every xIRI uses
    Tgpp33108Payload = 3,
    Proprietary = 4,
    Ipv4Packet = 5,
    Ipv6Packet = 6,
    EthernetFrame = 7,
    RtpPacket = 8,
    SipMessage = 9,
    DhcpMessage = 10,
    RadiusPacket = 11,
    GtpUMessage = 12,
    MsrpMessage = 13,
    Tgpp33108EpsIriContent = 14,
    MimeMessage = 15,
    TgppUnstructuredPdu = 16,
    Etsi102232Part1PsPduPayload = 17,
};

// Table 5.2.6-1.
enum class PayloadDirection : std::uint16_t {
    KeepaliveReserved = 0,
    Unknown = 1,
    ToTarget = 2,
    FromTarget = 3,
    MultipleDirections = 4,
    NotApplicable = 5,
};

// Table 5.3.1-2.
enum class AttributeType : std::uint16_t {
    Etsi102232Part1Defined = 1,
    Tgpp33128Defined = 2,
    Tgpp33108Defined = 3,
    Proprietary = 4,
    DomainId = 5,
    NetworkFunctionId = 6,
    InterceptionPointId = 7,
    SequenceNumber = 8,
    Timestamp = 9,
    SourceIpv4Address = 10,
    DestinationIpv4Address = 11,
    SourceIpv6Address = 12,
    DestinationIpv6Address = 13,
    SourcePort = 14,
    DestinationPort = 15,
    IpProtocol = 16,
    MatchedTargetIdentifier = 17,
    OtherTargetIdentifier = 18,
    MimeContentType = 19,
    MimeContentTransferEncoding = 20,
    AdditionalXidRelatedInformation = 21,
    SdpSessionDescription = 22,
};

// Table 5.3.1-1: 16-bit type, 16-bit length, contents. Unknown types are carried opaquely -- the
// list "is designed to be easily extended" and an MDF must tolerate what a newer POI sends.
struct ConditionalAttribute {
    std::uint16_t type = 0;
    std::vector<std::uint8_t> contents;

    bool operator==(const ConditionalAttribute&) const = default;
};

// Table 5.1-1. Header Length and Payload Length are derived on encode, never stored.
struct Pdu {
    std::uint16_t version = kPduVersion;
    PduType type = PduType::X2;
    PayloadFormat payload_format = PayloadFormat::Tgpp33128Payload;
    PayloadDirection payload_direction = PayloadDirection::Unknown;
    std::array<std::uint8_t, 16> xid{}; // clause 5.2.7: the X1 XID (a UUID) as a 128-bit integer
    std::uint64_t correlation_id = 0;   // clause 5.2.8: zero when the POI does not correlate
    std::vector<ConditionalAttribute> attributes;
    std::vector<std::uint8_t> payload;

    bool operator==(const Pdu&) const = default;
};

enum class DecodeError {
    Truncated,               // fewer bytes than Header Length + Payload Length announce
    HeaderLengthTooSmall,    // clause 5.2.3: below the 40-octet minimum
    AttributeOverrun,        // a TLV runs past Header Length
    UnsupportedMajorVersion, // clause 5.2.1: major (upper 8 bits) != 0 is a backwards-incompatible
                             // PDU
};

std::string_view to_string(DecodeError e);

// Wire encoding per table 5.1-1, all integers network byte order (clause 5.1). Header Length is
// 40 + the encoded attributes; Payload Length is payload.size().
std::vector<std::uint8_t> encode(const Pdu& pdu);

// Decodes exactly one PDU from the front of `bytes`; `consumed` receives its total length so a
// stream reader can advance. Bytes past the PDU are ignored, not an error.
tl::expected<Pdu, DecodeError> decode(std::span<const std::uint8_t> bytes,
                                      std::size_t* consumed = nullptr);

// Returns the number of octets the PDU at the front of `bytes` occupies, once the first 12 octets
// (Version..Payload Length) are available -- what a framing loop needs before it can call decode().
std::optional<std::size_t> frame_length(std::span<const std::uint8_t> bytes);

// Clause 5.1 keepalive PDUs: Version, PDU Type and Header Length populated, every other mandatory
// field zero, exactly one Sequence Number attribute (clause 6.2.4: the acknowledgement echoes it).
Pdu make_keepalive(std::uint32_t sequence_number);
Pdu make_keepalive_ack(std::uint32_t sequence_number);

// Table 5.4.1-1's "Permitted in X2"/"Permitted in X3" columns plus the keepalive shape of
// clause 5.1. Returns a description of the first violation, or nullopt when conformant. encode()
// does not call this: a test may want to build a deliberately malformed PDU.
std::optional<std::string> validate(const Pdu& pdu);

// Conditional attribute builders, one per clause. Each returns a ready ConditionalAttribute.
ConditionalAttribute attr_domain_id(std::string_view value); // 5.3.6, architecture-defined text
ConditionalAttribute
attr_network_function_id(std::string_view value); // 5.3.7, TS 33.128 5.3.1-2: e.g. the NF's FQDN
ConditionalAttribute attr_interception_point_id(std::string_view value); // 5.3.8
ConditionalAttribute attr_sequence_number(std::uint32_t value); // 5.3.9, four-octet unsigned
ConditionalAttribute attr_timestamp(std::uint32_t seconds,
                                    std::uint32_t nanoseconds); // 5.3.10, POSIX timespec as 2 x u32
ConditionalAttribute attr_source_ipv4(std::array<std::uint8_t, 4> addr);       // 5.3.11
ConditionalAttribute attr_destination_ipv4(std::array<std::uint8_t, 4> addr);  // 5.3.12
ConditionalAttribute attr_source_ipv6(std::array<std::uint8_t, 16> addr);      // 5.3.13
ConditionalAttribute attr_destination_ipv6(std::array<std::uint8_t, 16> addr); // 5.3.14
ConditionalAttribute attr_source_port(std::uint16_t port);                     // 5.3.15
ConditionalAttribute attr_destination_port(std::uint16_t port);                // 5.3.16
ConditionalAttribute attr_ip_protocol(std::uint8_t protocol); // 5.3.17, IANA number
// 5.3.18/5.3.19: "the contents of the TargetIdentifier tag without the enclosing TargetIdentifier
// tag itself, encoded in UTF-8" -- e.g. "<imsi>204081234567890</imsi>". The caller passes that
// inner XML; this library does not know TS 103 221-1's identifier vocabulary.
ConditionalAttribute attr_matched_target_identifier(std::string_view inner_xml);
ConditionalAttribute attr_other_target_identifier(std::string_view inner_xml);
ConditionalAttribute attr_mime_content_type(std::string_view value);              // 5.3.20
ConditionalAttribute attr_mime_content_transfer_encoding(std::string_view value); // 5.3.21
// 5.3.22: XID (16) + Correlation ID (8) + optional Sequence Number (4).
ConditionalAttribute
attr_additional_xid_related_information(std::array<std::uint8_t, 16> xid,
                                        std::uint64_t correlation_id,
                                        std::optional<std::uint32_t> sequence_number);
ConditionalAttribute attr_sdp_session_description(std::string_view sdp); // 5.3.23, UTF-8

// Typed readers for the fixed-format attributes. Clause 5.3.1: "the MDF shall use the first
// occurrence of a conditional attribute with a given Attribute Type" -- these return the first.
// nullopt when absent or when the contents have the wrong length for the clause's format.
std::optional<std::uint32_t> sequence_number(const Pdu& pdu);
struct Timespec {
    std::uint32_t seconds = 0;
    std::uint32_t nanoseconds = 0;
    bool operator==(const Timespec&) const = default;
};
std::optional<Timespec> timestamp(const Pdu& pdu);
std::optional<std::string> text_attribute(const Pdu& pdu,
                                          AttributeType type); // first occurrence, as UTF-8
std::vector<std::string>
text_attributes(const Pdu& pdu, AttributeType type); // every occurrence (5.3.18/19 allow many)

} // namespace li_core
#pragma GCC visibility pop
