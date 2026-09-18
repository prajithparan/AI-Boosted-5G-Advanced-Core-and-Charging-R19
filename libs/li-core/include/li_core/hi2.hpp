#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <tl/expected.hpp>
#include <vector>

#include "li_core/x1.hpp"
#include "li_core/x2x3_pdu.hpp"

// LI_HI2 delivery: the MDF2 half of TS 33.128 clause 5.5 (ADR-0374). Two steps, kept separate
// because they answer to different tables:
//
//   1. mediate_xiri() -- TS 33.128 table 5.5.2-2. Turn the BER XIRIPayload an IRI-POI sent over
//      LI_X2 into the BER IRIPayload the LEMF receives: same event CHOICE alternative, the
//      iRIPayloadOID of the ASN.1 the MDF2 generates with, the Target Identifiers available at
//      the MDF with their clause-5.5.5 provenance, and mediatedFromIndicator when the xIRI was
//      produced by a different release/version of TS33128Payloads than this MDF2 emits.
//   2. encode_iri_message() -- ETSI TS 102 232-1 PS-PDU (tables 5.5.1-1 and 5.5.2-1). Wrap that
//      IRIPayload in the HI2 envelope: PSHeader (LIID, CommunicationIdentifier, sequence number,
//      timestamp + qualifier, NFID, extended IPID) and a PSIRIPayload carrying the bytes in
//      IRIContents.threeGPP33128DefinedIRI.
//
// mediate_x2_pdu() runs both over one received X2 PDU, applying the table 5.5.1-1 mappings that
// are sourced from the PDU's own conditional attributes.
//
// Transport is NOT here: TS 102 232-1 clause 6.4 profiles HI2 over TCP, and the delivery client
// is a separate class (as x2x3_client is for LI_X2/LI_X3). This header produces the bytes.

#pragma GCC visibility push(default)
namespace li_core::hi2 {

// TS33128Payloads TargetIdentifierProvenance, applied per TS 33.128 clause 5.5.5.
enum class Provenance : std::uint8_t {
    LeaProvided = 1, // identifiers from the X1 provisioning message
    Observed = 2,    // identifiers present in the xIRI payload itself
    MatchedOn = 3,   // the LI_X2 Matched Target Identifier attribute
    Other = 4,       // the LI_X2 Other Target Identifier attribute, and anything else at the MDF
};

struct IriTargetIdentifier {
    x1::TargetIdentifier identifier;
    Provenance provenance = Provenance::Other;
};

// ETSI TS 102 232-1 clause 5.2.10.
enum class IriType : std::uint8_t { Begin = 1, End = 2, Continue = 3, Report = 4 };

// ETSI TS 102 232-1 clause 5.2.6.
enum class TimestampQualifier : std::uint8_t {
    Unknown = 0,
    TimeOfInterception = 1,
    TimeOfMediation = 2,
    TimeOfAggregation = 3,
};

// ETSI TS 102 232-1 clause 5.2.4 CommunicationIdentifier. operatorIdentifier is mandatory;
// TS 33.128 table 5.5.1-1 requires the whole field to be present on IRI messages.
struct CommunicationIdentifier {
    std::string operator_identifier;                       // 1..16 octets
    std::optional<std::string> network_element_identifier; // 1..16 octets
    std::optional<std::uint32_t> communication_identity_number;
    std::optional<std::string> delivery_country_code; // exactly 2 printable characters

    bool operator==(const CommunicationIdentifier&) const = default;
};

// Seconds since the Unix epoch plus a microsecond part: PSHeader carries both a GeneralizedTime
// (clause 5.2.6, second resolution) and the MicroSecondTimeStamp extension (clause 5.2.12).
struct Timestamp {
    std::uint64_t seconds = 0;
    std::uint32_t microseconds = 0;

