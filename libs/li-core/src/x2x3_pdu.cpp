#include "li_core/x2x3_pdu.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tl/expected.hpp>
#include <utility>
#include <vector>

// ETSI TS 103 221-2 V1.10.1 clause 5 -- see the header for the clause map.

namespace li_core {

namespace {

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void put_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

std::uint16_t get_u16(const std::uint8_t* octets) {
    return static_cast<std::uint16_t>((octets[0] << 8) | octets[1]);
}

std::uint32_t get_u32(const std::uint8_t* octets) {
    return (static_cast<std::uint32_t>(octets[0]) << 24) |
           (static_cast<std::uint32_t>(octets[1]) << 16) |
           (static_cast<std::uint32_t>(octets[2]) << 8) | static_cast<std::uint32_t>(octets[3]);
}

std::uint64_t get_u64(const std::uint8_t* octets) {
    return (static_cast<std::uint64_t>(get_u32(octets)) << 32) | get_u32(octets + 4);
}

ConditionalAttribute text_attr(AttributeType type, std::string_view value) {
    return {static_cast<std::uint16_t>(type),
            std::vector<std::uint8_t>(value.begin(), value.end())};
}

template <std::size_t N>
ConditionalAttribute bytes_attr(AttributeType type, const std::array<std::uint8_t, N>& value) {
    return {static_cast<std::uint16_t>(type),
            std::vector<std::uint8_t>(value.begin(), value.end())};
}

const ConditionalAttribute* first(const Pdu& pdu, AttributeType type) {
    for (const auto& attribute : pdu.attributes) {
        if (attribute.type == static_cast<std::uint16_t>(type)) {
            return &attribute;
        }
    }
    return nullptr;
}

// Offsets within the 40-octet mandatory header, table 5.1-1.
constexpr std::size_t kOffVersion = 0;
constexpr std::size_t kOffPduType = 2;
constexpr std::size_t kOffHeaderLength = 4;
constexpr std::size_t kOffPayloadLength = 8;
constexpr std::size_t kOffPayloadFormat = 12;
constexpr std::size_t kOffPayloadDirection = 14;
constexpr std::size_t kOffXid = 16;
constexpr std::size_t kOffCorrelationId = 32;
constexpr std::size_t kLengthsKnownAfter = 12; // Version..Payload Length

} // namespace

std::string_view to_string(DecodeError error) {
    switch (error) {
        case DecodeError::Truncated:
            return "truncated PDU";
        case DecodeError::HeaderLengthTooSmall:
            return "Header Length below the 40-octet minimum (clause 5.2.3)";
        case DecodeError::AttributeOverrun:
            return "conditional attribute runs past Header Length (clause 5.3.1)";
        case DecodeError::UnsupportedMajorVersion:
            return "unsupported major version (clause 5.2.1)";
    }
    return "unknown decode error";
}

std::vector<std::uint8_t> encode(const Pdu& pdu) {
    std::size_t header_length = kMandatoryHeaderLength;
    for (const auto& attribute : pdu.attributes) {
        header_length += 4 + attribute.contents.size();
    }

    std::vector<std::uint8_t> out;
    out.reserve(header_length + pdu.payload.size());
    put_u16(out, pdu.version);
    put_u16(out, static_cast<std::uint16_t>(pdu.type));
    put_u32(out, static_cast<std::uint32_t>(header_length));
    put_u32(out, static_cast<std::uint32_t>(pdu.payload.size()));
    put_u16(out, static_cast<std::uint16_t>(pdu.payload_format));
    put_u16(out, static_cast<std::uint16_t>(pdu.payload_direction));
    out.insert(out.end(), pdu.xid.begin(), pdu.xid.end());
    put_u64(out, pdu.correlation_id);
    for (const auto& attribute : pdu.attributes) {
        put_u16(out, attribute.type);
        put_u16(out, static_cast<std::uint16_t>(attribute.contents.size()));
        out.insert(out.end(), attribute.contents.begin(), attribute.contents.end());
    }
    out.insert(out.end(), pdu.payload.begin(), pdu.payload.end());
    return out;
}

std::optional<std::size_t> frame_length(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kLengthsKnownAfter) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(get_u32(bytes.data() + kOffHeaderLength)) +
           static_cast<std::size_t>(get_u32(bytes.data() + kOffPayloadLength));
}

