#include "nas_codec.hpp"

#include <cstring>

#include "aka_crypto/nas_security.hpp"

namespace amf::nas {

namespace {

// TS 24.007 §11.2.3.1.1 plain NAS message header (mobility management): Extended Protocol
// Discriminator, Security Header Type, Message Type. Values confirmed against
// simulators/ransim/vendor/UERANSIM/src/lib/nas/enums.hpp (EExtendedProtocolDiscriminator::
// MOBILITY_MANAGEMENT_MESSAGES, ESecurityHeaderType::NOT_PROTECTED, EMessageType::
// REGISTRATION_REQUEST/AUTHENTICATION_REQUEST) -- not guessed from memory.
constexpr std::uint8_t kEpdMobilityManagement = 0x7E;
constexpr std::uint8_t kSecurityHeaderNotProtected = 0x00;
constexpr std::uint8_t kMessageTypeRegistrationRequest = 0x41;
constexpr std::uint8_t kMessageTypeAuthenticationRequest = 0x56;
constexpr std::uint8_t kMessageTypeAuthenticationResponse = 0x57;
constexpr std::uint8_t kMessageTypeAuthenticationFailure = 0x59;
constexpr std::uint8_t kMessageTypeSecurityModeCommand = 0x5D;
constexpr std::uint8_t kMessageTypeSecurityModeComplete = 0x5E;
constexpr std::uint8_t kMessageTypeRegistrationAccept = 0x42;
// ADR-0278. 0x44, from the same real source every other message type here cites --
// simulators/ransim/vendor/UERANSIM/src/lib/nas/enums.hpp's EMessageType::REGISTRATION_REJECT
// (0b01000100), the file this codec already reads REGISTRATION_ACCEPT = 0x42 from.
constexpr std::uint8_t kMessageTypeRegistrationReject = 0x44;
constexpr std::uint8_t kMessageTypeRegistrationComplete = 0x43;
constexpr std::uint8_t kMessageTypeUlNasTransport = 0x67;
constexpr std::uint8_t kMessageTypeDlNasTransport = 0x68;
// TS 24.501 §9.7 message type values, confirmed against
// simulators/ransim/vendor/UERANSIM/src/lib/nas/enums.hpp's EMessageType (SERVICE_REQUEST=
// 0b01001100, SERVICE_REJECT=0b01001101, SERVICE_ACCEPT=0b01001110) -- not guessed.
constexpr std::uint8_t kMessageTypeServiceRequest = 0x4C;
constexpr std::uint8_t kMessageTypeServiceReject = 0x4D;
constexpr std::uint8_t kMessageTypeServiceAccept = 0x4E;
// ADR-0393. TS 24.501 §9.7 message type values, confirmed against
// simulators/ransim/vendor/UERANSIM/src/lib/nas/enums.hpp's EMessageType
// (DEREGISTRATION_REQUEST_UE_ORIGINATING=0b01000101, DEREGISTRATION_ACCEPT_UE_ORIGINATING=
// 0b01000110) -- not guessed. This project implements only the UE-originating direction; the
// UE-terminated values (0x47/0x48) are real but unused here (network-initiated deregistration is
// out of scope, disclosed).
constexpr std::uint8_t kMessageTypeDeregistrationRequestUeOriginating = 0x45;
constexpr std::uint8_t kMessageTypeDeregistrationAcceptUeOriginating = 0x46;

// TS 24.501 §9.1.1 security header type values (the byte carried in the outer secured envelope,
// distinct from the inner plaintext message's own header, which always carries
// kSecurityHeaderNotProtected). Confirmed against
// simulators/ransim/vendor/UERANSIM/src/lib/nas/enums.hpp's ESecurityHeaderType.
constexpr std::uint8_t kShtIntegrityProtected = 0x01;
constexpr std::uint8_t kShtIntegrityProtectedAndCiphered = 0x02;
constexpr std::uint8_t kShtIntegrityProtectedWithNewSecurityContext = 0x03;
constexpr std::uint8_t kShtIntegrityProtectedAndCipheredWithNewSecurityContext = 0x04;

// TS 24.501 IEIs, confirmed against UERANSIM's own onBuild registrations (msg.cpp) for
// AuthenticationResponse/AuthenticationFailure, not guessed.
constexpr std::uint8_t kIeiAuthenticationResponseParameter = 0x2D;
constexpr std::uint8_t kIeiAuthenticationFailureParameter = 0x30;
constexpr std::uint8_t kIeiUeSecurityCapability = 0x2E;

// UlNasTransport's optional IEs (TS 24.501 §8.2.10 / msg.cpp's own onBuild order: pduSessionId,
// oldPduSessionId, requestType, sNssai, dnn, additionalInformation).
constexpr std::uint8_t kIeiUlNasPduSessionId = 0x12;
constexpr std::uint8_t kIeiUlNasOldPduSessionId = 0x59;
constexpr std::uint8_t kIeiUlNasSNssai = 0x22;
constexpr std::uint8_t kIeiUlNasDnn = 0x25;
constexpr std::uint8_t kIeiUlNasAdditionalInformation = 0x24;
// requestType is a Type-1 half-octet IE (IENasKeySetIdentifier-style: no length byte, IEI packed
// into the high nibble alongside the value in the low nibble) -- checked via high-nibble match,
// not full-byte equality, unlike the Type-3/4 IEIs above.
constexpr std::uint8_t kIeiNibbleUlNasRequestType = 0x8;

// ServiceRequest's real optional IEs (TS 24.501 §8.2.20 / msg.cpp's own onBuild order:
// uplinkDataStatus, pduSessionStatus, allowedPduSessionStatus, nasMessageContainer) -- confirmed
// against simulators/ransim/vendor/UERANSIM/src/lib/nas/msg.cpp's real
// `ServiceRequest::onBuild`, not guessed. ServiceAccept reuses the same 0x50 pduSessionStatus IEI
// (msg.cpp's own `ServiceAccept::onBuild`) -- the same real IEI value UlNasTransport/UlNasTransport
// never touches, TS 24.501's own IEI namespace is per-message, not global.
constexpr std::uint8_t kIeiServiceUplinkDataStatus = 0x40;
constexpr std::uint8_t kIeiServicePduSessionStatus = 0x50;

// RegistrationAccept's real optional 5G-GUTI IE (TS 24.501 §9.11.3.4, confirmed against
// simulators/ransim/vendor/UERANSIM/src/lib/nas/msg.cpp's own `RegistrationAccept::onBuild`).
constexpr std::uint8_t kIeiRegistrationAcceptGuti = 0x77;
// TS 24.501 §9.11.3.4 Table 9.11.3.4.1's own "Type of identity" field value for 5G-GUTI (010),
// packed into the low 3 bits of a byte whose upper 5 bits are all spare/1 (0xF2 = 1111 0 010,
// confirmed against simulators/ransim/vendor/UERANSIM/src/lib/nas/ie6.cpp's own
// `IE5gsMobileIdentity::Encode`'s GUTI case: `stream.appendOctet(0xf2)`).
constexpr std::uint8_t kGutiIdentityTypeByte = 0xF2;

// This project's own real, disclosed lab PLMN identity -- the SAME fixed MCC=999/MNC=70 already
// used throughout this codebase (e.g. ngap_task.cpp's own kMcc/kMnc for GUAMI), duplicated here
// rather than shared across files, matching this project's existing convention for small,
// NF-local, fixed lab constants (e.g. each NF's own Diameter origin-host constant).
constexpr const char* kGutiMcc = "999";
constexpr const char* kGutiMnc = "70";

// TS 24.501 §9.11.3.5 Payload container type -- Type-1 half-octet, low nibble of its own byte
// (no paired field, so the high nibble is spare/0). Confirmed against
// simulators/ransim/vendor/UERANSIM/src/lib/nas/enums.hpp's EPayloadContainerType.
constexpr std::uint8_t kPayloadContainerTypeN1SmInformation = 0x01;

// This project's only NAS access type in scope (single gNB, 3GPP access -- see ADR-0031), used as
// the fixed BEARER input to 128-NEA2/128-NIA2 (aka_crypto::nea2_apply/nia2_mac). Confirmed against
// simulators/ransim/vendor/UERANSIM/src/ue/nas/enc.cpp's own `is3gppAccess ? 1 : 2` convention.
constexpr std::uint8_t kNasBearerId = 1;
constexpr std::uint8_t kDirectionUplink = 0;
constexpr std::uint8_t kDirectionDownlink = 1;

// TS 24.008 §10.5.1.13-style half-octet BCD digit -- '?' marks the 0xF spare/filler nibble
// (odd-length terminator), matching UERANSIM's own DecodeBcdString digit table.
char bcd_digit(std::uint8_t nibble) {
    return (nibble <= 9) ? static_cast<char>('0' + nibble) : '?';
}

// Same real MCC/MNC BCD-packing convention as ngap_task.cpp's own encode_plmn_identity
// (3 octets: mcc2|mcc1, mnc3|mcc3, mnc2|mnc1 -- mnc3=0xF for a real 2-digit MNC, this project's
// own kGutiMnc="70" case), duplicated locally per this file's own "hand-rolled, standalone"
// convention rather than reaching across NF-private files.
void append_plmn(std::vector<std::uint8_t>& out, const char* mcc, const char* mnc) {
    const int mcc1 = mcc[0] - '0';
    const int mcc2 = mcc[1] - '0';
    const int mcc3 = mcc[2] - '0';
    const bool mnc_is_3_digit = std::strlen(mnc) == 3;
    const int mnc1 = mnc[0] - '0';
    const int mnc2 = mnc[1] - '0';
    const int mnc3 = mnc_is_3_digit ? (mnc[2] - '0') : 0xF;
    out.push_back(static_cast<std::uint8_t>((mcc2 << 4) | mcc1));
    out.push_back(static_cast<std::uint8_t>((mnc3 << 4) | mcc3));
    out.push_back(static_cast<std::uint8_t>((mnc2 << 4) | mnc1));
}

// Shared secured-NAS-message envelope logic (TS 24.501 §9.1.1: EPD + SHT + MAC(4) + SeqNo(1) +
// NAS message container), used by every downlink message this AMF sends once a NAS security
// context exists -- SecurityModeCommand (sht=..NewSecurityContext, never ciphered) and
// RegistrationAccept (sht=..AndCiphered, always ciphered) differ only in `sht`/`ciphered`, not in
// how the envelope itself is built.
std::vector<std::uint8_t> encode_secured_downlink(const aka_crypto::NasIntKey& knas_int,
                                                  const aka_crypto::NasEncKey& knas_enc,
                                                  std::uint8_t sht,
                                                  bool ciphered,
                                                  std::uint32_t downlink_count,
                                                  const std::vector<std::uint8_t>& inner_plain) {
    const std::vector<std::uint8_t> wire_inner =
        ciphered ? aka_crypto::nea2_apply(
                       knas_enc, downlink_count, kNasBearerId, kDirectionDownlink, inner_plain)
                 : inner_plain;

    // TS 24.501's NAS MAC construction prepends the 1-octet NAS sequence number (COUNT's
    // low-order byte, the same byte transmitted as the envelope's own SeqNo field below) to the
    // transmitted bytes before computing the MAC -- confirmed directly against a real nr-ue build
    // (UERANSIM's nas_enc::ComputeMac: `OctetString::Concat(OctetString::FromOctet(count.sqn),
    // plainMessage)`, applied to the POST-CIPHER bytes). This is NOT something the raw
    // 128-NIA2/EIA2 algorithm itself needs (COUNT is already a separate parameter to EIA2), but a
    // TS 24.501 NAS-security-layer detail -- missed initially (self-consistency tests and a
    // cross-check against the raw EIA2 primitive alone couldn't catch it), found only by
    // instrumenting a real nr-ue build and diffing its internal MAC input against this project's
    // own for the exact same live exchange. See docs/DECISIONS.md ADR-0037.
    std::vector<std::uint8_t> mac_input;
    mac_input.reserve(wire_inner.size() + 1);
    mac_input.push_back(static_cast<std::uint8_t>(downlink_count & 0xff));
    mac_input.insert(mac_input.end(), wire_inner.begin(), wire_inner.end());
    const auto mac =
        aka_crypto::nia2_mac(knas_int, downlink_count, kNasBearerId, kDirectionDownlink, mac_input);

    std::vector<std::uint8_t> out;
    out.push_back(kEpdMobilityManagement);
    out.push_back(sht);
    out.push_back(static_cast<std::uint8_t>((mac >> 24) & 0xff));
    out.push_back(static_cast<std::uint8_t>((mac >> 16) & 0xff));
    out.push_back(static_cast<std::uint8_t>((mac >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(mac & 0xff));
    out.push_back(static_cast<std::uint8_t>(downlink_count & 0xff)); // sequence number
    out.insert(out.end(), wire_inner.begin(), wire_inner.end());
    return out;
}

// The uplink mirror of encode_secured_downlink -- verifies the MAC (always computed over the
// bytes as transmitted, i.e. still-ciphered when `ciphered`, matching UERANSIM's enc.cpp Decrypt()
// convention, see decode_security_mode_complete's original comment) then deciphers if applicable.
// std::nullopt means the envelope itself doesn't match (wrong EPD, wrong SHT, too short) -- not a
// verification failure, which is instead mac_valid=false on the returned struct (see
// SecurityModeCompleteOutcome-style split, kept consistent for RegistrationComplete below).
struct SecuredUplinkResult {
    bool mac_valid = false;
    std::vector<std::uint8_t> plain_inner; // only meaningful if mac_valid
};

std::optional<SecuredUplinkResult> decode_secured_uplink(const aka_crypto::NasIntKey& knas_int,
                                                         const aka_crypto::NasEncKey& knas_enc,
                                                         std::uint8_t expected_sht,
                                                         bool ciphered,
                                                         std::uint32_t uplink_count,
                                                         const std::vector<std::uint8_t>& p) {
    if (p.size() < 7 || p[0] != kEpdMobilityManagement || p[1] != expected_sht)
        return std::nullopt;

    const std::vector<std::uint8_t> wire_inner(p.begin() + 7, p.end());
    const std::uint32_t received_mac =
        (static_cast<std::uint32_t>(p[2]) << 24) | (static_cast<std::uint32_t>(p[3]) << 16) |
        (static_cast<std::uint32_t>(p[4]) << 8) | static_cast<std::uint32_t>(p[5]);
    // Same 1-octet NAS sequence number prefix as encode_secured_downlink's own MAC input -- see
    // that function's comment.
    std::vector<std::uint8_t> mac_input;
    mac_input.reserve(wire_inner.size() + 1);
    mac_input.push_back(static_cast<std::uint8_t>(uplink_count & 0xff));
    mac_input.insert(mac_input.end(), wire_inner.begin(), wire_inner.end());
    const auto expected_mac =
        aka_crypto::nia2_mac(knas_int, uplink_count, kNasBearerId, kDirectionUplink, mac_input);

    SecuredUplinkResult result;
    result.mac_valid = (expected_mac == received_mac);
    if (!result.mac_valid)
        return result;

    result.plain_inner =
        ciphered ? aka_crypto::nea2_apply(
                       knas_enc, uplink_count, kNasBearerId, kDirectionUplink, wire_inner)
                 : wire_inner;
    return result;
}

} // namespace

std::optional<RegistrationRequestInfo>
decode_registration_request(const std::vector<std::uint8_t>& p) {
    if (p.size() < 4 || p[0] != kEpdMobilityManagement || p[1] != kSecurityHeaderNotProtected ||
        p[2] != kMessageTypeRegistrationRequest) {
        return std::nullopt;
    }

    // byte3: ngKSI (high nibble) | 5GSRegistrationType (low nibble), TS 24.501 §9.11.3.32/§9.11.3.7
    // packed via the "IE1" half-octet convention -- neither value is needed to extract SUPI, so
    // not decoded further this stage.
    std::size_t off = 4;

    // 5GS Mobile Identity (TS 24.501 §9.11.3.4): Type-6 IE, 2-octet big-endian length, then value.
    if (off + 2 > p.size())
        return std::nullopt;
    const std::size_t id_len = (static_cast<std::size_t>(p[off]) << 8) | p[off + 1];
    off += 2;
    if (id_len < 8 || off + id_len > p.size())
        return std::nullopt;

    const std::uint8_t* id = &p[off];
    // byte0: bits0-2 identity type (1=SUCI), bit3 odd/even indicator, bits4-6 SUPI format
    // (0=IMSI). Only the common SUCI+IMSI-format case (encoded as the literal byte 0x01) is in
    // scope -- everything else (GUTI-based registration, a non-IMSI SUPI format) is unsupported.
    if ((id[0] & 0b111) != 0b001 || ((id[0] >> 4) & 0b111) != 0b000) {
        return std::nullopt;
    }

    // PLMN, TS 24.008 §10.5.1.13 half-octet BCD (same convention as
    // ngap_task.cpp's encode_plmn_identity, decode direction).
    const int mcc1 = id[1] & 0xF;
    const int mcc2 = (id[1] >> 4) & 0xF;
    const int mcc3 = id[2] & 0xF;
    const int mnc3_nibble = (id[2] >> 4) & 0xF;
    const int mnc1 = id[3] & 0xF;
    const int mnc2 = (id[3] >> 4) & 0xF;
    const bool long_mnc = mnc3_nibble != 0xF;
    const std::string mcc = {bcd_digit(static_cast<std::uint8_t>(mcc1)),
                             bcd_digit(static_cast<std::uint8_t>(mcc2)),
                             bcd_digit(static_cast<std::uint8_t>(mcc3))};
    const std::string mnc = long_mnc
                                ? std::string{bcd_digit(static_cast<std::uint8_t>(mnc1)),
                                              bcd_digit(static_cast<std::uint8_t>(mnc2)),
                                              bcd_digit(static_cast<std::uint8_t>(mnc3_nibble))}
                                : std::string{bcd_digit(static_cast<std::uint8_t>(mnc1)),
                                              bcd_digit(static_cast<std::uint8_t>(mnc2))};

    // Routing indicator (TS 24.501 §9.11.3.4, TS 23.003 §2.2A): 1 or 2 BCD-packed octets.
    // Disclosed limitation -- this project's only real UE config
    // (simulators/ransim/config/ue.yaml) uses a 4-digit ("0000") routing indicator, i.e. exactly
    // 2 clean octets with no 0xF padding, the only case decoded here (matching UERANSIM's own
    // DecodeBcdString(riLen=2,...) path for this exact config). A padded 1-octet (2-digit)
    // routing indicator is a real, valid TS 24.501 encoding this project does not yet handle --
    // flagged via the neither-nibble-is-0xF check below, not silently misparsed.
    std::size_t ri_off = 4;
    if (((id[ri_off + 1] >> 4) & 0xF) == 0xF || (id[ri_off + 1] & 0xF) == 0xF) {
        return std::nullopt;
    }
    ri_off += 2;

    const std::uint8_t protection_scheme_id = id[ri_off] & 0xF;
    if (protection_scheme_id != 0) {
        return std::nullopt; // only the null protection scheme is in scope this stage
    }
    ri_off += 2; // protection scheme id octet + home network public key id octet

    // Remaining bytes: BCD-packed MSIN digits, plaintext (protection scheme is null, so the
    // "scheme output" is the MSIN itself, not concealed) -- TS 23.003 §2.2A.2.
    std::string msin;
    for (std::size_t i = ri_off; i < id_len; ++i) {
        const std::uint8_t lo = id[i] & 0xF;
        const std::uint8_t hi = (id[i] >> 4) & 0xF;
        msin += bcd_digit(lo);
        if (hi == 0xF)
            break; // odd-length terminator
        msin += bcd_digit(hi);
    }

    RegistrationRequestInfo info;
    info.supi = "imsi-" + mcc + mnc + msin;

    // Scan the optional IEs that follow the 5GS Mobile Identity for ueSecurityCapability (IEI
    // 0x2E), a generic Type-4 TLV walk (IEI + 1-octet length + value, same shape
    // decode_authentication_outcome already uses). Disclosed limitation: this assumes every
    // optional IE preceding 0x2E in the wire stream is also Type-4-shaped -- true for every real
    // config this project drives (UERANSIM's RegistrationRequest builder,
    // simulators/ransim/vendor/UERANSIM/src/ue/nas/mm/register.cpp, never sets the three Type-1
    // half-octet optional IEs -- nonCurrentNgKsi 0xC, micoIndication 0xB,
    // networkSlicingIndication 0x9 -- that could otherwise appear before it for a fresh
    // INITIAL_REGISTRATION); a Type-1 IE appearing here would desync this walk, not silently
    // misparse past it, since the length byte would then be read from the wrong offset and either
    // the loop bails out (out-of-range) or finds no match -- ue_security_capability stays empty.
    std::size_t scan = off + id_len;
    while (scan + 2 <= p.size()) {
        const std::uint8_t iei = p[scan];
        const std::uint8_t len = p[scan + 1];
        if (scan + 2 + len > p.size())
            break;
        if (iei == kIeiUeSecurityCapability) {
            const auto* value = p.data() + scan + 2;
            info.ue_security_capability.assign(value, value + len);
            break;
        }
        scan += 2 + len;
    }

    return info;
}

std::vector<std::uint8_t> encode_authentication_request(const std::array<std::uint8_t, 16>& rand,
                                                        const std::array<std::uint8_t, 16>& autn,
                                                        int ngksi) {
    std::vector<std::uint8_t> out;
    out.push_back(kEpdMobilityManagement);
    out.push_back(kSecurityHeaderNotProtected);
    out.push_back(kMessageTypeAuthenticationRequest);
    out.push_back(static_cast<std::uint8_t>(ngksi & 0xF)); // high nibble spare(0), low nibble ngKSI

    // ABBA (TS 24.501 §9.11.3.10), mandatory, Type-4 IE (1-octet length + value). Default value
    // 0x0000 per TS 33.501 Annex A.7.1 -- this project doesn't negotiate a non-default ABBA.
    out.push_back(0x02);
    out.push_back(0x00);
    out.push_back(0x00);

    // Authentication parameter RAND (TS 24.501 §9.11.3.16), optional TV, IEI 0x21 -- Type-3 IE:
    // fixed 16-octet value, NO length octet (confirmed against
    // simulators/ransim/vendor/UERANSIM/src/lib/nas/ie3.hpp's IEAuthenticationParameterRand,
    // read-only, after a real nr-ue crashed decoding this project's first attempt, which wrongly
    // gave RAND a length octet like AUTN's genuinely-Type-4 encoding below).
    out.push_back(0x21);
    out.insert(out.end(), rand.begin(), rand.end());

    // Authentication parameter AUTN (TS 24.501 §9.11.3.15), optional TLV, IEI 0x20.
    out.push_back(0x20);
    out.push_back(static_cast<std::uint8_t>(autn.size()));
    out.insert(out.end(), autn.begin(), autn.end());

    return out;
}

std::optional<AuthenticationOutcome>
decode_authentication_outcome(const std::vector<std::uint8_t>& p) {
    if (p.size() < 3 || p[0] != kEpdMobilityManagement || p[1] != kSecurityHeaderNotProtected) {
        return std::nullopt;
    }

    if (p[2] == kMessageTypeAuthenticationResponse) {
        // AuthenticationResponse (TS 24.501 §8.2.8) has NO mandatory IEs -- just a sequence of
        // optional TLVs (authenticationResponseParameter IEI 0x2D, eapMessage IEI 0x78). Walk
        // them generically (Type-4: IEI + 1-octet length + value), same TLV-skip pattern
        // RegistrationRequest's own optional IEs would need if this project decoded them --
        // stopping as soon as RES* (0x2D) is found.
        std::size_t off = 3;
        while (off < p.size()) {
            if (off + 2 > p.size())
                return std::nullopt;
            const std::uint8_t iei = p[off];
            const std::uint8_t len = p[off + 1];
            if (off + 2 + len > p.size())
                return std::nullopt;
            if (iei == kIeiAuthenticationResponseParameter) {
                if (len != 16)
                    return std::nullopt; // RES* is always 16 octets for 5G-AKA
                AuthenticationOutcome out;
                out.success = true;
                const auto* value = p.data() + off + 2;
                std::copy(value, value + len, out.res_star.begin());
                return out;
            }
            off += 2 + len; // skip an IE we don't need (e.g. eapMessage)
        }
        return std::nullopt; // no RES* present -- an EAP-only response, out of scope
    }

    if (p[2] == kMessageTypeAuthenticationFailure) {
        // AuthenticationFailure (TS 24.501 §8.2.7): mandatory 5GMM cause (Type-3, 1 octet, no
        // IEI/length), then an optional authenticationFailureParameter (AUTS, IEI 0x30).
        if (p.size() < 4)
            return std::nullopt;
        AuthenticationOutcome out;
        out.success = false;
        out.mm_cause = p[3];
        std::size_t off = 4;
        if (off < p.size()) {
            if (off + 2 > p.size())
                return std::nullopt;
            const std::uint8_t iei = p[off];
            const std::uint8_t len = p[off + 1];
            if (off + 2 + len > p.size())
                return std::nullopt;
            if (iei == kIeiAuthenticationFailureParameter) {
                if (len != 14)
                    return std::nullopt; // AUTS is always 14 octets (SQN xor AK || MAC-S)
                std::array<std::uint8_t, 14> auts{};
                const auto* value = p.data() + off + 2;
                std::copy(value, value + len, auts.begin());
                out.auts = auts;
            }
        }
        return out;
    }

    return std::nullopt;
}

std::vector<std::uint8_t>
encode_security_mode_command(const aka_crypto::NasIntKey& knas_int,
                             const std::vector<std::uint8_t>& ue_security_capability,
                             std::uint32_t downlink_count) {
    // Inner plaintext message (TS 24.501 §8.2.25): own header (EPD + SHT=NOT_PROTECTED) then
    // mandatory selectedNasSecurityAlgorithms (Type-3, 1 octet: ciphering high nibble | integrity
    // low nibble, confirmed against
    // simulators/ransim/vendor/UERANSIM/src/lib/nas/ie3.cpp's IENasSecurityAlgorithms::Encode),
    // mandatoryIE1 ngKSI (1 octet, spare high nibble | TSC+KSI low nibble -- fixed to
    // native/ksi=0, this project's only security context, see ADR-0031), mandatory Type-4
    // replayedUeSecurityCapabilities (length + the bytes RegistrationRequestInfo captured
    // verbatim, no IEI -- mandatory-position Type-4 IEs never carry one).
    std::vector<std::uint8_t> inner;
    inner.push_back(kEpdMobilityManagement);
    inner.push_back(kSecurityHeaderNotProtected);
    inner.push_back(kMessageTypeSecurityModeCommand);
    inner.push_back(static_cast<std::uint8_t>((aka_crypto::kNea2AlgorithmIdentity << 4) |
                                              aka_crypto::kNia2AlgorithmIdentity));
    inner.push_back(0x00); // ngKSI: TSC=native(0), KSI=0
    inner.push_back(static_cast<std::uint8_t>(ue_security_capability.size()));
    inner.insert(inner.end(), ue_security_capability.begin(), ue_security_capability.end());

    // SecurityModeCommand is integrity-protected only, never ciphered (see this function's own
    // declaration comment).
    return encode_secured_downlink(knas_int,
                                   /*knas_enc unused, not ciphered*/ aka_crypto::NasEncKey{},
                                   kShtIntegrityProtectedWithNewSecurityContext,
                                   /*ciphered=*/false,
                                   downlink_count,
                                   inner);
}

std::optional<SecurityModeCompleteOutcome>
decode_security_mode_complete(const aka_crypto::NasIntKey& knas_int,
                              const aka_crypto::NasEncKey& knas_enc,
                              std::uint32_t uplink_count,
                              const std::vector<std::uint8_t>& p) {
    const auto result =
        decode_secured_uplink(knas_int,
                              knas_enc,
                              kShtIntegrityProtectedAndCipheredWithNewSecurityContext,
                              /*ciphered=*/true,
                              uplink_count,
                              p);
    if (!result.has_value())
        return std::nullopt;

    SecurityModeCompleteOutcome out;
    out.mac_valid = result->mac_valid;
    if (!out.mac_valid)
        return out;

    // A valid MAC on a message that doesn't decipher to a SecurityModeComplete header is not
    // realistically reachable (that would need an AES-128-CMAC collision against a
    // different plaintext), but checked anyway rather than assumed -- costs nothing here.
    const auto& plain_inner = result->plain_inner;
    if (plain_inner.size() < 3 || plain_inner[0] != kEpdMobilityManagement ||
        plain_inner[2] != kMessageTypeSecurityModeComplete) {
        out.mac_valid = false;
    }
    return out;
}

std::vector<std::uint8_t> encode_registration_accept(const aka_crypto::NasIntKey& knas_int,
                                                     const aka_crypto::NasEncKey& knas_enc,
                                                     std::uint32_t downlink_count,
                                                     std::uint32_t tmsi,
                                                     std::uint8_t amf_region_id,
                                                     std::uint16_t amf_set_id,
                                                     std::uint8_t amf_pointer) {
    // Inner plaintext message (TS 24.501 §8.2.7): own header, then the ONLY mandatory IE,
    // registrationResult (Type-4, 1 octet: smsOverNasAllowed bit3 | registrationResult bits0-2,
    // confirmed against
    // simulators/ransim/vendor/UERANSIM/src/lib/nas/ie4.cpp's IE5gsRegistrationResult::Encode).
    // Fixed to THREEGPP_ACCESS (this project's only access type, ADR-0031) and
    // smsOverNasAllowed=NOT_ALLOWED (no SMSF integration).
    constexpr std::uint8_t kRegistrationResultThreeGppAccess = 0b001;
    constexpr std::uint8_t kSmsOverNasNotAllowed = 0;
    std::vector<std::uint8_t> inner;
    inner.push_back(kEpdMobilityManagement);
    inner.push_back(kSecurityHeaderNotProtected);
    inner.push_back(kMessageTypeRegistrationAccept);
    inner.push_back(0x01); // registrationResult IE length
    inner.push_back(static_cast<std::uint8_t>((kSmsOverNasNotAllowed << 3) |
                                              kRegistrationResultThreeGppAccess));

    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #100/ADR-0075): real optional 5G-GUTI IE
    // -- see this function's own header comment for why this was a real, load-bearing
    // prerequisite for ServiceRequest, not an independent addition. Type-6 IE: IEI, 2-octet
    // big-endian length, then value (same TLV shape decode_registration_request's own SUCI
    // parsing already uses, just in the encode direction). Value layout confirmed against
    // simulators/ransim/vendor/UERANSIM/src/lib/nas/ie6.cpp's real
    // `IE5gsMobileIdentity::Encode`'s GUTI case: identity-type byte, PLMN (3 octets), AMF Region
    // ID (1 octet), AMF Set ID (10 bits) + AMF Pointer (6 bits) packed into 2 octets, then the
    // 5G-TMSI (4 octets).
    std::vector<std::uint8_t> guti_value;
    guti_value.push_back(kGutiIdentityTypeByte);
    append_plmn(guti_value, kGutiMcc, kGutiMnc);
    guti_value.push_back(amf_region_id);
    guti_value.push_back(static_cast<std::uint8_t>((amf_set_id >> 2) & 0xFF));
    guti_value.push_back(
        static_cast<std::uint8_t>(((amf_set_id & 0b11) << 6) | (amf_pointer & 0b111111)));
    guti_value.push_back(static_cast<std::uint8_t>((tmsi >> 24) & 0xFF));
    guti_value.push_back(static_cast<std::uint8_t>((tmsi >> 16) & 0xFF));
    guti_value.push_back(static_cast<std::uint8_t>((tmsi >> 8) & 0xFF));
    guti_value.push_back(static_cast<std::uint8_t>(tmsi & 0xFF));

    inner.push_back(kIeiRegistrationAcceptGuti);
    inner.push_back(static_cast<std::uint8_t>((guti_value.size() >> 8) & 0xFF));
    inner.push_back(static_cast<std::uint8_t>(guti_value.size() & 0xFF));
    inner.insert(inner.end(), guti_value.begin(), guti_value.end());

    return encode_secured_downlink(knas_int,
                                   knas_enc,
                                   kShtIntegrityProtectedAndCiphered,
                                   /*ciphered=*/true,
                                   downlink_count,
                                   inner);
}

std::vector<std::uint8_t> encode_registration_reject(const aka_crypto::NasIntKey& knas_int,
                                                     const aka_crypto::NasEncKey& knas_enc,
                                                     std::uint32_t downlink_count,
                                                     std::uint8_t mm_cause) {
    // Inner plaintext message (TS 24.501 §8.2.8): own header, then the ONE mandatory IE --
    // 5GMM cause, a Type-3 single octet with no length prefix, exactly as
    // encode_service_reject_plain already encodes it for ServiceReject. T3346/T3502 are optional
    // and not sent.
    std::vector<std::uint8_t> inner;
    inner.push_back(kEpdMobilityManagement);
    inner.push_back(kSecurityHeaderNotProtected);
    inner.push_back(kMessageTypeRegistrationReject);
    inner.push_back(mm_cause);

    // SECURED, unlike encode_service_reject_plain. The difference is real, not stylistic: this
    // reject is only ever sent after SecurityModeComplete has succeeded, so a NAS security context
    // exists and TS 24.501 §4.4.4.2 has the message protected with it. The plain form exists for
    // the case where there is no context to protect WITH (see encode_service_reject_plain's own
    // comment); using it here would throw away protection the network already has.
    return encode_secured_downlink(knas_int,
                                   knas_enc,
                                   kShtIntegrityProtectedAndCiphered,
                                   /*ciphered=*/true,
                                   downlink_count,
                                   inner);
}

std::optional<RegistrationCompleteOutcome>
decode_registration_complete(const aka_crypto::NasIntKey& knas_int,
                             const aka_crypto::NasEncKey& knas_enc,
                             std::uint32_t uplink_count,
                             const std::vector<std::uint8_t>& p) {
    const auto result = decode_secured_uplink(knas_int,
                                              knas_enc,
                                              kShtIntegrityProtectedAndCiphered,
                                              /*ciphered=*/true,
                                              uplink_count,
                                              p);
    if (!result.has_value())
        return std::nullopt;

    RegistrationCompleteOutcome out;
    out.mac_valid = result->mac_valid;
    if (!out.mac_valid)
        return out;

    // RegistrationComplete (TS 24.501 §8.2.5) has no mandatory IEs -- just header + message type,
    // matching simulators/ransim/vendor/UERANSIM/src/lib/nas/msg.cpp's own onBuild (only one
    // optional IE, sorTransparentContainer, which this project's UE config never triggers).
    const auto& plain_inner = result->plain_inner;
    if (plain_inner.size() < 3 || plain_inner[0] != kEpdMobilityManagement ||
        plain_inner[2] != kMessageTypeRegistrationComplete) {
        out.mac_valid = false;
    }
    return out;
}

std::optional<UlNasTransportInfo> decode_ul_nas_transport(const aka_crypto::NasIntKey& knas_int,
                                                          const aka_crypto::NasEncKey& knas_enc,
                                                          std::uint32_t uplink_count,
                                                          const std::vector<std::uint8_t>& p) {
    const auto result = decode_secured_uplink(knas_int,
                                              knas_enc,
                                              kShtIntegrityProtectedAndCiphered,
                                              /*ciphered=*/true,
                                              uplink_count,
                                              p);
    if (!result.has_value())
        return std::nullopt;

    UlNasTransportInfo out;
    out.mac_valid = result->mac_valid;
    if (!out.mac_valid)
        return out;

    const auto& inner = result->plain_inner;
    // header(3) + payloadContainerType(1) + payloadContainer length(2) = 6 bytes minimum, even for
    // a zero-length payload container.
    if (inner.size() < 6 || inner[0] != kEpdMobilityManagement ||
        inner[2] != kMessageTypeUlNasTransport) {
        out.mac_valid = false; // not a message this function decodes
        return out;
    }
    if ((inner[3] & 0x0F) != kPayloadContainerTypeN1SmInformation) {
        out.mac_valid = false; // SMS/LPP/UE policy container etc. -- out of scope
        return out;
    }

    std::size_t off = 4;
    const std::size_t container_len = (static_cast<std::size_t>(inner[off]) << 8) | inner[off + 1];
    off += 2;
    if (off + container_len > inner.size()) {
        out.mac_valid = false;
        return out;
    }
    // The payload container itself is opaque -- see this function's own declaration comment for
    // why it's intentionally not decoded further -- but is captured verbatim (ADR-0038) so the
    // caller can forward it to SMF unmodified.
    out.payload_container.assign(inner.begin() + static_cast<std::ptrdiff_t>(off),
                                 inner.begin() + static_cast<std::ptrdiff_t>(off + container_len));
    off += container_len;

    // Walk the optional IEs that follow for pduSessionId/sNssai/dnn, skipping the others (Type-3
    // TV for oldPduSessionId, Type-1 half-octet for requestType, Type-4 TLV for
    // additionalInformation) to stay in sync rather than stopping early -- UERANSIM's real UE
    // sends pduSessionId, requestType, sNssai, and dnn all in one message for PDU Session
    // Establishment (simulators/ransim/vendor/UERANSIM/src/ue/nas/sm/transport.cpp), so requestType
    // sitting between pduSessionId and sNssai/dnn must be skippable, not treated as "unknown,
    // stop."
    while (off < inner.size()) {
        const std::uint8_t tag = inner[off];
        if (tag == kIeiUlNasPduSessionId) {
            if (off + 2 > inner.size())
                break;
            out.pdu_session_id = inner[off + 1];
            off += 2;
        } else if (tag == kIeiUlNasOldPduSessionId) {
            if (off + 2 > inner.size())
                break;
            off += 2;
        } else if (tag == kIeiUlNasSNssai) {
            if (off + 2 > inner.size())
                break;
            const std::uint8_t len = inner[off + 1];
            if (off + 2 + len > inner.size())
                break;
            if (len >= 1)
                out.snssai_sst = inner[off + 2];
            if (len >= 4) {
                std::array<std::uint8_t, 3> sd{};
                sd[0] = inner[off + 3];
                sd[1] = inner[off + 4];
                sd[2] = inner[off + 5];
                out.snssai_sd = sd;
            }
            off += 2 + len;
        } else if (tag == kIeiUlNasDnn) {
            if (off + 2 > inner.size())
                break;
            const std::uint8_t len = inner[off + 1];
            if (off + 2 + len > inner.size())
                break;
            // TS 23.003 §9.1 label encoding: [1-octet label length][ascii label]..., concatenated
            // with no separators -- reconstruct the dotted string by joining labels with '.'.
            std::string dnn;
            std::size_t label_off = off + 2;
            const std::size_t label_end = off + 2 + len;
            while (label_off < label_end) {
                const std::uint8_t label_len = inner[label_off];
                if (label_off + 1 + label_len > label_end)
                    break;
                if (!dnn.empty())
                    dnn += '.';
                dnn.append(reinterpret_cast<const char*>(&inner[label_off + 1]), label_len);
                label_off += 1 + label_len;
            }
            out.dnn = dnn;
            off += 2 + len;
        } else if (tag == kIeiUlNasAdditionalInformation) {
            if (off + 2 > inner.size())
                break;
            const std::uint8_t len = inner[off + 1];
            if (off + 2 + len > inner.size())
                break;
            off += 2 + len;
        } else if ((tag >> 4) == kIeiNibbleUlNasRequestType) {
            off += 1;
        } else {
            break; // unknown optional IE -- stop rather than misparse, disclosed limitation
        }
    }

    return out;
}

std::vector<std::uint8_t>
encode_dl_nas_transport(const aka_crypto::NasIntKey& knas_int,
                        const aka_crypto::NasEncKey& knas_enc,
                        std::uint32_t downlink_count,
                        std::uint8_t pdu_session_id,
                        const std::vector<std::uint8_t>& n1_sm_container) {
    // Inner plaintext message (TS 24.501 §8.2.9): own header, then mandatory
    // payloadContainerType (Type-1, low nibble of its own byte) + payloadContainer (Type-6 LV-E),
    // then optional pduSessionId (Type-3 TV, IEI 0x12 -- confirmed against
    // DlNasTransport::onBuild's `b.optionalIE(0x12, &pduSessionId)`, same IEI value UlNasTransport
    // uses).
    std::vector<std::uint8_t> inner;
    inner.push_back(kEpdMobilityManagement);
    inner.push_back(kSecurityHeaderNotProtected);
    inner.push_back(kMessageTypeDlNasTransport);
    inner.push_back(kPayloadContainerTypeN1SmInformation);
    const auto container_len = static_cast<std::uint16_t>(n1_sm_container.size());
    inner.push_back(static_cast<std::uint8_t>(container_len >> 8));
    inner.push_back(static_cast<std::uint8_t>(container_len & 0xFF));
    inner.insert(inner.end(), n1_sm_container.begin(), n1_sm_container.end());
    inner.push_back(kIeiUlNasPduSessionId);
    inner.push_back(pdu_session_id);

    return encode_secured_downlink(knas_int,
                                   knas_enc,
                                   kShtIntegrityProtectedAndCiphered,
                                   /*ciphered=*/true,
                                   downlink_count,
                                   inner);
}

namespace {

// Shared by peek_service_request_tmsi and decode_service_request -- both need to walk the SAME
// real plaintext structure (inner header + byte3 serviceType/ngKSI + Type-6 TMSI IE), just at
// different points (before vs. after MAC verification is even possible). Returns std::nullopt if
// the bytes don't even match ServiceRequest's own inner shape.
struct ParsedServiceRequestInner {
    std::uint8_t ngksi = 0;
    std::uint8_t service_type = 0;
    std::uint32_t tmsi = 0;
    std::size_t offset_after_tmsi = 0;
};

std::optional<ParsedServiceRequestInner>
parse_service_request_inner(const std::vector<std::uint8_t>& plain_inner) {
    if (plain_inner.size() < 4 || plain_inner[0] != kEpdMobilityManagement ||
        plain_inner[1] != kSecurityHeaderNotProtected ||
        plain_inner[2] != kMessageTypeServiceRequest) {
        return std::nullopt;
    }

    ParsedServiceRequestInner out;
    // byte3: serviceType (high nibble) | ngKSI (low nibble) -- TS 24.501's own "IE1" half-octet
    // convention, confirmed against
    // simulators/ransim/vendor/UERANSIM/src/lib/nas/msg.hpp's own mandatoryIE1(ptr1, ptr2)
    // template: `*ptr1 = T::Decode((octet >> 4) & 0xF); *ptr2 = U::Decode(octet & 0xF)` where
    // ptr1=serviceType, ptr2=ngKSI (matching msg.cpp's own
    // `b.mandatoryIE1(&serviceType, &ngKSI)`).
    out.service_type = static_cast<std::uint8_t>((plain_inner[3] >> 4) & 0x0F);
    out.ngksi = static_cast<std::uint8_t>(plain_inner[3] & 0x0F);

    // Mandatory TMSI (TS 24.501 §9.11.3.4, real 5GSMobileIdentity Type-6 IE: 2-octet big-endian
    // length then value). A real UE's ServiceRequest always encodes this in the SHORT "TMSI" form
    // (identity-type byte 0xf4, no PLMN/AMF-Region prefix -- confirmed against
    // simulators/ransim/vendor/UERANSIM/src/ue/nas/mm/service.cpp's own
    // `request->tmsi.type = nas::EIdentityType::TMSI`, the real UE-side construction, not the
    // longer GUTI form RegistrationAccept's own encode_registration_accept sends). Value layout:
    // identity-type byte, AMF Set ID (10 bits) + AMF Pointer (6 bits) packed into 2 octets, then
    // the 5G-TMSI itself (4 octets) -- this project only needs the TMSI to look up a persisted
    // UeSecurityContext, so AMF Set ID/Pointer are walked past, not surfaced (this project's own
    // single-AMF-instance lab scope, same disclosed-narrowing precedent every other optional/
    // multi-variant IE in this file already uses).
    std::size_t off = 4;
    if (off + 2 > plain_inner.size())
        return std::nullopt;
    const std::size_t tmsi_ie_len =
        (static_cast<std::size_t>(plain_inner[off]) << 8) | plain_inner[off + 1];
    off += 2;
    // Real bug found and fixed via this file's own unit test: the short TMSI-form value is 7
    // bytes (identity-type byte + amfSetId-high + packed amfSetId-low|amfPointer + 4-octet
    // 5G-TMSI = 1+1+1+4), confirmed against
    // simulators/ransim/vendor/UERANSIM/src/lib/nas/ie6.cpp's real `IE5gsMobileIdentity::Encode`
    // TMSI case -- an earlier version assumed 6, one short.
    if (tmsi_ie_len != 7 || off + tmsi_ie_len > plain_inner.size() ||
        plain_inner[off] != 0xF4 /* identity type: TMSI */) {
        return std::nullopt;
    }
    // Real bug found and fixed via this file's own unit test
    // (PeekServiceRequestTmsiExtractsTmsiWithoutAnyKey): off+0=identity-type byte,
    // off+1=amfSetId>>2, off+2=packed(amfSetId-low|amfPointer), off+3..off+6=the real 5G-TMSI --
    // an earlier version read off+2..off+5 (one byte too early, folding the packed
    // amfSetId/pointer byte into the TMSI's own high byte and silently dropping the real last
    // TMSI byte). Never reached this project's own real interop verification (UERANSIM's
    // RegistrationAccept-with-GUTI path only exercises the ENCODE side of this IE shape, not this
    // DECODE side -- exactly why this needed its own unit test, not just live interop).
    out.tmsi = (static_cast<std::uint32_t>(plain_inner[off + 3]) << 24) |
               (static_cast<std::uint32_t>(plain_inner[off + 4]) << 16) |
               (static_cast<std::uint32_t>(plain_inner[off + 5]) << 8) |
               static_cast<std::uint32_t>(plain_inner[off + 6]);
    off += tmsi_ie_len;
    out.offset_after_tmsi = off;
    return out;
}

} // namespace

std::optional<std::uint32_t> peek_service_request_tmsi(const std::vector<std::uint8_t>& p) {
    // Real, load-bearing property this function relies on (see this section's own header
    // comment): ServiceRequest's outer envelope is ALWAYS security-header-type 0x01
    // (integrity-protected, never ciphered, TS 24.501 §4.4.4.3), so wire_inner == plain_inner --
    // no decryption is needed (or possible without a key this function deliberately doesn't take)
    // to reach the real plaintext bytes below.
    if (p.size() < 7 || p[0] != kEpdMobilityManagement || p[1] != kShtIntegrityProtected)
        return std::nullopt;

    const std::vector<std::uint8_t> plain_inner(p.begin() + 7, p.end());
    const auto parsed = parse_service_request_inner(plain_inner);
    if (!parsed.has_value())
        return std::nullopt;
    return parsed->tmsi;
}

std::optional<ServiceRequestInfo> decode_service_request(const aka_crypto::NasIntKey& knas_int,
                                                         std::uint32_t uplink_count,
                                                         const std::vector<std::uint8_t>& p) {
    const auto result = decode_secured_uplink(
        knas_int, {}, kShtIntegrityProtected, /*ciphered=*/false, uplink_count, p);
    if (!result.has_value())
        return std::nullopt;

    ServiceRequestInfo out;
    out.mac_valid = result->mac_valid;
    if (!out.mac_valid)
        return out;

    const auto parsed = parse_service_request_inner(result->plain_inner);
    if (!parsed.has_value()) {
        out.mac_valid = false;
        return out;
    }
    out.ngksi = parsed->ngksi;
    out.service_type = parsed->service_type;

    // Optional IEs (TS 24.501 §8.2.20 / msg.cpp's own onBuild order: uplinkDataStatus IEI 0x40,
    // pduSessionStatus IEI 0x50, allowedPduSessionStatus IEI 0x25, nasMessageContainer IEI 0x71).
    // Only the two this project's real N2-PDU-session-resource-setup-on-ServiceRequest use needs
    // are decoded; allowedPduSessionStatus/nasMessageContainer are walked past if present, not
    // surfaced -- disclosed, not silently misparsed (a real, valid TLV is still skipped by its
    // own length, so a later IE isn't corrupted by an earlier one being ignored).
    const auto& plain_inner = result->plain_inner;
    std::size_t off = parsed->offset_after_tmsi;
    while (off < plain_inner.size()) {
        const std::uint8_t iei = plain_inner[off];
        if (iei == kIeiServiceUplinkDataStatus || iei == kIeiServicePduSessionStatus) {
            if (off + 2 > plain_inner.size())
                break;
            const std::uint8_t len = plain_inner[off + 1];
            if (len != 2 || off + 2 + len > plain_inner.size())
                break;
            const std::uint16_t bitmap = static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(plain_inner[off + 3]) << 8) | plain_inner[off + 2]);
            if (iei == kIeiServiceUplinkDataStatus)
                out.uplink_data_status = bitmap;
            else
                out.pdu_session_status = bitmap;
            off += 2 + len;
        } else if (off + 2 <= plain_inner.size()) {
            // Any other Type-4/6 IE this stage doesn't decode (allowedPduSessionStatus,
            // nasMessageContainer) -- skip by its own real length so parsing doesn't desync.
            const std::uint8_t len = plain_inner[off + 1];
            off += 2 + len;
        } else {
            break;
        }
    }

