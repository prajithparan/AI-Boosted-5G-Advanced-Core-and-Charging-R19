#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// ADR-0265: the UE side of NAS, for tests -- enough of TS 24.501 to get a real UE through
// authentication against this project's real AMF/AUSF/UDM/UDR chain.
//
// Why this is not testing the code against itself. These encoders produce bytes that AMF's own
// `decode_registration_request`/`decode_authentication_outcome` accept, so the framing is
// mirror-derived. That would be circular if the decoders had never seen an independent
// implementation -- but they have: registration was originally built and verified against
// UERANSIM's own UE (ADR-0031/0032/0037), which is a separate codebase by a different author.
// Mirroring a format an independent implementation already agreed on is re-encoding, not
// self-agreement.
//
// The genuinely independent check is the cryptography, and it is what these tests assert on. RES*
// is computed here from the subscriber's real K/OPc and sent to AUSF, which independently computes
// its own expected value from UDR's stored credentials. A wrong RES* is rejected no matter how
// well-formed the NAS framing is. Assertions are therefore on AMF/AUSF *accepting* the exchange,
// never on this file round-tripping its own output.

namespace nf_test {

// TS 35.207 Test Set 1 K/OP -- the values nfs/udm seeds imsi-999700000000001 with (ADR-0026), and
// the same ones tests/integration/test_ausf_ue_authentication.cpp already uses.
constexpr const char* kTestSupi = "imsi-999700000000001";

struct AuthChallenge {
    std::array<std::uint8_t, 16> rand{};
    std::array<std::uint8_t, 16> autn{};
};

// Plain (unprotected) RegistrationRequest carrying a null-protection-scheme SUCI for `supi`, plus
// a UE Security Capability IE. TS 24.501 §8.2.6.
std::vector<std::uint8_t> build_registration_request(const std::string& supi);

// Pulls RAND and AUTN out of a plain AuthenticationRequest (TS 24.501 §8.2.1). std::nullopt if the
// PDU is not one.
std::optional<AuthChallenge> parse_authentication_request(const std::vector<std::uint8_t>& nas_pdu);

// Verifies AUTN's MAC against the subscriber's real credentials and returns RES* (TS 33.501
// Annex A.4). std::nullopt means the network's AUTN did not authenticate -- a real failure this
// driver refuses to paper over by answering anyway.
//
// Deliberately NOT checked: SQN freshness. A real UE rejects an out-of-range SQN and answers
// AuthenticationFailure(SYNCH_FAILURE); this project's seeded subscriber uses a fixed TS 35.207
// SQN that always trips that on first contact, and AMF implements the resync that follows
// (ADR-0037). That path is worth its own test, but it is not what these tests are about -- so this
// UE accepts any SQN and says so, rather than quietly depending on resync working.
std::optional<std::array<std::uint8_t, 16>>
compute_res_star(const AuthChallenge& challenge, const std::string& serving_network_name);

// Plain AuthenticationResponse carrying RES* as IEI 0x2D. TS 24.501 §8.2.2.
std::vector<std::uint8_t>
build_authentication_response(const std::array<std::uint8_t, 16>& res_star);

// The NAS keys the UE derives independently of the network: CK/IK -> KAUSF -> KSEAF -> KAMF ->
// KNASint/KNASenc (TS 33.501 Annex A). AMF reaches the same KAMF via AUSF, so a MAC AMF accepts is
// proof both sides derived the same key material -- which is what makes SecurityModeComplete a
// real assertion rather than a formatting exercise.
struct NasKeys {
    std::array<std::uint8_t, 16> knas_int{};
    std::array<std::uint8_t, 16> knas_enc{};
};

NasKeys derive_nas_keys(const AuthChallenge& challenge,
                        const std::string& supi,
                        const std::string& serving_network_name);

// SecurityModeComplete, integrity protected AND ciphered with the new security context
// (TS 24.501 §8.2.26, security header type 0x04). `uplink_count` is 0 for this first secured
// uplink message -- AMF verifies against exactly that.
std::vector<std::uint8_t> build_security_mode_complete(const NasKeys& keys,
                                                       std::uint32_t uplink_count);

// ADR-0267: the rest of the registration procedure, and this project's only post-registration NAS
// procedure -- PDU Session Establishment (TS 23.502 §4.3.2.2.1).
//
// AMF's own phase machine (nfs/amf/src/ngap_task.cpp's UeAuthState::Phase) fixes the NAS COUNT of
// each of these exactly: SecurityModeComplete=0, RegistrationComplete=1, UlNasTransport=2. They
// are parameters rather than constants for the same reason AMF's own encoders keep them explicit,
// but a caller that passes anything else gets a MAC AMF rejects.

// RegistrationComplete (TS 24.501 §8.2.5) -- no mandatory IEs, integrity protected AND ciphered
// with the context SecurityModeComplete established (security header type 0x02, NOT 0x04: the
// context is no longer new by this message).
//
// A real UE sends this only when RegistrationAccept carried a 5G-GUTI, an NSSCI indication, or a
// configured NSSAI (UERANSIM's own receiveInitialRegistrationAccept). AMF's
// encode_registration_accept does carry a real 5G-GUTI (ADR-0075), so this is genuinely owed.
std::vector<std::uint8_t> build_registration_complete(const NasKeys& keys,
                                                      std::uint32_t uplink_count);

// UlNasTransport (TS 24.501 §8.2.10) wrapping a real 5GSM PDU Session Establishment Request
// (§8.3.1) in its payload container, plus the transport-level IEs AMF routes on: PDU session ID,
// request type, S-NSSAI and DNN. Integrity protected and ciphered (security header type 0x02).
//
// The inner 5GSM message is a genuine one, not a header-shaped stub: EPD/pduSessionId/PTI/message
// type, the mandatory integrityProtectionMaximumDataRate, then pduSessionType=IPv4 and
// sscMode=SSC-mode-1 -- the combination SMF's own encode_establishment_accept answers with.
std::vector<std::uint8_t> build_pdu_session_establishment_request(const NasKeys& keys,
                                                                  std::uint32_t uplink_count,
                                                                  std::uint8_t pdu_session_id,
                                                                  std::uint8_t pti,
                                                                  const std::string& dnn,
                                                                  std::uint8_t sst,
                                                                  std::uint32_t sd);

// Verifies the MAC of a secured DOWNLINK NAS PDU and deciphers it, returning the inner plain
// message. std::nullopt means the PDU was not security-protected in a form this understands, or
// its MAC did not verify -- an independent check on AMF's own downlink protection, since these
// keys were derived by the UE side and never sent to AMF.
std::optional<std::vector<std::uint8_t>> open_secured_downlink(
    const NasKeys& keys, std::uint32_t downlink_count, const std::vector<std::uint8_t>& nas_pdu);

// Pulls the opaque N1 SM payload container out of a plain DlNasTransport (TS 24.501 §8.2.9) --
// AMF's delivery vehicle for the PDU Session Establishment Accept SMF built. std::nullopt if the
// message is not a DlNasTransport carrying an N1 SM container.
std::optional<std::vector<std::uint8_t>>
extract_dl_nas_payload_container(const std::vector<std::uint8_t>& plain_inner);

// ADR-0393: pulls the real 5G-GUTI value bytes (11 octets: identity-type byte, PLMN(3),
// AMF Region ID(1), AMF Set ID/Pointer packed(2), 5G-TMSI(4)) out of a deciphered
// RegistrationAccept, i.e. this driver's own `open_secured_downlink`'s returned plain_inner --
// the inverse of nfs/amf/src/nas_codec.cpp's own encode_registration_accept, so a Deregistration
// this driver later sends can carry the UE's REAL assigned identity instead of a fabricated one.
// std::nullopt if the message is not a RegistrationAccept carrying IEI 0x77 (this project's AMF
// always sends one, ADR-0075, so a real UE never hits this case in practice).
std::optional<std::vector<std::uint8_t>>
extract_guti_from_registration_accept(const std::vector<std::uint8_t>& plain_inner);

// DeregistrationRequestUeOriginating (TS 24.501 §8.2.12), integrity protected AND ciphered
// (security header type 0x02 -- sent under the association's ordinary post-SecurityModeComplete
// protection, not ServiceRequest's special never-ciphered case; see
// nfs/amf/src/nas_codec.cpp's decode_deregistration_request for why that distinction doesn't
// apply here). `guti_value` is normally extract_guti_from_registration_accept's own result --
// this driver sends whatever it's given, uninterpreted, matching AMF's own decoder (it walks the
// mobile identity TLV by length, never decodes its content). `switch_off` sets TS 24.501
// §5.5.2.2's "switch off" bit (byte layout: ngKSI in the high nibble, fixed 0 -- this project's
// only ever-allocated ngKSI, ADR-0031 -- packed with Bmp4Enc112(switchOff, reRegistrationRequired=
// spare/0, accessType=THREEGPP_ACCESS=0b01) in the low nibble, confirmed against
// simulators/ransim/vendor/UERANSIM/src/utils/bits.hpp's real Ranged8 bit order).
std::vector<std::uint8_t> build_deregistration_request(const NasKeys& keys,
                                                       std::uint32_t uplink_count,
                                                       const std::vector<std::uint8_t>& guti_value,
                                                       bool switch_off = false);

} // namespace nf_test
