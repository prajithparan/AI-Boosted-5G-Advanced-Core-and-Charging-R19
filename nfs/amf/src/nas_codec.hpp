#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "aka_crypto/kdf.hpp"

// Minimal NAS-5GS (TS 24.501) codec -- Stage 2+ of docs/DECISIONS.md's staged NGAP/NAS plan.
// Hand-rolled: NAS-5GS is TLV-encoded, not ASN.1, so no codegen tool applies the way NGAP's PER
// codec does (see ADR-0031 for why "hand-rolled partial parsing" was explicitly rejected for
// NGAP -- that concern doesn't transfer here, since there's no asn1c-equivalent for NAS-5GS to
// generate from in the first place; every real 5GC's NAS layer is hand-written for the same
// reason). Byte layouts below were derived by reading
// simulators/ransim/vendor/UERANSIM/src/lib/nas as a read-only reference oracle (arms-length per
// ADR-0016/ADR-0031 -- never copied, only read to confirm the real wire format against TS 24.501/
// TS 24.007's well-known message structures). Scope is exactly what this build's staged plan
// needs -- RegistrationRequest decode (SUCI extraction only, null protection scheme),
// AuthenticationRequest encode, AuthenticationResponse/AuthenticationFailure decode,
// SecurityModeCommand encode, and SecurityModeComplete decode -- not a general NAS parser.

