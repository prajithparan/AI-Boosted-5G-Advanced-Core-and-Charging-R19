#include "ue_nas_driver.hpp"

#include <cstring>

#include "aka_crypto/hex.hpp"
#include "aka_crypto/kdf.hpp"
#include "aka_crypto/milenage.hpp"
#include "aka_crypto/nas_security.hpp"

namespace nf_test {

namespace {

// TS 24.501 constants, mirroring nfs/amf/src/nas_codec.cpp's own named values rather than
// re-deriving them.
constexpr std::uint8_t kEpdMobilityManagement = 0x7E;
constexpr std::uint8_t kSecurityHeaderNotProtected = 0x00;
constexpr std::uint8_t kMessageTypeRegistrationRequest = 0x41;
constexpr std::uint8_t kMessageTypeAuthenticationRequest = 0x56;
constexpr std::uint8_t kMessageTypeAuthenticationResponse = 0x57;
constexpr std::uint8_t kIeiUeSecurityCapability = 0x2E;
constexpr std::uint8_t kIeiAuthResponseParameter = 0x2D;
constexpr std::uint8_t kIeiRand = 0x21;
constexpr std::uint8_t kMessageTypeSecurityModeComplete = 0x5E;
constexpr std::uint8_t kShtIntegrityProtectedAndCipheredWithNewSecurityContext = 0x04;
// TS 24.501 NAS security context: bearer id 1, direction 0 = uplink. Mirrors
// nfs/amf/src/nas_codec.cpp's own kNasBearerId/kDirectionUplink.
constexpr std::uint8_t kNasBearerId = 1;
constexpr std::uint8_t kDirectionUplink = 0;
constexpr std::uint8_t kIeiAutn = 0x20;
constexpr std::uint8_t kShtIntegrityProtectedAndCiphered = 0x02;
constexpr std::uint8_t kDirectionDownlink = 1;
constexpr std::uint8_t kMessageTypeRegistrationComplete = 0x43;
constexpr std::uint8_t kMessageTypeUlNasTransport = 0x67;
// ADR-0393, mirroring nfs/amf/src/nas_codec.cpp's own named values.
constexpr std::uint8_t kMessageTypeRegistrationAccept = 0x42;
constexpr std::uint8_t kIeiRegistrationAcceptGuti = 0x77;
constexpr std::uint8_t kMessageTypeDeregistrationRequestUeOriginating = 0x45;
constexpr std::uint8_t kMessageTypeDlNasTransport = 0x68;
constexpr std::uint8_t kPayloadContainerTypeN1SmInformation = 0x01;
// UlNasTransport's own optional IEIs, mirroring nfs/amf/src/nas_codec.cpp's named values.
constexpr std::uint8_t kIeiUlNasPduSessionId = 0x12;
constexpr std::uint8_t kIeiUlNasSNssai = 0x22;
constexpr std::uint8_t kIeiUlNasDnn = 0x25;
// Half-octet IEI 0x8 | ERequestType::INITIAL_REQUEST (0b001), TS 24.501 §9.11.3.47 -- the
// (iei << 4) | value packing UERANSIM's NasMessageBuilder::optionalIE1 uses.
constexpr std::uint8_t kUlNasRequestTypeInitial = 0x81;

// TS 24.501 5GSM, SMF's side of which is nfs/smf/src/nas_5gsm_codec.cpp -- the same named values,
// not re-derived.
constexpr std::uint8_t kEpdSessionManagement = 0x2E;
constexpr std::uint8_t kMessageTypeEstablishmentRequest = 0xC1;
// Half-octet IEIs of the 5GSM Establishment Request's optional IEs, per UERANSIM's own
// PduSessionEstablishmentRequest::onBuild: 0x9 pduSessionType, 0xA sscMode.
constexpr std::uint8_t kIeiNibblePduSessionType = 0x9;
constexpr std::uint8_t kIeiNibbleSscMode = 0xA;
constexpr std::uint8_t kPduSessionTypeIpv4 = 0b001;
constexpr std::uint8_t kSscMode1 = 0b001;
// TS 24.501 §9.11.4.7 -- UERANSIM's EMaximumDataRatePerUeForUserPlaneIntegrityProtection*::
// FULL_DATA_RATE, the value its own UE sends.
constexpr std::uint8_t kIntegrityProtectionFullDataRate = 0xFF;

// Every secured uplink message in this driver has the same envelope: cipher the inner plain
// message, then MAC the bytes AS TRANSMITTED (the ciphered inner, prefixed by the sequence-number
// octet -- see build_security_mode_complete's own note on why MACing the plaintext instead is the
// classic silent-rejection bug).
std::vector<std::uint8_t> seal_uplink(const NasKeys& keys,
                                      std::uint32_t uplink_count,
                                      std::uint8_t security_header_type,
                                      const std::vector<std::uint8_t>& inner_plain) {
    const auto ciphered = aka_crypto::nea2_apply(
        keys.knas_enc, uplink_count, kNasBearerId, kDirectionUplink, inner_plain);

    std::vector<std::uint8_t> mac_input;
    mac_input.reserve(ciphered.size() + 1);
    mac_input.push_back(static_cast<std::uint8_t>(uplink_count & 0xFF));
    mac_input.insert(mac_input.end(), ciphered.begin(), ciphered.end());
    const auto mac = aka_crypto::nia2_mac(
        keys.knas_int, uplink_count, kNasBearerId, kDirectionUplink, mac_input);

    std::vector<std::uint8_t> out;
    out.push_back(kEpdMobilityManagement);
    out.push_back(security_header_type);
    out.push_back(static_cast<std::uint8_t>((mac >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((mac >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((mac >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(mac & 0xFF));
    out.push_back(static_cast<std::uint8_t>(uplink_count & 0xFF));
    out.insert(out.end(), ciphered.begin(), ciphered.end());
    return out;
}

// Same TS 35.207 Test Set 1 credentials nfs/udm seeds imsi-999700000000001 with (ADR-0026), and
// that tests/integration/test_ausf_ue_authentication.cpp already proves work against this stack.
aka_crypto::Key128 test_k() {
    return *aka_crypto::from_hex<16>("465b5ce8b199b49faa5f0a2ee238a6bc");
}

aka_crypto::Key128 test_opc() {
    const auto op = *aka_crypto::from_hex<16>("cdc202d5123e20f62b6d676ac72cb318");
    return aka_crypto::derive_opc(test_k(), op);
}

// Half-octet BCD, filler 0xF for a 2-digit MNC -- the decode direction of this is
// nfs/amf/src/nas_codec.cpp's own SUCI PLMN parsing.
void append_plmn_bcd(std::vector<std::uint8_t>& out,
                     const std::string& mcc,
                     const std::string& mnc) {
    const auto d = [](char c) { return static_cast<std::uint8_t>(c - '0'); };
    const bool long_mnc = mnc.size() == 3;
    out.push_back(static_cast<std::uint8_t>((d(mcc[1]) << 4) | d(mcc[0])));
    out.push_back(static_cast<std::uint8_t>(((long_mnc ? d(mnc[2]) : 0xF) << 4) | d(mcc[2])));
    out.push_back(static_cast<std::uint8_t>((d(mnc[1]) << 4) | d(mnc[0])));
}

} // namespace

std::vector<std::uint8_t> build_registration_request(const std::string& supi) {
    // "imsi-<mcc(3)><mnc(2)><msin>" -- the only shape this lab seeds.
    const std::string digits = supi.substr(std::strlen("imsi-"));
    const std::string mcc = digits.substr(0, 3);
    const std::string mnc = digits.substr(3, 2);
    const std::string msin = digits.substr(5);

    // 5GS Mobile Identity value: SUCI, IMSI format, null protection scheme.
    std::vector<std::uint8_t> id;
    id.push_back(0x01); // identity type SUCI (bits0-2 = 001), SUPI format IMSI (bits4-6 = 000)
    append_plmn_bcd(id, mcc, mnc);
    // Routing indicator "0000" -- 2 clean BCD octets, the only form AMF's decoder accepts and the
    // one simulators/ransim/config/ue.yaml uses.
    id.push_back(0x00);
    id.push_back(0x00);
    id.push_back(0x00); // protection scheme id: null
    id.push_back(0x00); // home network public key id
    for (std::size_t i = 0; i < msin.size(); i += 2) {
        const std::uint8_t lo = static_cast<std::uint8_t>(msin[i] - '0');
        const std::uint8_t hi = (i + 1 < msin.size()) ? static_cast<std::uint8_t>(msin[i + 1] - '0')
                                                      : 0x0F; // odd-length terminator
        id.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }

    std::vector<std::uint8_t> out;
    out.push_back(kEpdMobilityManagement);
    out.push_back(kSecurityHeaderNotProtected);
    out.push_back(kMessageTypeRegistrationRequest);
    out.push_back(0x71); // ngKSI 7 (no key available) | 5GS registration type 1 (initial)
    out.push_back(static_cast<std::uint8_t>((id.size() >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(id.size() & 0xFF));
    out.insert(out.end(), id.begin(), id.end());

    // UE Security Capability (TS 24.501 §9.11.3.54). AMF replays these bytes verbatim in
    // SecurityModeCommand and a real UE compares them byte-for-byte (the anti-bidding-down check),
    // so the value matters even though AMF never interprets it. 0x80/0x80 advertises
    // 128-NEA1..NEA0/128-NIA1 style bitmaps with the top bit set; this lab always selects
    // 128-NEA2/128-NIA2, so what matters here is only that the IE is present and stable.
    out.push_back(kIeiUeSecurityCapability);
    out.push_back(0x02);
    out.push_back(0xE0); // 5G-EA0, 128-5G-EA1, 128-5G-EA2 supported
    out.push_back(0xE0); // 5G-IA0, 128-5G-IA1, 128-5G-IA2 supported
    return out;
}

std::optional<AuthChallenge>
parse_authentication_request(const std::vector<std::uint8_t>& nas_pdu) {
    // 0x7E 0x00 0x56 | ngKSI | ABBA(len,val...) | 0x21 + RAND(16) | 0x20 + len + AUTN(16)
    if (nas_pdu.size() < 4 || nas_pdu[0] != kEpdMobilityManagement ||
        nas_pdu[1] != kSecurityHeaderNotProtected ||
        nas_pdu[2] != kMessageTypeAuthenticationRequest) {
        return std::nullopt;
    }
    std::size_t off = 4;
    if (off >= nas_pdu.size()) {
        return std::nullopt;
    }
    off += 1 + nas_pdu[off]; // ABBA: 1-octet length + value

    AuthChallenge challenge;
    bool have_rand = false;
    bool have_autn = false;
    while (off < nas_pdu.size()) {
        const std::uint8_t iei = nas_pdu[off];
        if (iei == kIeiRand) {
            if (off + 1 + 16 > nas_pdu.size()) {
                return std::nullopt;
            }
            std::memcpy(challenge.rand.data(), &nas_pdu[off + 1], 16); // Type-3: no length octet
            off += 1 + 16;
            have_rand = true;
        } else if (iei == kIeiAutn) {
            if (off + 2 > nas_pdu.size()) {
                return std::nullopt;
            }
            const std::uint8_t len = nas_pdu[off + 1];
            if (len != 16 || off + 2 + len > nas_pdu.size()) {
                return std::nullopt;
            }
            std::memcpy(challenge.autn.data(), &nas_pdu[off + 2], 16);
            off += 2 + len;
            have_autn = true;
        } else {
            break;
        }
    }
    if (!have_rand || !have_autn) {
        return std::nullopt;
    }
    return challenge;
}

std::optional<std::array<std::uint8_t, 16>>
compute_res_star(const AuthChallenge& challenge, const std::string& serving_network_name) {
    const auto k = test_k();
    const auto opc = test_opc();
    const auto out = aka_crypto::f2345(opc, k, challenge.rand);

    // AUTN = (SQN xor AK) || AMF || MAC-A. Recover SQN with AK, then recompute MAC-A and compare:
    // this is the real network-authentication check, and it fails loudly if the credentials or the
    // vector are wrong rather than answering anyway.
    aka_crypto::Sqn sqn{};
    for (std::size_t i = 0; i < sqn.size(); ++i) {
        sqn[i] = static_cast<std::uint8_t>(challenge.autn[i] ^ out.ak[i]);
    }
    aka_crypto::Amf amf{};
    std::memcpy(amf.data(), &challenge.autn[6], amf.size());
    const auto expected_mac = aka_crypto::f1(opc, k, challenge.rand, sqn, amf);
    if (std::memcmp(expected_mac.data(), &challenge.autn[8], expected_mac.size()) != 0) {
        return std::nullopt;
    }

    return aka_crypto::derive_res_star(
        out.ck, out.ik, serving_network_name, challenge.rand, out.res);
}

std::vector<std::uint8_t>
build_authentication_response(const std::array<std::uint8_t, 16>& res_star) {
    std::vector<std::uint8_t> out;
    out.push_back(kEpdMobilityManagement);
    out.push_back(kSecurityHeaderNotProtected);
    out.push_back(kMessageTypeAuthenticationResponse);
    out.push_back(kIeiAuthResponseParameter);
    out.push_back(static_cast<std::uint8_t>(res_star.size()));
    out.insert(out.end(), res_star.begin(), res_star.end());
    return out;
}

NasKeys derive_nas_keys(const AuthChallenge& challenge,
                        const std::string& supi,
                        const std::string& serving_network_name) {
    const auto k = test_k();
    const auto opc = test_opc();
    const auto out = aka_crypto::f2345(opc, k, challenge.rand);

    // SQN xor AK is AUTN's own first six octets -- taken from the wire rather than recomputed, so
    // this matches what the network actually sent.
    aka_crypto::Ak48 sqn_xor_ak{};
    std::memcpy(sqn_xor_ak.data(), challenge.autn.data(), sqn_xor_ak.size());

    const auto kausf = aka_crypto::derive_kausf(out.ck, out.ik, serving_network_name, sqn_xor_ak);
    const auto kseaf = aka_crypto::derive_kseaf(kausf, serving_network_name);
    // KAMF is keyed on the SUPI without its "imsi-" prefix, and ABBA is the TS 33.501 Annex A.7.1
    // default -- both matching nfs/amf/src/ngap_task.cpp's own derivation exactly, because a
    // mismatch here produces a MAC AMF rejects.
    const aka_crypto::Abba abba{0x00, 0x00};
    const std::string bare_supi = supi.substr(std::strlen("imsi-"));
    const auto kamf = aka_crypto::derive_kamf(kseaf, bare_supi, abba);

    NasKeys keys;
    keys.knas_int = aka_crypto::derive_knas_int(kamf, aka_crypto::kNia2AlgorithmIdentity);
    keys.knas_enc = aka_crypto::derive_knas_enc(kamf, aka_crypto::kNea2AlgorithmIdentity);
    return keys;
}

std::vector<std::uint8_t> build_security_mode_complete(const NasKeys& keys,
                                                       std::uint32_t uplink_count) {
    const std::vector<std::uint8_t> inner_plain{
        kEpdMobilityManagement, kSecurityHeaderNotProtected, kMessageTypeSecurityModeComplete};
    // Security header type 0x04: this is the first message under a security context AMF has only
    // just commanded, so it is the "with new security context" variant. Every later uplink message
    // uses 0x02.
    return seal_uplink(
        keys, uplink_count, kShtIntegrityProtectedAndCipheredWithNewSecurityContext, inner_plain);
}

std::vector<std::uint8_t> build_registration_complete(const NasKeys& keys,
                                                      std::uint32_t uplink_count) {
    // TS 24.501 §8.2.5: header and message type only. The one optional IE
    // (sorTransparentContainer) is never triggered here -- AMF's RegistrationAccept carries no SOR
    // container.
    const std::vector<std::uint8_t> inner_plain{
        kEpdMobilityManagement, kSecurityHeaderNotProtected, kMessageTypeRegistrationComplete};
    return seal_uplink(keys, uplink_count, kShtIntegrityProtectedAndCiphered, inner_plain);
}

std::vector<std::uint8_t> build_pdu_session_establishment_request(const NasKeys& keys,
                                                                  std::uint32_t uplink_count,
                                                                  std::uint8_t pdu_session_id,
                                                                  std::uint8_t pti,
                                                                  const std::string& dnn,
                                                                  std::uint8_t sst,
                                                                  std::uint32_t sd) {
    // --- The 5GSM message itself (TS 24.501 §8.3.1). AMF never decodes this; it forwards the
    // bytes to SMF as SmContextCreateData.n1SmMsg, and SMF's own nas_5gsm_codec.cpp decodes them.
    std::vector<std::uint8_t> sm;
    sm.push_back(kEpdSessionManagement);
    sm.push_back(pdu_session_id);
    sm.push_back(pti);
    sm.push_back(kMessageTypeEstablishmentRequest);
    // Mandatory integrityProtectionMaximumDataRate (TS 24.501 §9.11.4.7): uplink then downlink,
    // Type-3, no IEI and no length -- matching UERANSIM's own
    // IEIntegrityProtectionMaximumDataRate::Encode field order.
    sm.push_back(kIntegrityProtectionFullDataRate);
    sm.push_back(kIntegrityProtectionFullDataRate);
    // Optional half-octet IEs, (iei << 4) | value. IPv4 / SSC mode 1 is the only combination SMF
    // answers for (nfs/smf/src/nas_5gsm_codec.cpp's encode_establishment_accept), so asking for
    // anything else would be asking for a rejection this test is not about.
    sm.push_back(static_cast<std::uint8_t>((kIeiNibblePduSessionType << 4) | kPduSessionTypeIpv4));
    sm.push_back(static_cast<std::uint8_t>((kIeiNibbleSscMode << 4) | kSscMode1));

    // --- The 5GMM UlNasTransport that carries it (TS 24.501 §8.2.10).
    std::vector<std::uint8_t> inner;
    inner.push_back(kEpdMobilityManagement);
    inner.push_back(kSecurityHeaderNotProtected);
    inner.push_back(kMessageTypeUlNasTransport);
    // payloadContainerType: a half-octet IE with no IEI of its own, so the high nibble is 0 --
    // exactly what AMF's decode_ul_nas_transport reads as `inner[3] & 0x0F`.
    inner.push_back(kPayloadContainerTypeN1SmInformation);
    inner.push_back(static_cast<std::uint8_t>((sm.size() >> 8) & 0xFF));
    inner.push_back(static_cast<std::uint8_t>(sm.size() & 0xFF));
    inner.insert(inner.end(), sm.begin(), sm.end());

    inner.push_back(kIeiUlNasPduSessionId);
    inner.push_back(pdu_session_id);
    inner.push_back(kUlNasRequestTypeInitial);

    // S-NSSAI (TS 24.501 §9.11.2.8), SST+SD form -- the 4-octet length AMF's decoder recognises
    // and the sst=1/sd=000001 this lab configures (simulators/ransim/config/ue.yaml).
    inner.push_back(kIeiUlNasSNssai);
    inner.push_back(0x04);
    inner.push_back(sst);
    inner.push_back(static_cast<std::uint8_t>((sd >> 16) & 0xFF));
    inner.push_back(static_cast<std::uint8_t>((sd >> 8) & 0xFF));
    inner.push_back(static_cast<std::uint8_t>(sd & 0xFF));

    // DNN (TS 24.501 §9.11.2.1a) in TS 23.003 §9.1 label form: each dot-separated label prefixed
    // by its own length octet. The inverse of AMF's own decoder, which rejoins the labels with
    // dots.
    std::vector<std::uint8_t> apn;
    std::size_t start = 0;
    while (start <= dnn.size()) {
        const std::size_t dot = dnn.find('.', start);
        const std::string label =
            dnn.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        apn.push_back(static_cast<std::uint8_t>(label.size()));
        apn.insert(apn.end(), label.begin(), label.end());
        if (dot == std::string::npos) {
            break;
        }
        start = dot + 1;
    }
    inner.push_back(kIeiUlNasDnn);
    inner.push_back(static_cast<std::uint8_t>(apn.size()));
    inner.insert(inner.end(), apn.begin(), apn.end());

    return seal_uplink(keys, uplink_count, kShtIntegrityProtectedAndCiphered, inner);
}

std::optional<std::vector<std::uint8_t>> open_secured_downlink(
    const NasKeys& keys, std::uint32_t downlink_count, const std::vector<std::uint8_t>& nas_pdu) {
    // 0x7E | SHT | MAC(4) | SEQ(1) | ciphered inner.
    if (nas_pdu.size() < 8 || nas_pdu[0] != kEpdMobilityManagement ||
        nas_pdu[1] != kShtIntegrityProtectedAndCiphered) {
        return std::nullopt;
    }
    const std::vector<std::uint8_t> ciphered(nas_pdu.begin() + 7, nas_pdu.end());

    // The MAC covers the bytes as transmitted -- the ciphered inner prefixed by the sequence
    // number octet, same convention as the uplink direction.
    std::vector<std::uint8_t> mac_input;
    mac_input.reserve(ciphered.size() + 1);
    mac_input.push_back(nas_pdu[6]);
    mac_input.insert(mac_input.end(), ciphered.begin(), ciphered.end());
    const auto expected = aka_crypto::nia2_mac(
        keys.knas_int, downlink_count, kNasBearerId, kDirectionDownlink, mac_input);
    const std::uint32_t carried = (static_cast<std::uint32_t>(nas_pdu[2]) << 24) |
                                  (static_cast<std::uint32_t>(nas_pdu[3]) << 16) |
                                  (static_cast<std::uint32_t>(nas_pdu[4]) << 8) |
                                  static_cast<std::uint32_t>(nas_pdu[5]);
    if (expected != carried) {
        return std::nullopt;
    }

    return aka_crypto::nea2_apply(
        keys.knas_enc, downlink_count, kNasBearerId, kDirectionDownlink, ciphered);
}

std::optional<std::vector<std::uint8_t>>
extract_dl_nas_payload_container(const std::vector<std::uint8_t>& plain_inner) {
    // header(3) + payloadContainerType(1) + 2-octet container length = 6 octets minimum, the same
    // shape AMF's encode_dl_nas_transport writes.
    if (plain_inner.size() < 6 || plain_inner[0] != kEpdMobilityManagement ||
        plain_inner[2] != kMessageTypeDlNasTransport ||
        (plain_inner[3] & 0x0F) != kPayloadContainerTypeN1SmInformation) {
        return std::nullopt;
    }
    const std::size_t len =
        (static_cast<std::size_t>(plain_inner[4]) << 8) | static_cast<std::size_t>(plain_inner[5]);
    if (6 + len > plain_inner.size()) {
        return std::nullopt;
    }
    return std::vector<std::uint8_t>(plain_inner.begin() + 6,
                                     plain_inner.begin() + 6 + static_cast<std::ptrdiff_t>(len));
}

std::optional<std::vector<std::uint8_t>>
extract_guti_from_registration_accept(const std::vector<std::uint8_t>& plain_inner) {
    // header(3) + mandatory registrationResult IE (Type-4: len(1) + value(1)) = 5 bytes before
    // any optional IE begins -- same shape nfs/amf/src/nas_codec.cpp's own
    // encode_registration_accept writes.
    if (plain_inner.size() < 5 || plain_inner[0] != kEpdMobilityManagement ||
        plain_inner[2] != kMessageTypeRegistrationAccept) {
        return std::nullopt;
    }
    std::size_t off = 5;
    while (off < plain_inner.size()) {
        const std::uint8_t iei = plain_inner[off];
        if (off + 3 > plain_inner.size()) {
            break;
        }
        const std::size_t len =
            (static_cast<std::size_t>(plain_inner[off + 1]) << 8) | plain_inner[off + 2];
        if (off + 3 + len > plain_inner.size()) {
            break;
        }
        if (iei == kIeiRegistrationAcceptGuti) {
            return std::vector<std::uint8_t>(plain_inner.begin() + off + 3,
                                             plain_inner.begin() + off + 3 +
                                                 static_cast<std::ptrdiff_t>(len));
        }
        off += 3 + len;
    }
    return std::nullopt;
}

std::vector<std::uint8_t> build_deregistration_request(const NasKeys& keys,
                                                       std::uint32_t uplink_count,
                                                       const std::vector<std::uint8_t>& guti_value,
                                                       bool switch_off) {
    // ngKSI (high nibble, fixed 0 -- this project's only ever-allocated ngKSI, ADR-0031) packed
    // with deRegistrationType (low nibble): Bmp4Enc112(switchOff, reRegistrationRequired,
    // accessType) -- switchOff at bit3, reRegistrationRequired (spare, 0) at bit2, accessType
    // (THREEGPP_ACCESS=0b01) at bits0-1. Confirmed against
    // simulators/ransim/vendor/UERANSIM/src/utils/bits.hpp's real Ranged8 bit order (first
    // argument ends up in the HIGHEST bit position of the group).
    constexpr std::uint8_t kAccessTypeThreeGpp = 0b01;
    const std::uint8_t dereg_type_nibble =
        static_cast<std::uint8_t>((switch_off ? 0b1000 : 0) | kAccessTypeThreeGpp);
    const std::uint8_t ngksi_dereg_byte = static_cast<std::uint8_t>(dereg_type_nibble & 0x0F);

    std::vector<std::uint8_t> inner;
    inner.push_back(kEpdMobilityManagement);
    inner.push_back(kSecurityHeaderNotProtected);
    inner.push_back(kMessageTypeDeregistrationRequestUeOriginating);
    inner.push_back(ngksi_dereg_byte);
    // 5GS Mobile Identity (Type-6 LV-E): 2-octet big-endian length, then the value AMF's own
    // decoder walks past by length without interpreting -- see decode_deregistration_request's
    // own comment for why. Sent uninterpreted, whatever the caller gave (normally a real 5G-GUTI
    // extracted from this UE's own RegistrationAccept).
    inner.push_back(static_cast<std::uint8_t>((guti_value.size() >> 8) & 0xFF));
    inner.push_back(static_cast<std::uint8_t>(guti_value.size() & 0xFF));
    inner.insert(inner.end(), guti_value.begin(), guti_value.end());

    return seal_uplink(keys, uplink_count, kShtIntegrityProtectedAndCiphered, inner);
}

} // namespace nf_test
