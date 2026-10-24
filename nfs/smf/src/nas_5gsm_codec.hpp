#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Minimal TS 24.501 5GSM (Session Management) NAS codec, SMF's side of the PDU Session
// Establishment procedure (TS 23.502 §4.3.2.2.1 steps 5-11). SMF is the only NF in this project
// that decodes/encodes real 5GSM content -- AMF deliberately stays opaque to it (see
// nfs/amf/src/nas_codec.hpp's decode_ul_nas_transport comment) and only routes the raw bytes
// between the UE and here, via SmContextCreateData.n1SmMsg (request direction) and
// Namf_Communication's N1N2MessageTransfer (response direction, TS29518_Namf_Communication.yaml).
//
// Byte layouts confirmed against UERANSIM's real, independent implementation -- the same
// arms-length reference-oracle methodology as every other NAS codec in this project (ADR-0016):
// simulators/ransim/vendor/UERANSIM/src/lib/nas/msg.cpp (PduSessionEstablishmentRequest::onBuild/
// PduSessionEstablishmentAccept::onBuild, EncodeSm's header field order), src/lib/nas/base.hpp
// (Type-1/3/4/6 IE encoding rules), src/lib/nas/enums.hpp (EPD/message type/PDU session type/SSC
// mode/session-AMBR unit values), src/lib/nas/ie6.cpp (confirms IEQoSRules is treated as an
// opaque octet string by UERANSIM -- its internal structure is never validated by the peer this
// project interops with, though this codec still encodes a genuinely spec-shaped rule, not
// arbitrary bytes, per CLAUDE.md's non-fabrication rule).
//
// See docs/DECISIONS.md ADR-0038.

namespace smf::nas5gsm {

struct EstablishmentRequestInfo {
    std::uint8_t pdu_session_id = 0;
    std::uint8_t pti = 0; // Procedure Transaction Identity, TS 24.501 §9.6 -- must be echoed back
                          // in the Accept.
};

// Decodes only the 5GSM message header (EPD, PDU session ID, PTI, message type) of a PDU Session
// Establishment Request (TS 24.501 §8.3.1) -- NOT the full IE list (pduSessionType/sscMode/
// smCapability/...). Disclosed, deliberate scope: this build always responds with a fixed IPv4/
// SSC-mode-1 Accept matching UERANSIM's own hardcoded request content (its sendEstablishmentRequest
// unconditionally rejects any config.type != IPV4 before even building the message -- see
// establishment.cpp), so nothing else in the request currently affects what SMF sends back.
// std::nullopt if the bytes aren't shaped like a PDU Session Establishment Request.
std::optional<EstablishmentRequestInfo>
decode_establishment_request(const std::vector<std::uint8_t>& p);

// Encodes a real TS 24.501 §8.3.5 PDU Session Establishment Accept: PDU session type IPv4, SSC
// mode 1 (this build's only supported combination, matching the request), one QoS rule, and
// session-AMBR.
//
// The QoS rule is minimal but genuinely spec-valid, not an arbitrary shortcut: TS 24.501
// §9.11.4.13 permits (and requires, for the default rule) zero packet filters when the rule is
// the QoS rule associated with the default QoS rule (DQR bit = 1) -- this is that rule, operation
// code "create new QoS rule", precedence 1, no packet filter list.
//
// session_ambr_uplink/downlink: TS 29.571 BitRate strings (e.g. "1 Gbps") as returned by PCF's
// real SmPolicyDecision.sessRules[...].authSessAmbr -- NOT fabricated here, parsed from what PCF
// actually decided. Only the units UERANSIM's own EUnitForSessionAmbr enum supports (bps through
// Gbps, powers of 4/1000 per TS 24.501 §9.11.4.14 Table 9.11.4.14.1) are parsed; an unparseable
// string falls back to 1 Mbps, disclosed via the returned bool (true = parsed for real).
// qfi: TS 24.501 §9.11.4.13's QoS Flow Identifier (6 bits, 0-63) -- this build derives it from
// PCF's real AuthorizedDefaultQos.n5qi (5QI), NOT a fabricated value, though a real network would
// allocate QFI via separate QoS flow binding rather than reusing the 5QI numerically -- disclosed
// simplification, no QoS flow binding subsystem exists in this project.
std::vector<std::uint8_t> encode_establishment_accept(std::uint8_t pdu_session_id,
                                                      std::uint8_t pti,
                                                      const std::string& session_ambr_uplink,
                                                      const std::string& session_ambr_downlink,
                                                      std::uint8_t qfi);

// Encodes a real TS 24.501 §8.3.6 PDU Session Establishment Reject (ADR-0279) -- what a UE is
// owed when its session cannot be established, in place of the Accept.
//
// sm_cause: TS 24.501 §9.11.4.2 5GSM cause. The slice-admission case is 0x45
// (INSUFFICIENT_RESOURCES_FOR_SPECIFIC_SLICE), read from
// simulators/ransim/vendor/UERANSIM/src/lib/nas/enums.hpp's ESmCause, the same real source this
// file's message types come from.
//
// Unsecured by construction, like the Accept: a 5GSM message is carried inside a 5GMM
// DlNasTransport, and it is that OUTER message AMF protects with the NAS security context. This
// container is the payload, not a NAS message on the wire by itself.
std::vector<std::uint8_t>
encode_establishment_reject(std::uint8_t pdu_session_id, std::uint8_t pti, std::uint8_t sm_cause);

// ADR-0523: the UE- and network-requested PDU session release messages (TS 24.501 6.3.3, 6.4.3).
// Message types from TS 24.501 table 9.7.2: RELEASE REQUEST 0xD1, RELEASE COMMAND 0xD3, RELEASE
// COMPLETE 0xD4.
enum class SmMessage : std::uint8_t {
    kReleaseRequest = 0xD1,
    kReleaseCommand = 0xD3,
    kReleaseComplete = 0xD4,
};

// The 5GSM header (EPD, PDU session ID, PTI, message type) of a received message, plus the
// optional 5GSM cause (IEI 0x59, TV) when present. std::nullopt if the bytes are not a 5GSM
// message. Only the header and the cause are read; Extended PCO is ignored.
struct SmHeader {
    std::uint8_t pdu_session_id = 0;
    std::uint8_t pti = 0;
    std::uint8_t message_type = 0;
    std::optional<std::uint8_t> sm_cause;
};
std::optional<SmHeader> decode_sm_header(const std::vector<std::uint8_t>& bytes);

// TS 24.501 8.3.14 PDU SESSION RELEASE COMMAND with its one mandatory IE, the 5GSM cause (V).
// `pti` is the RELEASE REQUEST's PTI when the release answers one, else 0 ("No procedure
// transaction identity assigned", TS 24.501 6.3.3.2). No optional IE is sent (no back-off timer,
// no Access type: single-access sessions only).
std::vector<std::uint8_t>
encode_release_command(std::uint8_t pdu_session_id, std::uint8_t pti, std::uint8_t sm_cause);

} // namespace smf::nas5gsm