namespace amf::nas {

struct RegistrationRequestInfo {
    // "imsi-<mcc><mnc><msin>", reconstructed directly from a null-protection-scheme SUCI's
    // plaintext scheme output -- see decode_registration_request's own comment for why this is a
    // faithful reconstruction, not a fabrication, and why it's the correct string to pass to
    // AUSF's supiOrSuci today.
    std::string supi;
    // Raw TLV *value* bytes (NOT including the 0x2E IEI byte) of the UE Security Capability IE
    // (TS 24.501 §9.11.3.54), captured verbatim so Stage 4's SecurityModeCommand can replay it
    // unmodified -- the UE rejects SMC with UE_SECURITY_CAP_MISMATCH otherwise (TS 24.501 §5.4.2,
    // the anti-bidding-down-attack check), confirmed against
    // simulators/ransim/vendor/UERANSIM/src/ue/nas/mm/security.cpp's own
    // `DeepEqualsIe(msg.replayedUeSecurityCapabilities, createSecurityCapabilityIe())`. This
    // project doesn't need to interpret the bitmap (which algorithms the UE supports) since it
    // always selects the same fixed pair (128-NEA2/128-NIA2, see aka_crypto/nas_security.hpp) --
    // byte-exact replay is all TS 24.501 requires here. Empty if the IE wasn't present (out of
    // scope: no real UE config in this project ever omits it, see
    // simulators/ransim/vendor/UERANSIM/src/ue/nas/mm/register.cpp's unconditional
    // `request->ueSecurityCapability = createSecurityCapabilityIe()`).
    std::vector<std::uint8_t> ue_security_capability;
};

// Returns std::nullopt (the caller logs why) if the NAS-PDU isn't a plain, unprotected
// RegistrationRequest carrying a null-protection-scheme SUCI encoding an IMSI-format SUPI --
// every other case (ciphered NAS, GUTI-based registration, a real protection scheme) is
// explicitly out of scope for this stage, not silently misparsed.
std::optional<RegistrationRequestInfo>
decode_registration_request(const std::vector<std::uint8_t>& nas_pdu);

// ngksi: 0-6, a fresh key set identifier this AMF is allocating for this authentication run (no
// prior security context exists yet, so any fresh value is correct; this lab's single-UE-at-a-time
// scope, see ADR-0031, makes a fixed value 0 unambiguous -- disclosed, not a spec-mandated value).
std::vector<std::uint8_t> encode_authentication_request(const std::array<std::uint8_t, 16>& rand,
                                                        const std::array<std::uint8_t, 16>& autn,
                                                        int ngksi);

// Result of decoding a NAS-PDU carried in UplinkNASTransport in response to a Stage 2
// AuthenticationRequest -- TS 24.501 defines exactly two real outcomes here (ignoring EAP, out of
// scope), AuthenticationResponse (success, carries RES*) or AuthenticationFailure (carries a
// cause and, for a SQN synchronization failure specifically, AUTS). Both are decoded here --
// not just the success path -- because a real UE talking to this project's currently-seeded test
// subscriber (see docs/DECISIONS.md ADR-0032) *always* hits the failure path on first contact
// (UDM's seeded SQN is TS 35.207 Test Set 1's fixed, large value, which legitimately exceeds any
// fresh UE's SQN-MS=0), so decoding it correctly (not crashing, not misparsing) matters in
// practice, not just in theory.
struct AuthenticationOutcome {
    bool success = false;
    // Valid iff success. TS 24.501 §9.11.3.17 AuthenticationResponseParameter -- RES*, 16 octets.
    std::array<std::uint8_t, 16> res_star{};
    // Valid iff !success. TS 24.501 §9.11.3.28 5GMM cause -- e.g. 0x14 MAC_FAILURE,
    // 0x15 SYNCH_FAILURE (numeric values confirmed against
    // simulators/ransim/vendor/UERANSIM/src/lib/nas/enums.hpp's EMmCause, not guessed).
    std::uint8_t mm_cause = 0;
    // Valid iff !success and the UE included one (TS 24.501 §9.11.3.1
    // AuthenticationFailureParameter) -- present for SYNCH_FAILURE, absent otherwise. 14 octets:
    // SQN xor AK (6) || MAC-S (8), confirmed against
    // simulators/ransim/vendor/UERANSIM/src/ue/nas/keys.cpp's CalculateAuts, not assumed from
    // general AKA knowledge alone.
    //
    // STALE COMMENT CORRECTED (ADR-0265): this used to read "Resynchronization ... is NOT
    // implemented -- disclosed gap, see ADR-0032". That stopped being true at ADR-0037, which
    // implemented AMF-side SQN resynchronisation (see ngap_task.cpp's own `sqn_resync_attempted`
    // and the `resync_info` it passes to AUSF). This field is decoded, surfaced, and now really
    // acted on. Corrected rather than left to justify current behaviour with an obsolete reason --
    // the same discipline ADR-0250/0256/0257/0258 applied elsewhere.
    std::optional<std::array<std::uint8_t, 14>> auts;
};

// Returns std::nullopt (the caller logs why) for anything other than a plain, unprotected
// AuthenticationResponse carrying authenticationResponseParameter (RES*) or a plain
// AuthenticationFailure -- e.g. an EAP-only AuthenticationResponse is out of scope (this project
// only implements the 5G-AKA path, not EAP-AKA' end-to-end over NAS).
std::optional<AuthenticationOutcome>
decode_authentication_outcome(const std::vector<std::uint8_t>& nas_pdu);

// Encodes a SecurityModeCommand (TS 24.501 §8.2.25, network->UE), integrity-protected only (never
// ciphered -- the UE cannot yet be assumed to trust the new KNASenc when it first receives this
// message, confirmed against
// simulators/ransim/vendor/UERANSIM/src/ue/nas/enc.cpp's own MakeSecurityHeaderType always
// returning INTEGRITY_PROTECTED_WITH_NEW_SECURITY_CONTEXT for this message type regardless of the
// selected ciphering algorithm), secured with 128-NIA2 over knas_int. Only the mandatory IEs this
// stage's single-UE, single-algorithm-pair happy path needs are encoded --
// selectedNasSecurityAlgorithms (fixed to 128-NEA2/128-NIA2, this project's only implemented pair),
// a fresh ngKSI=0, and ue_security_capability replayed verbatim (see RegistrationRequestInfo's own
// comment). Optional IEs (imeiSvRequest, EPS algorithms, additional5GSecurityInformation,
// eapMessage, abba, replayedS1UeNetworkCapability) are out of scope -- disclosed simplification,
// not silently dropped support for a case this build actually hits.
//
// downlink_count: this association's NAS downlink COUNT for this message. This project's
// single-registration-per-association scope (ADR-0031) means this is always the first secured
// downlink message, so callers always pass 0 -- kept as an explicit parameter (not hardcoded)
// so a future multi-message-per-association stage doesn't need a signature change.
std::vector<std::uint8_t>
encode_security_mode_command(const aka_crypto::NasIntKey& knas_int,
                             const std::vector<std::uint8_t>& ue_security_capability,
                             std::uint32_t downlink_count);

struct SecurityModeCompleteOutcome {
    // False means the MAC genuinely didn't verify (wrong keys, a tampered/replayed message, or a
    // NAS COUNT desync) -- a real, caller-visible outcome, not a decode failure. A decode failure
    // (message too short/wrong EPD/wrong security header type to even be a candidate) is instead
    // std::nullopt from decode_security_mode_complete itself, so the two are never conflated.
    bool mac_valid = false;
};

// Verifies MAC (128-NIA2) and deciphers (128-NEA2) a NAS-PDU carried in UplinkNASTransport,
// expected to be a SecurityModeComplete (TS 24.501 §8.2.26) responding to
// encode_security_mode_command's output. uplink_count mirrors downlink_count's own doc comment --
// always 0 in this project's current scope.
std::optional<SecurityModeCompleteOutcome>
decode_security_mode_complete(const aka_crypto::NasIntKey& knas_int,
                              const aka_crypto::NasEncKey& knas_enc,
                              std::uint32_t uplink_count,
                              const std::vector<std::uint8_t>& nas_pdu);

// Encodes a RegistrationAccept (TS 24.501 §8.2.7, network->UE), integrity-protected AND ciphered
// (the normal secured-message case, not SecurityModeCommand's "new security context" variant --
// by this point the NAS security context is established and confirmed, not being bootstrapped).
// Encodes the one mandatory IE (registrationResult) plus, as of the ServiceRequest gap-closure
// (docs/CAPABILITY_GAP_ANALYSIS.md task #100/ADR-0075), the real optional 5G-GUTI IE (TS 24.501
// §9.11.3.4, IEI 0x77, confirmed against
// simulators/ransim/vendor/UERANSIM/src/lib/nas/msg.cpp's own RegistrationAccept::onBuild) --
// without a GUTI, a UE has no TMSI to identify itself with on a later ServiceRequest, so this was
// a real, load-bearing prerequisite, not an independent nice-to-have. Still-disclosed IEs left
// out: allowed NSSAI, TAI list, network feature support, etc. -- out of this project's scope.
//
// tmsi: the fresh 5G-TMSI this AMF is assigning (allocated by the caller, e.g. via a Redis INCR --
// see ngap_task.cpp -- so it's unique across this AMF instance's own lifetime, matching
// ChargingDataStore's own real ID-allocation precedent). amf_region_id/amf_set_id/amf_pointer:
// this AMF instance's own real, disclosed lab identity (TS 24.501 §9.11.3.4's own GUTI structure
// needs all three to let a UE's later ServiceRequest be routed back to the AMF that issued the
// GUTI -- this project's own single-AMF-instance lab scope means these are fixed, disclosed
// constants, not a real multi-AMF-set deployment's dynamically-assigned values).
//
// downlink_count: this association's second secured downlink message (the first was
// SecurityModeCommand's downlink_count=0) -- callers always pass 1 in this project's current
// single-registration-per-association scope, kept explicit for the same reason
// encode_security_mode_command's downlink_count parameter is.
std::vector<std::uint8_t> encode_registration_accept(const aka_crypto::NasIntKey& knas_int,
                                                     const aka_crypto::NasEncKey& knas_enc,
                                                     std::uint32_t downlink_count,
                                                     std::uint32_t tmsi,
                                                     std::uint8_t amf_region_id,
                                                     std::uint16_t amf_set_id,
                                                     std::uint8_t amf_pointer);

// RegistrationReject (TS 24.501 §8.2.8) -- ADR-0278, sent when Network Slice Admission Control
// refuses the UE's slice. SECURED, because this is only reachable after SecurityModeComplete has
// succeeded and a NAS security context therefore exists (contrast encode_service_reject_plain,
// whose plain form exists precisely for the case where there is none).
//
// mm_cause: TS 24.501 §9.11.3.2 5GMM cause. The NSAC case is 0x45
// INSUFFICIENT_RESOURCES_FOR_SLICE, read from
// simulators/ransim/vendor/UERANSIM/src/lib/nas/enums.hpp's EMmCause, the same real source this
// file's other cause values cite.
//
// downlink_count: 1 -- it replaces RegistrationAccept, which is this association's second secured
// downlink message (SecurityModeCommand was 0). A rejected UE never receives both.
std::vector<std::uint8_t> encode_registration_reject(const aka_crypto::NasIntKey& knas_int,
                                                     const aka_crypto::NasEncKey& knas_enc,
                                                     std::uint32_t downlink_count,
                                                     std::uint8_t mm_cause);

struct RegistrationCompleteOutcome {
    // Same split as SecurityModeCompleteOutcome::mac_valid -- see that struct's own comment.
    bool mac_valid = false;
};

// Verifies MAC and deciphers a NAS-PDU carried in UplinkNASTransport, expected to be a
// RegistrationComplete (TS 24.501 §8.2.6) responding to encode_registration_accept's output --
// the final message of the registration procedure this project's staged NGAP/NAS plan targets.
// uplink_count: this association's second secured uplink message (the first was
// SecurityModeComplete's uplink_count=0) -- callers always pass 1 in this project's current scope.
//
// CORRECTED (ADR-0267): this comment used to say "NOT CURRENTLY CALLED ... unreachable in
// practice", on the reasoning that a real UE only sends RegistrationComplete if
// RegistrationAccept carried a 5G-GUTI, an NSSCI=CHANGED indication, or a configuredNSSAI
// (UERANSIM's own receiveInitialRegistrationAccept,
// simulators/ransim/vendor/UERANSIM/src/ue/nas/mm/register.cpp:346-426) -- and that
// encode_registration_accept sends none of those. The second half stopped being true when
// ADR-0075 added a real 5G-GUTI to encode_registration_accept, so a real UE now genuinely owes a
// RegistrationComplete. It is called: handle_uplink_nas_transport_registration_complete in
// nfs/amf/src/ngap_task.cpp, on the AwaitingRegistrationComplete phase, and ADR-0267's test drives
// it end to end.
std::optional<RegistrationCompleteOutcome>
decode_registration_complete(const aka_crypto::NasIntKey& knas_int,
                             const aka_crypto::NasEncKey& knas_enc,
                             std::uint32_t uplink_count,
                             const std::vector<std::uint8_t>& nas_pdu);

struct UlNasTransportInfo {
    // Same split as SecurityModeCompleteOutcome::mac_valid -- see that struct's own comment.
    bool mac_valid = false;
    // Valid iff mac_valid. TS 24.501 §9.11.3.41 5GS PDU Session Identity 2. 0 (never a valid
    // assigned value, TS 24.501 range is 1-15) means the UE didn't include it -- out of scope:
    // this project's only implemented SM procedure (PDU Session Establishment) always includes
    // it, per simulators/ransim/vendor/UERANSIM/src/ue/nas/sm/transport.cpp's own unconditional
    // `ulTransport.pduSessionId = ...`.
    std::uint8_t pdu_session_id = 0;
    // TS 24.501 §9.11.2.1a (DNN), decoded from TS 23.003 §9.1's label-length-prefixed APN
    // encoding back into a dotted string (e.g. "internet"), confirmed against
    // simulators/ransim/vendor/UERANSIM/src/lib/nas/utils.cpp's own DnnFromApn (the exact inverse
    // operation). std::nullopt if the UE didn't include it.
    std::optional<std::string> dnn;
    // TS 24.501 §9.11.2.8 (S-NSSAI) -- only the SST(+SD) case is decoded (this project's only
    // configured S-NSSAI shape, sst=1/sd=1, see simulators/ransim/config/gnb.yaml); the
    // mapped-HPLMN-S-NSSAI length variants (TS 24.501's own IESNssai::Decode has five, this
    // decodes one) are out of scope, disclosed rather than silently misparsed -- an S-NSSAI with
    // a different length just leaves these std::nullopt.
    std::optional<std::uint8_t> snssai_sst;
    std::optional<std::array<std::uint8_t, 3>> snssai_sd;
    // The opaque 5GSM payload container bytes (the PDU Session Establishment Request itself), NOT
    // decoded here (see this function's own comment for why) but captured verbatim so the caller
    // can forward them to SMF as SmContextCreateData.n1SmMsg, TS 29.502's real mechanism for
    // getting the actual 5GSM content to the NF that's allowed to understand it. Added by
    // docs/DECISIONS.md ADR-0038 -- previously this project discarded these bytes entirely.
    std::vector<std::uint8_t> payload_container;
};

// Verifies MAC and deciphers a NAS-PDU carried in UplinkNASTransport, expected to be a
// UlNasTransport (TS 24.501 §8.2.10) wrapping N1 SM information (payloadContainerType=1) -- this
// project's only implemented post-registration NAS procedure, PDU Session Establishment
// (TS 23.502 §4.3.2.2.1). The actual 5GSM payload container bytes (the PDU Session Establishment
// Request itself) are NOT decoded here -- TS 24.501's whole point of the payload-container
// mechanism is that AMF routes SM messages without understanding their contents; only SMF decodes
// real 5GSM content. The container bytes are still captured verbatim (UlNasTransportInfo::
// payload_container, ADR-0038) so the caller can forward them to SMF unmodified, matching the real
// mechanism: AMF stays opaque to the content, SMF is the only NF that parses it. The
// transport-level optional IEs that determine WHERE to route the request (PDU session ID, S-NSSAI,
// DNN) are additionally extracted; requestType/oldPduSessionId/additionalInformation are walked
// past (skipped) to reach them but not surfaced -- unused by SMF's current CreateSMContext handler.
//
// uplink_count: this association's THIRD secured uplink message -- SecurityModeComplete=0,
// RegistrationComplete=1, so this UlNasTransport is 2. CORRECTED (ADR-0267): this comment said
// "second ... no RegistrationComplete is ever sent ... callers pass 1", which was already
// contradicted by the caller (handle_uplink_nas_transport_pdu_session_establishment passes 2, and
// says why in its own comment) after ADR-0075 gave RegistrationAccept a real 5G-GUTI. The code was
// right and this comment was stale; ADR-0267's test drives a real UE through both counts.
std::optional<UlNasTransportInfo> decode_ul_nas_transport(const aka_crypto::NasIntKey& knas_int,
                                                          const aka_crypto::NasEncKey& knas_enc,
                                                          std::uint32_t uplink_count,
                                                          const std::vector<std::uint8_t>& nas_pdu);

// Encodes a secured DlNasTransport (TS 24.501 §8.2.9) wrapping opaque N1 SM information
// (payloadContainerType=1) -- AMF's delivery vehicle for the PDU Session Establishment Accept SMF
// builds and sends via Namf_Communication's N1N2MessageTransfer (ADR-0038). AMF does not decode
// n1_sm_container -- same opaque-payload-container discipline as decode_ul_nas_transport's own
// comment, just in the downlink direction. Byte layout confirmed against UERANSIM's real
// DlNasTransport::onBuild (simulators/ransim/vendor/UERANSIM/src/lib/nas/msg.cpp): mandatory
// payloadContainerType (Type-1) + payloadContainer (Type-6 LV-E) + optional pduSessionId
// (Type-3 TV, IEI 0x12 -- same IEI value UlNasTransport uses for the same IE).
//
// downlink_count: this association's second secured downlink message (SecurityModeCommand=0,
// RegistrationAccept=1) -- callers pass 2 in this project's current single-PDU-session scope.
std::vector<std::uint8_t> encode_dl_nas_transport(const aka_crypto::NasIntKey& knas_int,
                                                  const aka_crypto::NasEncKey& knas_enc,
                                                  std::uint32_t downlink_count,
                                                  std::uint8_t pdu_session_id,
                                                  const std::vector<std::uint8_t>& n1_sm_container);

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #100/ADR-0075): ServiceRequest (TS 24.501
// §8.2.20/§5.6.1), the real, dominant CM-IDLE->CM-CONNECTED NAS procedure this project never
// implemented. Real, load-bearing protocol property confirmed against
// simulators/ransim/vendor/UERANSIM/src/ue/nas/mm/... call sites and TS 24.501 §4.4.4.3:
// ServiceRequest is sent "integrity protected" but NEVER ciphered (security header type 0x01),
// specifically so the network can read the plaintext 5G-TMSI IT CARRIES to look up which UE's
// security context to verify the MAC against -- solving the real chicken-and-egg problem of
// "we don't know who this is until we decrypt it, but we can't decrypt it until we know who this
// is." This is why decode is split into two real steps below rather than one function taking a
// pre-known knas_int the way every other decode_* in this file does.

// Step 1: reads the 5G-TMSI directly from the message's own plaintext region (real per the
// non-ciphered property above -- this is not "peeking before decryption succeeds", it's reading
// bytes TS 24.501 itself never encrypts). Returns std::nullopt if the envelope doesn't even match
// ServiceRequest's own shape (wrong EPD/security-header-type/message-type) -- no MAC is checked
// yet, so this alone must never be treated as authenticating the UE.
std::optional<std::uint32_t> peek_service_request_tmsi(const std::vector<std::uint8_t>& nas_pdu);

struct ServiceRequestInfo {
    // False means the MAC genuinely didn't verify against the security context the caller looked
    // up using peek_service_request_tmsi's own result -- same real, caller-visible-outcome split
    // as SecurityModeCompleteOutcome::mac_valid. A wrong TMSI (no matching persisted context) is
    // the caller's own problem, handled before this function is even called.
    bool mac_valid = false;
    // TS 24.501 §9.11.3.32, the ngKSI the UE believes is current -- checked by the caller against
    // UeSecurityContext::ngksi (a real mismatch is TS 24.501's own security-context-desync case).
    std::uint8_t ngksi = 0;
    // TS 24.501 §9.11.3.50, real values 0=SIGNALLING/1=DATA/2=MOBILE_TERMINATED_SERVICES/
    // 3=EMERGENCY_SERVICES/4=EMERGENCY_SERVICES_FALLBACK/5=HIGH_PRIORITY_ACCESS/
    // 6=ELEVATED_SIGNALLING (confirmed against
    // simulators/ransim/vendor/UERANSIM/src/lib/nas/enums.hpp's EServiceType, not guessed).
    std::uint8_t service_type = 0;
    // TS 24.501 §9.11.3.56 Uplink data status -- a real bitmap of which PDU session IDs (1-15,
    // bit N-1 for PDU session N) have uplink data pending, the real signal that drives N2 PDU
    // session resource setup as part of this same procedure (TS 23.502 §4.2.3.2). Only present
    // (and only meaningfully non-empty) for serviceType=DATA. std::nullopt if the UE didn't
    // include the optional IE.
    std::optional<std::uint16_t> uplink_data_status;
    // TS 24.501 §9.11.3.44 PDU session status -- same real bitmap shape as uplink_data_status,
    // reporting which PDU sessions the UE itself still considers active (lets AMF/SMF detect and
    // clean up a session the UE already silently dropped). std::nullopt if absent.
    std::optional<std::uint16_t> pdu_session_status;
};

// Step 2: called once the caller has looked up a UeSecurityContext for peek_service_request_tmsi's
// own returned TMSI -- verifies the MAC (never deciphers, see this section's own header comment)
// using that context's knas_int and the NEXT expected uplink_count
// (UeSecurityContextStore::next_uplink_count's own return value for this same TMSI).
std::optional<ServiceRequestInfo> decode_service_request(const aka_crypto::NasIntKey& knas_int,
                                                         std::uint32_t uplink_count,
                                                         const std::vector<std::uint8_t>& nas_pdu);

// Encodes a ServiceAccept (TS 24.501 §8.2.19, network->UE) responding to a real ServiceRequest --
// integrity-protected AND ciphered (TS 24.501 §4.4.4.3: unlike the request itself, the network's
// response uses full protection since by this point the existing security context is already
// confirmed valid, the same "normal secured message" case RegistrationAccept uses). Only the
// mandatory-in-practice pduSessionStatus IE this project's real UE-context-cleanup use needs is
// encoded (TS 24.501 defines it optional; every other optional IE --
// pduSessionReactivationResult/ErrorCause, eapMessage -- is out of scope, disclosed, not silently
// dropped support for a case this build hits). pdu_session_status: the real, current bitmap AMF
// itself tracks (mirrors the UE's own IE shape), so the UE can reconcile.
std::vector<std::uint8_t> encode_service_accept(const aka_crypto::NasIntKey& knas_int,
                                                const aka_crypto::NasEncKey& knas_enc,
                                                std::uint32_t downlink_count,
                                                std::uint16_t pdu_session_status);

// Encodes a PLAIN (unprotected) ServiceReject (TS 24.501 §8.2.21, network->UE) -- real, disclosed
// scope: only the plain/unprotected case is encoded (used for the real "no matching persisted
// security context for this TMSI" reject case, TS 24.501 §5.6.1's own "if the UE... is not known"
// outcome, where a network doesn't have a security context to protect the reject WITH in the
// first place). A real security-context-mismatch reject (known TMSI, wrong ngKSI/failed MAC)
// would use the SECURED form instead -- not implemented this pass, disclosed rather than silently
// sent unprotected when a real context does exist.
// mm_cause: TS 24.501 §9.11.3.2 5GMM cause -- e.g. 0x03 ILLEGAL_UE, 0x09
// UE_IDENTITY_CANNOT_BE_DERIVED_FROM_NETWORK (confirmed against
// simulators/ransim/vendor/UERANSIM/src/lib/nas/enums.hpp's EMmCause, same real source this
// file's other cause values already cite).
std::vector<std::uint8_t> encode_service_reject_plain(std::uint8_t mm_cause);

// ADR-0393: UE-originating 5GMM Deregistration (TS 23.502 §4.2.2.3, TS 24.501 §8.2.12/§5.5.2.2).
// Byte layout confirmed against simulators/ransim/vendor/UERANSIM/src/lib/nas/msg.cpp's real
// DeRegistrationRequestUeOriginating::onBuild (`b.mandatoryIE1(&ngKSI, &deRegistrationType);
// b.mandatoryIE(&mobileIdentity);`) and ie1.cpp's IEDeRegistrationType::Decode
// (`bits::Bmp4Dec112(val, &switchOff, &reRegistrationRequired, &accessType)` -- switchOff is bit3,
// reRegistrationRequired bit2 (spare in the UE->network direction per that struct's own comment,
// not decoded), accessType bits0-1).
//
// The 5GS Mobile Identity IE (mandatory, Type-6 LV-E) is walked past by its own length -- not
// decoded. A real UE sends its assigned 5G-GUTI here (TS 24.501 §5.5.2.2.1), but this AMF already
// knows which UE this is from the association's own UeAuthState (this is the SAME association
// registration ran on, never a fresh one -- unlike ServiceRequest, which genuinely needs identity
// from the wire to find a security context in the first place). Decoding a specific identity kind
// here would add real parsing work with no caller who needs the result; skipping the TLV via its
// own length field is correct regardless of what identity type it carries.
struct DeregistrationRequestOutcome {
    bool mac_valid = false;
    // TS 24.501 §5.5.2.2's "switch off" bit -- true means the UE is powering off and TS 24.501
    // requires the network NOT send DEREGISTRATION ACCEPT for it (clause 6.2.2.2.3's own two
    // distinct UE-initiated trigger bullets for the LI record make the same split); false is the
    // ordinary case, where the network's own ACCEPT is what completes the procedure.
    bool switch_off = false;
};

std::optional<DeregistrationRequestOutcome>
decode_deregistration_request(const aka_crypto::NasIntKey& knas_int,
                              const aka_crypto::NasEncKey& knas_enc,
                              std::uint32_t uplink_count,
                              const std::vector<std::uint8_t>& nas_pdu);

// Encodes DeregistrationAccept (TS 24.501 §8.2.11/§5.5.2.2.2) -- header and message type only, no
// IEs at all (confirmed against UERANSIM's own DeRegistrationAcceptUeOriginating::onBuild, an
// EMPTY body). Integrity protected AND ciphered (security header type 0x02), the same "ordinary
// secured message" protection every post-SecurityModeComplete downlink in this file uses. Not
// sent at all when the request's own switch_off was true -- see DeregistrationRequestOutcome's
// own comment; the caller is responsible for that branch, this function only builds the bytes.
std::vector<std::uint8_t> encode_deregistration_accept(const aka_crypto::NasIntKey& knas_int,
                                                       const aka_crypto::NasEncKey& knas_enc,
                                                       std::uint32_t downlink_count);

} // namespace amf::nas
