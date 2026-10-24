#include "nas_5gsm_codec.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace smf::nas5gsm {

namespace {

// TS 24.501 §9.7 Table 9.7.1: Extended Protocol Discriminator for 5GSM messages. Confirmed against
// UERANSIM's EExtendedProtocolDiscriminator::SESSION_MANAGEMENT_MESSAGES (enums.hpp).
constexpr std::uint8_t kEpdSessionManagement = 0x2E;

constexpr std::uint8_t kMessageTypeEstablishmentRequest = 0xC1;
constexpr std::uint8_t kMessageTypeEstablishmentAccept = 0xC2;
// ADR-0279. 0xC3, from the same real source this file's other message types cite --
// simulators/ransim/vendor/UERANSIM/src/lib/nas/enums.hpp's
// EMessageType::PDU_SESSION_ESTABLISHMENT_REJECT (0b11000011).
constexpr std::uint8_t kMessageTypeEstablishmentReject = 0xC3;

// TS 24.501 §9.11.4.11 PDU session type values.
constexpr std::uint8_t kPduSessionTypeIpv4 = 0b001;
// TS 24.501 §9.11.4.16 SSC mode values.
constexpr std::uint8_t kSscMode1 = 0b001;

// TS 24.501 §9.11.4.14 Table 9.11.4.14.1, Unit for Session-AMBR values.
enum class SessionAmbrUnit : std::uint8_t {
    kValueNotUsed = 0x00,
    kMult1Kbps = 0x01,
    kMult4Kbps = 0x02,
    kMult16Kbps = 0x03,
    kMult64Kbps = 0x04,
    kMult256Kbps = 0x05,
    kMult1Mbps = 0x06,
    kMult4Mbps = 0x07,
    kMult16Mbps = 0x08,
    kMult64Mbps = 0x09,
    kMult256Mbps = 0x0A,
    kMult1Gbps = 0x0B,
    kMult4Gbps = 0x0C,
    kMult16Gbps = 0x0D,
    kMult64Gbps = 0x0E,
    kMult256Gbps = 0x0F,
    kMult1Tbps = 0x10,
};

// Parses a TS 29.571 BitRate string ("<number> <unit>", unit one of bps/Kbps/Mbps/Gbps/Tbps) into
// TS 24.501 §9.11.4.14's unit+2-octet-value encoding. Only whole-number values in the units
// UERANSIM's own EUnitForSessionAmbr enum supports are handled for real; anything else (fractional
// values, an unrecognized unit, a value too large for 2 octets) falls back to 1 Mbps and reports
// false -- disclosed, not silently wrong.
struct ParsedAmbr {
    SessionAmbrUnit unit;
    std::uint16_t value;
};

ParsedAmbr parse_bitrate(const std::string& bitrate, bool& parsed_for_real) {
    double number = 0.0;
    char unit_buf[16] = {};
    if (std::sscanf(bitrate.c_str(), "%lf %15s", &number, unit_buf) == 2 &&
        number == static_cast<double>(static_cast<std::uint64_t>(number)) && number >= 1.0 &&
        number <= 65535.0) {
        const std::string unit = unit_buf;
        SessionAmbrUnit u;
        if (unit == "Kbps") {
            u = SessionAmbrUnit::kMult1Kbps;
        } else if (unit == "Mbps") {
            u = SessionAmbrUnit::kMult1Mbps;
        } else if (unit == "Gbps") {
            u = SessionAmbrUnit::kMult1Gbps;
        } else if (unit == "Tbps") {
            u = SessionAmbrUnit::kMult1Tbps;
        } else {
            parsed_for_real = false;
            return {SessionAmbrUnit::kMult1Mbps, 1};
        }
        parsed_for_real = true;
        return {u, static_cast<std::uint16_t>(number)};
    }
    parsed_for_real = false;
    return {SessionAmbrUnit::kMult1Mbps, 1};
}

} // namespace

std::optional<EstablishmentRequestInfo>
decode_establishment_request(const std::vector<std::uint8_t>& p) {
    if (p.size() < 4 || p[0] != kEpdSessionManagement || p[3] != kMessageTypeEstablishmentRequest) {
        return std::nullopt;
    }
    EstablishmentRequestInfo out;
    out.pdu_session_id = p[1];
    out.pti = p[2];
    return out;
}

