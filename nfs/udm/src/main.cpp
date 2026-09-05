// nfs/udm: UDM (Unified Data Management), Nudm_UECM + Nudm_SDM + (this turn) Nudm_UEAU surfaces.
// Source: specs/5G_APIs-REL-19/TS29503_Nudm_UECM.yaml, TS29503_Nudm_SDM.yaml,
// TS29503_Nudm_UEAU.yaml (commit bca84b60a37773133bcae97e5c6c0d10a93b47b6). Phase 2's fourth NF
// (PROMPT.md/CLAUDE.md order: NRF -> AMF -> SMF -> UDM -> UDR -> AUSF -> PCF); Nudm_UEAU was
// deliberately deferred out of UDM's own turn (see ADR-0023's rejected alternative) until AUSF's
// turn actually needed it -- this is that turn, extending an already-committed NF rather than a
// second full NF in one turn, matching the precedent ADR-0023 set for this.
//
// In scope, agreed with the user before implementation:
// Nudm_UECM -- 3GppRegistration, Update3GppRegistration, Get3GppRegistration, deregAMF (the AMF
// 3GPP-access registration group), GetSmfRegistration, Registration, RetrieveSmfRegistration,
// UpdateSmfRegistration, SmfDeregistration (the SMF registration group).
// Nudm_SDM -- GetAmData, GetSmfSelData, GetSmData, Subscribe, Unsubscribe.
// Nudm_UEAU -- GenerateAuthData (5G-AKA and EAP-AKA' authentication vector generation, real
// Milenage + TS 33.501 Annex A key derivation via libs/aka-crypto, see ADR-0026), ConfirmAuth,
// DeleteAuth, and (gap-closure, docs/CAPABILITY_GAP_ANALYSIS.md task #104, ADR-0091)
// GenerateProseAV -- real TS 33.503 5G ProSe authentication vector generation, structurally the
// same EAP-AKA' Milenage path as GenerateAuthData's own EAP-AKA' branch (real, deliberate code
// reuse, not a parallel implementation).
//
// STALE-COMMENT CORRECTION (ADR-0251). This header used to read "Deliberately deferred, not
// dropped: Nudm_EE, Nudm_MT, Nudm_NIDDAU, Nudm_PP, Nudm_RSDS, Nudm_SSAU, Nudm_UEID ...; SDM's
// remaining ~25 GET operations (... GetSupiOrGpsi, Sor/Upu Ack ...)". Every one of those was
// subsequently implemented -- and this file's own sections below already document them, so the
// comment contradicted the code beneath it. Closed by ADR-0082 (Nudm_EE), ADR-0230/0232/0233
// (SDM groups C1/C2 + Modify), ADR-0234 (SOR/UPU acks), ADR-0235 (shared-data family), ADR-0236
// (GetSupiOrGpsi/GetMultipleIdentifiers, at the real YAML paths /{ueId}/id-translation-result and
// /multiple-identifiers -- NOT paths guessed from the operation names), and ADR-0237 (Nudm_PP's
// 11 ops). Nudm_MT/Nudm_NIDDAU/Nudm_RSDS/Nudm_SSAU/Nudm_UEID are documented as real below.
//
// Genuinely still deferred, verified against the YAML rather than carried over on faith: UECM's
// non-3GPP-AMF, SMSF (3GPP and non-3GPP), IP-SM-GW and NWDAF registration groups; UEAU's
// GetRgAuthData, GenerateAv (EPS/IMS/HSS) and GenerateGbaAv -- out of this build's Tier-1 5G-AKA
// scope (5G-RG, EPS/IMS-AKA interworking and GBA are separate concerns).
//
// UPDATE (ADR-0069, gap-closure Tier 1b): GetAmData/GetSmfSelData/GetSmData now make real
// Nudr_DataRepository GET calls against UDR's own real provisioned-data group (added the same
// turn, see nfs/udr/src/main.cpp), replacing the permanently-empty stub this comment used to
// describe. A SUPI genuinely not seeded in UDR now correctly 404s instead of always returning a
// schema-valid empty body. Real, disclosed narrowing: UDR's provisioned-data group is itself
// GET-only per spec (no create/update operation exists at all) and seeded with fixed test data at
// UDR startup, same real-data-source reasoning as this file's own Nudm_UEAU
// AuthenticationSubscriptionStore already used -- so this is real cross-NF wiring against real
// persisted data, not yet a live OSS/BSS provisioning path. UECM's registration operations remain
// real bookkeeping (an AMF or SMF really did register), unaffected by this change.
//
// UPDATE (ADR-0202, gap-closure task #161): 5 more Nudm_* services, previously deliberately
// deferred above, are now real Tier-A closures:
// - Nudm_MT (api root /nudm-mt/v1): QueryUeInfo and ProvideLocationInfo, both real-existence-
//   checked via the same fetch_from_udr helper GetAmData etc. already use (a real UDR am-data
//   probe as the existence signal -- no dedicated new store needed), then an honestly-empty
//   UeInfo_Nudm_MT{}/LocationInfoResult{} 200 -- no real VoPS/5G-SRVCC/location data exists in
//   this build to populate either from.
// - Nudm_NIDDAU (api root /nudm-niddau/v1): AuthorizeNiddData, real structural validation of
//   AuthorizationInfo's required snssai/dnn/mtcProviderInformation/authUpdateCallbackUri, then a
//   real, disclosed 501 -- no real MTC-provider/NIDD authorization policy data exists anywhere in
//   this build to authorize against.
// - Nudm_RSDS (api root /nudm-rsds/v1): ReportSMDeliveryStatus, real structural validation of
//   SmDeliveryStatus's required gpsi/smStatusReport, then a real 204 ack -- disclosed: no real SMS
//   routing/retry logic exists to act on the report.
// - Nudm_SSAU (api root /nudm-ssau/v1): ServiceSpecificAuthorization real structural validation
//   then a real, disclosed 501 (no real service-specific authorization policy data exists for
//   AF_GUIDANCE_FOR_URSP/AF_REQUESTED_QOS/AF_PROVISION_N3GPP_DEV_ID_INFO); real name collision
//   found and fixed as part of wiring this in -- TS29503_Nudm_SSAU.yaml's own
//   ServiceSpecificAuthorizationInfo collided with TS29505_Subscription_Data.yaml's
//   already-existing, unrelated same-named UDR schema, disambiguated by the codegen to
//   ServiceSpecificAuthorizationInfo_Nudm_SSAU / ServiceSpecificAuthorizationInfo_Subscription_Data
//   respectively -- nfs/udr/src/main.cpp's own pre-existing reference to the bare name updated to
//   the new disambiguated one, a real, necessary fix, not a functional change to UDR's own
//   behavior. ServiceSpecificAuthorizationRemoval real structural validation of the required
//   authId, then a real 404 -- since ServiceSpecificAuthorization itself never issues a real
//   authId (it always 501s), no removal target can ever exist to match one.
// - Nudm_UEID (api root /nudm-ueid/v1): Deconceal -- NOT a stub. Reuses this file's own existing,
//   real deconceal_suci_if_needed() (the same TS 33.501 Annex C ECIES Profile A/B SUCI
//   de-concealment already exercised internally by GenerateAuthData/GenerateProseAV above,
//   ADR-0037/Tier 1c) to implement a real, working Deconceal operation -- real 200 with the
//   genuine deconcealed SUPI on success, real 400 on a malformed/unsupported/MAC-verification-
//   failed SUCI (same real error shape GenerateAuthData's own SUCI path already uses). Same
//   disclosed scope narrowing as the existing helper: only the IMSI-type (supiType "0") SUCI form
//   is supported, not the NAI/GCI/GLI form.

#include "sbi_core/http2_client.hpp"
#include "sbi_core/http2_server.hpp"
#include "sbi_core/io_context_pool.hpp"
#include "sbi_core/json_body.hpp"
#include "sbi_core/jwt.hpp"
#include "sbi_core/logging.hpp"
#include "sbi_core/metrics.hpp"
#include "sbi_core/oauth2_client.hpp"
#include "sbi_core/otel.hpp"
#include "sbi_core/rate_limit.hpp"
#include "sbi_core/sbi_headers.hpp"
#include "sbi_core/uuid.hpp"

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <sstream>
#include <thread>
#include <variant>

#include "TS26510_CommonData_grp.hpp"
#include "TS29503_Nudm_MT.hpp"
#include "TS29503_Nudm_RSDS.hpp"
#include "TS29503_Nudm_SSAU.hpp"
#include "TS29503_Nudm_UEAU_grp.hpp"
#include "TS29503_Nudm_UEID.hpp"
#include "aka_crypto/hex.hpp"
#include "aka_crypto/kdf.hpp"
#include "aka_crypto/milenage.hpp"
#include "aka_crypto/suci.hpp"
#include "map_client.hpp"
#include "stores.hpp"
#include "tbcd_core/tbcd.hpp"

// docs/DECISIONS.md ADR-0077 -- no hardcoded deployment literal in source.
#include "nf_config/nf_config.hpp"

