#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <tl/expected.hpp>
#include <variant>
#include <vector>

// TS 33.128 V19.7.0 xIRI payload facade -- the BER-encoded @TS33128Payloads.XIRIPayload that fills
// an X2 PDU's Payload field with Payload Format 2 (TS 33.128 table 5.3.2-1). The ASN.1 module is
// specs/3gpp/33128-attachments/TS33128Payloads.asn (module OID ... ts33128(19) r19(19)
// version7(7)), compiled by asn1c into li_generated. This header is the ONLY way NF code reaches
// that codec: the generated C types stay inside li_core's shared object (ADR-0364 explains the
// NGAP symbol clash that forces this), so POIs describe events with the C++ structs below and
// never include a generated header.
//
// Coverage in this increment: the XIRIEvent alternatives listed in `Event`. Each struct carries
// the members its ASN.1 SEQUENCE marks non-OPTIONAL; OPTIONAL members are added when the POI that
// produces the event is built against TS 33.128's M/C/O table for it (e.g. table 6.2.2.2.2-1 for
// AMFRegistration) -- that table, not this file, decides what a conformant xIRI carries.

#pragma GCC visibility push(default)
namespace li_core::xiri {

// TS33128Payloads: AMFRegistrationType ::= ENUMERATED { initial(1) ... disasterInitial(7) }
enum class AmfRegistrationType : std::uint8_t {
    Initial = 1,
    Mobility = 2,
    Periodic = 3,
    Emergency = 4,
    SnpnOnboarding = 5,
    DisasterMobility = 6,
    DisasterInitial = 7,
};

// AMFRegistrationResult ::= ENUMERATED { threeGPPAccess(1), nonThreeGPPAccess(2),
// threeGPPAndNonThreeGPPAccess(3) }
enum class AmfRegistrationResult : std::uint8_t {
    ThreeGppAccess = 1,
    NonThreeGppAccess = 2,
    ThreeGppAndNonThreeGppAccess = 3,
};

// AMFDirection ::= ENUMERATED { networkInitiated(1), uEInitiated(2) } -- TS 33.128 6.2.2.2.3
// says whether a deregistration was network- or UE-initiated.
enum class AmfDirection : std::uint8_t {
    NetworkInitiated = 1,
    UeInitiated = 2,
};

// AccessType ::= ENUMERATED { threeGPPAccess(1), nonThreeGPPAccess(2),
// threeGPPandNonThreeGPPAccess(3) } (see TS 24.501 clause 9.11.3.20). A distinct ASN.1 type from
// AMFRegistrationResult even though the arcs coincide.
enum class AccessType : std::uint8_t {
    ThreeGppAccess = 1,
    NonThreeGppAccess = 2,
    ThreeGppAndNonThreeGppAccess = 3,
};

// IMSI ::= NumericString (SIZE(6..15)); NAI ::= UTF8String; SUPI ::= CHOICE { iMSI [1], nAI [2] }
struct Imsi {
    std::string digits;
    bool operator==(const Imsi&) const = default;
};
struct Nai {
    std::string value;
    bool operator==(const Nai&) const = default;
};
using Supi = std::variant<Imsi, Nai>;

// FiveGGUTI ::= SEQUENCE { mCC NumericString(SIZE(3)), mNC NumericString(SIZE(2..3)),
// aMFRegionID INTEGER(0..255), aMFSetID INTEGER(0..1023), aMFPointer INTEGER(0..63),
// fiveGTMSI INTEGER(0..4294967295) }
struct FiveGGuti {
    std::string mcc;
    std::string mnc;
    std::uint8_t amf_region_id = 0;
    std::uint16_t amf_set_id = 0;
    std::uint8_t amf_pointer = 0;
    std::uint32_t five_g_tmsi = 0;
    bool operator==(const FiveGGuti&) const = default;
};

// XIRIEvent.registration [1] AMFRegistration -- TS 33.128 clause 6.2.2.2. Non-OPTIONAL members.
struct AmfRegistration {
    AmfRegistrationType registration_type = AmfRegistrationType::Initial;
    AmfRegistrationResult registration_result = AmfRegistrationResult::ThreeGppAccess;
    Supi supi;
    FiveGGuti guti;
    bool operator==(const AmfRegistration&) const = default;
};

// XIRIEvent.deregistration [2] AMFDeregistration -- TS 33.128 clause 6.2.2.2.3, table
// 6.2.2.2.3-1. Only the two M members here; every identifier (sUPI/sUCI/gUTI/...) is C
// ("if available") and correlated at the MDF2 by the X2 PDU's XID, so a minimal record is
// conformant. The C identifier members are added when the AMF POI that produces this event is
// wired against real AMF state.
struct AmfDeregistration {
    AmfDirection deregistration_direction = AmfDirection::NetworkInitiated;
    AccessType access_type = AccessType::ThreeGppAccess;
    bool operator==(const AmfDeregistration&) const = default;
};

// XIRIEvent.startOfInterceptionWithRegisteredUE [4] AMFStartOfInterceptionWithRegisteredUE --
// TS 33.128 clause 6.2.2.2.5, table 6.2.2.2.5-1. Generated when LI is activated on a UE that is
// already 5GMM-REGISTERED. Only the M members (registrationResult, sUPI, gUTI) here;
// registrationType/slice and the rest are C/O, added when the AMF POI is wired to real state.
struct AmfStartOfInterceptionWithRegisteredUE {
    AmfRegistrationResult registration_result = AmfRegistrationResult::ThreeGppAccess;
    Supi supi;
    FiveGGuti guti;
    bool operator==(const AmfStartOfInterceptionWithRegisteredUE&) const = default;
};

// --- TS 33.128 Location, the 5G NGAP user-location path ------------------------------------
// Location is a SEQUENCE of seven OPTIONAL branches; the AMF POI reports the NGAP-sourced form,
// Location.locationInfo.userLocation (table 6.2.2.2.4-1 form 1). Only that path is modelled here;
// positioningInfo/geoInfo/4G/IMS/coarseLocation, and the non-3GPP/UTRA/GERA/PLMN access types of
// UserLocation, are deferred (added when a POI that produces them is built). PLMNID/TAI/NCGI/ECGI
// mandatory members follow the ASN.1 exactly.

// PLMNID ::= SEQUENCE { mCC NumericString(3), mNC NumericString(2..3) }
struct Plmnid {
    std::string mcc;
    std::string mnc;
    bool operator==(const Plmnid&) const = default;
};

// TAI ::= SEQUENCE { pLMNID, tAC OCTET STRING(2..3), nID OPTIONAL }
struct Tai {
    Plmnid plmn;
    std::vector<std::uint8_t> tac; // 2..3 octets
    bool operator==(const Tai&) const = default;
};

// NCGI ::= SEQUENCE { pLMNID, nRCellID BIT STRING(36), nID OPTIONAL }
struct Ncgi {
    Plmnid plmn;
    std::uint64_t nr_cell_id = 0; // 36 bits
    bool operator==(const Ncgi&) const = default;
};

// ECGI ::= SEQUENCE { pLMNID, eUTRACellID BIT STRING(28), nID OPTIONAL }
struct Ecgi {
    Plmnid plmn;
    std::uint32_t eutra_cell_id = 0; // 28 bits
    bool operator==(const Ecgi&) const = default;
};

// NRLocation ::= SEQUENCE { tAI, nCGI, ... OPTIONAL }; only the two M members.
struct NrLocation {
    Tai tai;
    Ncgi ncgi;
    bool operator==(const NrLocation&) const = default;
};

// EUTRALocation ::= SEQUENCE { tAI, eCGI, ... OPTIONAL }; only the two M members.
struct EutraLocation {
    Tai tai;
    Ecgi ecgi;
    bool operator==(const EutraLocation&) const = default;
};

// UserLocation ::= SEQUENCE (all OPTIONAL); the two 5G access types modelled.
struct UserLocation {
    std::optional<NrLocation> nr;
    std::optional<EutraLocation> eutra;
    bool operator==(const UserLocation&) const = default;
};

// Location ::= SEQUENCE (all OPTIONAL); only locationInfo.userLocation modelled.
struct Location {
    std::optional<UserLocation> user_location;
    bool operator==(const Location&) const = default;
};

// XIRIEvent.locationUpdate [3] AMFLocationUpdate -- TS 33.128 clause 6.2.2.2.4, table
// 6.2.2.2.4-1. M members: sUPI and location; every other member is C/O.
struct AmfLocationUpdate {
    Supi supi;
    Location location;
    bool operator==(const AmfLocationUpdate&) const = default;
};

using Event = std::variant<AmfRegistration,
                           AmfDeregistration,
                           AmfStartOfInterceptionWithRegisteredUE,
                           AmfLocationUpdate>;

// BER-encodes XIRIPayload { xIRIPayloadOID = {4 19 19 7 1}, event }. The OID is the module's own
// xIRIPayloadOID (tS33128PayloadsOID xIRI(1)) -- TS 33.128 table 5.3.2-3: "the value of the
// xIRIPayloadOID specified in the version of the ASN.1 used by the IRI-POI". DER is used for the
// encoder side (a valid BER encoding, canonical); the decoder accepts any BER.
tl::expected<std::vector<std::uint8_t>, std::string> encode_xiri_payload(const Event& event);

struct DecodedXiri {
    std::vector<std::uint32_t> payload_oid; // relative OID arcs as received
    Event event;
};
// Decodes a BER XIRIPayload. Events outside `Event` decode structurally but are reported as an
// error naming the CHOICE alternative -- the MDF2 will widen this as POIs are added.
tl::expected<DecodedXiri, std::string> decode_xiri_payload(std::span<const std::uint8_t> bytes);

// The arcs encode_xiri_payload() writes, for tests and the MDF2's version check.
inline constexpr std::uint32_t kXiriPayloadOidArcs[] = {4, 19, 19, 7, 1};

} // namespace li_core::xiri
#pragma GCC visibility pop