std::vector<std::uint8_t> encode_establishment_accept(std::uint8_t pdu_session_id,
                                                      std::uint8_t pti,
                                                      const std::string& session_ambr_uplink,
                                                      const std::string& session_ambr_downlink,
                                                      std::uint8_t qfi) {
    std::vector<std::uint8_t> out;
    out.push_back(kEpdSessionManagement);
    out.push_back(pdu_session_id);
    out.push_back(pti);
    out.push_back(kMessageTypeEstablishmentAccept);

    // selectedSscMode (high nibble) | selectedPduSessionType (low nibble) -- TS 24.501 §8.3.5,
    // Type-1 packed IE, confirmed against PduSessionEstablishmentAccept::onBuild's
    // mandatoryIE1(&selectedSscMode, &selectedPduSessionType).
    out.push_back(static_cast<std::uint8_t>((kSscMode1 << 4) | kPduSessionTypeIpv4));

    // authorizedQoSRules (TS 24.501 §9.11.4.13): Type-6 IE, 2-octet length prefix, then one QoS
    // rule: rule identifier(1) + rule length(2) + [operation code=001 "create new QoS rule" (bits
    // 8-6) | DQR=1 (bit 5) | number of packet filters=0000 (bits 4-1)](1) + QoS rule precedence(1)
    // + [spare(1 bit)=0 | segregation(1 bit)=0 | QFI(6 bits)](1). Zero packet filters is only
    // spec-valid for the default rule (DQR=1), which this is -- not an arbitrary shortcut.
    constexpr std::uint8_t kQosRuleId = 1;
    constexpr std::uint8_t kOpCodeCreateDqrNoFilters = 0b001'1'0000;
    constexpr std::uint8_t kPrecedence = 1;
    const std::uint8_t qfi_byte = static_cast<std::uint8_t>(qfi & 0x3F);
    const std::vector<std::uint8_t> qos_rule = {
        kQosRuleId, 0x00, 0x03, kOpCodeCreateDqrNoFilters, kPrecedence, qfi_byte};
    const std::uint16_t qos_rules_len = static_cast<std::uint16_t>(qos_rule.size());
    out.push_back(static_cast<std::uint8_t>(qos_rules_len >> 8));
    out.push_back(static_cast<std::uint8_t>(qos_rules_len & 0xFF));
    out.insert(out.end(), qos_rule.begin(), qos_rule.end());

    // sessionAmbr (TS 24.501 §9.11.4.14): Type-4 IE, 1-octet length prefix (always 6), then
    // unit+value for downlink then uplink -- confirmed against IESessionAmbr::Encode's field
    // order (unitForSessionAmbrForDownlink, sessionAmbrForDownlink, unitForSessionAmbrForUplink,
    // sessionAmbrForUplink).
    bool dl_parsed = false;
    bool ul_parsed = false;
    const auto dl = parse_bitrate(session_ambr_downlink, dl_parsed);
    const auto ul = parse_bitrate(session_ambr_uplink, ul_parsed);
    out.push_back(0x06); // length
    out.push_back(static_cast<std::uint8_t>(dl.unit));
    out.push_back(static_cast<std::uint8_t>(dl.value >> 8));
    out.push_back(static_cast<std::uint8_t>(dl.value & 0xFF));
    out.push_back(static_cast<std::uint8_t>(ul.unit));
    out.push_back(static_cast<std::uint8_t>(ul.value >> 8));
    out.push_back(static_cast<std::uint8_t>(ul.value & 0xFF));

    return out;
}

std::vector<std::uint8_t>
encode_establishment_reject(std::uint8_t pdu_session_id, std::uint8_t pti, std::uint8_t sm_cause) {
    // TS 24.501 §8.3.6. The 5GSM header is the same four bytes every message in this file uses
    // (EPD, PDU session ID, PTI, message type), then the ONE mandatory IE: 5GSM cause, a Type-3
    // single octet with no length prefix.
    //
    // The PTI is echoed from the request, exactly as the Accept echoes it -- a UE matches the
    // answer to its own request by that value, so a reject that dropped it would be unmatchable.
    std::vector<std::uint8_t> out;
    out.push_back(kEpdSessionManagement);
    out.push_back(pdu_session_id);
    out.push_back(pti);
    out.push_back(kMessageTypeEstablishmentReject);
    out.push_back(sm_cause);
    return out;
}

std::optional<SmHeader> decode_sm_header(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 4 || bytes[0] != kEpdSessionManagement) {
        return std::nullopt;
    }
    SmHeader h;
    h.pdu_session_id = bytes[1];
    h.pti = bytes[2];
    h.message_type = bytes[3];
    // TS 24.501 8.3.12.1.1 / 8.3.15.1.1: the 5GSM cause is the first optional IE (IEI 0x59, TV,
    // two octets) of RELEASE REQUEST and RELEASE COMPLETE.
    constexpr std::uint8_t kIei5gsmCause = 0x59;
    if (bytes.size() >= 6 && bytes[4] == kIei5gsmCause) {
        h.sm_cause = bytes[5];
    }
    return h;
}

std::vector<std::uint8_t>
encode_release_command(std::uint8_t pdu_session_id, std::uint8_t pti, std::uint8_t sm_cause) {
    return {kEpdSessionManagement,
            pdu_session_id,
            pti,
            static_cast<std::uint8_t>(SmMessage::kReleaseCommand),
            sm_cause};
}

} // namespace smf::nas5gsm