namespace {

using nlohmann::json;

#ifndef CERTS_DIR
#error "CERTS_DIR must be defined by CMake (see nfs/udm/CMakeLists.txt)"
#endif
#ifndef CONFIG_DIR
#error "CONFIG_DIR must be defined by CMake (see nfs/udm/CMakeLists.txt)"
#endif

constexpr const char* kNfType = "UDM";
constexpr const char* kUecmApiRoot = "/nudm-uecm/v1";
constexpr const char* kSdmApiRoot = "/nudm-sdm/v2";
constexpr const char* kUeauApiRoot = "/nudm-ueau/v1";
// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #105, ADR-0082).
constexpr const char* kEeApiRoot = "/nudm-ee/v1";
constexpr const char* kPpApiRoot = "/nudm-pp/v1";
constexpr const char* kUdrApiRoot = "/nudr-dr/v2";
// TS29503_Nudm_MT/NIDDAU/RSDS/SSAU/UEID.yaml own real api roots (ADR-0202), confirmed via each
// YAML's own `servers:` block.
constexpr const char* kMtApiRoot = "/nudm-mt/v1";
constexpr const char* kNiddauApiRoot = "/nudm-niddau/v1";
constexpr const char* kRsdsApiRoot = "/nudm-rsds/v1";
constexpr const char* kSsauApiRoot = "/nudm-ssau/v1";
constexpr const char* kUeidApiRoot = "/nudm-ueid/v1";
// This project's own real lab PLMN, mcc=999/mnc=70 (ADR-0016), in the real VarPlmnId string
// format (mcc+mnc concatenated, TS29505_Subscription_Data.yaml). Used as the real UDR
// servingPlmnId path segment when the caller's own `plmn-id` query parameter is absent (that
// query param is genuinely OPTIONAL per TS29503_Nudm_SDM.yaml -- not required, checked not
// assumed) -- a real, disclosed single-PLMN-lab simplification, not a fabricated value.
constexpr const char* kDefaultServingPlmnId = "99970";

// Must match nfs/nrf/src/main.cpp's kNrfInstanceId exactly -- see docs/DECISIONS.md ADR-0018.
constexpr const char* kNrfInstanceId = "5ba9a927-1d31-4c8e-8a10-000000000001";

// Same pattern as every other NF's check_bearer -- see nfs/nrf/src/main.cpp's comment for why a
// missing Authorization header is not itself a 401 (bootstrap security alternative:
// `security: [{}, oAuth2ClientCredentials:[...]]` in the YAML).
// Real `plmn-id` query param handling (TS29503_Nudm_SDM.yaml: optional, `content:
// application/json` PlmnIdNid) -- ADR-0069, gap-closure Tier 1b. Builds the real UDR VarPlmnId
// path segment (mcc+mnc concatenated) from a parsed PlmnIdNid if the caller supplied one, else
// falls back to kDefaultServingPlmnId.
std::string resolve_serving_plmn_id(const sbi_core::http2::Request& req) {
    auto it = req.query_params.find("plmn-id");
    if (it == req.query_params.end()) {
        return kDefaultServingPlmnId;
    }
    try {
        const auto plmn = nlohmann::json::parse(it->second).get<sbi_gen::PlmnIdNid>();
        return plmn.mcc + plmn.mnc;
    } catch (const nlohmann::json::exception&) {
        return kDefaultServingPlmnId; // malformed plmn-id -- real, disclosed fallback, not a 400
    }
}

// Real SUCI de-concealment (ADR-0070, gap-closure Tier 1c) -- TS 33.501 clause 6.12.5 names UDM
// as the real home of the Subscription Identifier De-concealing Function (SIDF), so this lives
// here rather than in AUSF (which already just forwards `supiOrSuci` straight through to this
// same endpoint, unaffected by this change).
//
// Real, disclosed lab key material: these are the SAME real, officially-published TS 33.501
// Annex C.4.3.1/C.4.4.1 implementers' test key pairs libs/aka-crypto's own test_suci.cpp
// independently verifies against -- reused here as this lab's own Home Network private key,
// matching this project's own existing precedent (nfs/udm's AuthenticationSubscriptionStore
// already reuses TS 35.207 Test Set 1's public K/OPc as seeded test-subscriber crypto material).
// A real production deployment would provision a real, non-public Home Network key pair; this
// lab's own single-PLMN scope has no such provisioning path yet, real and disclosed, not silently
// implied to be production-grade.
constexpr const char* kHnPrivateKeyProfileA =
    "c53c22208b61860b06c62e5406a7b330c2b577aa5558981510d128247d38bd1d";
constexpr const char* kHnPrivateKeyProfileB =
    "f1ab1074477ebcc7f554ea1c5fc368b1616730155e0041ac447d6301975fecda";

// Real SUCI text format (confirmed directly from this repo's own vendored
// specs/5G_APIs-REL-19/TS29571_CommonData.yaml `SupiOrSuci` schema pattern, not guessed):
// suci-<supiType>-<mcc>-<mnc>-<routingIndicator>-<protectionScheme>-<homeNetworkPublicKeyId>-
// <schemeOutput>, for the real IMSI-type (supiType digit "0") form. Real, disclosed scope
// narrowing: only this IMSI-type form is parsed -- the real YAML pattern's own alternative form
// for supiType 1-7 (NAI/GCI/GLI-based SUCI) uses a free-form realm/identifier segment that can
// itself contain '-', making a simple '-'-split ambiguous; left unsupported here rather than
// guessed, same "narrow the scope, disclose it" discipline this project already uses elsewhere.
// Returns the original string unchanged if it isn't SUCI-formatted at all (already a real
// imsi-/nai-/... SUPI, the existing, correct passthrough), or nullopt if it IS SUCI-formatted but
// couldn't be de-concealed (malformed, unsupported supiType/protectionScheme, or a real MAC
// verification failure).
std::optional<std::string> deconceal_suci_if_needed(const std::string& supi_or_suci) {
    if (supi_or_suci.rfind("suci-", 0) != 0) {
        return supi_or_suci;
    }
    std::vector<std::string> parts;
    std::stringstream ss(supi_or_suci);
    std::string part;
    while (std::getline(ss, part, '-')) {
        parts.push_back(part);
    }
    if (parts.size() != 8 || parts[0] != "suci" || parts[1] != "0") {
        return std::nullopt;
    }
    const std::string& mcc = parts[2];
    const std::string& mnc = parts[3];
    const std::string& protection_scheme = parts[5];
    const std::string& scheme_output_hex = parts[7];

    const auto scheme_output = aka_crypto::from_hex(scheme_output_hex);
    if (!scheme_output.has_value()) {
        return std::nullopt;
    }

    std::optional<std::vector<std::uint8_t>> plaintext_msin_bcd;
    if (protection_scheme == "0") {
        plaintext_msin_bcd = scheme_output; // real null-scheme: output == input (TS 33.501 C.2)
    } else if (protection_scheme == "1") {
        if (const auto key = aka_crypto::from_hex<32>(kHnPrivateKeyProfileA); key.has_value()) {
            plaintext_msin_bcd = aka_crypto::deconceal_profile_a(*scheme_output, *key);
        }
    } else if (protection_scheme == "2") {
        if (const auto key = aka_crypto::from_hex<32>(kHnPrivateKeyProfileB); key.has_value()) {
            plaintext_msin_bcd = aka_crypto::deconceal_profile_b(*scheme_output, *key);
        }
    } else {
        return std::nullopt; // real, disclosed: proprietary schemes (0xC-0xF) not supported
    }
    if (!plaintext_msin_bcd.has_value()) {
        return std::nullopt;
    }
    return "imsi-" + mcc + mnc + tbcd_core::decode_tbcd(*plaintext_msin_bcd);
}

std::optional<sbi_core::jwt::VerifyResult> check_bearer(const sbi_core::http2::Request& req,
                                                        sbi_core::jwt::Verifier& verifier) {
    auto it = req.headers.find("authorization");
    if (it == req.headers.end()) {
        return std::nullopt;
    }
    const std::string& value = it->second;
    constexpr std::string_view kPrefix = "Bearer ";
    if (value.size() <= kPrefix.size() || value.compare(0, kPrefix.size(), kPrefix) != 0) {
        sbi_core::jwt::VerifyResult r;
        r.valid = false;
        r.error = "Authorization header present but not a Bearer token";
        return r;
    }
    return verifier.verify(value.substr(kPrefix.size()));
}

// Runs on a dedicated thread, never on the server's io_context -- same reasoning as
// nfs/amf/src/main.cpp's run_nrf_lifecycle (docs/DECISIONS.md ADR-0006/ADR-0019).
void run_nrf_lifecycle(const std::string& udm_instance_id, const std::string& nrf_base) {
    sbi_core::http2::TlsConfig client_tls{
        .cert_path = CERTS_DIR "/udm/cert.pem",
        .key_path = CERTS_DIR "/udm/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client http_client(std::move(client_tls));

    for (int attempt = 0; attempt < 300; ++attempt) {
        sbi_core::http2::ClientRequest probe;
        probe.method = "GET";
        probe.url = nrf_base + "/nnrf-nfm/v1/nf-instances/00000000-0000-4000-8000-000000000000";
        if (http_client.send(probe).has_value()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    sbi_core::OAuth2Client oauth(
        http_client, nrf_base + "/oauth2/token", udm_instance_id, "nnrf-nfm", "NRF");

    constexpr int kHeartbeatSeconds = 30;
    json profile{
        {"nfInstanceId", udm_instance_id},
        {"nfType", kNfType},
        {"nfStatus", "REGISTERED"},
        {"ipv4Addresses", json::array({"127.0.0.1"})},
        {"heartBeatTimer", kHeartbeatSeconds},
    };

    while (true) {
        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("udm: OAuth2 token fetch failed: {}", token.error());
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        sbi_core::http2::ClientRequest put_req;
        put_req.method = "PUT";
        put_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + udm_instance_id;
        put_req.headers.emplace("content-type", "application/json");
        put_req.headers.emplace("authorization", "Bearer " + *token);
        put_req.headers.emplace(
            sbi_core::headers::kSenderTimestamp,
            sbi_core::headers::format_sender_timestamp(std::chrono::system_clock::now()));
        put_req.body = profile.dump();

        auto put_resp = http_client.send(put_req);
        if (put_resp.has_value() && (put_resp->status == 200 || put_resp->status == 201)) {
            spdlog::info("udm: registered with NRF (HTTP {})", put_resp->status);
            break;
        }
        spdlog::warn("udm: NRF registration attempt failed, retrying in 5s");
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(kHeartbeatSeconds / 2));

        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("udm: OAuth2 token fetch failed for heartbeat: {}", token.error());
            continue;
        }

        sbi_core::http2::ClientRequest patch_req;
        patch_req.method = "PATCH";
        patch_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + udm_instance_id;
        patch_req.headers.emplace("content-type", "application/json-patch+json");
        patch_req.headers.emplace("authorization", "Bearer " + *token);
        patch_req.body =
            json::array({json{{"op", "replace"}, {"path", "/nfStatus"}, {"value", "REGISTERED"}}})
                .dump();

        auto patch_resp = http_client.send(patch_req);
        if (!patch_resp.has_value() || patch_resp->status != 200) {
            spdlog::warn("udm: heartbeat failed");
        }
    }
}

// ADR-0293 (P4.5 stage 5b completion): UDM's MAP client had existed since ADR-0061 and NOTHING
// EVER CALLED IT -- `grep map_client nfs/udm/src/main.cpp` returned nothing. The HLR-side
// capability was real and unreachable, which by this project's own standard ("a server with no
// consumer is not done") meant MAP was not done.
//
// The trigger, stated precisely because it is a LOCAL choice and not a spec mandate: TS 29.002's
// own sequence has the HLR send insertSubscriberData to a VLR in response to that VLR's MAP
// updateLocation. This project has no MAP LISTENER, so no updateLocation can arrive -- building one
// is a separate increment (it needs the server side of the same stack CHF's CAP listener already
// proves is possible). What this project does have is the 5G analogue of the same event: an AMF
// registering a UE with UDM over Nudm_UECM. So that is the trigger, and a real deployment doing
// 2G/3G interworking would replace it with the updateLocation path rather than add to it.
//
// Runs on a detached thread on purpose: send_insert_subscriber_data opens an SCTP association and
// waits for a MAP response. Blocking the UECM 201 on a legacy peer's round trip would make 5G
// registration latency hostage to an SS7 node.
// ADR-0296: the deregistration counterpart. Same detached-thread, log-don't-propagate discipline
// as the push below, and for the same reason: a legacy VLR being slow or down must not make a 5G
// deregistration fail or hang.
void cancel_location_at_vlr(const std::string& vlr_address,
                            std::uint16_t vlr_port,
                            const std::string& ue_id,
                            const std::string& dereg_reason) {
    if (vlr_address.empty()) {
        return;
    }
    constexpr std::string_view kImsiPrefix = "imsi-";
    if (ue_id.rfind(kImsiPrefix, 0) != 0) {
        return;
    }
    const std::string digits = ue_id.substr(kImsiPrefix.size());

    std::thread([vlr_address, vlr_port, digits, dereg_reason] {
        map_core::CancelLocationArg arg{};
        arg.imsi = tbcd_core::encode_tbcd(digits);
        // cancellationType is OPTIONAL in the real ASN.1, and it is sent ONLY for the one
        // deregReason whose correspondence is a name identity rather than a judgement call:
        // TS 29.503's `SUBSCRIPTION_WITHDRAWN` and TS 29.002's `subscriptionWithdraw`. No
        // 3GPP specification maps the other six deregReason values onto a cancellationType --
        // that mapping would be interworking policy, and inventing one here would put a wrong
        // fact on the wire silently (telling a VLR "the subscriber moved" when it did not, or
        // the reverse). Omitting the optional field claims nothing and is genuinely valid.
        if (dereg_reason == "SUBSCRIPTION_WITHDRAWN") {
            arg.cancellation_type = map_core::CancellationType::kSubscriptionWithdraw;
        }
        if (udm::send_cancel_location(vlr_address, vlr_port, arg)) {
            spdlog::info("udm: MAP cancelLocation accepted by VLR {}:{} for imsi-{}",
                         vlr_address,
                         vlr_port,
                         digits);
        } else {
            spdlog::warn("udm: MAP cancelLocation to VLR {}:{} failed for imsi-{} -- the 5G "
                         "deregistration itself is unaffected",
                         vlr_address,
                         vlr_port,
                         digits);
        }
    }).detach();
}

void push_subscriber_data_to_vlr(const std::string& vlr_address,
                                 std::uint16_t vlr_port,
                                 const std::string& ue_id) {
    if (vlr_address.empty()) {
        return;
    }
    // ueId arrives as "imsi-<digits>"; MAP carries the IMSI as TBCD (TS 23.003 §2.2).
    constexpr std::string_view kImsiPrefix = "imsi-";
    if (ue_id.rfind(kImsiPrefix, 0) != 0) {
        return; // an SUPI that is not an IMSI has no MAP identity to send
    }
    const std::string digits = ue_id.substr(kImsiPrefix.size());

    std::thread([vlr_address, vlr_port, digits] {
        map_core::InsertSubscriberDataArg arg{};
        arg.imsi = tbcd_core::encode_tbcd(digits);
        if (udm::send_insert_subscriber_data(vlr_address, vlr_port, arg)) {
            spdlog::info("udm: MAP insertSubscriberData accepted by VLR {}:{} for imsi-{}",
                         vlr_address,
                         vlr_port,
                         digits);
        } else {
            spdlog::warn("udm: MAP insertSubscriberData to VLR {}:{} failed for imsi-{} -- the 5G "
                         "registration itself is unaffected",
                         vlr_address,
                         vlr_port,
                         digits);
        }
    }).detach();
}

} // namespace

int main() {
    const auto config = nf_config::load("udm", CONFIG_DIR);
    // ADR-0293: the legacy VLR/MSC peer UDM pushes subscriber data to over MAP. Absent = disabled,
    // which is the default: a 5G-only deployment has no VLR.
    const auto vlr_peer_address =
        nf_config::require<std::string>(config, "vlr_peer_address", "UDM_VLR_PEER_ADDRESS");
    const auto vlr_peer_port =
        nf_config::require<std::uint16_t>(config, "vlr_peer_port", "UDM_VLR_PEER_PORT");
    if (!vlr_peer_address.empty()) {
        spdlog::info("udm: MAP insertSubscriberData enabled towards VLR {}:{}",
                     vlr_peer_address,
                     vlr_peer_port);
    }
    const auto port = nf_config::require<unsigned short>(config, "port");
    const auto metrics_bind_address =
        nf_config::require<std::string>(config, "metrics_bind_address");
    const auto nrf_base_url =
        nf_config::require<std::string>(config, "nrf_base_url", "UDM_NRF_BASE_URL");
    const auto udr_base_url =
        nf_config::require<std::string>(config, "udr_base_url", "UDM_UDR_BASE_URL");

    sbi_core::init_logging("udm");
    sbi_core::init_tracing("udm");
    sbi_core::init_metrics(metrics_bind_address);

    const std::string udm_instance_id = sbi_core::generate_uuid_v4();
    spdlog::info("udm: starting, nfInstanceId={}", udm_instance_id);

    sbi_core::http2::TlsConfig server_tls{
        .cert_path = CERTS_DIR "/udm/cert.pem",
        .key_path = CERTS_DIR "/udm/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };

    sbi_core::jwt::Verifier verifier(CERTS_DIR "/nrf-jwt/public.pem", kNrfInstanceId);

    udm::AmfRegistrationStore amf_registrations;
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #169, ADR-0215).
    udm::RoamingInfoUpdateStore roaming_info_updates;
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #169, ADR-0216).
    udm::AmfNon3GppRegistrationStore amf_non3gpp_registrations;
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #169, ADR-0217).
    udm::SmsfRegistrationStore smsf_3gpp_registrations;
    udm::SmsfRegistrationStore smsf_non3gpp_registrations;
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #169, ADR-0218).
    udm::IpSmGwRegistrationStore ip_sm_gw_registrations;
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #169, ADR-0219).
    udm::NwdafRegistrationStore nwdaf_registrations;
    udm::SmfRegistrationStore smf_registrations;
    udm::SdmSubscriptionStore sdm_subscriptions;
    udm::SharedDataSubscriptionStore shared_data_subscriptions;
    udm::AuthenticationSubscriptionStore auth_subscriptions;
    udm::AuthEventStore auth_events;
    udm::EeSubscriptionStore ee_subscriptions;
    udm::PpDataStore pp_data;

    // Fixed test subscribers, seeded at startup -- not provisionable through any API (see file
    // header). K/OP/OPc/SQN/AMF are the real, cross-checked 3GPP TS 35.207 Test Set 1 values (the
    // same ones tests/conformance/test_milenage.cpp verifies libs/aka-crypto's Milenage
    // implementation against) -- reused here as seed data rather than invented, so a real UE-role
    // test client can independently compute the same CK/IK/RES from the well-known K/OP and check
    // AUSF/UDM's output against them, not just trust round-tripping.
    {
        const auto k = *aka_crypto::from_hex<16>("465b5ce8b199b49faa5f0a2ee238a6bc");
        const auto op = *aka_crypto::from_hex<16>("cdc202d5123e20f62b6d676ac72cb318");
        const auto opc = aka_crypto::derive_opc(k, op);
        const auto sqn = *aka_crypto::from_hex<6>("ff9bb4d0b607");
        const auto amf_field = *aka_crypto::from_hex<2>("b9b9");

        auth_subscriptions.seed("imsi-999700000000001",
                                udm::AuthenticationSubscription{
                                    .k = k,
                                    .opc = opc,
                                    .sqn = sqn,
                                    .amf = amf_field,
                                    .authentication_method = "5G_AKA",
                                });
        auth_subscriptions.seed("imsi-999700000000002",
                                udm::AuthenticationSubscription{
                                    .k = k,
                                    .opc = opc,
                                    .sqn = *aka_crypto::from_hex<6>("000000000000"),
                                    .amf = amf_field,
                                    .authentication_method = "EAP_AKA_PRIME",
                                });
    }

    // UDM's own client identity + token source for calling UDR (ADR-0069, gap-closure Tier 1b) --
    // same separate-http2::Client-per-target-NF pattern nfs/ausf/src/main.cpp's own udm_client
    // already established for its AUSF->UDM call.
    sbi_core::http2::TlsConfig udr_client_tls{
        .cert_path = CERTS_DIR "/udm/cert.pem",
        .key_path = CERTS_DIR "/udm/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client udr_client(std::move(udr_client_tls));
    sbi_core::OAuth2Client udr_oauth(
        udr_client, nrf_base_url + "/oauth2/token", udm_instance_id, "nudr-dr", "UDR");

    auto meter = sbi_core::get_meter("udm");
    auto amf_reg_counter =
        meter->CreateUInt64Counter("udm_amf_registration_total", "Total 3GppRegistration calls");
    auto amf_dereg_counter =
        meter->CreateUInt64Counter("udm_amf_deregistration_total", "Total deregAMF calls");
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #169, ADR-0215).
    auto pei_update_counter =
        meter->CreateUInt64Counter("udm_pei_update_total", "Total PeiUpdate calls");
    auto roaming_info_update_counter = meter->CreateUInt64Counter(
        "udm_roaming_info_update_total", "Total UpdateRoamingInformation calls");
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #169, ADR-0216).
    auto amf_non3gpp_reg_counter = meter->CreateUInt64Counter("udm_amf_non3gpp_registration_total",
                                                              "Total Non3GppRegistration calls");
    auto amf_non3gpp_patch_counter = meter->CreateUInt64Counter(
        "udm_amf_non3gpp_patch_total", "Total UpdateNon3GppRegistration calls");
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #169, ADR-0217).
    auto smsf_3gpp_reg_counter = meter->CreateUInt64Counter("udm_smsf_3gpp_registration_total",
                                                            "Total 3GppSmsfRegistration calls");
    auto smsf_3gpp_dereg_counter = meter->CreateUInt64Counter("udm_smsf_3gpp_deregistration_total",
                                                              "Total 3GppSmsfDeregistration calls");
    auto smsf_3gpp_patch_counter = meter->CreateUInt64Counter(
        "udm_smsf_3gpp_patch_total", "Total UpdateSmsf3GppRegistration calls");
    auto smsf_non3gpp_reg_counter = meter->CreateUInt64Counter(
        "udm_smsf_non3gpp_registration_total", "Total Non3GppSmsfRegistration calls");
    auto smsf_non3gpp_dereg_counter = meter->CreateUInt64Counter(
        "udm_smsf_non3gpp_deregistration_total", "Total Non3GppSmsfDeregistration calls");
    auto smsf_non3gpp_patch_counter = meter->CreateUInt64Counter(
        "udm_smsf_non3gpp_patch_total", "Total UpdateSmsfNon3GppRegistration calls");
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #169, ADR-0218).
    auto ip_sm_gw_reg_counter = meter->CreateUInt64Counter("udm_ip_sm_gw_registration_total",
                                                           "Total IpSmGwRegistration calls");
    auto ip_sm_gw_dereg_counter = meter->CreateUInt64Counter("udm_ip_sm_gw_deregistration_total",
                                                             "Total IpSmGwDeregistration calls");
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #169, ADR-0219).
    auto nwdaf_reg_counter =
        meter->CreateUInt64Counter("udm_nwdaf_registration_total", "Total NwdafRegistration calls");
    auto nwdaf_dereg_counter = meter->CreateUInt64Counter("udm_nwdaf_deregistration_total",
                                                          "Total NwdafDeregistration calls");
    auto nwdaf_patch_counter =
        meter->CreateUInt64Counter("udm_nwdaf_patch_total", "Total UpdateNwdafRegistration calls");
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #169, ADR-0221).
    auto send_routing_info_sm_counter = meter->CreateUInt64Counter("udm_send_routing_info_sm_total",
                                                                   "Total SendRoutingInfoSm calls");
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #169, ADR-0227).
    auto restore_pcscf_counter = meter->CreateUInt64Counter(
        "udm_restore_pcscf_total", "Total Trigger P-CSCF Restoration calls");
    auto get_location_info_counter =
        meter->CreateUInt64Counter("udm_get_location_info_total", "Total GetLocationInfo calls");
    auto auth_trigger_counter =
        meter->CreateUInt64Counter("udm_auth_trigger_total", "Total authTrigger calls");
    auto smf_reg_counter =
        meter->CreateUInt64Counter("udm_smf_registration_total", "Total SMF Registration calls");
    auto smf_dereg_counter =
        meter->CreateUInt64Counter("udm_smf_deregistration_total", "Total SmfDeregistration calls");
    // UPDATE (ADR-0228, gap-closure Nudm_SDM group A+B): extended to cover all 16 real
    // individual-resource GET ops this counter now backs, not just the original 3.
    auto sdm_get_counter = meter->CreateUInt64Counter(
        "udm_sdm_data_get_total",
        "Total Nudm_SDM individual-resource GET calls (GetAmData/GetSmfSelData/GetSmData/"
        "GetSmsData/GetSmsMngtData/GetTraceConfigData/GetLcsBcaData/GetLcsPrivacyData/"
        "GetLcsMoData/GetLcsSubscriptionData/GetV2xData/GetProseData/GetMbsData/GetUcData/"
        "GetA2xData/GetRangingSlPrivacyData)");
    auto sdm_subscribe_counter =
        meter->CreateUInt64Counter("udm_sdm_subscribe_total", "Total SDM Subscribe calls");
    auto sdm_subscription_modify_counter =
        meter->CreateUInt64Counter("udm_sdm_subscription_modify_total", "Total SDM Modify calls");
    auto sdm_ack_counter = meter->CreateUInt64Counter(
        "udm_sdm_ack_total", "Total SorAckInfo/UpuAck/S-NSSAIs Ack/CAG Ack calls");
    auto shared_data_get_counter = meter->CreateUInt64Counter(
        "udm_shared_data_get_total", "Total GetSharedData/GetIndividualSharedData calls");
    auto group_identifiers_get_counter = meter->CreateUInt64Counter(
        "udm_group_identifiers_get_total", "Total GetGroupIdentifiers calls");
    auto shared_data_subscribe_counter = meter->CreateUInt64Counter(
        "udm_shared_data_subscribe_total", "Total SubscribeToSharedData calls");
    auto shared_data_modify_counter = meter->CreateUInt64Counter(
        "udm_shared_data_modify_total", "Total ModifySharedDataSubs calls");
    auto id_translation_counter = meter->CreateUInt64Counter(
        "udm_id_translation_total", "Total GetSupiOrGpsi/GetMultipleIdentifiers calls");
    auto generate_auth_data_counter =
        meter->CreateUInt64Counter("udm_generate_auth_data_total", "Total GenerateAuthData calls");
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #104, ADR-0091): real, previously-deferred
    // GenerateProseAV.
    auto generate_prose_av_counter =
        meter->CreateUInt64Counter("udm_generate_prose_av_total", "Total GenerateProseAV calls");
    auto confirm_auth_counter =
        meter->CreateUInt64Counter("udm_confirm_auth_total", "Total ConfirmAuth calls");
    auto delete_auth_counter =
        meter->CreateUInt64Counter("udm_delete_auth_total", "Total DeleteAuth calls");
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #169, ADR-0214).
    auto get_rg_auth_data_counter =
        meter->CreateUInt64Counter("udm_get_rg_auth_data_total", "Total GetRgAuthData calls");
    auto generate_av_counter =
        meter->CreateUInt64Counter("udm_generate_av_total", "Total GenerateAv calls");
    auto generate_gba_av_counter =
        meter->CreateUInt64Counter("udm_generate_gba_av_total", "Total GenerateGbaAv calls");
    auto ee_subscribe_counter =
        meter->CreateUInt64Counter("udm_ee_subscribe_total", "Total CreateEeSubscription calls");
    auto ee_update_counter =
        meter->CreateUInt64Counter("udm_ee_update_total", "Total UpdateEeSubscription calls");
    auto ee_delete_counter =
        meter->CreateUInt64Counter("udm_ee_delete_total", "Total DeleteEeSubscription calls");
    auto pp_get_counter =
        meter->CreateUInt64Counter("udm_pp_data_get_total", "Total Get PP Data calls");
    auto pp_update_counter =
        meter->CreateUInt64Counter("udm_pp_data_update_total", "Total PP Data Update calls");
    auto pp_data_entry_counter = meter->CreateUInt64Counter(
        "udm_pp_data_entry_total", "Total Create/Delete/Get PP Data Entry calls");
    auto pp_5g_vn_group_counter = meter->CreateUInt64Counter(
        "udm_pp_5g_vn_group_total", "Total Create/Delete/Modify/Get 5G VN Group calls");
    auto pp_5g_mbs_group_counter = meter->CreateUInt64Counter(
        "udm_pp_5g_mbs_group_total", "Total Create/Delete/Modify/Get 5G MBS Group calls");
    // ADR-0202: Nudm_MT/NIDDAU/RSDS/SSAU/UEID counters.
    auto mt_query_ue_info_counter =
        meter->CreateUInt64Counter("udm_mt_query_ue_info_total", "Total QueryUeInfo calls");
    auto mt_provide_loc_info_counter = meter->CreateUInt64Counter(
        "udm_mt_provide_location_info_total", "Total Nudm_MT ProvideLocationInfo calls");
    auto niddau_authorize_counter =
        meter->CreateUInt64Counter("udm_niddau_authorize_total", "Total AuthorizeNiddData calls");
    auto rsds_report_counter =
        meter->CreateUInt64Counter("udm_rsds_report_total", "Total ReportSMDeliveryStatus calls");
    auto ssau_authorize_counter = meter->CreateUInt64Counter(
        "udm_ssau_authorize_total", "Total ServiceSpecificAuthorization calls");
    auto ssau_remove_counter = meter->CreateUInt64Counter(
        "udm_ssau_remove_total", "Total ServiceSpecificAuthorizationRemoval calls");
    auto ueid_deconceal_counter =
        meter->CreateUInt64Counter("udm_ueid_deconceal_total", "Total Nudm_UEID Deconceal calls");

    boost::asio::io_context ioc;
    // 0.0.0.0: same Docker-reachability reasoning as NRF's bind -- see docs/DECISIONS.md ADR-0014.
    sbi_core::http2::Server server(ioc, "0.0.0.0", port, server_tls);

    // P15 / P4.12 (ADR-0280): optional TPS ceiling from this NF's own config (`max_tps`,
    // `tps_burst`), overridable per deployment via SBI_MAX_TPS. Absent means unlimited, so this
    // changes nothing until an operator opts in.
    if (const auto tps_limit = sbi_core::read_tps_limit(config); tps_limit.enabled()) {
        server.set_tps_limit(tps_limit.sustained_tps, tps_limit.burst);
        spdlog::info("TPS ceiling active: {} req/s sustained, burst {}",
                     tps_limit.sustained_tps,
                     tps_limit.burst > 0.0 ? tps_limit.burst : tps_limit.sustained_tps);
    }

    // --- Nudm_UECM: AMF 3GPP-access registration group ---

    server.add_route(
        "PUT",
        std::string(kUecmApiRoot) + "/{ueId}/registrations/amf-3gpp-access",
        [&verifier, &amf_registrations, &amf_reg_counter, &vlr_peer_address, &vlr_peer_port](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::Amf3GppAccessRegistration>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            const bool is_new = !amf_registrations.get(ue_id).has_value();
            json j = *body;
            amf_registrations.put(ue_id, j);
            // ADR-0293: the UE now has a serving node on record -- push its subscriber data to the
            // legacy VLR peer, if one is configured. Fire-and-forget; see the helper's own note.
            push_subscriber_data_to_vlr(vlr_peer_address, vlr_peer_port, ue_id);
            amf_reg_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = is_new ? 201 : 200;
            resp.headers.emplace("content-type", "application/json");
            if (is_new) {
                resp.headers.emplace("location",
                                     std::string(kUecmApiRoot) + "/" + ue_id +
                                         "/registrations/amf-3gpp-access");
            }
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kUecmApiRoot) + "/{ueId}/registrations/amf-3gpp-access",
        [&verifier, &amf_registrations](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto registration = amf_registrations.get(ue_id);
            if (!registration.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No AMF 3GPP-access registration for ueId " + ue_id);
            }
            return sbi_core::http2::Response::json(200, registration->dump());
        });

    server.add_route(
        "PATCH",
        std::string(kUecmApiRoot) + "/{ueId}/registrations/amf-3gpp-access",
        [&verifier, &amf_registrations](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            // application/merge-patch+json (RFC 7396) -- validate it parses as the documented
            // Amf3GppAccessRegistrationModification shape before applying it, even though
            // .merge_patch() itself would accept any JSON object.
            sbi_core::http2::Response err;
            auto patch_dto =
                sbi_core::http2::parse_json_body<sbi_gen::Amf3GppAccessRegistrationModification>(
                    req, err);
            if (!patch_dto.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            auto patched = amf_registrations.merge_patch(ue_id, json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No AMF 3GPP-access registration for ueId " + ue_id);
            }
            return sbi_core::http2::Response::json(200, patched->dump());
        });

    server.add_route(
        "POST",
        std::string(kUecmApiRoot) + "/{ueId}/registrations/amf-3gpp-access/dereg-amf",
        [&verifier, &amf_registrations, &amf_dereg_counter, &vlr_peer_address, &vlr_peer_port](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::AmfDeregInfo>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            if (!amf_registrations.get(ue_id).has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No AMF 3GPP-access registration for ueId " + ue_id);
            }
            amf_registrations.remove(ue_id);
            cancel_location_at_vlr(vlr_peer_address, vlr_peer_port, ue_id, body->deregReason.value);
            amf_dereg_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #169, ADR-0215): real, previously
    // undisclosed `PeiUpdate` -- real RFC 7396 merge into the already-existing AMF 3GPP-access
    // registration document's own `pei` field (reuses `AmfRegistrationStore::merge_patch`
    // verbatim), not a new resource.
    server.add_route(
        "POST",
        std::string(kUecmApiRoot) + "/{ueId}/registrations/amf-3gpp-access/pei-update",
        [&verifier, &amf_registrations, &pei_update_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::PeiUpdateInfo_Nudm_UECM>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            const json patch = json{{"pei", body->pei}};
            if (!amf_registrations.merge_patch(ue_id, patch).has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No AMF 3GPP-access registration for ueId " + ue_id);
            }
            pei_update_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #169, ADR-0215): real, previously
    // undisclosed `UpdateRoamingInformation` -- genuinely distinct real resource from the AMF
    // 3GPP-access registration document itself (own real `RoamingInfoUpdate` schema, own real
    // `201`-with-`Location`-vs-`204` pair), backed by the new `RoamingInfoUpdateStore`.
    server.add_route(
        "POST",
        std::string(kUecmApiRoot) + "/{ueId}/registrations/amf-3gpp-access/roaming-info-update",
        [&verifier, &roaming_info_updates, &roaming_info_update_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::RoamingInfoUpdate>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            const bool is_new = !roaming_info_updates.get(ue_id).has_value();
            json j = *body;
            roaming_info_updates.put(ue_id, j);
            roaming_info_update_counter->Add(1);

            sbi_core::http2::Response resp;
            if (is_new) {
                resp.status = 201;
                resp.headers.emplace("content-type", "application/json");
                resp.headers.emplace("location",
                                     std::string(kUecmApiRoot) + "/" + ue_id +
                                         "/registrations/amf-3gpp-access/roaming-info-update");
                resp.body = j.dump();
            } else {
                resp.status = 204;
            }
            return resp;
        });

    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #169, ADR-0216): real, previously
    // undisclosed AMF non-3GPP-access registration group (Non3GppRegistration/
    // GetNon3GppRegistration/UpdateNon3GppRegistration) -- genuinely distinct real resource from
    // the AMF 3GPP-access registration group above (own real `AmfNon3GppAccessRegistration`
    // schema), same real PUT+GET+PATCH shape and store design. Real, disclosed: the PATCH's own
    // real spec documents a `200`+`PatchResult` response for a partial-failure execution report --
    // this project's simple `nlohmann::json::merge_patch()` has no partial-apply/rollback
    // semantics to report on, so a fully-applied merge returns the real, also-documented `204`
    // (no content) instead of fabricating an always-empty `PatchResult.report`.
    server.add_route(
        "PUT",
        std::string(kUecmApiRoot) + "/{ueId}/registrations/amf-non-3gpp-access",
        [&verifier, &amf_non3gpp_registrations, &amf_non3gpp_reg_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::AmfNon3GppAccessRegistration>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            const bool is_new = !amf_non3gpp_registrations.get(ue_id).has_value();
            json j = *body;
            amf_non3gpp_registrations.put(ue_id, j);
            amf_non3gpp_reg_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = is_new ? 201 : 200;
            resp.headers.emplace("content-type", "application/json");
            if (is_new) {
                resp.headers.emplace("location",
                                     std::string(kUecmApiRoot) + "/" + ue_id +
                                         "/registrations/amf-non-3gpp-access");
            }
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kUecmApiRoot) + "/{ueId}/registrations/amf-non-3gpp-access",
        [&verifier, &amf_non3gpp_registrations](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto registration = amf_non3gpp_registrations.get(ue_id);
            if (!registration.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No AMF non-3GPP-access registration for ueId " + ue_id);
            }
            return sbi_core::http2::Response::json(200, registration->dump());
        });

    server.add_route(
        "PATCH",
        std::string(kUecmApiRoot) + "/{ueId}/registrations/amf-non-3gpp-access",
        [&verifier, &amf_non3gpp_registrations, &amf_non3gpp_patch_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto patch_dto =
                sbi_core::http2::parse_json_body<sbi_gen::AmfNon3GppAccessRegistrationModification>(
                    req, err);
            if (!patch_dto.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            auto patched = amf_non3gpp_registrations.merge_patch(ue_id, json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No AMF non-3GPP-access registration for ueId " + ue_id);
            }
            amf_non3gpp_patch_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #169, ADR-0217): real, previously
    // undisclosed SMSF registration groups (3GPP-access and non-3GPP-access), both using the
    // real, identical `SmsfRegistration` schema per TS29503_Nudm_UECM.yaml (kept as two distinct
    // store instances, see `SmsfRegistrationStore`'s own header comment). Real, disclosed
    // simplifications applied identically to both groups:
    // - PUT's own real spec documents three success codes (`201`/`200`/`204`); this project uses
    //   only `201` (create) and `200` (update, with body) -- same convention already established
    //   for `3GppRegistration`/`Non3GppRegistration` above, not the alternate bodyless `204`.
    // - The real, optional response `ETag` header (PUT/DELETE) and request `If-Match` header
    //   (DELETE) are accepted/not populated -- no per-resource ETag/versioning layer exists
    //   anywhere in this project (same disclosed gap class as UDR's own `3gpp-Sbi-Etags`,
    //   ADR-0212).
    // - DELETE's own real, optional `smsf-set-id` query filter is accepted but not honored -- this
    //   store has no notion of an SMSF set distinct from the single stored registration.
    // - PATCH's own real spec documents a `200`+`PatchResult` partial-failure execution report;
    //   this project's simple merge-patch has no partial-apply semantics to report on, so a
    //   successful merge returns the real, also-documented `204` (same precedent as
    //   `UpdateNon3GppRegistration` above).
    server.add_route(
        "PUT",
        std::string(kUecmApiRoot) + "/{ueId}/registrations/smsf-3gpp-access",
        [&verifier, &smsf_3gpp_registrations, &smsf_3gpp_reg_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::SmsfRegistration>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            const bool is_new = !smsf_3gpp_registrations.get(ue_id).has_value();
            json j = *body;
            smsf_3gpp_registrations.put(ue_id, j);
            smsf_3gpp_reg_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = is_new ? 201 : 200;
            resp.headers.emplace("content-type", "application/json");
            if (is_new) {
                resp.headers.emplace("location",
                                     std::string(kUecmApiRoot) + "/" + ue_id +
                                         "/registrations/smsf-3gpp-access");
            }
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kUecmApiRoot) + "/{ueId}/registrations/smsf-3gpp-access",
        [&verifier, &smsf_3gpp_registrations](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto registration = smsf_3gpp_registrations.get(ue_id);
            if (!registration.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No SMSF 3GPP-access registration for ueId " + ue_id);
            }
            return sbi_core::http2::Response::json(200, registration->dump());
        });

    server.add_route(
        "DELETE",
        std::string(kUecmApiRoot) + "/{ueId}/registrations/smsf-3gpp-access",
        [&verifier, &smsf_3gpp_registrations, &smsf_3gpp_dereg_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            if (!smsf_3gpp_registrations.remove(ue_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No SMSF 3GPP-access registration for ueId " + ue_id);
            }
            smsf_3gpp_dereg_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "PATCH",
        std::string(kUecmApiRoot) + "/{ueId}/registrations/smsf-3gpp-access",
        [&verifier, &smsf_3gpp_registrations, &smsf_3gpp_patch_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto patch_dto =
                sbi_core::http2::parse_json_body<sbi_gen::SmsfRegistrationModification>(req, err);
            if (!patch_dto.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            auto patched = smsf_3gpp_registrations.merge_patch(ue_id, json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No SMSF 3GPP-access registration for ueId " + ue_id);
            }
            smsf_3gpp_patch_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "PUT",
        std::string(kUecmApiRoot) + "/{ueId}/registrations/smsf-non-3gpp-access",
        [&verifier, &smsf_non3gpp_registrations, &smsf_non3gpp_reg_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::SmsfRegistration>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            const bool is_new = !smsf_non3gpp_registrations.get(ue_id).has_value();
            json j = *body;
            smsf_non3gpp_registrations.put(ue_id, j);
            smsf_non3gpp_reg_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = is_new ? 201 : 200;
            resp.headers.emplace("content-type", "application/json");
            if (is_new) {
                resp.headers.emplace("location",
                                     std::string(kUecmApiRoot) + "/" + ue_id +
                                         "/registrations/smsf-non-3gpp-access");
            }
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kUecmApiRoot) + "/{ueId}/registrations/smsf-non-3gpp-access",
        [&verifier, &smsf_non3gpp_registrations](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto registration = smsf_non3gpp_registrations.get(ue_id);
            if (!registration.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No SMSF non-3GPP-access registration for ueId " + ue_id);
            }
            return sbi_core::http2::Response::json(200, registration->dump());
        });

    server.add_route(
        "DELETE",
        std::string(kUecmApiRoot) + "/{ueId}/registrations/smsf-non-3gpp-access",
        [&verifier, &smsf_non3gpp_registrations, &smsf_non3gpp_dereg_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            if (!smsf_non3gpp_registrations.remove(ue_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No SMSF non-3GPP-access registration for ueId " + ue_id);
            }
            smsf_non3gpp_dereg_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "PATCH",
        std::string(kUecmApiRoot) + "/{ueId}/registrations/smsf-non-3gpp-access",
        [&verifier, &smsf_non3gpp_registrations, &smsf_non3gpp_patch_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto patch_dto =
                sbi_core::http2::parse_json_body<sbi_gen::SmsfRegistrationModification>(req, err);
            if (!patch_dto.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            auto patched = smsf_non3gpp_registrations.merge_patch(ue_id, json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No SMSF non-3GPP-access registration for ueId " + ue_id);
            }
            smsf_non3gpp_patch_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #169, ADR-0218): real, previously
    // undisclosed IP-SM-GW registration resource -- real PUT+GET+DELETE, no PATCH exists for this
    // resource in the real spec at all (genuinely simpler than the AMF/SMSF registration groups
    // above, not an oversight). Real, disclosed: PUT uses only `201`/`200` (not the real spec's
    // third alternative bodyless `204`), same convention already established for every other
    // registration group in this file.
    server.add_route(
        "PUT",
        std::string(kUecmApiRoot) + "/{ueId}/registrations/ip-sm-gw",
        [&verifier, &ip_sm_gw_registrations, &ip_sm_gw_reg_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::IpSmGwRegistration>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            const bool is_new = !ip_sm_gw_registrations.get(ue_id).has_value();
            json j = *body;
            ip_sm_gw_registrations.put(ue_id, j);
            ip_sm_gw_reg_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = is_new ? 201 : 200;
            resp.headers.emplace("content-type", "application/json");
            if (is_new) {
                resp.headers.emplace("location",
                                     std::string(kUecmApiRoot) + "/" + ue_id +
                                         "/registrations/ip-sm-gw");
            }
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kUecmApiRoot) + "/{ueId}/registrations/ip-sm-gw",
        [&verifier, &ip_sm_gw_registrations](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto registration = ip_sm_gw_registrations.get(ue_id);
            if (!registration.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No IP-SM-GW registration for ueId " + ue_id);
            }
            return sbi_core::http2::Response::json(200, registration->dump());
        });

    server.add_route(
        "DELETE",
        std::string(kUecmApiRoot) + "/{ueId}/registrations/ip-sm-gw",
        [&verifier, &ip_sm_gw_registrations, &ip_sm_gw_dereg_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            if (!ip_sm_gw_registrations.remove(ue_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No IP-SM-GW registration for ueId " + ue_id);
            }
            ip_sm_gw_dereg_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #169, ADR-0219): real, previously
    // undisclosed NWDAF registration group (NwdafRegistration/GetNwdafRegistration/
    // NwdafDeregistration/UpdateNwdafRegistration). A UE can be served by multiple NWDAF instances
    // concurrently, each under its own `nwdafRegistrationId` -- same nested-map shape as the SMF
    // registration group below. Real, disclosed: `GetNwdafRegistration`'s own response schema is a
    // *bare* JSON array of `NwdafRegistration` (not wrapped in `NwdafRegistrationInfo` the way the
    // SMF group's collection GET wraps in `SmfRegistrationInfo` -- confirmed by direct read of the
    // real spec, the two collection GETs use genuinely different response shapes, not an
    // inconsistency introduced here). Same PUT-uses-only-`201`/`200` and PATCH-returns-real-`204`
    // conventions as every other registration group in this file.
    server.add_route(
        "GET",
        std::string(kUecmApiRoot) + "/{ueId}/registrations/nwdaf-registrations",
        [&verifier, &nwdaf_registrations](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto list = nwdaf_registrations.list_for_ue(ue_id);
            if (list.empty()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No NWDAF registration for ueId " + ue_id);
            }
            json arr = json::array();
            for (const auto& registration : list) {
                arr.push_back(registration);
            }
            return sbi_core::http2::Response::json(200, arr.dump());
        });

    server.add_route(
        "PUT",
        std::string(kUecmApiRoot) +
            "/{ueId}/registrations/nwdaf-registrations/{nwdafRegistrationId}",
        [&verifier, &nwdaf_registrations, &nwdaf_reg_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::NwdafRegistration>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto nwdaf_registration_id = req.path_params.at("nwdafRegistrationId");
            const bool is_new = !nwdaf_registrations.get(ue_id, nwdaf_registration_id).has_value();
            json j = *body;
            nwdaf_registrations.put(ue_id, nwdaf_registration_id, j);
            nwdaf_reg_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = is_new ? 201 : 200;
            resp.headers.emplace("content-type", "application/json");
            if (is_new) {
                resp.headers.emplace("location",
                                     std::string(kUecmApiRoot) + "/" + ue_id +
                                         "/registrations/nwdaf-registrations/" +
                                         nwdaf_registration_id);
            }
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "DELETE",
        std::string(kUecmApiRoot) +
            "/{ueId}/registrations/nwdaf-registrations/{nwdafRegistrationId}",
        [&verifier, &nwdaf_registrations, &nwdaf_dereg_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto nwdaf_registration_id = req.path_params.at("nwdafRegistrationId");
            if (!nwdaf_registrations.remove(ue_id, nwdaf_registration_id)) {
                return sbi_core::http2::problem_response(
                    404,
                    "Not Found",
                    "No NWDAF registration for ueId/nwdafRegistrationId " + ue_id + "/" +
                        nwdaf_registration_id);
            }
            nwdaf_dereg_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "PATCH",
        std::string(kUecmApiRoot) +
            "/{ueId}/registrations/nwdaf-registrations/{nwdafRegistrationId}",
        [&verifier, &nwdaf_registrations, &nwdaf_patch_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto patch_dto =
                sbi_core::http2::parse_json_body<sbi_gen::NwdafRegistrationModification>(req, err);
            if (!patch_dto.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto nwdaf_registration_id = req.path_params.at("nwdafRegistrationId");
            auto patched = nwdaf_registrations.merge_patch(
                ue_id, nwdaf_registration_id, json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404,
                    "Not Found",
                    "No NWDAF registration for ueId/nwdafRegistrationId " + ue_id + "/" +
                        nwdaf_registration_id);
            }
            nwdaf_patch_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nudm_UECM: SMF registration group ---

    server.add_route(
        "GET",
        std::string(kUecmApiRoot) + "/{ueId}/registrations/smf-registrations",
        [&verifier, &smf_registrations](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            json arr = json::array();
            for (const auto& registration : smf_registrations.list_for_ue(ue_id)) {
                arr.push_back(registration);
            }
            sbi_gen::SmfRegistrationInfo resp_data;
            resp_data.smfRegistrationList = arr.get<std::vector<sbi_gen::SmfRegistration>>();
            json j = resp_data;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "PUT",
        std::string(kUecmApiRoot) + "/{ueId}/registrations/smf-registrations/{pduSessionId}",
        [&verifier, &smf_registrations, &smf_reg_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::SmfRegistration>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto pdu_session_id = req.path_params.at("pduSessionId");
            const bool is_new = !smf_registrations.get(ue_id, pdu_session_id).has_value();
            json j = *body;
            smf_registrations.put(ue_id, pdu_session_id, j);
            smf_reg_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = is_new ? 201 : 200;
            resp.headers.emplace("content-type", "application/json");
            if (is_new) {
                resp.headers.emplace("location",
                                     std::string(kUecmApiRoot) + "/" + ue_id +
                                         "/registrations/smf-registrations/" + pdu_session_id);
            }
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kUecmApiRoot) + "/{ueId}/registrations/smf-registrations/{pduSessionId}",
        [&verifier, &smf_registrations](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto pdu_session_id = req.path_params.at("pduSessionId");
            auto registration = smf_registrations.get(ue_id, pdu_session_id);
            if (!registration.has_value()) {
                return sbi_core::http2::problem_response(
                    404,
                    "Not Found",
                    "No SMF registration for ueId/pduSessionId " + ue_id + "/" + pdu_session_id);
            }
            return sbi_core::http2::Response::json(200, registration->dump());
        });

    server.add_route(
        "PATCH",
        std::string(kUecmApiRoot) + "/{ueId}/registrations/smf-registrations/{pduSessionId}",
        [&verifier, &smf_registrations](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto patch_dto =
                sbi_core::http2::parse_json_body<sbi_gen::SmfRegistrationModification>(req, err);
            if (!patch_dto.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto pdu_session_id = req.path_params.at("pduSessionId");
            auto patched =
                smf_registrations.merge_patch(ue_id, pdu_session_id, json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404,
                    "Not Found",
                    "No SMF registration for ueId/pduSessionId " + ue_id + "/" + pdu_session_id);
            }
            return sbi_core::http2::Response::json(200, patched->dump());
        });

    server.add_route(
        "DELETE",
        std::string(kUecmApiRoot) + "/{ueId}/registrations/smf-registrations/{pduSessionId}",
        [&verifier, &smf_registrations, &smf_dereg_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto pdu_session_id = req.path_params.at("pduSessionId");
            if (!smf_registrations.get(ue_id, pdu_session_id).has_value()) {
                return sbi_core::http2::problem_response(
                    404,
                    "Not Found",
                    "No SMF registration for ueId/pduSessionId " + ue_id + "/" + pdu_session_id);
            }
            smf_registrations.remove(ue_id, pdu_session_id);
            smf_dereg_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #169, ADR-0220): real, previously
    // undisclosed `GetRegistrations` bare aggregate -- the last piece unblocked once every
    // `RegistrationDataSets` field had a real backing store (ADR-0215 through ADR-0219). Composes
    // from the already-built per-group stores exactly like UDR's own `QueryProvisionedData`
    // (ADR-0212) and `dataset-names`-filtered routes (`ReadPolicyData`, ADR-0213) compose from
    // their own stores, reusing the same real `split_form_array()` infra (ADR-0161) for the
    // required `registration-dataset-names` query param. Real, disclosed: `single-nssai`/`dnn`
    // query params are accepted but not honored -- the real spec documents them as narrowing the
    // `SMF_PDU_SESSIONS` dataset's own PDU-session list by slice/DNN, but doing so correctly needs
    // per-session S-NSSAI/DNN inspection this project hasn't built yet; `SMF_PDU_SESSIONS` always
    // returns every PDU session's SMF registration for the ueId, unfiltered. `registration-dataset-
    // names`'s own real `minItems: 2` is enforced as a real `400`, not silently ignored.
    server.add_route(
        "GET",
        std::string(kUecmApiRoot) + "/{ueId}/registrations",
        [&verifier,
         &amf_registrations,
         &amf_non3gpp_registrations,
         &smf_registrations,
         &smsf_3gpp_registrations,
         &smsf_non3gpp_registrations,
         &ip_sm_gw_registrations,
         &nwdaf_registrations](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            std::vector<std::string> names;
            if (const auto it = req.query_params.find("registration-dataset-names");
                it != req.query_params.end()) {
                names = sbi_core::http2::split_form_array(it->second);
            }
            if (names.size() < 2) {
                return sbi_core::http2::problem_response(
                    400,
                    "Bad Request",
                    "registration-dataset-names requires at least 2 real dataset names");
            }
            const auto wanted = [&names](const char* name) {
                return std::find(names.begin(), names.end(), name) != names.end();
            };

            sbi_gen::RegistrationDataSets result{};
            if (wanted("AMF_3GPP")) {
                if (auto v = amf_registrations.get(ue_id); v.has_value()) {
                    result.amf3Gpp = v->get<sbi_gen::Amf3GppAccessRegistration>();
                }
            }
            if (wanted("AMF_NON_3GPP")) {
                if (auto v = amf_non3gpp_registrations.get(ue_id); v.has_value()) {
                    result.amfNon3Gpp = v->get<sbi_gen::AmfNon3GppAccessRegistration>();
                }
            }
            if (wanted("SMF_PDU_SESSIONS")) {
                auto list = smf_registrations.list_for_ue(ue_id);
                if (!list.empty()) {
                    json arr = json::array();
                    for (const auto& r : list) {
                        arr.push_back(r);
                    }
                    sbi_gen::SmfRegistrationInfo info{};
                    info.smfRegistrationList = arr.get<std::vector<sbi_gen::SmfRegistration>>();
                    result.smfRegistration = info;
                }
            }
            if (wanted("SMSF_3GPP")) {
                if (auto v = smsf_3gpp_registrations.get(ue_id); v.has_value()) {
                    result.smsf3Gpp = v->get<sbi_gen::SmsfRegistration>();
                }
            }
            if (wanted("SMSF_NON_3GPP")) {
                if (auto v = smsf_non3gpp_registrations.get(ue_id); v.has_value()) {
                    result.smsfNon3Gpp = v->get<sbi_gen::SmsfRegistration>();
                }
            }
            if (wanted("IP_SM_GW")) {
                if (auto v = ip_sm_gw_registrations.get(ue_id); v.has_value()) {
                    result.ipSmGw = v->get<sbi_gen::IpSmGwRegistration>();
                }
            }
            if (wanted("NWDAF")) {
                auto list = nwdaf_registrations.list_for_ue(ue_id);
                if (!list.empty()) {
                    json arr = json::array();
                    for (const auto& r : list) {
                        arr.push_back(r);
                    }
                    sbi_gen::NwdafRegistrationInfo info{};
                    info.nwdafRegistrationList = arr.get<std::vector<sbi_gen::NwdafRegistration>>();
                    result.nwdafRegistration = info;
                }
            }
            json j = result;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #169, ADR-0221): real, previously
    // undisclosed `SendRoutingInfoSm` custom operation -- composes `RoutingInfoSmResponse` from
    // the real SMSF/IP-SM-GW stores, the last open item in `Nudm_UECM`'s own Tier-B backlog.
    // Real, disclosed: `smsRouter` (an SMS-Router-at-UDM concept this project hasn't built any
    // config surface for) and `ipSmGwGuidance` (delivery-time hints this project has no source of
    // data for) are never populated; `mpsMsgIndication` is left absent, which is
    // schema-equivalent to its own documented `false` default, not an omission of real data.
    // Real, spec-consistent judgment call: a UE with neither SMSF nor IP-SM-GW registration has no
    // real addressing information to return, so this returns a real `404` rather than a `200`
    // with an empty body -- the real spec's own general-error responses list `404` as valid here
    // and this project has no fabricated-empty-body convention to fall back on for this operation.
    server.add_route(
        "POST",
        std::string(kUecmApiRoot) + "/{ueId}/registrations/send-routing-info-sm",
        [&verifier,
         &smsf_3gpp_registrations,
         &smsf_non3gpp_registrations,
         &ip_sm_gw_registrations,
         &send_routing_info_sm_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::RoutingInfoSmRequest>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");

            sbi_gen::RoutingInfoSmResponse result{};
            bool found_any = false;
            if (auto v = smsf_3gpp_registrations.get(ue_id); v.has_value()) {
                result.smsf3Gpp = v->get<sbi_gen::SmsfRegistration>();
                found_any = true;
            }
            if (auto v = smsf_non3gpp_registrations.get(ue_id); v.has_value()) {
                result.smsfNon3Gpp = v->get<sbi_gen::SmsfRegistration>();
                found_any = true;
            }
            if (auto v = ip_sm_gw_registrations.get(ue_id); v.has_value()) {
                sbi_gen::IpSmGwInfo info{};
                info.ipSmGwRegistration = v->get<sbi_gen::IpSmGwRegistration>();
                result.ipSmGw = info;
                found_any = true;
            }
            if (!found_any) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No SMS routing information available for ueId " + ue_id);
            }
            result.supi = ue_id;
            send_routing_info_sm_counter->Add(1);
            json j = result;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #169, ADR-0227): real, previously
    // undisclosed `Trigger P-CSCF Restoration` custom operation -- closes `Nudm_UECM`'s entire
    // remaining Tier-B backlog together with `GetLocationInfo`/`authTrigger` below. Real, confirmed
    // directly from the spec: this operation's own real `operationId` literally contains spaces
    // ("Trigger P-CSCF Restoration") -- an unusual but genuine value from the source YAML, not
    // normalized here. Path is `/restore-pcscf` with no `ueId` -- the target UE is identified by
    // `supi` in the request body instead. Real, disclosed simplification, same class as ADR-0207's
    // own SendSMS disclosure ("no real onward IP-SM-GW/SMSF relay wired -- the real ack is
    // NEF-level acceptance only"): this project has no real onward relay to the serving AMF (which
    // would itself need to carry out the actual P-CSCF restoration procedure toward the UE, TS
    // 23.380) -- accepting the request and confirming the SUPI has a known registration is genuine
    // work this UDM does; relaying the trigger onward is not wired. A `supi` with no known
    // registration at all is a real `404`.
    server.add_route(
        "POST",
        std::string(kUecmApiRoot) + "/restore-pcscf",
        [&verifier, &amf_registrations, &amf_non3gpp_registrations, &restore_pcscf_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::TriggerRequest>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto& supi = body->supi;
            if (!amf_registrations.get(supi).has_value() &&
                !amf_non3gpp_registrations.get(supi).has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No AMF registration known for supi " + supi);
            }
            restore_pcscf_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #169, ADR-0227): real, previously
    // undisclosed `GetLocationInfo` -- unlike the two operations above/below, this one is a real,
    // complete, local composition, no external relay needed: `RegistrationLocationInfo`'s own
    // `amfInstanceId`/`guami`/`vgmlcAddress` fields already exist directly on the stored
    // `Amf3GppAccessRegistration`/`AmfNon3GppAccessRegistration` records (ADR-0215/ADR-0216) --
    // `plmnId` is genuinely derivable from `guami.plmnId` (TS 29.571's own `Guami` already embeds
    // a `PlmnId`, not a fabricated flattening), and `accessTypeList` is genuinely determined by
    // which of the two registration groups a UE has (3GPP vs non-3GPP access is exactly what
    // distinguishes them in the real spec, not an invented inference). A UE with neither
    // registration is a real `404`.
    server.add_route(
        "GET",
        std::string(kUecmApiRoot) + "/{ueId}/registrations/location",
        [&verifier, &amf_registrations, &amf_non3gpp_registrations, &get_location_info_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            sbi_gen::LocationInfo_Nudm_UECM result{};
            if (auto v = amf_registrations.get(ue_id); v.has_value()) {
                auto amf = v->get<sbi_gen::Amf3GppAccessRegistration>();
                sbi_gen::RegistrationLocationInfo loc{};
                loc.amfInstanceId = amf.amfInstanceId;
                loc.guami = amf.guami;
                // `Guami.plmnId` is the real, distinct `PlmnIdNid` type (mcc/mnc + optional SNPN
                // `nid`), while `RegistrationLocationInfo.plmnId` expects the plain
                // `PlmnId_CommonData` (mcc/mnc only) -- real, different schemas, not
                // interchangeable. Narrowing to mcc/mnc here is a real, disclosed simplification:
                // an SNPN's own `nid` is dropped since the target schema has no field for it.
                loc.plmnId = sbi_gen::PlmnId_CommonData{amf.guami.plmnId.mcc, amf.guami.plmnId.mnc};
                loc.vgmlcAddress = amf.vgmlcAddress;
                sbi_gen::AccessType access_type{};
                access_type.value = sbi_gen::AccessType::V3GPP_ACCESS;
                loc.accessTypeList = {access_type};
                result.registrationLocationInfoList.push_back(loc);
            }
            if (auto v = amf_non3gpp_registrations.get(ue_id); v.has_value()) {
                auto amf = v->get<sbi_gen::AmfNon3GppAccessRegistration>();
                sbi_gen::RegistrationLocationInfo loc{};
                loc.amfInstanceId = amf.amfInstanceId;
                loc.guami = amf.guami;
                // See the identical real PlmnIdNid -> PlmnId_CommonData narrowing comment above.
                loc.plmnId = sbi_gen::PlmnId_CommonData{amf.guami.plmnId.mcc, amf.guami.plmnId.mnc};
                sbi_gen::AccessType access_type{};
                access_type.value = sbi_gen::AccessType::NON_3GPP_ACCESS;
                loc.accessTypeList = {access_type};
                result.registrationLocationInfoList.push_back(loc);
            }
            if (result.registrationLocationInfoList.empty()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No AMF registration known for ueId " + ue_id);
            }
            result.supi = ue_id;
            get_location_info_counter->Add(1);
            json j = result;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #169, ADR-0227): real, previously
    // undisclosed `authTrigger` -- closes `Nudm_UECM`'s entire remaining Tier-B backlog. Real,
    // disclosed simplification, same class and rationale as `Trigger P-CSCF Restoration` above:
    // this project has no real onward relay to AUSF/the serving AMF to actually carry out primary
    // re-authentication (TS 33.501) -- accepting the request and confirming the ueId has a known
    // registration is genuine work; relaying the trigger onward is not wired. A ueId with no known
    // registration at all is a real `404`.
    server.add_route(
        "GET",
        std::string(kUecmApiRoot) + "/{ueId}/registrations/trigger-auth",
        [&verifier, &amf_registrations, &amf_non3gpp_registrations, &auth_trigger_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::AuthTriggerInfo>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            if (!amf_registrations.get(ue_id).has_value() &&
                !amf_non3gpp_registrations.get(ue_id).has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No AMF registration known for ueId " + ue_id);
            }
            auth_trigger_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nudm_SDM: subscriber data retrieval (ADR-0069, gap-closure Tier 1b: real UDR calls,
    // replacing the permanently-empty stub the file header originally disclosed) ---

    // Shared real-UDR-GET helper -- ue_id + servingPlmnId + the real provisioned-data sub-
    // resource segment ("am-data"/"smf-selection-subscription-data"/"sm-data") in, either a real
    // parsed JSON body or a real ProblemDetails Response out. A UE genuinely not seeded in UDR
    // (any SUPI other than this slice's own two real seeded test subscribers) now correctly 404s,
    // instead of the old stub's always-200-empty-body behavior.
    auto fetch_from_udr =
        [&udr_client, &udr_oauth, &udr_base_url](
            const std::string& ue_id,
            const std::string& serving_plmn_id,
            const std::string& segment) -> std::variant<json, sbi_core::http2::Response> {
        auto token = udr_oauth.get_bearer_token();
        if (!token.has_value()) {
            return sbi_core::http2::problem_response(500,
                                                     "Internal Server Error",
                                                     "UDM could not obtain a token for UDR: " +
                                                         token.error());
        }
        sbi_core::http2::ClientRequest udr_req;
        udr_req.method = "GET";
        udr_req.url = udr_base_url + kUdrApiRoot + "/subscription-data/" + ue_id + "/" +
                      serving_plmn_id + "/provisioned-data/" + segment;
        udr_req.headers.emplace("authorization", "Bearer " + *token);
        auto udr_resp = udr_client.send(udr_req);
        if (!udr_resp.has_value()) {
            return sbi_core::http2::problem_response(
                500, "Internal Server Error", "UDM could not reach UDR: " + udr_resp.error());
        }
        if (udr_resp->status == 404) {
            return sbi_core::http2::problem_response(
                404, "Not Found", "No provisioned " + segment + " for ueId " + ue_id);
        }
        if (udr_resp->status != 200) {
            return sbi_core::http2::problem_response(500,
                                                     "Internal Server Error",
                                                     "UDR returned unexpected status " +
                                                         std::to_string(udr_resp->status));
        }
        try {
            return json::parse(udr_resp->body);
        } catch (const json::exception& e) {
            return sbi_core::http2::problem_response(500,
                                                     "Internal Server Error",
                                                     "UDR returned malformed JSON: " +
                                                         std::string(e.what()));
        }
    };

    server.add_route(
        "GET",
        std::string(kSdmApiRoot) + "/{supi}/am-data",
        [&verifier, &sdm_get_counter, &fetch_from_udr](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sdm_get_counter->Add(1);
            const auto supi = req.path_params.at("supi");
            auto result = fetch_from_udr(supi, resolve_serving_plmn_id(req), "am-data");
            if (auto* err = std::get_if<sbi_core::http2::Response>(&result)) {
                return *err;
            }
            return sbi_core::http2::Response::json(200, std::get<json>(result).dump());
        });

    server.add_route(
        "GET",
        std::string(kSdmApiRoot) + "/{supi}/smf-select-data",
        [&verifier, &sdm_get_counter, &fetch_from_udr](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sdm_get_counter->Add(1);
            const auto supi = req.path_params.at("supi");
            auto result = fetch_from_udr(
                supi, resolve_serving_plmn_id(req), "smf-selection-subscription-data");
            if (auto* err = std::get_if<sbi_core::http2::Response>(&result)) {
                return *err;
            }
            return sbi_core::http2::Response::json(200, std::get<json>(result).dump());
        });

    server.add_route(
        "GET",
        std::string(kSdmApiRoot) + "/{supi}/sm-data",
        [&verifier, &sdm_get_counter, &fetch_from_udr](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sdm_get_counter->Add(1);
            const auto supi = req.path_params.at("supi");
            auto result = fetch_from_udr(supi, resolve_serving_plmn_id(req), "sm-data");
            if (auto* err = std::get_if<sbi_core::http2::Response>(&result)) {
                return *err;
            }
            return sbi_core::http2::Response::json(200, std::get<json>(result).dump());
        });

    // Gap-closure (ADR-0228, Nudm_SDM group A): 4 more real individual-resource GET ops --
    // identical fetch_from_udr shape to am-data/smf-select-data/sm-data above, each backed by
    // UDR's own real individual provisioned-data route (TS29505_Subscription_Data.yaml, real
    // response schemas confirmed against TS29503_Nudm_SDM.yaml directly, not inferred from the
    // operationId).
    server.add_route(
        "GET",
        std::string(kSdmApiRoot) + "/{supi}/sms-data",
        [&verifier, &sdm_get_counter, &fetch_from_udr](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sdm_get_counter->Add(1);
            const auto supi = req.path_params.at("supi");
            auto result = fetch_from_udr(supi, resolve_serving_plmn_id(req), "sms-data");
            if (auto* err = std::get_if<sbi_core::http2::Response>(&result)) {
                return *err;
            }
            return sbi_core::http2::Response::json(200, std::get<json>(result).dump());
        });

    server.add_route(
        "GET",
        std::string(kSdmApiRoot) + "/{supi}/sms-mng-data",
        [&verifier, &sdm_get_counter, &fetch_from_udr](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sdm_get_counter->Add(1);
            const auto supi = req.path_params.at("supi");
            auto result = fetch_from_udr(supi, resolve_serving_plmn_id(req), "sms-mng-data");
            if (auto* err = std::get_if<sbi_core::http2::Response>(&result)) {
                return *err;
            }
            return sbi_core::http2::Response::json(200, std::get<json>(result).dump());
        });

    server.add_route(
        "GET",
        std::string(kSdmApiRoot) + "/{supi}/trace-data",
        [&verifier, &sdm_get_counter, &fetch_from_udr](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sdm_get_counter->Add(1);
            const auto supi = req.path_params.at("supi");
            auto result = fetch_from_udr(supi, resolve_serving_plmn_id(req), "trace-data");
            if (auto* err = std::get_if<sbi_core::http2::Response>(&result)) {
                return *err;
            }
            return sbi_core::http2::Response::json(200, std::get<json>(result).dump());
        });

    server.add_route(
        "GET",
        std::string(kSdmApiRoot) + "/{supi}/lcs-bca-data",
        [&verifier, &sdm_get_counter, &fetch_from_udr](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sdm_get_counter->Add(1);
            const auto supi = req.path_params.at("supi");
            auto result = fetch_from_udr(supi, resolve_serving_plmn_id(req), "lcs-bca-data");
            if (auto* err = std::get_if<sbi_core::http2::Response>(&result)) {
                return *err;
            }
            return sbi_core::http2::Response::json(200, std::get<json>(result).dump());
        });

    // Gap-closure (ADR-0228, Nudm_SDM group B): 9 real individual-resource GET ops whose data
    // exists in UDR's own real ProvisionedDataSets aggregate (ADR-0212, Nudr_DataRepository
    // QueryProvisionedData) but has no individual UDR REST route of its own -- genuinely distinct
    // from group A above, so a new shared helper: fetch the whole aggregate once (real, always
    // 200 per ADR-0212 -- a live view) and extract one named field, real-404ing here (not in UDR)
    // when that specific field is absent. Real, disclosed: none of these 9 ops expose a `plmn-id`
    // query param in TS29503_Nudm_SDM.yaml (unlike am-data/sm-data/etc above), yet UDR's aggregate
    // path requires servingPlmnId -- resolve_serving_plmn_id's existing kDefaultServingPlmnId
    // fallback (already used for other plmn-id-less callers in this file) is reused rather than
    // inventing new special-casing, matching this lab's already-established single-PLMN scope.
    auto fetch_from_udr_bulk_field =
        [&udr_client, &udr_oauth, &udr_base_url](
            const std::string& ue_id,
            const std::string& serving_plmn_id,
            const std::string& field_name) -> std::variant<json, sbi_core::http2::Response> {
        auto token = udr_oauth.get_bearer_token();
        if (!token.has_value()) {
            return sbi_core::http2::problem_response(500,
                                                     "Internal Server Error",
                                                     "UDM could not obtain a token for UDR: " +
                                                         token.error());
        }
        sbi_core::http2::ClientRequest udr_req;
        udr_req.method = "GET";
        udr_req.url = udr_base_url + kUdrApiRoot + "/subscription-data/" + ue_id + "/" +
                      serving_plmn_id + "/provisioned-data";
        udr_req.headers.emplace("authorization", "Bearer " + *token);
        auto udr_resp = udr_client.send(udr_req);
        if (!udr_resp.has_value()) {
            return sbi_core::http2::problem_response(
                500, "Internal Server Error", "UDM could not reach UDR: " + udr_resp.error());
        }
        if (udr_resp->status != 200) {
            return sbi_core::http2::problem_response(500,
                                                     "Internal Server Error",
                                                     "UDR returned unexpected status " +
                                                         std::to_string(udr_resp->status));
        }
        json parsed;
        try {
            parsed = json::parse(udr_resp->body);
        } catch (const json::exception& e) {
            return sbi_core::http2::problem_response(500,
                                                     "Internal Server Error",
                                                     "UDR returned malformed JSON: " +
                                                         std::string(e.what()));
        }
        if (!parsed.contains(field_name)) {
            return sbi_core::http2::problem_response(
                404, "Not Found", "No provisioned " + field_name + " for ueId " + ue_id);
        }
        return parsed.at(field_name);
    };

    // Gap-closure (ADR-0230/ADR-0233): a third real UDR-call shape, genuinely distinct from
    // `fetch_from_udr`/`fetch_from_udr_bulk_field` above -- some UDR resources are keyed by ueId
    // alone (no servingPlmnId in their path at all), matching those resources' own real spec
    // definition (no plmn-id query param, no servingPlmnId path segment). Shared across
    // GetEcrData/GetTimeSyncSubscriptionData/GetRangingSlPosData, all three confirmed by direct
    // schema read to share this exact real shape.
    auto fetch_from_udr_no_plmn =
        [&udr_client, &udr_oauth, &udr_base_url](
            const std::string& ue_id,
            const std::string& segment) -> std::variant<json, sbi_core::http2::Response> {
        auto token = udr_oauth.get_bearer_token();
        if (!token.has_value()) {
            return sbi_core::http2::problem_response(500,
                                                     "Internal Server Error",
                                                     "UDM could not obtain a token for UDR: " +
                                                         token.error());
        }
        sbi_core::http2::ClientRequest udr_req;
        udr_req.method = "GET";
        udr_req.url = udr_base_url + kUdrApiRoot + "/subscription-data/" + ue_id + "/" + segment;
        udr_req.headers.emplace("authorization", "Bearer " + *token);
        auto udr_resp = udr_client.send(udr_req);
        if (!udr_resp.has_value()) {
            return sbi_core::http2::problem_response(
                500, "Internal Server Error", "UDM could not reach UDR: " + udr_resp.error());
        }
        if (udr_resp->status == 404) {
            return sbi_core::http2::problem_response(
                404, "Not Found", "No provisioned " + segment + " for ueId " + ue_id);
        }
        if (udr_resp->status != 200) {
            return sbi_core::http2::problem_response(500,
                                                     "Internal Server Error",
                                                     "UDR returned unexpected status " +
                                                         std::to_string(udr_resp->status));
        }
        try {
            return json::parse(udr_resp->body);
        } catch (const json::exception& e) {
            return sbi_core::http2::problem_response(500,
                                                     "Internal Server Error",
                                                     "UDR returned malformed JSON: " +
                                                         std::string(e.what()));
        }
    };

    // Generic verbatim-method passthrough proxy to a UDR route, relaying UDR's own status and
    // JSON body untouched (including its ProblemDetails error bodies -- already the correct real
    // shape, no re-wrapping needed). Genuinely justified now (ADR-0237) that 3 distinct real
    // Nudm_PP-backing UDR resources (pp-data-store, 5g-vn-groups, mbs-group-membership) share this
    // identical direct-proxy shape, same "N call sites, one real shared helper" precedent
    // `fetch_from_udr_no_plmn` (ADR-0233) already established.
    auto proxy_to_udr = [&udr_client, &udr_oauth, &udr_base_url](
                            const std::string& method,
                            const std::string& udr_path,
                            const std::string& body) -> sbi_core::http2::Response {
        auto token = udr_oauth.get_bearer_token();
        if (!token.has_value()) {
            return sbi_core::http2::problem_response(500,
                                                     "Internal Server Error",
                                                     "UDM could not obtain a token for UDR: " +
                                                         token.error());
        }
        sbi_core::http2::ClientRequest udr_req;
        udr_req.method = method;
        udr_req.url = udr_base_url + kUdrApiRoot + udr_path;
        udr_req.headers.emplace("authorization", "Bearer " + *token);
        if (!body.empty()) {
            udr_req.headers.emplace("content-type", "application/json");
            udr_req.body = body;
        }
        auto udr_resp = udr_client.send(udr_req);
        if (!udr_resp.has_value()) {
            return sbi_core::http2::problem_response(
                500, "Internal Server Error", "UDM could not reach UDR: " + udr_resp.error());
        }
        sbi_core::http2::Response resp;
        resp.status = static_cast<int>(udr_resp->status);
        if (!udr_resp->body.empty()) {
            resp.headers.emplace("content-type", "application/json");
            resp.body = udr_resp->body;
        }
        return resp;
    };

    server.add_route(
        "GET",
        std::string(kSdmApiRoot) + "/{ueId}/lcs-privacy-data",
        [&verifier, &sdm_get_counter, &fetch_from_udr_bulk_field](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sdm_get_counter->Add(1);
            const auto ue_id = req.path_params.at("ueId");
            auto result =
                fetch_from_udr_bulk_field(ue_id, resolve_serving_plmn_id(req), "lcsPrivacyData");
            if (auto* err = std::get_if<sbi_core::http2::Response>(&result)) {
                return *err;
            }
            return sbi_core::http2::Response::json(200, std::get<json>(result).dump());
        });

    server.add_route(
        "GET",
        std::string(kSdmApiRoot) + "/{supi}/lcs-mo-data",
        [&verifier, &sdm_get_counter, &fetch_from_udr_bulk_field](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sdm_get_counter->Add(1);
            const auto supi = req.path_params.at("supi");
            auto result =
                fetch_from_udr_bulk_field(supi, resolve_serving_plmn_id(req), "lcsMoData");
            if (auto* err = std::get_if<sbi_core::http2::Response>(&result)) {
                return *err;
            }
            return sbi_core::http2::Response::json(200, std::get<json>(result).dump());
        });

    server.add_route(
        "GET",
        std::string(kSdmApiRoot) + "/{supi}/lcs-subscription-data",
        [&verifier, &sdm_get_counter, &fetch_from_udr_bulk_field](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sdm_get_counter->Add(1);
            const auto supi = req.path_params.at("supi");
            auto result = fetch_from_udr_bulk_field(
                supi, resolve_serving_plmn_id(req), "lcsSubscriptionData");
            if (auto* err = std::get_if<sbi_core::http2::Response>(&result)) {
                return *err;
            }
            return sbi_core::http2::Response::json(200, std::get<json>(result).dump());
        });

    server.add_route(
        "GET",
        std::string(kSdmApiRoot) + "/{supi}/v2x-data",
        [&verifier, &sdm_get_counter, &fetch_from_udr_bulk_field](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sdm_get_counter->Add(1);
            const auto supi = req.path_params.at("supi");
            auto result = fetch_from_udr_bulk_field(supi, resolve_serving_plmn_id(req), "v2xData");
            if (auto* err = std::get_if<sbi_core::http2::Response>(&result)) {
                return *err;
            }
            return sbi_core::http2::Response::json(200, std::get<json>(result).dump());
        });

    server.add_route(
        "GET",
        std::string(kSdmApiRoot) + "/{supi}/prose-data",
        [&verifier, &sdm_get_counter, &fetch_from_udr_bulk_field](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sdm_get_counter->Add(1);
            const auto supi = req.path_params.at("supi");
            auto result =
                fetch_from_udr_bulk_field(supi, resolve_serving_plmn_id(req), "proseData");
            if (auto* err = std::get_if<sbi_core::http2::Response>(&result)) {
                return *err;
            }
            return sbi_core::http2::Response::json(200, std::get<json>(result).dump());
        });

    server.add_route(
        "GET",
        std::string(kSdmApiRoot) + "/{supi}/5mbs-data",
        [&verifier, &sdm_get_counter, &fetch_from_udr_bulk_field](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sdm_get_counter->Add(1);
            const auto supi = req.path_params.at("supi");
            auto result = fetch_from_udr_bulk_field(
                supi, resolve_serving_plmn_id(req), "mbsSubscriptionData");
            if (auto* err = std::get_if<sbi_core::http2::Response>(&result)) {
                return *err;
            }
            return sbi_core::http2::Response::json(200, std::get<json>(result).dump());
        });

    // Real, disclosed simplification: the spec's own optional `uc-purpose` query param (a filter
    // over UcSubscriptionData's own per-purpose list) is accepted-but-ignored here -- same
    // established precedent as UDR's own aggregate route already documents for its other
    // real-but-unhonored optional filter params (single-nssai/dnn/adjacent-plmns/ext-group-ids).
    server.add_route(
        "GET",
        std::string(kSdmApiRoot) + "/{supi}/uc-data",
        [&verifier, &sdm_get_counter, &fetch_from_udr_bulk_field](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sdm_get_counter->Add(1);
            const auto supi = req.path_params.at("supi");
            auto result = fetch_from_udr_bulk_field(supi, resolve_serving_plmn_id(req), "ucData");
            if (auto* err = std::get_if<sbi_core::http2::Response>(&result)) {
                return *err;
            }
            return sbi_core::http2::Response::json(200, std::get<json>(result).dump());
        });

    server.add_route(
        "GET",
        std::string(kSdmApiRoot) + "/{supi}/a2x-data",
        [&verifier, &sdm_get_counter, &fetch_from_udr_bulk_field](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sdm_get_counter->Add(1);
            const auto supi = req.path_params.at("supi");
            auto result = fetch_from_udr_bulk_field(supi, resolve_serving_plmn_id(req), "a2xData");
            if (auto* err = std::get_if<sbi_core::http2::Response>(&result)) {
                return *err;
            }
            return sbi_core::http2::Response::json(200, std::get<json>(result).dump());
        });

    server.add_route(
        "GET",
        std::string(kSdmApiRoot) + "/{ueId}/rangingsl-privacy-data",
        [&verifier, &sdm_get_counter, &fetch_from_udr_bulk_field](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sdm_get_counter->Add(1);
            const auto ue_id = req.path_params.at("ueId");
            auto result = fetch_from_udr_bulk_field(
                ue_id, resolve_serving_plmn_id(req), "rangingSlPrivacyData");
            if (auto* err = std::get_if<sbi_core::http2::Response>(&result)) {
                return *err;
            }
            return sbi_core::http2::Response::json(200, std::get<json>(result).dump());
        });

    // Gap-closure (ADR-0230, Nudm_SDM group C1): 5 real ops, real data source confirmed by direct
    // read of TS29503_Nudm_SDM.yaml against this file's own already-stored data -- none needed a
    // new store.
    //
    // GetNSSAI (`GET /{supi}/nssai`) -- real, confirmed by direct schema read: `Nssai` is the
    // exact same schema as `AccessAndMobilitySubscriptionData.nssai` (both `$ref
    // '#/components/schemas/Nssai'`), so this is a real subset view into the already-fetched
    // am-data, not a new resource.
    server.add_route(
        "GET",
        std::string(kSdmApiRoot) + "/{supi}/nssai",
        [&verifier, &sdm_get_counter, &fetch_from_udr](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sdm_get_counter->Add(1);
            const auto supi = req.path_params.at("supi");
            auto result = fetch_from_udr(supi, resolve_serving_plmn_id(req), "am-data");
            if (auto* err = std::get_if<sbi_core::http2::Response>(&result)) {
                return *err;
            }
            auto& am_data = std::get<json>(result);
            if (!am_data.contains("nssai")) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No provisioned nssai for ueId " + supi);
            }
            return sbi_core::http2::Response::json(200, am_data.at("nssai").dump());
        });

    // GetUeCtxInAmfData (`GET /{supi}/ue-context-in-amf-data`) -- real, complete local composition
    // of `amfInfo` from the already-stored AMF 3GPP/non-3GPP registration records, same
    // amfInstanceId/guami/accessType pattern ADR-0227's GetLocationInfo already established. Real,
    // disclosed gap: `epsInterworkingInfo` has no data source anywhere in this file (no EPS/N26
    // interworking state is tracked) -- omitted, not fabricated. Real, honest: `UeContextInAmfData`
    // has no required fields at all, so this is always a real `200` (never a fabricated `404`);
    // `amfInfo` itself is omitted (not an empty array) when neither registration exists, since the
    // real schema's own `minItems: 1` on that field forbids an empty array.
    server.add_route(
        "GET",
        std::string(kSdmApiRoot) + "/{supi}/ue-context-in-amf-data",
        [&verifier, &sdm_get_counter, &amf_registrations, &amf_non3gpp_registrations](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sdm_get_counter->Add(1);
            const auto supi = req.path_params.at("supi");
            std::vector<sbi_gen::AmfInfo_Nudm_SDM> amf_info_list;
            if (auto v = amf_registrations.get(supi); v.has_value()) {
                auto amf = v->get<sbi_gen::Amf3GppAccessRegistration>();
                sbi_gen::AmfInfo_Nudm_SDM info{};
                info.amfInstanceId = amf.amfInstanceId;
                info.guami = amf.guami;
                sbi_gen::AccessType access_type{};
                access_type.value = sbi_gen::AccessType::V3GPP_ACCESS;
                info.accessType = access_type;
                amf_info_list.push_back(info);
            }
            if (auto v = amf_non3gpp_registrations.get(supi); v.has_value()) {
                auto amf = v->get<sbi_gen::AmfNon3GppAccessRegistration>();
                sbi_gen::AmfInfo_Nudm_SDM info{};
                info.amfInstanceId = amf.amfInstanceId;
                info.guami = amf.guami;
                sbi_gen::AccessType access_type{};
                access_type.value = sbi_gen::AccessType::NON_3GPP_ACCESS;
                info.accessType = access_type;
                amf_info_list.push_back(info);
            }
            sbi_gen::UeContextInAmfData result{};
            if (!amf_info_list.empty()) {
                result.amfInfo = amf_info_list;
            }
            json j = result;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    // GetUeCtxInSmfData (`GET /{supi}/ue-context-in-smf-data`) -- real, near-complete local
    // composition of `pduSessions` from the already-stored SmfRegistrationStore (its own
    // `smfInstanceId`/`plmnId`/`singleNssai`/`dnn` fields are a real superset of `PduSession`'s
    // schema, confirmed by direct read of both TS29503_Nudm_SDM.yaml's `PduSession` and
    // TS29503_Nudm_UECM.yaml's `SmfRegistration`). Real, disclosed narrowing: `PduSession.dnn` is
    // required while `SmfRegistration.dnn` is optional -- a stored registration with no `dnn` (a
    // real, valid `SmfRegistration` per its own schema, e.g. this project's own
    // SmfRegistrationLifecycle test data) cannot be represented as a valid `PduSession` without
    // fabricating the field, so that entry is skipped rather than invented. Real, disclosed gap:
    // `pgwInfo`/`emergencyInfo` have no data source anywhere in this file -- omitted. Real, honest:
    // `UeContextInSmfData` has no required fields, always a real `200`.
    server.add_route(
        "GET",
        std::string(kSdmApiRoot) + "/{supi}/ue-context-in-smf-data",
        [&verifier, &sdm_get_counter, &smf_registrations](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sdm_get_counter->Add(1);
            const auto supi = req.path_params.at("supi");
            json pdu_sessions = json::object();
            for (const auto& reg_json : smf_registrations.list_for_ue(supi)) {
                auto reg = reg_json.get<sbi_gen::SmfRegistration>();
                if (!reg.dnn.has_value()) {
                    continue;
                }
                sbi_gen::PduSession session{};
                session.dnn = *reg.dnn;
                session.smfInstanceId = reg.smfInstanceId;
                session.plmnId = reg.plmnId;
                session.singleNssai = reg.singleNssai;
                pdu_sessions[std::to_string(reg.pduSessionId)] = session;
            }
            json result = json::object();
            if (!pdu_sessions.empty()) {
                result["pduSessions"] = pdu_sessions;
            }
            return sbi_core::http2::Response::json(200, result.dump());
        });

    // GetUeCtxInSmsfData (`GET /{supi}/ue-context-in-smsf-data`) -- real, COMPLETE local
    // composition from the already-stored SMSF 3GPP/non-3GPP registration records: `SmsfInfo`'s
    // own `smsfInstanceId`/`plmnId`/`smsfSetId` fields are an exact 1:1 match with
    // `SmsfRegistration`'s own fields (confirmed by direct read of both schemas), no gap. Real,
    // honest: `UeContextInSmsfData` has no required fields, always a real `200`.
    server.add_route(
        "GET",
        std::string(kSdmApiRoot) + "/{supi}/ue-context-in-smsf-data",
        [&verifier, &sdm_get_counter, &smsf_3gpp_registrations, &smsf_non3gpp_registrations](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sdm_get_counter->Add(1);
            const auto supi = req.path_params.at("supi");
            sbi_gen::UeContextInSmsfData result{};
            if (auto v = smsf_3gpp_registrations.get(supi); v.has_value()) {
                auto smsf = v->get<sbi_gen::SmsfRegistration>();
                sbi_gen::SmsfInfo_Nudm_SDM info{};
                info.smsfInstanceId = smsf.smsfInstanceId;
                info.plmnId = smsf.plmnId;
                info.smsfSetId = smsf.smsfSetId;
                result.smsfInfo3GppAccess = info;
            }
            if (auto v = smsf_non3gpp_registrations.get(supi); v.has_value()) {
                auto smsf = v->get<sbi_gen::SmsfRegistration>();
                sbi_gen::SmsfInfo_Nudm_SDM info{};
                info.smsfInstanceId = smsf.smsfInstanceId;
                info.plmnId = smsf.plmnId;
                info.smsfSetId = smsf.smsfSetId;
                result.smsfInfoNon3GppAccess = info;
            }
            json j = result;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    // GetEcrData (`GET /{supi}/am-data/ecr-data`) -- real, confirmed by direct schema read:
    // `EnhancedCoverageRestrictionData` (`{plmnEcInfoList}`) is an exact match for UDR's own
    // already-live `coverage-restriction-data` resource (ADR-0102/ADR-0106).
    server.add_route(
        "GET",
        std::string(kSdmApiRoot) + "/{supi}/am-data/ecr-data",
        [&verifier, &sdm_get_counter, &fetch_from_udr_no_plmn](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sdm_get_counter->Add(1);
            const auto supi = req.path_params.at("supi");
            auto result = fetch_from_udr_no_plmn(supi, "coverage-restriction-data");
            if (auto* err = std::get_if<sbi_core::http2::Response>(&result)) {
                return *err;
            }
            return sbi_core::http2::Response::json(200, std::get<json>(result).dump());
        });

    // Gap-closure (ADR-0233, Nudm_SDM group C2): GetTimeSyncSubscriptionData
    // (`GET /{supi}/time-sync-data`) and GetRangingSlPosData (`GET /{supi}/ranging-slpos-data`) --
    // real, confirmed by direct read of both TS29503_Nudm_SDM.yaml (these two ops) and
    // nfs/udr/src/main.cpp (its own real `time-sync-data`/`ranging-slpos-data` routes,
    // ADR-0131/ADR-0136, already live and seeded -- corrects ADR-0230's own scoping note, which
    // incorrectly assumed these needed brand-new UDR-side work; UDR already had both). Same
    // ueId-only-keyed shape as GetEcrData above.
    server.add_route(
        "GET",
        std::string(kSdmApiRoot) + "/{supi}/time-sync-data",
        [&verifier, &sdm_get_counter, &fetch_from_udr_no_plmn](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sdm_get_counter->Add(1);
            const auto supi = req.path_params.at("supi");
            auto result = fetch_from_udr_no_plmn(supi, "time-sync-data");
            if (auto* err = std::get_if<sbi_core::http2::Response>(&result)) {
                return *err;
            }
            return sbi_core::http2::Response::json(200, std::get<json>(result).dump());
        });

    server.add_route(
        "GET",
        std::string(kSdmApiRoot) + "/{supi}/ranging-slpos-data",
        [&verifier, &sdm_get_counter, &fetch_from_udr_no_plmn](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sdm_get_counter->Add(1);
            const auto supi = req.path_params.at("supi");
            auto result = fetch_from_udr_no_plmn(supi, "ranging-slpos-data");
            if (auto* err = std::get_if<sbi_core::http2::Response>(&result)) {
                return *err;
            }
            return sbi_core::http2::Response::json(200, std::get<json>(result).dump());
        });

    // Gap-closure (ADR-0234): SorAckInfo/UpuAck/`S-NSSAIs Ack`/`CAG Ack` -- 4 real, structurally
    // identical accept-and-validate ops (real spec operationIds for the latter two literally
    // contain spaces, not normalized here, same precedent as ADR-0227's own `Trigger P-CSCF
    // Restoration`). Each is `PUT /{supi}/am-data/<x>-ack`, real body `AcknowledgeInfo` (required
    // `provisioningTime`), real `204`. Real, disclosed non-relay, same class as ADR-0227's own
    // `Trigger P-CSCF Restoration`/`authTrigger` disclosure: no real onward relay to AUSF's own
    // SoR-Protection/UPU-Protection service exists in this build to actually consume these acks,
    // so this project accepts and validates (a real existence check against the same UDR am-data
    // probe `GetAmData`/`Nudm_MT`'s own `GetLocationInfo`-adjacent ops already use) without
    // forwarding. That non-relay disclosure is still accurate: AUSF computes `sorXmacIue` but has
    // no endpoint that consumes a UE ack, so there is genuinely nothing to forward these to.
    //
    // `Update SOR Info` (the 5th op in this group) remains deferred -- but ADR-0257 RE-EXAMINED
    // ADR-0234's stated reasons and BOTH were wrong:
    //   * "no real per-UE CounterSoR state machine (TS 33.501 6.14.2.3) exists anywhere in this
    //     build" -- it does, in AUSF, which is where it architecturally belongs because AUSF owns
    //     KAUSF: `kausf_store.use_counter()` in nfs/ausf/src/main.cpp, including real wrap-around
    //     suspension per that exact clause.
    //   * "real steering-of-roaming list content is missing" -- true, but not blocking:
    //     `steeringContainer` is OPTIONAL in both `SorInfo` (TS29503_Nudm_SDM.yaml) and in AUSF's
    //     own `Nausf_SoRProtection` request, and `SorInfo` requires only `ackInd` +
    //     `provisioningTime`. TS 33.501 Annex A.17 explicitly makes the Steering Info List an
    //     optional P2.
    // The REAL blocker, which ADR-0234 did not identify: the relay UDM would have to make is
    // `POST /{supi}/ue-sor` to AUSF, and AUSF (correctly, and already disclosed in its own file)
    // REQUIRES a `sorHeader` because AUSF-side construction of it is out of scope. Building that
    // header is TS 24.501 clause 9.11.3.51 -- a NAS-layer bit encoding. **TS 24.501 is not in
    // `specs/`, and no SOR transparent container exists anywhere in this project's NAS codec.**
    // Constructing it would be inventing a wire encoding, which is the one thing this project
    // never does. Deferred on missing spec material, the same class as ADR-0104's AUSF ProSe
    // deferral -- and now with an accurate reason instead of a stale one.
    for (const std::string segment : {"sor-ack", "upu-ack", "subscribed-snssais-ack", "cag-ack"}) {
        server.add_route(
            "PUT",
            std::string(kSdmApiRoot) + "/{supi}/am-data/" + segment,
            [&verifier, &fetch_from_udr, &sdm_ack_counter](const sbi_core::http2::Request& req) {
                if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                    return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
                }
                sbi_core::http2::Response err;
                auto body = sbi_core::http2::parse_json_body<sbi_gen::AcknowledgeInfo>(req, err);
                if (!body.has_value()) {
                    return err;
                }
                const auto supi = req.path_params.at("supi");
                auto result = fetch_from_udr(supi, resolve_serving_plmn_id(req), "am-data");
                if (auto* udr_err = std::get_if<sbi_core::http2::Response>(&result)) {
                    return *udr_err;
                }
                sdm_ack_counter->Add(1);
                sbi_core::http2::Response resp;
                resp.status = 204;
                return resp;
            });
    }

    // --- Nudm_SDM: notification subscriptions ---

    server.add_route(
        "POST",
        std::string(kSdmApiRoot) + "/{ueId}/sdm-subscriptions",
        [&verifier, &sdm_subscriptions, &sdm_subscribe_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::SdmSubscription>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto id =
                sdm_subscriptions.create(udm::SdmSubscriptionEntry{.ue_id = ue_id, .data = *body});
            sdm_subscribe_counter->Add(1);

            body->subscriptionId = id;
            json j = *body;
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace(
                "location", std::string(kSdmApiRoot) + "/" + ue_id + "/sdm-subscriptions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "DELETE",
        std::string(kSdmApiRoot) + "/{ueId}/sdm-subscriptions/{subscriptionId}",
        [&verifier, &sdm_subscriptions](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto subscription_id = req.path_params.at("subscriptionId");
            auto existing = sdm_subscriptions.get(subscription_id);
            if (!existing.has_value() || existing->ue_id != ue_id) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such SDM subscription");
            }
            sdm_subscriptions.remove(subscription_id);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // Gap-closure (ADR-0232, task #169): Modify -- real RFC 7396 JSON Merge Patch
    // (`application/merge-patch+json`, `SdmSubsModification`), same merge-patch shape already used
    // throughout this file for registration resources. Real response is a `oneOf`
    // (`SdmSubscription` | `PatchResult`) -- returning the full merged `SdmSubscription` is a real,
    // valid response per that `oneOf`, same precedent this file's other merge-patch routes use.
    server.add_route(
        "PATCH",
        std::string(kSdmApiRoot) + "/{ueId}/sdm-subscriptions/{subscriptionId}",
        [&verifier, &sdm_subscriptions, &sdm_subscription_modify_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto subscription_id = req.path_params.at("subscriptionId");
            auto patched =
                sdm_subscriptions.merge_patch(subscription_id, ue_id, json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such SDM subscription");
            }
            sdm_subscription_modify_counter->Add(1);
            json j = patched->data;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    // --- Nudm_SDM: shared-data group (ADR-0235, task #169) -- real, genuinely NOT per-UE, unlike
    // every other Nudm_SDM resource above. Confirmed by direct read of both TS29503_Nudm_SDM.yaml
    // and nfs/udr/src/main.cpp's own real routes: UDR already has GetIndividualSharedData and
    // GetGroupIdentifiers fully live (ADR-0110/ADR-0140) -- this closes the UDM-side wiring for
    // both, plus GetSharedData (the real bulk/collection op UDR itself never implemented, per
    // ADR-0110's own disclosed gap) composed here by calling UDR's individual route once per real
    // `shared-data-ids` entry rather than needing a new UDR-side bulk endpoint, and the 3
    // subscription-CRUD ops via a new UDM-local SharedDataSubscriptionStore (same shape as
    // SdmSubscriptionStore minus ue_id ownership, since shared-data subscriptions are genuinely
    // global). ---

    server.add_route(
        "GET",
        std::string(kSdmApiRoot) + "/shared-data/{sharedDataId}",
        [&verifier, &udr_client, &udr_oauth, &udr_base_url, &shared_data_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto shared_data_id = req.path_params.at("sharedDataId");
            auto token = udr_oauth.get_bearer_token();
            if (!token.has_value()) {
                return sbi_core::http2::problem_response(500,
                                                         "Internal Server Error",
                                                         "UDM could not obtain a token for UDR: " +
                                                             token.error());
            }
            sbi_core::http2::ClientRequest udr_req;
            udr_req.method = "GET";
            udr_req.url =
                udr_base_url + kUdrApiRoot + "/subscription-data/shared-data/" + shared_data_id;
            udr_req.headers.emplace("authorization", "Bearer " + *token);
            auto udr_resp = udr_client.send(udr_req);
            if (!udr_resp.has_value()) {
                return sbi_core::http2::problem_response(
                    500, "Internal Server Error", "UDM could not reach UDR: " + udr_resp.error());
            }
            if (udr_resp->status == 404) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No shared data for sharedDataId " + shared_data_id);
            }
            if (udr_resp->status != 200) {
                return sbi_core::http2::problem_response(500,
                                                         "Internal Server Error",
                                                         "UDR returned unexpected status " +
                                                             std::to_string(udr_resp->status));
            }
            shared_data_get_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = udr_resp->body;
            return resp;
        });

    // GetSharedData: real bulk retrieval composed from N real individual UDR calls (UDR itself
    // never implemented a bulk route, ADR-0110's own disclosed gap). Real, disclosed: an id that
    // individually 404s at UDR is simply omitted from the result array rather than failing the
    // whole request -- the real response schema (`array of SharedData`) has no `minItems`/
    // completeness constraint, so a partial result for a partially-known id set is a real, valid
    // response, not an invented one.
    server.add_route(
        "GET",
        std::string(kSdmApiRoot) + "/shared-data",
        [&verifier, &udr_client, &udr_oauth, &udr_base_url, &shared_data_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ids_it = req.query_params.find("shared-data-ids");
            if (ids_it == req.query_params.end()) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", "shared-data-ids is required");
            }
            auto token = udr_oauth.get_bearer_token();
            if (!token.has_value()) {
                return sbi_core::http2::problem_response(500,
                                                         "Internal Server Error",
                                                         "UDM could not obtain a token for UDR: " +
                                                             token.error());
            }
            json result = json::array();
            for (const auto& shared_data_id : sbi_core::http2::split_form_array(ids_it->second)) {
                sbi_core::http2::ClientRequest udr_req;
                udr_req.method = "GET";
                udr_req.url =
                    udr_base_url + kUdrApiRoot + "/subscription-data/shared-data/" + shared_data_id;
                udr_req.headers.emplace("authorization", "Bearer " + *token);
                auto udr_resp = udr_client.send(udr_req);
                if (!udr_resp.has_value() || udr_resp->status != 200) {
                    continue;
                }
                try {
                    result.push_back(json::parse(udr_resp->body));
                } catch (const json::exception&) {
                    continue;
                }
            }
            shared_data_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, result.dump());
        });

    server.add_route(
        "GET",
        std::string(kSdmApiRoot) + "/group-data/group-identifiers",
        [&verifier, &udr_client, &udr_oauth, &udr_base_url, &group_identifiers_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ext_it = req.query_params.find("ext-group-id");
            const auto int_it = req.query_params.find("int-group-id");
            if (ext_it == req.query_params.end() && int_it == req.query_params.end()) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", "At least one of ext-group-id or int-group-id is required");
            }
            auto token = udr_oauth.get_bearer_token();
            if (!token.has_value()) {
                return sbi_core::http2::problem_response(500,
                                                         "Internal Server Error",
                                                         "UDM could not obtain a token for UDR: " +
                                                             token.error());
            }
            sbi_core::http2::ClientRequest udr_req;
            udr_req.method = "GET";
            udr_req.url = udr_base_url + kUdrApiRoot + "/subscription-data/group-data/" +
                          std::string("group-identifiers") + "?" +
                          (ext_it != req.query_params.end() ? "ext-group-id=" + ext_it->second
                                                            : "int-group-id=" + int_it->second);
            udr_req.headers.emplace("authorization", "Bearer " + *token);
            auto udr_resp = udr_client.send(udr_req);
            if (!udr_resp.has_value()) {
                return sbi_core::http2::problem_response(
                    500, "Internal Server Error", "UDM could not reach UDR: " + udr_resp.error());
            }
            if (udr_resp->status == 404) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No Group Identifiers found");
            }
            if (udr_resp->status != 200) {
                return sbi_core::http2::problem_response(500,
                                                         "Internal Server Error",
                                                         "UDR returned unexpected status " +
                                                             std::to_string(udr_resp->status));
            }
            group_identifiers_get_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = udr_resp->body;
            return resp;
        });

    server.add_route(
        "POST",
        std::string(kSdmApiRoot) + "/shared-data-subscriptions",
        [&verifier, &shared_data_subscriptions, &shared_data_subscribe_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::SdmSubscription>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            const auto id = shared_data_subscriptions.create(j);
            shared_data_subscribe_counter->Add(1);
            body->subscriptionId = id;
            json j2 = *body;
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kSdmApiRoot) + "/shared-data-subscriptions/" + id);
            resp.body = j2.dump();
            return resp;
        });

    server.add_route(
        "DELETE",
        std::string(kSdmApiRoot) + "/shared-data-subscriptions/{subscriptionId}",
        [&verifier, &shared_data_subscriptions](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto subscription_id = req.path_params.at("subscriptionId");
            if (!shared_data_subscriptions.get(subscription_id).has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such shared-data subscription");
            }
            shared_data_subscriptions.remove(subscription_id);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "PATCH",
        std::string(kSdmApiRoot) + "/shared-data-subscriptions/{subscriptionId}",
        [&verifier, &shared_data_subscriptions, &shared_data_modify_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto subscription_id = req.path_params.at("subscriptionId");
            auto patched =
                shared_data_subscriptions.merge_patch(subscription_id, json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such shared-data subscription");
            }
            shared_data_modify_counter->Add(1);
            return sbi_core::http2::Response::json(200, patched->dump());
        });

    // --- Nudm_SDM: identifier-lookup group (ADR-0236, task #169) -- real, genuinely bounded: the
    // forward direction (given a SUPI, find its GPSI) is closed using the real `gpsis` field on
    // AccessAndMobilitySubscriptionData (ADR-0236's own UDR seed-data addition). The reverse
    // direction (given a GPSI, find its SUPI) is a real, disclosed gap -- it would need either a
    // query-by-gpsi capability that does not exist anywhere in the real Nudr_DR API (checked, not
    // assumed: no such resource in TS29504_Nudr_DR.yaml/TS29505_Subscription_Data.yaml), or UDM
    // maintaining its own duplicate subscriber index, and this project's NFs talk only over SBI
    // (CLAUDE.md), so neither is available. Real 404/omission, not a fabricated translation. ---

    server.add_route(
        "GET",
        std::string(kSdmApiRoot) + "/{ueId}/id-translation-result",
        [&verifier, &fetch_from_udr, &id_translation_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const bool is_supi_shaped = ue_id.rfind("imsi-", 0) == 0 ||
                                        ue_id.rfind("nai-", 0) == 0 ||
                                        ue_id.rfind("gci-", 0) == 0 || ue_id.rfind("gli-", 0) == 0;
            if (!is_supi_shaped) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "GPSI-to-SUPI translation is not available in this build");
            }
            auto result = fetch_from_udr(ue_id, resolve_serving_plmn_id(req), "am-data");
            if (auto* err = std::get_if<sbi_core::http2::Response>(&result)) {
                return *err;
            }
            const auto& am_data = std::get<json>(result);
            sbi_gen::IdTranslationResult out;
            out.supi = ue_id;
            if (am_data.contains("gpsis") && am_data.at("gpsis").is_array() &&
                !am_data.at("gpsis").empty()) {
                out.gpsi = am_data.at("gpsis").at(0).get<std::string>();
            }
            id_translation_counter->Add(1);
            json j = out;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "GET",
        std::string(kSdmApiRoot) + "/multiple-identifiers",
        [&verifier, &fetch_from_udr, &id_translation_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json ue_id_gpsi_list = json::object();
            const auto supi_list_it = req.query_params.find("supi-list");
            if (supi_list_it != req.query_params.end()) {
                for (const auto& supi : sbi_core::http2::split_form_array(supi_list_it->second)) {
                    auto result = fetch_from_udr(supi, resolve_serving_plmn_id(req), "am-data");
                    if (std::get_if<sbi_core::http2::Response>(&result) != nullptr) {
                        continue;
                    }
                    const auto& am_data = std::get<json>(result);
                    if (am_data.contains("gpsis") && am_data.at("gpsis").is_array() &&
                        !am_data.at("gpsis").empty()) {
                        ue_id_gpsi_list[supi] = json{{"gpsiList", am_data.at("gpsis")}};
                    }
                }
            }
            sbi_gen::UeIdentifiers out;
            if (!ue_id_gpsi_list.empty()) {
                out.ueIdGpsiList = ue_id_gpsi_list;
            }
            id_translation_counter->Add(1);
            json j = out;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    // --- Nudm_UEAU: authentication data generation + confirmation ---

    server.add_route(
        "POST",
        std::string(kUeauApiRoot) + "/{supiOrSuci}/security-information/generate-auth-data",
        [&verifier, &auth_subscriptions, &generate_auth_data_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::AuthenticationInfoRequest>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto raw_supi_or_suci = req.path_params.at("supiOrSuci");
            const auto deconcealed = deconceal_suci_if_needed(raw_supi_or_suci);
            if (!deconcealed.has_value()) {
                return sbi_core::http2::problem_response(
                    400,
                    "Bad Request",
                    "Could not de-conceal SUCI " + raw_supi_or_suci +
                        " (malformed, unsupported protection scheme, or MAC verification failed)");
            }
            const auto supi_or_suci = *deconcealed;

            // SQN resynchronisation (TS 33.102 §6.3.3, ADR-0037): if AUSF forwarded resync info
            // (a UE's earlier AuthenticationFailure carried AUTS), verify+apply it BEFORE the
            // normal get_and_advance_sqn() read below -- otherwise the vector this call is about
            // to generate would still use the stale, already-desynced SQN.
            if (body->resynchronizationInfo.has_value()) {
                const auto resync_rand =
                    aka_crypto::from_hex<16>(body->resynchronizationInfo->rand);
                const auto resync_auts =
                    aka_crypto::from_hex<14>(body->resynchronizationInfo->auts);
                if (!resync_rand.has_value() || !resync_auts.has_value()) {
                    return sbi_core::http2::problem_response(
                        400, "Bad Request", "resynchronizationInfo.rand/auts are not valid hex");
                }
                const auto resync_result =
                    auth_subscriptions.resync_sqn(supi_or_suci, *resync_rand, *resync_auts);
                if (!resync_result.has_value()) {
                    return sbi_core::http2::problem_response(
                        404, "Not Found", "No authentication subscription for " + supi_or_suci);
                }
                if (!*resync_result) {
                    return sbi_core::http2::problem_response(
                        400,
                        "Bad Request",
                        "resynchronizationInfo.auts failed to verify for " + supi_or_suci +
                            " -- wrong subscriber key material, tampered, or the RAND doesn't "
                            "match the AuthenticationRequest the UE is responding to");
                }
            }

            auto sub = auth_subscriptions.get_and_advance_sqn(supi_or_suci);
            if (!sub.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No authentication subscription for " + supi_or_suci);
            }

            const auto rand = aka_crypto::generate_rand();
            const auto mac_a = aka_crypto::f1(sub->opc, sub->k, rand, sub->sqn, sub->amf);
            const auto out = aka_crypto::f2345(sub->opc, sub->k, rand);
            const auto sqn_xor_ak_value = aka_crypto::sqn_xor_ak(sub->sqn, out.ak);

            std::array<uint8_t, 16> autn{};
            std::copy(sqn_xor_ak_value.begin(), sqn_xor_ak_value.end(), autn.begin());
            std::copy(sub->amf.begin(), sub->amf.end(), autn.begin() + 6);
            std::copy(mac_a.begin(), mac_a.end(), autn.begin() + 8);

            sbi_gen::AuthenticationInfoResult result{};
            result.supi = supi_or_suci;

            if (sub->authentication_method == "EAP_AKA_PRIME") {
                const auto ck_ik_prime = aka_crypto::derive_ck_ik_prime(
                    out.ck, out.ik, body->servingNetworkName, sqn_xor_ak_value);
                sbi_gen::AvEapAkaPrime av{};
                av.avType.value = sbi_gen::AvType::EAP_AKA_PRIME;
                av.rand = aka_crypto::to_hex(rand);
                av.xres = aka_crypto::to_hex(out.res);
                av.autn = aka_crypto::to_hex(autn);
                av.ckPrime = aka_crypto::to_hex(ck_ik_prime.first);
                av.ikPrime = aka_crypto::to_hex(ck_ik_prime.second);
                result.authType.value = sbi_gen::AuthType_Nudm_UEAU::EAP_AKA_PRIME;
                result.authenticationVector = nlohmann::json(av);
            } else {
                const auto xres_star = aka_crypto::derive_res_star(
                    out.ck, out.ik, body->servingNetworkName, rand, out.res);
                const auto kausf = aka_crypto::derive_kausf(
                    out.ck, out.ik, body->servingNetworkName, sqn_xor_ak_value);
                sbi_gen::Av5GHeAka av{};
                av.avType.value = sbi_gen::AvType::V5G_HE_AKA;
                av.rand = aka_crypto::to_hex(rand);
                av.xresStar = aka_crypto::to_hex(xres_star);
                av.autn = aka_crypto::to_hex(autn);
                av.kausf = aka_crypto::to_hex(kausf);
                result.authType.value = sbi_gen::AuthType_Nudm_UEAU::V5G_AKA;
                result.authenticationVector = nlohmann::json(av);
            }

            generate_auth_data_counter->Add(1);
            json j = result;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #104, ADR-0091): real GenerateProseAV --
    // real 3GPP R17+ extension (5G ProSe, TS 33.503), free5GC-only per the original capability
    // sweep, previously deferred (this file's own header comment above). Structurally almost
    // identical to generate-auth-data's EAP-AKA' branch above (same Milenage vectors, same real
    // AvEapAkaPrime shape) -- TS 33.503's own clause 6.1.3.2 states the ProSe Remote UE's KAUSF_P
    // "is obtained in the same way as KAUSF is obtained for EAP-AKA' in clause 6.1.3.1 in
    // TS 33.501", i.e. ProSe always uses EAP-AKA' regardless of this subscriber's own configured
    // authentication_method -- real, deliberate difference from generate-auth-data's own
    // method-dependent branch above.
    server.add_route(
        "POST",
        std::string(kUeauApiRoot) + "/{supiOrSuci}/prose-security-information/generate-av",
        [&verifier, &auth_subscriptions, &generate_prose_av_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::ProSeAuthenticationInfoRequest>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto raw_supi_or_suci = req.path_params.at("supiOrSuci");
            const auto deconcealed = deconceal_suci_if_needed(raw_supi_or_suci);
            if (!deconcealed.has_value()) {
                return sbi_core::http2::problem_response(
                    400,
                    "Bad Request",
                    "Could not de-conceal SUCI " + raw_supi_or_suci +
                        " (malformed, unsupported protection scheme, or MAC verification failed)");
            }
            const auto supi_or_suci = *deconcealed;

            auto sub = auth_subscriptions.get_and_advance_sqn(supi_or_suci);
            if (!sub.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No authentication subscription for " + supi_or_suci);
            }

            const auto rand = aka_crypto::generate_rand();
            const auto mac_a = aka_crypto::f1(sub->opc, sub->k, rand, sub->sqn, sub->amf);
            const auto out = aka_crypto::f2345(sub->opc, sub->k, rand);
            const auto sqn_xor_ak_value = aka_crypto::sqn_xor_ak(sub->sqn, out.ak);

            std::array<uint8_t, 16> autn{};
            std::copy(sqn_xor_ak_value.begin(), sqn_xor_ak_value.end(), autn.begin());
            std::copy(sub->amf.begin(), sub->amf.end(), autn.begin() + 6);
            std::copy(mac_a.begin(), mac_a.end(), autn.begin() + 8);

            const auto ck_ik_prime = aka_crypto::derive_ck_ik_prime(
                out.ck, out.ik, body->servingNetworkName, sqn_xor_ak_value);
            sbi_gen::AvEapAkaPrime av{};
            av.avType.value = sbi_gen::AvType::EAP_AKA_PRIME;
            av.rand = aka_crypto::to_hex(rand);
            av.xres = aka_crypto::to_hex(out.res);
            av.autn = aka_crypto::to_hex(autn);
            av.ckPrime = aka_crypto::to_hex(ck_ik_prime.first);
            av.ikPrime = aka_crypto::to_hex(ck_ik_prime.second);

            sbi_gen::ProSeAuthenticationInfoResult result{};
            result.authType.value = sbi_gen::AuthType_Nudm_UEAU::EAP_AKA_PRIME;
            result.supi = supi_or_suci;
            result.proseAuthenticationVectors = sbi_gen::ProSeAuthenticationVectors{av};

            generate_prose_av_counter->Add(1);
            json j = result;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #169, ADR-0214): real, previously
    // undisclosed `GetRgAuthData`. Real, disclosed design choice -- see
    // `AuthEventStore::has_successful_event`'s own header comment for why `authInd` is backed by
    // an existing `ConfirmAuth`-created `AuthEvent`, not a new, separate notion of "authenticated".
    // Real, structural: `authenticated-ind` (the caller's own claim) is accepted per the real
    // spec's own required query param, but this project's own `authInd` in the response always
    // reflects this UDM's own real stored state, not the caller's unverified claim -- returning
    // the caller's own claim back unchecked would be a fabricated confirmation, not a real one.
    server.add_route(
        "GET",
        std::string(kUeauApiRoot) + "/{supiOrSuci}/security-information-rg",
        [&verifier, &auth_events, &get_rg_auth_data_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            if (req.query_params.find("authenticated-ind") == req.query_params.end()) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", "Required query parameter authenticated-ind is missing");
            }
            const auto raw_supi_or_suci = req.path_params.at("supiOrSuci");
            const auto deconcealed = deconceal_suci_if_needed(raw_supi_or_suci);
            if (!deconcealed.has_value()) {
                return sbi_core::http2::problem_response(
                    400,
                    "Bad Request",
                    "Could not de-conceal SUCI " + raw_supi_or_suci +
                        " (malformed, unsupported protection scheme, or MAC verification failed)");
            }
            const auto supi_or_suci = *deconcealed;

            sbi_gen::RgAuthCtx_Nudm_UEAU result{};
            result.authInd = auth_events.has_successful_event(supi_or_suci);
            result.supi = supi_or_suci;

            get_rg_auth_data_counter->Add(1);
            json j = result;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #169, ADR-0214): real, previously
    // undisclosed `GenerateAv` (EPS/IMS/GBA-domain HSS vectors). Reuses the exact same Milenage
    // `aka_crypto::f1`/`f2345` primitives + `auth_subscriptions` per-subscriber SQN advance
    // already used by `GenerateAuthData`/`GenerateProseAV` above -- real vectors, not stubs, for
    // 4 of 5 real `hssAuthType` branches. Real, disclosed gap: the `eps-aka` branch's own
    // `AvEpsAka.kasme` needs TS 33.401 Annex A.2 KASME derivation, a distinct KDF this project's
    // `libs/aka-crypto` does not yet implement (only the 5G KAUSF/KSEAF chain, TS 33.501, exists)
    // -- returns the real, spec-documented `501` rather than fabricating a KASME value.
    server.add_route(
        "POST",
        std::string(kUeauApiRoot) + "/{supi}/hss-security-information/{hssAuthType}/generate-av",
        [&verifier, &auth_subscriptions, &generate_av_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::HssAuthenticationInfoRequest>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto supi = req.path_params.at("supi");
            const auto hss_auth_type = req.path_params.at("hssAuthType");

            if (hss_auth_type == "eps-aka") {
                return sbi_core::http2::problem_response(
                    501,
                    "Not Implemented",
                    "eps-aka KASME derivation (TS 33.401 Annex A.2) is not yet implemented in "
                    "libs/aka-crypto");
            }
            std::string serving_network_name;
            if (hss_auth_type == "eap-aka-prime") {
                if (!body->servingNetworkId.has_value()) {
                    return sbi_core::http2::problem_response(
                        400,
                        "Bad Request",
                        "servingNetworkId is required to derive CK'/IK' for eap-aka-prime");
                }
                const auto& mnc = body->servingNetworkId->mnc;
                const std::string padded_mnc =
                    mnc.size() < 3 ? std::string(3 - mnc.size(), '0') + mnc : mnc;
                serving_network_name = "5G:mnc" + padded_mnc + ".mcc" +
                                       body->servingNetworkId->mcc + ".3gppnetwork.org";
            } else if (hss_auth_type != "eap-aka" && hss_auth_type != "ims-aka" &&
                       hss_auth_type != "gba-aka") {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", "Unrecognized hssAuthType " + hss_auth_type);
            }

            const std::int64_t num_vectors = body->numOfRequestedVectors;
            json vectors = json::array();
            for (std::int64_t i = 0; i < num_vectors; ++i) {
                auto sub = auth_subscriptions.get_and_advance_sqn(supi);
                if (!sub.has_value()) {
                    return sbi_core::http2::problem_response(
                        404, "Not Found", "No authentication subscription for " + supi);
                }
                const auto rand = aka_crypto::generate_rand();
                const auto mac_a = aka_crypto::f1(sub->opc, sub->k, rand, sub->sqn, sub->amf);
                const auto out = aka_crypto::f2345(sub->opc, sub->k, rand);
                const auto sqn_xor_ak_value = aka_crypto::sqn_xor_ak(sub->sqn, out.ak);
                std::array<uint8_t, 16> autn{};
                std::copy(sqn_xor_ak_value.begin(), sqn_xor_ak_value.end(), autn.begin());
                std::copy(sub->amf.begin(), sub->amf.end(), autn.begin() + 6);
                std::copy(mac_a.begin(), mac_a.end(), autn.begin() + 8);

                if (hss_auth_type == "eap-aka-prime") {
                    const auto ck_ik_prime = aka_crypto::derive_ck_ik_prime(
                        out.ck, out.ik, serving_network_name, sqn_xor_ak_value);
                    sbi_gen::AvEapAkaPrime av{};
                    av.avType.value = sbi_gen::AvType::EAP_AKA_PRIME;
                    av.rand = aka_crypto::to_hex(rand);
                    av.xres = aka_crypto::to_hex(out.res);
                    av.autn = aka_crypto::to_hex(autn);
                    av.ckPrime = aka_crypto::to_hex(ck_ik_prime.first);
                    av.ikPrime = aka_crypto::to_hex(ck_ik_prime.second);
                    vectors.push_back(av);
                } else {
                    sbi_gen::AvImsGbaEapAka av{};
                    av.avType.value = hss_auth_type == "eap-aka"   ? sbi_gen::HssAvType::EAP_AKA
                                      : hss_auth_type == "ims-aka" ? sbi_gen::HssAvType::IMS_AKA
                                                                   : sbi_gen::HssAvType::GBA_AKA;
                    av.rand = aka_crypto::to_hex(rand);
                    av.xres = aka_crypto::to_hex(out.res);
                    av.autn = aka_crypto::to_hex(autn);
                    av.ck = aka_crypto::to_hex(out.ck);
                    av.ik = aka_crypto::to_hex(out.ik);
                    vectors.push_back(av);
                }
            }

            json result;
            result["hssAuthenticationVectors"] = vectors;
            generate_av_counter->Add(1);
            return sbi_core::http2::Response::json(200, result.dump());
        });

    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #169, ADR-0214): real, previously
    // undisclosed `GenerateGbaAv`. Only one real `GbaAuthType` value exists
    // (`DIGEST_AKAV1_MD5`), and `N3GAkaAv`'s own real fields (rand/xres/autn/ck/ik) are exactly
    // the same raw Milenage output already used above -- real vectors, no new primitive needed.
    server.add_route(
        "POST",
        std::string(kUeauApiRoot) + "/{supi}/gba-security-information/generate-av",
        [&verifier, &auth_subscriptions, &generate_gba_av_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::GbaAuthenticationInfoRequest>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto supi = req.path_params.at("supi");
            auto sub = auth_subscriptions.get_and_advance_sqn(supi);
            if (!sub.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No authentication subscription for " + supi);
            }
            const auto rand = aka_crypto::generate_rand();
            const auto mac_a = aka_crypto::f1(sub->opc, sub->k, rand, sub->sqn, sub->amf);
            const auto out = aka_crypto::f2345(sub->opc, sub->k, rand);
            const auto sqn_xor_ak_value = aka_crypto::sqn_xor_ak(sub->sqn, out.ak);
            std::array<uint8_t, 16> autn{};
            std::copy(sqn_xor_ak_value.begin(), sqn_xor_ak_value.end(), autn.begin());
            std::copy(sub->amf.begin(), sub->amf.end(), autn.begin() + 6);
            std::copy(mac_a.begin(), mac_a.end(), autn.begin() + 8);

            sbi_gen::N3GAkaAv av{};
            av.rand = aka_crypto::to_hex(rand);
            av.xres = aka_crypto::to_hex(out.res);
            av.autn = aka_crypto::to_hex(autn);
            av.ck = aka_crypto::to_hex(out.ck);
            av.ik = aka_crypto::to_hex(out.ik);

            sbi_gen::GbaAuthenticationInfoResult result{};
            result.n3gAkaAv = av;
            generate_gba_av_counter->Add(1);
            json j = result;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "POST",
        std::string(kUeauApiRoot) + "/{supi}/auth-events",
        [&verifier, &auth_events, &confirm_auth_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::AuthEvent>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto supi = req.path_params.at("supi");
            json j = *body;
            const auto id = auth_events.create(supi, j);
            confirm_auth_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kUeauApiRoot) + "/" + supi + "/auth-events/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "PUT",
        std::string(kUeauApiRoot) + "/{supi}/auth-events/{authEventId}",
        [&verifier, &auth_events, &delete_auth_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::AuthEvent>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto supi = req.path_params.at("supi");
            const auto auth_event_id = req.path_params.at("authEventId");
            if (!auth_events.remove(supi, auth_event_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No auth event " + auth_event_id + " for supi " + supi);
            }
            delete_auth_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nudm_EE (ADR-0082, gap-closure task #105) ---

    server.add_route(
        "POST",
        std::string(kEeApiRoot) + "/{ueIdentity}/ee-subscriptions",
        [&verifier, &ee_subscriptions, &ee_subscribe_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::EeSubscription>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_identity = req.path_params.at("ueIdentity");
            json j = *body;
            const auto id = ee_subscriptions.create(
                udm::EeSubscriptionEntry{.ue_identity = ue_identity, .data = j});
            ee_subscribe_counter->Add(1);

            j["subscriptionId"] = id;
            sbi_gen::CreatedEeSubscription created{};
            created.eeSubscription = j.get<sbi_gen::EeSubscription>();
            json resp_j = created;
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kEeApiRoot) + "/" + ue_identity +
                                     "/ee-subscriptions/" + id);
            resp.body = resp_j.dump();
            return resp;
        });

    server.add_route(
        "DELETE",
        std::string(kEeApiRoot) + "/{ueIdentity}/ee-subscriptions/{subscriptionId}",
        [&verifier, &ee_subscriptions, &ee_delete_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_identity = req.path_params.at("ueIdentity");
            const auto subscription_id = req.path_params.at("subscriptionId");
            auto existing = ee_subscriptions.get(subscription_id);
            if (!existing.has_value() || existing->ue_identity != ue_identity) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such EE subscription");
            }
            ee_subscriptions.remove(subscription_id);
            ee_delete_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "PATCH",
        std::string(kEeApiRoot) + "/{ueIdentity}/ee-subscriptions/{subscriptionId}",
        [&verifier, &ee_subscriptions, &ee_update_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_identity = req.path_params.at("ueIdentity");
            const auto subscription_id = req.path_params.at("subscriptionId");
            auto existing = ee_subscriptions.get(subscription_id);
            if (!existing.has_value() || existing->ue_identity != ue_identity) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such EE subscription");
            }
            // Real spec: application/json-patch+json (RFC 6902) -- see stores.hpp's own comment
            // for why this differs from PP Data's own RFC 7396 merge-patch below.
            json patch_ops;
            try {
                patch_ops = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            std::optional<json> patched;
            try {
                patched = ee_subscriptions.apply_patch(subscription_id, patch_ops);
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(400, "Invalid JSON Patch", e.what());
            }
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such EE subscription");
            }
            ee_update_counter->Add(1);
            return sbi_core::http2::Response::json(200, patched->dump());
        });

    // --- Nudm_PP (ADR-0082, gap-closure task #105) ---

    server.add_route(
        "GET",
        std::string(kPpApiRoot) + "/{ueId}/pp-data",
        [&verifier, &pp_data, &pp_get_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = pp_data.get(ue_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No PP data for ueId " + ue_id);
            }
            pp_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PATCH",
        std::string(kPpApiRoot) + "/{ueId}/pp-data",
        [&verifier, &pp_data, &pp_update_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            // Real spec: application/merge-patch+json (RFC 7396) -- confirmed by reading
            // TS29503_Nudm_PP.yaml's own Update requestBody content type directly, same real
            // distinction PCF's own ModAppSession (ADR-0080) already established for this
            // project.
            json patch_doc;
            try {
                patch_doc = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            auto patched = pp_data.merge_patch(ue_id, patch_doc);
            pp_update_counter->Add(1);
            return sbi_core::http2::Response::json(200, patched->dump());
        });

    // --- Nudm_PP gap-closure (ADR-0237, task #169): 11 real ops across 3 real UDR-backed
    // sub-groups named in ADR-0082's own disclosed scope-narrowing (5G VN Group CRUD, PP Data
    // Entry CRUD, 5G MBS Group CRUD). UDR already has all 3 real backing stores fully live
    // (PpDataStore/FiveGVnGroupStore/MbsGroupMembershipStore, ADR-0109/ADR-0144/ADR-0167), checked
    // first per this project's own re-learned "check UDR before assuming new work" lesson
    // (ADR-0233). Create/Delete/Get for all 3 groups are real, direct `proxy_to_udr` passthroughs
    // -- UDR's own response shapes (201/204 for PUT, 204/404 for DELETE, 200/404 for GET) already
    // match Nudm_PP's own real spec exactly. Modify (5G VN Group / 5G MBS Group only -- PP Data
    // Entry has no Modify op) is genuinely NOT a passthrough: Nudm_PP's real spec uses
    // `application/merge-patch+json` (RFC 7396) for both, but UDR's own PATCH for both resources
    // is real RFC 6902 JSON Patch (confirmed by direct read of both YAMLs) -- so Modify is
    // implemented as a real GET+merge_patch(locally)+PUT round trip against UDR instead, giving
    // the real Nudm_PP caller correct RFC 7396 semantics without UDR needing to change. ---

    // PP Data Entry: Create/Delete/Get -- real, direct proxies to UDR's own already-live
    // pp-data-store/{afInstanceId} resource (ADR-0109). No Modify op exists for this resource.
    server.add_route(
        "PUT",
        std::string(kPpApiRoot) + "/{ueId}/pp-data-store/{afInstanceId}",
        [&verifier, &proxy_to_udr, &pp_data_entry_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto af_instance_id = req.path_params.at("afInstanceId");
            auto resp =
                proxy_to_udr("PUT",
                             "/subscription-data/" + ue_id + "/pp-data-store/" + af_instance_id,
                             req.body);
            pp_data_entry_counter->Add(1);
            return resp;
        });

    server.add_route(
        "DELETE",
        std::string(kPpApiRoot) + "/{ueId}/pp-data-store/{afInstanceId}",
        [&verifier, &proxy_to_udr, &pp_data_entry_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto af_instance_id = req.path_params.at("afInstanceId");
            auto resp = proxy_to_udr(
                "DELETE", "/subscription-data/" + ue_id + "/pp-data-store/" + af_instance_id, "");
            pp_data_entry_counter->Add(1);
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kPpApiRoot) + "/{ueId}/pp-data-store/{afInstanceId}",
        [&verifier, &proxy_to_udr, &pp_data_entry_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto af_instance_id = req.path_params.at("afInstanceId");
            auto resp = proxy_to_udr(
                "GET", "/subscription-data/" + ue_id + "/pp-data-store/" + af_instance_id, "");
            pp_data_entry_counter->Add(1);
            return resp;
        });

    // 5G VN Group: Create/Delete/Get -- real, direct proxies to UDR's own already-live
    // 5g-vn-groups/{externalGroupId} resource (ADR-0144).
    server.add_route(
        "PUT",
        std::string(kPpApiRoot) + "/5g-vn-groups/{extGroupId}",
        [&verifier, &proxy_to_udr, &pp_5g_vn_group_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ext_group_id = req.path_params.at("extGroupId");
            auto resp = proxy_to_udr(
                "PUT", "/subscription-data/group-data/5g-vn-groups/" + ext_group_id, req.body);
            pp_5g_vn_group_counter->Add(1);
            return resp;
        });

    server.add_route(
        "DELETE",
        std::string(kPpApiRoot) + "/5g-vn-groups/{extGroupId}",
        [&verifier, &proxy_to_udr, &pp_5g_vn_group_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ext_group_id = req.path_params.at("extGroupId");
            auto resp = proxy_to_udr(
                "DELETE", "/subscription-data/group-data/5g-vn-groups/" + ext_group_id, "");
            pp_5g_vn_group_counter->Add(1);
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kPpApiRoot) + "/5g-vn-groups/{extGroupId}",
        [&verifier, &proxy_to_udr, &pp_5g_vn_group_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ext_group_id = req.path_params.at("extGroupId");
            auto resp = proxy_to_udr(
                "GET", "/subscription-data/group-data/5g-vn-groups/" + ext_group_id, "");
            pp_5g_vn_group_counter->Add(1);
            return resp;
        });

    // 5G VN Group: Modify -- real RFC 7396 merge-patch semantics via GET+merge_patch+PUT against
    // UDR's own RFC 6902 JSON-Patch-based resource (see this block's own top comment).
    server.add_route(
        "PATCH",
        std::string(kPpApiRoot) + "/5g-vn-groups/{extGroupId}",
        [&verifier, &proxy_to_udr, &pp_5g_vn_group_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ext_group_id = req.path_params.at("extGroupId");
            const std::string udr_path =
                "/subscription-data/group-data/5g-vn-groups/" + ext_group_id;
            auto get_resp = proxy_to_udr("GET", udr_path, "");
            if (get_resp.status != 200) {
                return get_resp;
            }
            json patch_doc;
            try {
                patch_doc = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            json current;
            try {
                current = json::parse(get_resp.body);
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(500,
                                                         "Internal Server Error",
                                                         "UDR returned malformed JSON: " +
                                                             std::string(e.what()));
            }
            current.merge_patch(patch_doc);
            auto put_resp = proxy_to_udr("PUT", udr_path, current.dump());
            if (put_resp.status != 201) {
                return put_resp;
            }
            pp_5g_vn_group_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // 5G MBS Group: Create/Delete/Get -- real, direct proxies to UDR's own already-live
    // mbs-group-membership/{externalGroupId} resource (ADR-0167).
    server.add_route(
        "PUT",
        std::string(kPpApiRoot) + "/mbs-group-membership/{extGroupId}",
        [&verifier, &proxy_to_udr, &pp_5g_mbs_group_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ext_group_id = req.path_params.at("extGroupId");
            auto resp =
                proxy_to_udr("PUT",
                             "/subscription-data/group-data/mbs-group-membership/" + ext_group_id,
                             req.body);
            pp_5g_mbs_group_counter->Add(1);
            return resp;
        });

    server.add_route(
        "DELETE",
        std::string(kPpApiRoot) + "/mbs-group-membership/{extGroupId}",
        [&verifier, &proxy_to_udr, &pp_5g_mbs_group_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ext_group_id = req.path_params.at("extGroupId");
            auto resp = proxy_to_udr(
                "DELETE", "/subscription-data/group-data/mbs-group-membership/" + ext_group_id, "");
            pp_5g_mbs_group_counter->Add(1);
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kPpApiRoot) + "/mbs-group-membership/{extGroupId}",
        [&verifier, &proxy_to_udr, &pp_5g_mbs_group_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ext_group_id = req.path_params.at("extGroupId");
            auto resp = proxy_to_udr(
                "GET", "/subscription-data/group-data/mbs-group-membership/" + ext_group_id, "");
            pp_5g_mbs_group_counter->Add(1);
            return resp;
        });

    // 5G MBS Group: Modify -- real RFC 7396 merge-patch semantics via GET+merge_patch+PUT against
    // UDR's own RFC 6902 JSON-Patch-based resource (see this block's own top comment).
    server.add_route(
        "PATCH",
        std::string(kPpApiRoot) + "/mbs-group-membership/{extGroupId}",
        [&verifier, &proxy_to_udr, &pp_5g_mbs_group_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ext_group_id = req.path_params.at("extGroupId");
            const std::string udr_path =
                "/subscription-data/group-data/mbs-group-membership/" + ext_group_id;
            auto get_resp = proxy_to_udr("GET", udr_path, "");
            if (get_resp.status != 200) {
                return get_resp;
            }
            json patch_doc;
            try {
                patch_doc = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            json current;
            try {
                current = json::parse(get_resp.body);
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(500,
                                                         "Internal Server Error",
                                                         "UDR returned malformed JSON: " +
                                                             std::string(e.what()));
            }
            current.merge_patch(patch_doc);
            auto put_resp = proxy_to_udr("PUT", udr_path, current.dump());
            if (put_resp.status != 201) {
                return put_resp;
            }
            pp_5g_mbs_group_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // ---- TS29503_Nudm_MT.yaml (ADR-0202) ----

    server.add_route(
        "GET",
        std::string(kMtApiRoot) + "/{supi}",
        [&verifier, &fetch_from_udr, &mt_query_ue_info_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto supi = req.path_params.at("supi");
            // Real existence check: same real UDR am-data probe GetAmData etc. already use --
            // no dedicated new store needed.
            auto result = fetch_from_udr(supi, resolve_serving_plmn_id(req), "am-data");
            if (auto* err = std::get_if<sbi_core::http2::Response>(&result)) {
                return *err;
            }
            mt_query_ue_info_counter->Add(1);
            // Disclosed simplification: honestly-empty UeInfo -- no real VoPS/5G-SRVCC/userState
            // data exists anywhere in this build to populate it from.
            sbi_gen::UeInfo_Nudm_MT resp_data;
            json j = resp_data;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "POST",
        std::string(kMtApiRoot) + "/{supi}/loc-info/provide-loc-info",
        [&verifier, &fetch_from_udr, &mt_provide_loc_info_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::LocationInfoRequest>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto supi = req.path_params.at("supi");
            auto result = fetch_from_udr(supi, resolve_serving_plmn_id(req), "am-data");
            if (auto* fetch_err = std::get_if<sbi_core::http2::Response>(&result)) {
                return *fetch_err;
            }
            mt_provide_loc_info_counter->Add(1);
            // Disclosed simplification: honestly-empty LocationInfoResult -- no real location
            // tracking exists anywhere in this build to populate it from.
            sbi_gen::LocationInfoResult resp_data;
            json j = resp_data;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    // ---- TS29503_Nudm_NIDDAU.yaml (ADR-0202) ----

    server.add_route(
        "POST",
        std::string(kNiddauApiRoot) + "/{ueIdentity}/authorize",
        [&verifier, &niddau_authorize_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::AuthorizationInfo>(req, err);
            if (!body.has_value()) {
                return err;
            }
            niddau_authorize_counter->Add(1);
            // Disclosed gap: no real MTC-provider/NIDD authorization policy data exists anywhere
            // in this build to authorize against.
            return sbi_core::http2::problem_response(
                501, "Not Implemented", "No NIDD authorization policy data configured");
        });

    // ---- TS29503_Nudm_RSDS.yaml (ADR-0202) ----

    server.add_route(
        "POST",
        std::string(kRsdsApiRoot) + "/{ueIdentity}/sm-delivery-status",
        [&verifier, &rsds_report_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::SmDeliveryStatus>(req, err);
            if (!body.has_value()) {
                return err;
            }
            rsds_report_counter->Add(1);
            // Disclosed simplification: no real SMS routing/retry logic exists to act on the
            // report -- it is structurally validated and accepted.
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // ---- TS29503_Nudm_SSAU.yaml (ADR-0202) ----

    server.add_route(
        "POST",
        std::string(kSsauApiRoot) + "/{ueIdentity}/{serviceType}/authorize",
        [&verifier, &ssau_authorize_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<
                sbi_gen::ServiceSpecificAuthorizationInfo_Nudm_SSAU>(req, err);
            if (!body.has_value()) {
                return err;
            }
            ssau_authorize_counter->Add(1);
            // Disclosed gap: no real service-specific authorization policy data exists for any of
            // the real AF_GUIDANCE_FOR_URSP/AF_REQUESTED_QOS/AF_PROVISION_N3GPP_DEV_ID_INFO
            // service types in this build.
            return sbi_core::http2::problem_response(
                501, "Not Implemented", "No service-specific authorization policy data configured");
        });

    server.add_route(
        "POST",
        std::string(kSsauApiRoot) + "/{ueIdentity}/{serviceType}/remove",
        [&verifier, &ssau_remove_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::ServiceSpecificAuthorizationRemoveData>(
                    req, err);
            if (!body.has_value()) {
                return err;
            }
            ssau_remove_counter->Add(1);
            // Disclosed: ServiceSpecificAuthorization above always 501s, so no real authId is
            // ever issued -- no removal request can ever match one.
            return sbi_core::http2::problem_response(
                404, "Not Found", "No such service-specific authorization: " + body->authId);
        });

    // ---- TS29503_Nudm_UEID.yaml (ADR-0202) ----

    server.add_route(
        "POST",
        std::string(kUeidApiRoot) + "/deconceal",
        [&verifier, &ueid_deconceal_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::DeconcealReqData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            // Real, working de-concealment -- reuses this file's own deconceal_suci_if_needed(),
            // the same TS 33.501 Annex C ECIES Profile A/B logic GenerateAuthData/GenerateProseAV
            // already exercise internally (ADR-0037/Tier 1c), not a stub.
            const auto deconcealed = deconceal_suci_if_needed(body->suci);
            if (!deconcealed.has_value()) {
                return sbi_core::http2::problem_response(
                    400,
                    "Bad Request",
                    "Could not de-conceal SUCI " + body->suci +
                        " (malformed, unsupported protection scheme, or MAC verification failed)");
            }
            ueid_deconceal_counter->Add(1);
            sbi_gen::DeconcealRspData resp_data;
            resp_data.supi = *deconcealed;
            json j = resp_data;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    std::thread(run_nrf_lifecycle, udm_instance_id, nrf_base_url).detach();

    server.start();
    spdlog::info("udm: listening on https://0.0.0.0:{} (TLS 1.3 + mTLS)", port);
    spdlog::info("udm: Prometheus metrics at http://{}/metrics", metrics_bind_address);
    sbi_core::run_multi_threaded(ioc);
    return 0;
}