    return out;
}

std::vector<std::uint8_t> encode_service_accept(const aka_crypto::NasIntKey& knas_int,
                                                const aka_crypto::NasEncKey& knas_enc,
                                                std::uint32_t downlink_count,
                                                std::uint16_t pdu_session_status) {
    // Inner plaintext message (TS 24.501 §8.2.19): own header, no mandatory IEs (ServiceAccept
    // has none -- confirmed against
    // simulators/ransim/vendor/UERANSIM/src/lib/nas/msg.hpp's own `struct ServiceAccept` having
    // no non-optional members), then the real optional pduSessionStatus IE (Type-4, IEI 0x50,
    // same 2-octet bitmap layout ServiceRequest's own uplinkDataStatus/pduSessionStatus use --
    // confirmed against ie4.cpp's real IEPduSessionStatus::Encode).
    std::vector<std::uint8_t> inner;
    inner.push_back(kEpdMobilityManagement);
    inner.push_back(kSecurityHeaderNotProtected);
    inner.push_back(kMessageTypeServiceAccept);
    inner.push_back(kIeiServicePduSessionStatus);
    inner.push_back(0x02); // length
    inner.push_back(static_cast<std::uint8_t>(pdu_session_status & 0xFF));
    inner.push_back(static_cast<std::uint8_t>((pdu_session_status >> 8) & 0xFF));

    return encode_secured_downlink(knas_int,
                                   knas_enc,
                                   kShtIntegrityProtectedAndCiphered,
                                   /*ciphered=*/true,
                                   downlink_count,
                                   inner);
}

std::vector<std::uint8_t> encode_service_reject_plain(std::uint8_t mm_cause) {
    // Plain NAS message (TS 24.501 §8.2.21): own header, then the one mandatory IE, 5GMM cause
    // (Type-3, 1 octet, no IEI at mandatory position -- same convention every other mandatory
    // Type-3/4 IE in this file already uses).
    std::vector<std::uint8_t> out;
    out.push_back(kEpdMobilityManagement);
    out.push_back(kSecurityHeaderNotProtected);
    out.push_back(kMessageTypeServiceReject);
    out.push_back(mm_cause);
    return out;
}

std::optional<DeregistrationRequestOutcome>
decode_deregistration_request(const aka_crypto::NasIntKey& knas_int,
                              const aka_crypto::NasEncKey& knas_enc,
                              std::uint32_t uplink_count,
                              const std::vector<std::uint8_t>& p) {
    // Sent under the association's ordinary post-SecurityModeComplete protection -- same
    // "integrity protected and ciphered" envelope as UlNasTransport/RegistrationComplete, not the
    // ServiceRequest special case (that one is deliberately never ciphered -- see this file's own
    // ServiceRequest section header comment for why; a Deregistration always arrives on an
    // association whose identity is already known from UeAuthState, so no such chicken-and-egg
    // problem applies here).
    const auto result = decode_secured_uplink(
        knas_int, knas_enc, kShtIntegrityProtectedAndCiphered, /*ciphered=*/true, uplink_count, p);
    if (!result.has_value())
        return std::nullopt;

    DeregistrationRequestOutcome out;
    out.mac_valid = result->mac_valid;
    if (!out.mac_valid)
        return out;

    const auto& inner = result->plain_inner;
    // header(3) + ngKSI/deregistrationType packed byte(1) + mobileIdentity length(2) = 6 bytes
    // minimum, even for a zero-length mobile identity (which never happens in practice, but the
    // length field itself is always present).
    if (inner.size() < 6 || inner[0] != kEpdMobilityManagement ||
        inner[2] != kMessageTypeDeregistrationRequestUeOriginating) {
        out.mac_valid = false; // not a message this function decodes
        return out;
    }

    // mandatoryIE1(&ngKSI, &deRegistrationType) packs ngKSI into the high nibble and
    // deRegistrationType into the low nibble (confirmed against decode_registration_request's own
    // analogous byte, ngKSI-high/registrationType-low -- the same packing convention, not a
    // separate guess). deRegistrationType's own value is itself Bmp4Enc112(switchOff,
    // reRegistrationRequired, accessType): switchOff is bit3 of that nibble (bit3 of the whole
    // byte), matching ie1.cpp's Bmp4Dec112(val, &switchOff, &reRegistrationRequired, &accessType)
    // -- reRegistrationRequired (bit2) and accessType (bits0-1) are not read; this build has
    // exactly one AMF, one gNB and one access type in scope (ADR-0031), so nothing branches on
    // them.
    out.switch_off = (inner[3] & 0b1000) != 0;

    // 5GS Mobile Identity (TS 24.501 §9.11.3.4): Type-6 IE, 2-octet big-endian length, then value
    // -- walked past by its own length field, not decoded (see this function's own declaration
    // comment in nas_codec.hpp for why no caller needs the result).
    const std::size_t id_len = (static_cast<std::size_t>(inner[4]) << 8) | inner[5];
    if (6 + id_len > inner.size()) {
        out.mac_valid = false;
        return out;
    }

    return out;
}

std::vector<std::uint8_t> encode_deregistration_accept(const aka_crypto::NasIntKey& knas_int,
                                                       const aka_crypto::NasEncKey& knas_enc,
                                                       std::uint32_t downlink_count) {
    // Inner plaintext message (TS 24.501 §8.2.11): header and message type only -- confirmed
    // against simulators/ransim/vendor/UERANSIM/src/lib/nas/msg.cpp's own
    // DeRegistrationAcceptUeOriginating::onBuild, an EMPTY body (no IEs at all, mandatory or
    // optional).
    const std::vector<std::uint8_t> inner{kEpdMobilityManagement,
                                          kSecurityHeaderNotProtected,
                                          kMessageTypeDeregistrationAcceptUeOriginating};
    return encode_secured_downlink(knas_int,
                                   knas_enc,
                                   kShtIntegrityProtectedAndCiphered,
                                   /*ciphered=*/true,
                                   downlink_count,
                                   inner);
}

} // namespace amf::nas