    bool operator==(const Timestamp&) const = default;
};

// One LI_HI2 message: exactly one PS-PDU carrying one IRI payload. Aggregating several IRI
// payloads into one PDU (TS 102 232-1 clause 6.2.3) is not done here -- see the header note in
// hi2.cpp and ADR-0374.
struct IriMessage {
    std::string liid; // TS 103 280 LIID: 1..25 octets
    CommunicationIdentifier communication_identifier;
    std::uint32_t sequence_number = 0;
    std::optional<std::string> authorization_country_code;     // PSHeader [2], 2 characters
    std::optional<std::string> interception_point_id;          // PSHeader [6], 1..8 characters
    std::optional<std::string> extended_interception_point_id; // PSHeader [9] <- LI_X2 IPID
    std::optional<std::string> network_function_identifier;    // PSHeader [10] <- LI_X2 NFID
    std::optional<Timestamp> timestamp;                        // PSHeader [5] and [7]
    TimestampQualifier timestamp_qualifier = TimestampQualifier::TimeOfInterception;
    IriType iri_type = IriType::Report;
    std::vector<std::uint8_t> iri_payload; // BER TS33128Payloads.IRIPayload

    bool operator==(const IriMessage&) const = default;
};

// Encode one PS-PDU (BER). Every length/format constraint the ASN.1 states is checked first, so a
// caller gets a named error rather than a constraint failure inside asn1c.
tl::expected<std::vector<std::uint8_t>, std::string> encode_iri_message(const IriMessage& message);

// Decode one PS-PDU back. Used by the tests and by a receiving LEMF-side tool; an MDF2 does not
// need it.
tl::expected<IriMessage, std::string> decode_iri_message(std::span<const std::uint8_t> bytes);

// Step 1 above. `xiri_payload` is the BER XIRIPayload from an LI_X2 PDU's payload (Payload Format
// 2). Returns the BER IRIPayload for IRIContents.threeGPP33128DefinedIRI.
struct MediationResult {
    std::vector<std::uint8_t> iri_payload;
    // True when the xIRI's relative OID differed from this MDF2's, so table 5.5.2-2's
    // mediatedFromIndicator was set to the received xIRIPayload.relativeOID.
    bool mediated_from_indicator = false;
};
tl::expected<MediationResult, std::string>
mediate_xiri(std::span<const std::uint8_t> xiri_payload,
             const std::vector<IriTargetIdentifier>& target_identifiers);

// What the MDF2 knows that the X2 PDU does not carry: the warrant's LIID, the operator's
// identifiers, the HI2 sequence number it is up to, and the IRI-Type this event maps to.
struct MediationContext {
    std::string liid;
    CommunicationIdentifier communication_identifier;
    std::uint32_t sequence_number = 0;
    IriType iri_type = IriType::Report;
    std::optional<std::string> authorization_country_code;
    std::optional<std::string> interception_point_id;
    // Identifiers the MDF holds from elsewhere -- clause 5.5.5's "lEAProvided" (the X1
    // provisioning message) and any other identifier at the MDF. Merged with the ones read from
    // the PDU's Matched/Other Target Identifier attributes.
    std::vector<IriTargetIdentifier> target_identifiers;
};

// Step 1 + step 2 for one received LI_X2 PDU, applying the table 5.5.1-1 mappings sourced from
// the PDU: NFID -> networkFunctionIdentifier, IPID -> extendedInterceptionPointID, the PDU
// timestamp -> PSHeader timestamp with qualifier timeOfInterception(1), Matched/Other Target
// Identifier -> provenance matchedOn/other.
tl::expected<std::vector<std::uint8_t>, std::string>
mediate_x2_pdu(const Pdu& pdu, const MediationContext& context);

// The relative OID arcs this MDF2 stamps into IRIPayload.iRIPayloadOID:
// {threeGPP(4) ts33128(19) r19(19) version7(7) iRI(3)}. Public so a test can assert the value
// rather than re-derive it.
inline constexpr std::uint32_t kIriPayloadOidArcs[] = {4, 19, 19, 7, 3};
// ETSI TS 102 232-1 version43 li-psDomainId: {0 4 0 2 2 5 1 43}.
inline constexpr std::uint32_t kLiPsDomainIdArcs[] = {0, 4, 0, 2, 2, 5, 1, 43};

} // namespace li_core::hi2
#pragma GCC visibility pop