tl::expected<Pdu, DecodeError> decode(std::span<const std::uint8_t> bytes, std::size_t* consumed) {
    if (bytes.size() < kMandatoryHeaderLength) {
        return tl::unexpected(DecodeError::Truncated);
    }
    const std::uint8_t* octets = bytes.data();
    const std::uint16_t version = get_u16(octets + kOffVersion);
    if ((version >> 8) != (kPduVersion >> 8)) {
        return tl::unexpected(DecodeError::UnsupportedMajorVersion);
    }
    const std::uint32_t header_length = get_u32(octets + kOffHeaderLength);
    const std::uint32_t payload_length = get_u32(octets + kOffPayloadLength);
    if (header_length < kMandatoryHeaderLength) {
        return tl::unexpected(DecodeError::HeaderLengthTooSmall);
    }
    const std::size_t total = static_cast<std::size_t>(header_length) + payload_length;
    if (bytes.size() < total) {
        return tl::unexpected(DecodeError::Truncated);
    }

    Pdu pdu;
    pdu.version = version;
    pdu.type = static_cast<PduType>(get_u16(octets + kOffPduType));
    pdu.payload_format = static_cast<PayloadFormat>(get_u16(octets + kOffPayloadFormat));
    pdu.payload_direction = static_cast<PayloadDirection>(get_u16(octets + kOffPayloadDirection));
    std::memcpy(pdu.xid.data(), octets + kOffXid, pdu.xid.size());
    pdu.correlation_id = get_u64(octets + kOffCorrelationId);

    std::size_t pos = kMandatoryHeaderLength;
    while (pos < header_length) {
        if (header_length - pos < 4) {
            return tl::unexpected(DecodeError::AttributeOverrun);
        }
        ConditionalAttribute attribute;
        attribute.type = get_u16(octets + pos);
        const std::uint16_t len = get_u16(octets + pos + 2);
        pos += 4;
        if (header_length - pos < len) {
            return tl::unexpected(DecodeError::AttributeOverrun);
        }
        attribute.contents.assign(octets + pos, octets + pos + len);
        pos += len;
        pdu.attributes.push_back(std::move(attribute));
    }
    pdu.payload.assign(octets + header_length, octets + total);
    if (consumed) {
        *consumed = total;
    }
    return pdu;
}

Pdu make_keepalive(std::uint32_t sequence_number) {
    Pdu pdu;
    pdu.type = PduType::Keepalive;
    pdu.payload_format = PayloadFormat::KeepaliveReserved;
    pdu.payload_direction = PayloadDirection::KeepaliveReserved;
    pdu.attributes.push_back(attr_sequence_number(sequence_number));
    return pdu;
}

Pdu make_keepalive_ack(std::uint32_t sequence_number) {
    Pdu pdu = make_keepalive(sequence_number);
    pdu.type = PduType::KeepaliveAck;
    return pdu;
}

std::optional<std::string> validate(const Pdu& pdu) {
    if (pdu.version != kPduVersion) {
        return "Version is not 6 (clause 5.2.1)";
    }
    const auto fmt = static_cast<std::uint16_t>(pdu.payload_format);
    switch (pdu.type) {
        case PduType::Keepalive:
        case PduType::KeepaliveAck: {
            // Clause 5.1: all other mandatory fields zero, no payload, a Sequence Number.
            if (fmt != 0 || pdu.payload_direction != PayloadDirection::KeepaliveReserved) {
                return "keepalive PDU must have zero Payload Format and Payload Direction (clause "
                       "5.1)";
            }
            if (pdu.xid != std::array<std::uint8_t, 16>{} || pdu.correlation_id != 0) {
                return "keepalive PDU must have zero XID and Correlation ID (clauses 5.2.7, 5.2.8)";
            }
            if (!pdu.payload.empty()) {
                return "keepalive PDU carries a payload (clause 5.2.4)";
            }
            if (!sequence_number(pdu)) {
                return "keepalive PDU lacks a Sequence Number (clause 5.1)";
            }
            return std::nullopt;
        }
        case PduType::X2:
        case PduType::X3:
            break;
        default:
            return "unknown PDU Type (table 5.2.2-1)";
    }
    if (fmt == 0 || fmt > 17) {
        return "Payload Format outside table 5.4.1-1";
    }
    if (pdu.payload_direction == PayloadDirection::KeepaliveReserved ||
        static_cast<std::uint16_t>(pdu.payload_direction) > 5) {
        return "Payload Direction outside table 5.2.6-1 for a content PDU";
    }
    // Table 5.4.1-1 "Permitted in X2" = No: 7, 8, 12, 13, 16. "Permitted in X3" = No: 9, 10,
    // 11, 14.
    const bool x2_forbidden = fmt == 7 || fmt == 8 || fmt == 12 || fmt == 13 || fmt == 16;
    const bool x3_forbidden = fmt == 9 || fmt == 10 || fmt == 11 || fmt == 14;
    if (pdu.type == PduType::X2 && x2_forbidden) {
        return "Payload Format not permitted in X2 (table 5.4.1-1)";
    }
    if (pdu.type == PduType::X3 && x3_forbidden) {
        return "Payload Format not permitted in X3 (table 5.4.1-1)";
    }
    for (const auto& attribute : pdu.attributes) {
        if (attribute.contents.size() > 0xFFFF) {
            return "conditional attribute longer than its 16-bit length field";
        }
    }
    return std::nullopt;
}

ConditionalAttribute attr_domain_id(std::string_view value) {
    return text_attr(AttributeType::DomainId, value);
}
ConditionalAttribute attr_network_function_id(std::string_view value) {
    return text_attr(AttributeType::NetworkFunctionId, value);
}
ConditionalAttribute attr_interception_point_id(std::string_view value) {
    return text_attr(AttributeType::InterceptionPointId, value);
}

ConditionalAttribute attr_sequence_number(std::uint32_t value) {
    ConditionalAttribute attribute{static_cast<std::uint16_t>(AttributeType::SequenceNumber), {}};
    put_u32(attribute.contents, value);
    return attribute;
}

ConditionalAttribute attr_timestamp(std::uint32_t seconds, std::uint32_t nanoseconds) {
    ConditionalAttribute attribute{static_cast<std::uint16_t>(AttributeType::Timestamp), {}};
    put_u32(attribute.contents, seconds);
    put_u32(attribute.contents, nanoseconds);
    return attribute;
}

ConditionalAttribute attr_source_ipv4(std::array<std::uint8_t, 4> addr) {
    return bytes_attr(AttributeType::SourceIpv4Address, addr);
}
ConditionalAttribute attr_destination_ipv4(std::array<std::uint8_t, 4> addr) {
    return bytes_attr(AttributeType::DestinationIpv4Address, addr);
}
ConditionalAttribute attr_source_ipv6(std::array<std::uint8_t, 16> addr) {
    return bytes_attr(AttributeType::SourceIpv6Address, addr);
}
ConditionalAttribute attr_destination_ipv6(std::array<std::uint8_t, 16> addr) {
    return bytes_attr(AttributeType::DestinationIpv6Address, addr);
}

ConditionalAttribute attr_source_port(std::uint16_t port) {
    ConditionalAttribute attribute{static_cast<std::uint16_t>(AttributeType::SourcePort), {}};
    put_u16(attribute.contents, port);
    return attribute;
}

ConditionalAttribute attr_destination_port(std::uint16_t port) {
    ConditionalAttribute attribute{static_cast<std::uint16_t>(AttributeType::DestinationPort), {}};
    put_u16(attribute.contents, port);
    return attribute;
}

ConditionalAttribute attr_ip_protocol(std::uint8_t protocol) {
    return {static_cast<std::uint16_t>(AttributeType::IpProtocol), {protocol}};
}

ConditionalAttribute attr_matched_target_identifier(std::string_view inner_xml) {
    return text_attr(AttributeType::MatchedTargetIdentifier, inner_xml);
}
ConditionalAttribute attr_other_target_identifier(std::string_view inner_xml) {
    return text_attr(AttributeType::OtherTargetIdentifier, inner_xml);
}
ConditionalAttribute attr_mime_content_type(std::string_view value) {
    return text_attr(AttributeType::MimeContentType, value);
}
ConditionalAttribute attr_mime_content_transfer_encoding(std::string_view value) {
    return text_attr(AttributeType::MimeContentTransferEncoding, value);
}

ConditionalAttribute
attr_additional_xid_related_information(std::array<std::uint8_t, 16> xid,
                                        std::uint64_t correlation_id,
                                        std::optional<std::uint32_t> sequence_number) {
    ConditionalAttribute attribute{
        static_cast<std::uint16_t>(AttributeType::AdditionalXidRelatedInformation), {}};
    attribute.contents.insert(attribute.contents.end(), xid.begin(), xid.end());
    put_u64(attribute.contents, correlation_id);
    if (sequence_number) {
        put_u32(attribute.contents, *sequence_number);
    }
    return attribute;
}

ConditionalAttribute attr_sdp_session_description(std::string_view sdp) {
    return text_attr(AttributeType::SdpSessionDescription, sdp);
}

std::optional<std::uint32_t> sequence_number(const Pdu& pdu) {
    const auto* attribute = first(pdu, AttributeType::SequenceNumber);
    if (!attribute || attribute->contents.size() != 4) {
        return std::nullopt;
    }
    return get_u32(attribute->contents.data());
}

std::optional<Timespec> timestamp(const Pdu& pdu) {
    const auto* attribute = first(pdu, AttributeType::Timestamp);
    if (!attribute || attribute->contents.size() != 8) {
        return std::nullopt;
    }
    return Timespec{get_u32(attribute->contents.data()), get_u32(attribute->contents.data() + 4)};
}

std::optional<std::string> text_attribute(const Pdu& pdu, AttributeType type) {
    const auto* attribute = first(pdu, type);
    if (!attribute) {
        return std::nullopt;
    }
    return std::string(attribute->contents.begin(), attribute->contents.end());
}

std::vector<std::string> text_attributes(const Pdu& pdu, AttributeType type) {
    std::vector<std::string> out;
    for (const auto& attribute : pdu.attributes) {
        if (attribute.type == static_cast<std::uint16_t>(type)) {
            out.emplace_back(attribute.contents.begin(), attribute.contents.end());
        }
    }
    return out;
}

} // namespace li_core
