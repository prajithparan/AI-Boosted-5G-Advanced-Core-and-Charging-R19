// nfs/smf: SMF (Session Management Function), Nsmf_PDUSession /sm-contexts surface.
// Source: specs/5G_APIs-REL-19/TS29502_Nsmf_PDUSession.yaml (commit
// bca84b60a37773133bcae97e5c6c0d10a93b47b6). Phase 2's third NF (PROMPT.md/CLAUDE.md order:
// NRF -> AMF -> SMF -> UDM -> UDR -> AUSF -> PCF).
//
// In scope: the /sm-contexts collection -- CreateSMContext (PostSmContexts),
// RetrieveSMContext (RetrieveSmContext), UpdateSMContext (UpdateSmContext),
// ReleaseSMContext (ReleaseSmContext) -- the actual AMF-triggered PDU Session Establishment flow
// (TS 23.502 clause 4.3.2.2.1), agreed with the user as this turn's scope after
// docs/DECISIONS.md ADR-0020 (multipart/related codec) unblocked CreateSMContext, which is
// multipart/related-ONLY per spec (no application/json alternative exists for its request body).
// This turn (see ADR-0029) additionally wires CreateSMContext/ReleaseSMContext to a real PCF --
// SM Policy Association Establishment/Termination (Npcf_SMPolicyControl) -- now that PCF exists
// (ADR-0028); SMF is a real SBI client to PCF here, the same pattern AUSF's turn established for
// calling UDM (ADR-0027).
//
// Deliberately deferred, not dropped:
// - The /pdu-sessions collection (PostPduSessions/UpdatePduSession/ReleasePduSession/
//   RetrievePduSession) -- the I-SMF/inter-SMF roaming scenario, not the standard AMF-triggered
//   flow this turn targets.
// - SendMoData/TransferMoData -- small-data-over-NAS operations, multipart-only, peripheral to
//   the core session lifecycle.
// - UpdateSMContext still does NOT call PCF's UpdateSMPolicy -- kept out of this turn's scope
//   (only Create/Release wired, see ADR-0029) to keep the turn to the two operations CLAUDE.md's
//   stated PDU-session-establishment goal actually needs.
// - AMF is NOT wired to PCF this turn -- AMF has no real NAS/N1 Registration trigger in this
//   build (no NGAP, ADR-0016), so there is no correct place to attach AM Policy Association
//   Establishment yet; deferred rather than attached to the wrong procedure. See ADR-0029.
//
// Disclosed simplifications, real and not hidden:
// - SMF still has no real UPF (N4/PFCP is Phase 3) and no real UDM (subscription data retrieval).
// - CreateSMContext requires supi/pduSessionId/dnn/sNssai to be present even though
//   SmContextCreateData's schema allows them to be absent (e.g. unauthenticated-SUPI edge cases)
//   -- this build's PCF wiring has nothing to fall back to without them, so a request missing any
//   of them gets a 400, not a silent best-effort attempt. See ADR-0029.
// - The PduSessionType SMF sends to PCF is a fixed default (IPV4), not the UE's real requested
//   type -- that's negotiated inside the NAS SM message (n1SmMsg, an opaque binary blob this
//   build never decodes, same class of gap as every other NAS-decoding simplification here).
// - ReleaseSMContext's DeleteSMPolicy call to PCF is best-effort: local release still succeeds
//   (204) even if PCF is unreachable, so a downstream PCF outage can't strand SMF's own cleanup
//   path -- disclosed, not silently swallowed (logged on failure). CreateSMContext, by contrast,
//   fails closed if PCF is unreachable or errors, matching TS 23.502's real intent that SM Policy
//   Association Establishment failure fails PDU session establishment.
// - UpdateSMContext acknowledges (204) without fabricating SmContextUpdatedData content (EBI
//   allocation, N1/N2 info, ...) for every N2SmInfoType except PATH_SWITCH_REQ -- there is
//   nothing real behind those other fields yet. UPDATE (gap-closure, docs/
//   CAPABILITY_GAP_ANALYSIS.md task #101, ADR-0092): PATH_SWITCH_REQ now has real behavior --
//   decodes the real, AMF-relayed PathSwitchRequestTransfer (n2SmInfo), issues a real PFCP
//   Session Modification creating this project's first-ever real downlink PDR/FAR (with a real
//   TS 29.244 §8.2.56 Outer Header Creation pointing GTP-U at the new gNB), and returns a real
//   PathSwitchRequestAcknowledgeTransfer carrying UPF's own real N3 uplink F-TEID. Real, disclosed
//   scope: the downlink PDR's own match criteria is SourceInterface-only (no UE IP Address IE --
//   this project has never allocated/tracked a real UE IP address anywhere, a real, deeper,
//   separate gap found while scoping this ADR); the other 20 real N2SmInfoType values (PDU_RES_*,
//   HANDOVER_*, ...) remain unreal. AMF's own PathSwitchRequest handler (ADR-0090) does NOT yet
//   call this endpoint -- that relay wiring (plus persisting the SM context ref somewhere AMF can
//   retrieve it across associations) is real, separate, deliberately deferred scope, not built
//   this pass.
// - Error responses use the generic ProblemDetails shape (sbi_core::http2::problem_response,
//   application/problem+json) rather than each operation's bespoke *Error schema
//   (SmContextCreateError, SmContextUpdateError) -- same simplification NRF/AMF already use.
//
// Phase 4 addition (see nfs/chf/src/main.cpp's own file header for CHF's approved scope): SMF
// calls CHF's real Nchf_ConvergedCharging_Create at N40, right after the N4 Session Establishment
// call, using a hardcoded base URL (kChfBase) -- matching this file's own existing pattern for
// PCF/AMF (kPcfBase/kAmfBase), not the Nnrf_NFDiscovery path Stage 2's UPF discovery used (that
// was explicitly the "first real use of this NRF capability", see ADR-0041; every other NF-to-NF
// call in this file, before and after, uses a hardcoded base URL). Best-effort/non-fatal, same
// discipline as the N4 Session Establishment call right above it in CreateSMContext's handler --
// no real billing/quota dependency exists yet for a charging-data failure to correctly block on.
//
// P4.2 rename (see nfs/chf/src/main.cpp's own file header for the full explanation): every
// `sbi_gen::ChargingDataRequest`/`ChargingDataResponse`/`MultipleUnitUsage`/`UsedUnitContainer`/
// `NFIdentification`/`NodeFunctionality` reference in this file is now suffixed
// `_Nchf_ConvergedCharging` -- a real sbi-codegen schema-name collision with the newly-added
// Nchf_OfflineOnlyCharging service, not a functional change. Verified via full rebuild + 146/146
// tests, including this file's own real integration test (test_smf_pdu_session.cpp).
//
// UPDATE (ADR-0201, gap-closure task #160): the two SMF services explicitly deferred above are
// now real, closing a Tier-A gap (neither YAML was in the sbi-codegen pilot set at all):
// - TS29508_Nsmf_EventExposure.yaml (api root /nsmf-event-exposure/v1): all 4 operations, real
//   full subscription CRUD backed by a new EventSubscriptionStore (event_subscription_store.hpp
//   -- same real assign-id/store/remove shape as SmContextStore above, kept as a separate type
//   since an event subscription is a genuinely distinct resource from an SM context). Real
//   structural validation via the generated `NsmfEventExposure` DTO (required
//   `notifId`/`notifUri`/`eventSubs`). Disclosed: no real event notification delivery exists (same
//   gap class as AMF's own subscription types, ADR-0199/ADR-0200) -- subscriptions are stored and
//   can be created/read/replaced/removed for real, but nothing in this build ever fires one, since
//   none of the real `SmfEvent` values (AC_TY_CH, UP_PATH_CH, PDU_SES_REL, ...) have a trigger
//   path wired to them.
// - TS29542_Nsmf_NIDD.yaml (api root /nsmf-nidd/v1): Deliver, real multipart-only structural
//   validation of `DeliverReqData`'s required `mtData`, then checks whether the path's
//   `pduSessionRef` matches a live SM context. Disclosed, deliberate simplification: this
//   project's only real concept of a "live PDU session" is `SmContextStore`'s own
//   `smContextRef`-keyed resource (TS29502_Nsmf_PDUSession.yaml) -- `Nsmf_NIDD`'s own
//   `pduSessionRef` path parameter is treated as referring to that same id space, since there is
//   no separate real "pdu-sessions" resource anywhere else in this build to check against instead.
//   Real 404 if no match; real 204 if found -- disclosed: no real NAS/5G-SM Non-IP-Data-Delivery
//   pipeline exists to actually push the MT data to a UE, same class of gap as every other
//   NAS-adjacent simplification in this file.

#include "sbi_core/datetime.hpp"
#include "sbi_core/http2_client.hpp"
#include "sbi_core/http2_server.hpp"
#include "sbi_core/io_context_pool.hpp"
#include "sbi_core/json_body.hpp"
#include "sbi_core/jwt.hpp"
#include "sbi_core/logging.hpp"
#include "sbi_core/metrics.hpp"
#include "sbi_core/multipart.hpp"
#include "sbi_core/oauth2_client.hpp"
#include "sbi_core/otel.hpp"
#include "sbi_core/rate_limit.hpp"
#include "sbi_core/sbi_headers.hpp"
#include "sbi_core/uuid.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <mutex>
#include <optional>
#include <sys/socket.h>
#include <thread>
#include <unordered_map>

#include "TS26510_CommonData_grp.hpp"
#include "TS29542_Nsmf_NIDD.hpp"
#include "ambr.hpp"
#include "event_subscription_store.hpp"
#include "nas_5gsm_codec.hpp"
#include "ngap_core/ngap_codec.hpp"
#include "pfcp_core/common_ies.hpp"
#include "pfcp_core/header.hpp"
#include "pfcp_core/ie.hpp"
#include "pfcp_core/session_ies.hpp"
#include "pfcp_peer.hpp"
#include "sm_context_store.hpp"

// docs/DECISIONS.md ADR-0077 -- no hardcoded deployment literal in source.
#include "nf_config/nf_config.hpp"

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #101, ADR-0092): real NGAP PER codec for the
// PathSwitchRequestTransfer/PathSwitchRequestAcknowledgeTransfer transparent containers AMF
// relays verbatim as n2SmInfo -- see nfs/smf/CMakeLists.txt's own comment for why this is a real,
// legitimate shared-library dependency (ngap_generated), not an NF-private-header violation.
extern "C" {
#include <AssociatedQosFlowItem.h>
#include <Cause.h>
#include <GTPTunnel.h>
#include <HandoverCommandTransfer.h>
#include <HandoverPreparationUnsuccessfulTransfer.h>
#include <HandoverRequestAcknowledgeTransfer.h>
#include <HandoverResourceAllocationUnsuccessfulTransfer.h>
#include <NonDynamic5QIDescriptor.h>
#include <PDUSessionResourceModifyConfirmTransfer.h>
#include <PDUSessionResourceModifyIndicationTransfer.h>
#include <PDUSessionResourceModifyResponseTransfer.h>
#include <PDUSessionResourceModifyUnsuccessfulTransfer.h>
#include <PDUSessionResourceNotifyReleasedTransfer.h>
#include <PDUSessionResourceNotifyTransfer.h>
#include <PDUSessionResourceReleaseResponseTransfer.h>
#include <PDUSessionResourceSetupRequestTransfer.h>
#include <PDUSessionResourceSetupResponseTransfer.h>
#include <PDUSessionResourceSetupUnsuccessfulTransfer.h>
#include <PDUSessionType.h>
#include <PathSwitchRequestAcknowledgeTransfer.h>
#include <PathSwitchRequestSetupFailedTransfer.h>
#include <PathSwitchRequestTransfer.h>
#include <PathSwitchRequestUnsuccessfulTransfer.h>
#include <QosFlowAcceptedItem.h>
#include <QosFlowAcceptedList.h>
#include <QosFlowListWithCause.h>
#include <QosFlowModifyConfirmItem.h>
#include <QosFlowModifyConfirmList.h>
#include <QosFlowSetupRequestItem.h>
#include <QosFlowSetupRequestList.h>
#include <SecondaryRATDataUsageReportTransfer.h>
#include <UEContextResumeRequestTransfer.h>
#include <UEContextResumeResponseTransfer.h>
#include <UEContextSuspendRequestTransfer.h>
#include <UPTransportLayerInformation.h>
#include <asn_application.h>
#include <per_decoder.h>
#include <per_encoder.h>
}

namespace {

// Real Aligned PER (X.691) encode/decode of a single ASN.1 type -- the exact same underlying
// asn1c runtime entry points nfs/amf/src/ngap_codec.cpp's own per_encode/decode_value use (see
// its own comment on ADR-0031's Aligned PER patch), reimplemented locally here rather than
// reaching into AMF's private ngap_codec.{hpp,cpp} (CLAUDE.md's "no NF includes another NF's
// private headers" rule) -- both this project's own two real, independent uses of the shared
// ngap_generated library.
std::vector<std::uint8_t> ngap_per_encode(const asn_TYPE_descriptor_s* type_descriptor,
                                          const void* value) {
    void* buffer = nullptr;
    const ssize_t encoded = aper_encode_to_new_buffer(type_descriptor, nullptr, value, &buffer);
    if (encoded < 0 || buffer == nullptr) {
        return {};
    }
    std::vector<std::uint8_t> out(static_cast<std::size_t>(encoded));
    std::memcpy(out.data(), buffer, out.size());
    std::free(buffer);
    return out;
}

void* ngap_per_decode(const asn_TYPE_descriptor_s* type_descriptor,
                      const std::vector<std::uint8_t>& bytes) {
    void* out = nullptr;
    const asn_dec_rval_t rv =
        aper_decode_complete(nullptr, type_descriptor, &out, bytes.data(), bytes.size());
    if (rv.code != RC_OK) {
        if (out != nullptr) {
            ASN_STRUCT_FREE(*type_descriptor, out);
        }
        return nullptr;
    }
    return out;
}

using nlohmann::json;

#ifndef CERTS_DIR
#error "CERTS_DIR must be defined by CMake (see nfs/smf/CMakeLists.txt)"
#endif
#ifndef CONFIG_DIR
#error "CONFIG_DIR must be defined by CMake (see nfs/smf/CMakeLists.txt)"
#endif

constexpr const char* kNfType = "SMF";
// No real service-to-rating-group mapping exists in this codebase (that's TS 32.298/32.299
// charging-characteristics configuration, not modeled here) -- every PDU session's usage is
// charged under this one fixed rating group, disclosed here and in ADR-0048, same category of
// simplification as PCF's own fixed-default policy (ADR-0028).
constexpr std::int64_t kDefaultRatingGroup = 1;
constexpr const char* kApiRoot = "/nsmf-pdusession/v1";
// TS29508_Nsmf_EventExposure.yaml / TS29542_Nsmf_NIDD.yaml own real api roots (ADR-0201),
// confirmed via each YAML's own `servers:` block.
constexpr const char* kEventExposureApiRoot = "/nsmf-event-exposure/v1";
constexpr const char* kNiddApiRoot = "/nsmf-nidd/v1";
// This build only ever creates one URR per session (Stage 1) -- shared here, not redefined
// per-function, since Stage 5's real Update URR needs to reference the exact same ID Stage 1's
// Create URR used.
constexpr std::uint32_t kUrrId = 1;
// ADR-0308: this session's QER id. Distinct from kUrrId's namespace -- TS 29.244 gives QER, URR
// and FAR ids their own id spaces, so reusing the value 1 here is correct, not a collision.
constexpr std::uint32_t kQerId = 1;

// Must match nfs/nrf/src/main.cpp's kNrfInstanceId exactly -- see docs/DECISIONS.md ADR-0018.
constexpr const char* kNrfInstanceId = "5ba9a927-1d31-4c8e-8a10-000000000001";

// Same pattern as nfs/nrf and nfs/amf's check_bearer -- see those files' comments for why a
// missing Authorization header is not itself a 401 (bootstrap security alternative:
// `security: [{}, oAuth2ClientCredentials:[...]]` in the YAML).
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
void run_nrf_lifecycle(const std::string& smf_instance_id, const std::string& nrf_base) {
    sbi_core::http2::TlsConfig client_tls{
        .cert_path = CERTS_DIR "/smf/cert.pem",
        .key_path = CERTS_DIR "/smf/key.pem",
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
        http_client, nrf_base + "/oauth2/token", smf_instance_id, "nnrf-nfm", "NRF");

    constexpr int kHeartbeatSeconds = 30;
    json profile{
        {"nfInstanceId", smf_instance_id},
        {"nfType", kNfType},
        {"nfStatus", "REGISTERED"},
        {"ipv4Addresses", json::array({"127.0.0.1"})},
        {"heartBeatTimer", kHeartbeatSeconds},
    };

    while (true) {
        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("smf: OAuth2 token fetch failed: {}", token.error());
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        sbi_core::http2::ClientRequest put_req;
        put_req.method = "PUT";
        put_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + smf_instance_id;
        put_req.headers.emplace("content-type", "application/json");
        put_req.headers.emplace("authorization", "Bearer " + *token);
        put_req.headers.emplace(
            sbi_core::headers::kSenderTimestamp,
            sbi_core::headers::format_sender_timestamp(std::chrono::system_clock::now()));
        put_req.body = profile.dump();

        auto put_resp = http_client.send(put_req);
        if (put_resp.has_value() && (put_resp->status == 200 || put_resp->status == 201)) {
            spdlog::info("smf: registered with NRF (HTTP {})", put_resp->status);
            break;
        }
        spdlog::warn("smf: NRF registration attempt failed, retrying in 5s");
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(kHeartbeatSeconds / 2));

        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("smf: OAuth2 token fetch failed for heartbeat: {}", token.error());
            continue;
        }

        sbi_core::http2::ClientRequest patch_req;
        patch_req.method = "PATCH";
        patch_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + smf_instance_id;
        patch_req.headers.emplace("content-type", "application/json-patch+json");
        patch_req.headers.emplace("authorization", "Bearer " + *token);
        patch_req.body =
            json::array({json{{"op", "replace"}, {"path", "/nfStatus"}, {"value", "REGISTERED"}}})
                .dump();

        auto patch_resp = http_client.send(patch_req);
        if (!patch_resp.has_value() || patch_resp->status != 200) {
            spdlog::warn("smf: heartbeat failed");
        }
    }
}

// Discovers UPF via a real Nnrf_NFDiscovery call (TS 29.510 SearchNFInstances,
// GET /nnrf-disc/v1/nf-instances) -- the first real use of NRF's discovery service anywhere in
// this project; every other NF-to-NF call so far has used a hardcoded base URL constant (see
// kPcfBase/kAmfBase above) rather than dynamic discovery. Not a hardcoded address here because
// ADR-0040 (UPF's own turn) explicitly promised this stage would close that gap for real.
// Retries forever (same "keep trying, NRF/UPF may not be up yet" discipline run_nrf_lifecycle
// itself already uses) until at least one UPF instance with a real ipv4Addresses entry is found.
std::string discover_upf_ipv4(sbi_core::http2::Client& http_client,
                              sbi_core::OAuth2Client& oauth,
                              const std::string& nrf_base) {
    while (true) {
        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("smf: OAuth2 token fetch failed for UPF discovery: {}", token.error());
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = nrf_base + "/nnrf-disc/v1/nf-instances?target-nf-type=UPF&requester-nf-type=SMF";
        req.headers.emplace("authorization", "Bearer " + *token);
        auto resp = http_client.send(req);
        if (!resp.has_value() || resp->status != 200) {
            spdlog::warn("smf: Nnrf_NFDiscovery for UPF failed, retrying in 2s");
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        try {
            const auto body = json::parse(resp->body);
            for (const auto& instance : body.at("nfInstances")) {
                if (instance.contains("ipv4Addresses") && !instance.at("ipv4Addresses").empty()) {
                    return instance.at("ipv4Addresses")[0].get<std::string>();
                }
            }
        } catch (const json::exception& e) {
            spdlog::warn("smf: malformed Nnrf_NFDiscovery response: {}", e.what());
        }
        spdlog::info("smf: no UPF registered with NRF yet, retrying discovery in 2s");
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

// Real PFCP/N4 Association Setup with UPF (TS 29.244 SS6.2.6.2/SS7.4.4.1-2) -- Stage 2 of
// docs/DECISIONS.md's Phase 3 staged plan (ADR-0041). Runs on its own dedicated thread doing
// blocking UDP I/O, same discipline as nfs/upf/src/main.cpp's own PFCP loop and every other
// blocking-transport thread in this project (ADR-0006/ADR-0030). Retries the whole procedure
// (T1 timer + N1 retries per TS 29.244 SS6.4, both this build's own reasonable fixed choices --
// the spec leaves the exact values implementation-specific) until UPF replies with Cause=accepted.
// This lab's loopback-only scope, reused by both PFCP client call sites below.
constexpr std::array<std::uint8_t, 4> kSmfNodeIpv4{127, 0, 0, 1};

// Thread-safe holder for the UPF endpoint learned via run_pfcp_lifecycle's real Nnrf_NFDiscovery +
// Association Setup (Stage 2) -- read by CreateSMContext's route handler (the ioc thread) to
// perform Stage 3's real N4 Session Establishment. Deferred from Stage 2 on purpose (ADR-0041:
// "nothing reads it yet") until this stage actually needed it -- exactly the kind of storage
// CLAUDE.md's engineering rules say not to add before there's a real reader.
class UpfEndpointStore {
public:
    void set(std::string ipv4) {
        std::lock_guard<std::mutex> lock(mutex_);
        ipv4_ = std::move(ipv4);
    }
    std::optional<std::string> get() {
        std::lock_guard<std::mutex> lock(mutex_);
        return ipv4_;
    }

private:
    std::mutex mutex_;
    std::optional<std::string> ipv4_;
};

// ADR-0050 Stage 3: what a real Sx Session Report Request's SEID resolves to -- the pieces
// perform_n40_charging_data_update needs to build a real Nchf_ConvergedCharging_Update call.
// `cp_seid` is the same value perform_n4_session_establishment generated and sent as the CP
// F-SEID at Session Establishment (UPF's Stage 2 code echoes it back verbatim as the Session
// Report Request's header SEID, per TS 29.244's addressing rule already relied on elsewhere in
// this file). Written by CreateSMContext's handler (the ioc thread); read from PfcpPeer's own
// receive thread when a report arrives -- hence the mutex, same reasoning as
// nfs/upf/src/main.cpp's TeidSessionStore.
struct CpSeidSessionInfo {
    std::string supi;
    std::string charging_data_ref;
    // ADR-0050 Stage 5: what a later real Session Modification Request (pushing a re-authorized
    // quota back to UPF) needs -- the UP function's own F-SEID for this session (the header SEID
    // a CP-originated session-related message must carry, per this file's own established
    // addressing-rule comment) and the UPF IP to send it to.
    std::uint64_t up_seid = 0;
    std::string upf_ip;
};

class CpSeidSessionStore {
public:
    void put(std::uint64_t cp_seid, CpSeidSessionInfo info) {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_[cp_seid] = std::move(info);
    }

    // std::nullopt if no session was ever registered for this cp_seid. Pure read -- the real
    // per-ChargingDataRef invocation-sequence counter this used to also track here has moved to
    // ChargingDataInvocationSeqStore (see its own comment for why: a session with no granted quota
    // never gets an entry here at all, but its ChargingDataRef can still be Released, so invocation
    // sequencing can't be keyed by cp_seid).
    std::optional<CpSeidSessionInfo> get(std::uint64_t cp_seid) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = sessions_.find(cp_seid);
        if (it == sessions_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

private:
    std::mutex mutex_;
    std::unordered_map<std::uint64_t, CpSeidSessionInfo> sessions_;
};

// Pending-items cleanup turn (2026-08-10): the real, single per-ChargingDataRef invocation-
// sequence counter TS 32.291 requires ("strictly increasing" across Create/Update/Release for one
// charging data resource). Replaces two separate, inconsistent counters this codebase used to
// have -- CpSeidSessionStore's own per-cp_seid counter (ADR-0050 Stage 3, used only by Update) and
// Release's hardcoded literal `2` (ADR-0046) -- which could collide if both an Update and a
// Release landed on the same ChargingDataRef, a real TS 32.291 violation ADR-0050's own Stage 3/6
// text disclosed but didn't fix. Keyed by charging_data_ref (a string), not cp_seid: a session
// with no granted quota never gets a CpSeidSessionStore entry at all (Stage 3's own registration
// guard, since only URR'd sessions can ever produce a Usage Report), but its ChargingDataRef can
// still be Released -- cp_seid was never the right key for this.
class ChargingDataInvocationSeqStore {
public:
    // Seeds the counter for a freshly-created ChargingDataRef -- 1 was Create's own
    // invocationSequenceNumber, so the next real call (Update or Release, whichever happens first)
    // gets 2.
    void put(const std::string& charging_data_ref) {
        std::lock_guard<std::mutex> lock(mutex_);
        next_seq_[charging_data_ref] = 2;
    }

    // Returns the invocationSequenceNumber to use for this call, advancing the counter for next
    // time. Falls back to 2 (logged) for a ref this store never learned about, rather than
    // fabricating a plausible-looking value that might collide -- shouldn't happen given every
    // real Create call site registers its ref here, but checked rather than assumed.
    std::int64_t get_and_advance(const std::string& charging_data_ref) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = next_seq_.find(charging_data_ref);
        if (it == next_seq_.end()) {
            spdlog::warn("smf: no invocation-sequence counter registered for ChargingDataRef={}, "
                         "using an untracked value",
                         charging_data_ref);
            return 2;
        }
        return it->second++;
    }

private:
    std::mutex mutex_;
    std::unordered_map<std::string, std::int64_t> next_seq_;
};

void run_pfcp_lifecycle(const std::string& smf_instance_id,
                        const std::string& nrf_base,
                        UpfEndpointStore& upf_endpoint_store,
                        smf::PfcpPeer& pfcp_peer) {
    sbi_core::http2::TlsConfig client_tls{
        .cert_path = CERTS_DIR "/smf/cert.pem",
        .key_path = CERTS_DIR "/smf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client http_client(std::move(client_tls));
    sbi_core::OAuth2Client oauth(
        http_client, nrf_base + "/oauth2/token", smf_instance_id, "nnrf-disc", "NRF");

    const std::string upf_ip = discover_upf_ipv4(http_client, oauth, nrf_base);
    spdlog::info("smf: discovered UPF at {} via Nnrf_NFDiscovery", upf_ip);

    const boost::asio::ip::udp::endpoint upf_endpoint(boost::asio::ip::make_address(upf_ip),
                                                      pfcp_core::kPfcpPort);

    while (true) {
        pfcp_core::Header req_header;
        req_header.has_seid = false;
        req_header.message_type = pfcp_core::MessageType::AssociationSetupRequest;
        req_header.sequence_number = pfcp_peer.allocate_sequence_number();

        std::vector<std::uint8_t> ies;
        pfcp_core::encode_ie(ies,
                             static_cast<std::uint16_t>(pfcp_core::IeType::NodeId),
                             pfcp_core::encode_node_id_ipv4(kSmfNodeIpv4));
        pfcp_core::encode_ie(ies,
                             static_cast<std::uint16_t>(pfcp_core::IeType::RecoveryTimeStamp),
                             pfcp_core::encode_recovery_time_stamp(std::time(nullptr)));
        pfcp_core::encode_ie(ies,
                             static_cast<std::uint16_t>(pfcp_core::IeType::CpFunctionFeatures),
                             pfcp_core::encode_cp_function_features_none());

        auto pdu = pfcp_core::encode_header(req_header, static_cast<std::uint16_t>(ies.size()));
        pdu.insert(pdu.end(), ies.begin(), ies.end());

        const auto resp_ie_bytes = pfcp_peer.send_request_and_await_response(
            upf_endpoint,
            pdu,
            pfcp_core::MessageType::AssociationSetupResponse,
            req_header.sequence_number,
            "Association Setup");
        const auto resp_ies =
            resp_ie_bytes.has_value() ? pfcp_core::decode_ies(*resp_ie_bytes) : std::nullopt;
        const auto* cause_ie =
            resp_ies.has_value()
                ? pfcp_core::find_ie(*resp_ies,
                                     static_cast<std::uint16_t>(pfcp_core::IeType::Cause))
                : nullptr;
        const auto cause =
            cause_ie != nullptr ? pfcp_core::decode_cause(cause_ie->value) : std::nullopt;

        if (cause.has_value() && *cause == pfcp_core::Cause::RequestAccepted) {
            spdlog::info("smf: PFCP Sx Association established with UPF at {}", upf_ip);
            upf_endpoint_store.set(upf_ip);
            return;
        }
        spdlog::warn("smf: PFCP Association Setup did not succeed (cause={}), backing off and "
                     "restarting the procedure",
                     cause.has_value() ? static_cast<int>(*cause) : -1);
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

// Real N4 Session Establishment (TS 29.244 §7.5.2/§7.5.3, ADR-0042), SMF's side. Builds one
// uplink PDR (PDR ID=1, Source Interface=Access, F-TEID CH-requested so UPF allocates its own
// local GTP-U endpoint) associated with one FAR (FAR ID=1, Apply Action=FORW, Destination
// Interface=Core) -- the minimal real slice TS 23.502's PDU Session Establishment needs. No
// downlink PDR/FAR: that needs the gNB's N3 GTP-U endpoint (NGAP PDU Session Resource Setup,
// still not implemented -- see nfs/upf/src/main.cpp's own disclosure). Best-effort: failure is
// logged, not fatal to CreateSMContext's own 201 response, same discipline ADR-0038 already
// established for the N1N2MessageTransfer call below this one in the handler.
//
// ADR-0050 Stage 1: if granted_total_volume_octets is set (a real grant from CHF's rating engine,
// ADR-0048), also creates one URR (URR ID=1, volume-based measurement) and associates it with the
// uplink PDR via Create PDR's own optional URR ID field (TS 29.244 §7.5.2.2's real table -- "This
// IE shall be present if a measurement action shall be applied to packets matching this PDR"),
// with Volume Threshold set to 90% of the granted quota and Volume Quota set to the full granted
// amount -- the exact ratio TS 29.244 Annex C.2.1.1's real worked example uses (quota=100 Mbytes,
// threshold=90 Mbytes), not an arbitrary choice.
//
// ADR-0050 Stage 3: returns the real CP F-SEID this call generated (instead of plain bool) on
// success, so CreateSMContext's handler can register it in CpSeidSessionStore -- the only way to
// resolve a later, real Session Report Request's header SEID back to this session's
// ChargingDataRef for a real Nchf_ConvergedCharging_Update call.
struct N4EstablishmentResult {
    std::uint64_t cp_seid = 0;
    // ADR-0050 Stage 5: the UP function's own F-SEID, needed to address a later real Session
    // Modification Request back to this exact session -- TS 29.244's addressing rule this file
    // already relies on elsewhere means a CP-originated session-related message's header SEID must
    // carry the *receiving* entity's own SEID, i.e. UPF's, not SMF's own cp_seid.
    std::uint64_t up_seid = 0;
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #101, ADR-0092): UPF's own real, allocated
    // N3 (uplink GTP-U) TEID + IPv4 -- computed by this function already (CreatedPdr's own
    // F-TEID) but, before this ADR, discarded after only being logged. Real, load-bearing use: a
    // real PathSwitchRequestAcknowledgeTransfer's own `uL-NGU-UP-TNLInformation` needs UPF's real
    // N3 receive endpoint, not a fabricated one -- see handle_update_sm_context's own
    // PATH_SWITCH_REQ branch.
    std::optional<std::uint32_t> ul_teid;
    std::optional<std::array<std::uint8_t, 4>> ul_ipv4;
};

// ADR-0260: rebuild a Cause for an outgoing transfer from a decoded incoming one, so SMF echoes
// the cause NG-RAN actually reported instead of inventing one. The five real CHOICE arms are
// enumerated longs, so this is a value copy. The sixth, `choice_Extensions`, is a pointer into
// the decoded structure: shallow-copying it would double-free on the outgoing transfer's own
// free, and this codec has no deep-copy helper. An extension-coded Cause therefore returns false
// and the caller answers 500 rather than substituting a cause value SMF made up.
bool copy_cause(const Cause_t& in, Cause_t& out) {
    switch (in.present) {
        case Cause_PR_radioNetwork:
            out.choice.radioNetwork = in.choice.radioNetwork;
            break;
        case Cause_PR_transport:
            out.choice.transport = in.choice.transport;
            break;
        case Cause_PR_nas:
            out.choice.nas = in.choice.nas;
            break;
        case Cause_PR_protocol:
            out.choice.protocol = in.choice.protocol;
            break;
        case Cause_PR_misc:
            out.choice.misc = in.choice.misc;
            break;
        default:
            return false;
    }
    out.present = in.present;
    return true;
}

// ADR-0260: the N2SmInfoType values SMF really receives, validates by decoding, and acknowledges
// with 204 because they carry no instruction SMF can act on beyond what it records. Decoding is
// not ceremony: it is what makes a malformed transfer a 400 instead of a silently accepted 204.
struct AckOnlyN2SmInfo {
    const std::string* type_value;
    asn_TYPE_descriptor_t* descriptor;
    const char* transfer_name;
};

// ADR-0259: a real NG-RAN GTP-U endpoint, read out of an NGAP UPTransportLayerInformation.
// Returns std::nullopt when the CHOICE is not a gTPTunnel or the address/TEID are not the real
// 4-octet IPv4 shape -- callers answer 400 rather than guessing at a tunnel.
struct GtpTunnelEndpoint {
    std::array<std::uint8_t, 4> ipv4{};
    std::uint32_t teid{};
};

std::optional<GtpTunnelEndpoint> read_gtp_tunnel(const UPTransportLayerInformation_t* tnl) {
    if (tnl == nullptr || tnl->present != UPTransportLayerInformation_PR_gTPTunnel ||
        tnl->choice.gTPTunnel == nullptr) {
        return std::nullopt;
    }
    const auto* gtp_tunnel = tnl->choice.gTPTunnel;
    if (gtp_tunnel->transportLayerAddress.size != 4 || gtp_tunnel->gTP_TEID.size != 4) {
        return std::nullopt;
    }
    GtpTunnelEndpoint endpoint;
    std::copy(gtp_tunnel->transportLayerAddress.buf,
              gtp_tunnel->transportLayerAddress.buf + 4,
              endpoint.ipv4.begin());
    endpoint.teid = (static_cast<std::uint32_t>(gtp_tunnel->gTP_TEID.buf[0]) << 24) |
                    (static_cast<std::uint32_t>(gtp_tunnel->gTP_TEID.buf[1]) << 16) |
                    (static_cast<std::uint32_t>(gtp_tunnel->gTP_TEID.buf[2]) << 8) |
                    static_cast<std::uint32_t>(gtp_tunnel->gTP_TEID.buf[3]);
    return endpoint;
}

// ADR-0259: every N2SmInfoType that hands SMF a new NG-RAN downlink endpoint needs exactly the
// same real UPF consequence -- repoint the downlink FAR's OuterHeaderCreation at it. ADR-0092
// built that inline for PATH_SWITCH_REQ; four more values need it verbatim, so it lives here once
// rather than five times. Returns an error string on refusal, std::nullopt on success.
//
// Unchanged from ADR-0092 and still disclosed: the downlink PDR's match criteria is
// SourceInterface=Core only, with no UE IP Address IE, because this project has never allocated
// or tracked a real UE IP address anywhere.
std::optional<std::string> install_downlink_far(smf::PfcpPeer& pfcp_peer,
                                                std::uint64_t up_seid,
                                                const std::string& upf_ip,
                                                std::uint32_t gnb_teid,
                                                const std::array<std::uint8_t, 4>& gnb_ipv4,
                                                const char* label) {
    std::vector<std::uint8_t> dl_pdi;
    pfcp_core::encode_ie(dl_pdi,
                         static_cast<std::uint16_t>(pfcp_core::IeType::SourceInterface),
                         pfcp_core::encode_source_interface(pfcp_core::InterfaceValue::Core));

    std::vector<std::uint8_t> dl_create_pdr;
    pfcp_core::encode_ie(dl_create_pdr,
                         static_cast<std::uint16_t>(pfcp_core::IeType::PdrId),
                         pfcp_core::encode_pdr_id(2));
    pfcp_core::encode_ie(dl_create_pdr,
                         static_cast<std::uint16_t>(pfcp_core::IeType::Precedence),
                         pfcp_core::encode_precedence(100));
    pfcp_core::encode_ie(dl_create_pdr, static_cast<std::uint16_t>(pfcp_core::IeType::Pdi), dl_pdi);
    pfcp_core::encode_ie(dl_create_pdr,
                         static_cast<std::uint16_t>(pfcp_core::IeType::FarId),
                         pfcp_core::encode_far_id(2));

    std::vector<std::uint8_t> dl_forwarding_parameters;
    pfcp_core::encode_ie(
        dl_forwarding_parameters,
        static_cast<std::uint16_t>(pfcp_core::IeType::DestinationInterface),
        pfcp_core::encode_destination_interface(pfcp_core::InterfaceValue::Access));
    pfcp_core::encode_ie(dl_forwarding_parameters,
                         static_cast<std::uint16_t>(pfcp_core::IeType::OuterHeaderCreation),
                         pfcp_core::encode_outer_header_creation_gtpu_ipv4(gnb_teid, gnb_ipv4));

    std::vector<std::uint8_t> dl_create_far;
    pfcp_core::encode_ie(dl_create_far,
                         static_cast<std::uint16_t>(pfcp_core::IeType::FarId),
                         pfcp_core::encode_far_id(2));
    pfcp_core::encode_ie(dl_create_far,
                         static_cast<std::uint16_t>(pfcp_core::IeType::ApplyAction),
                         pfcp_core::encode_apply_action_forward());
    pfcp_core::encode_ie(dl_create_far,
                         static_cast<std::uint16_t>(pfcp_core::IeType::ForwardingParameters),
                         dl_forwarding_parameters);

    std::vector<std::uint8_t> mod_ies;
    pfcp_core::encode_ie(
        mod_ies, static_cast<std::uint16_t>(pfcp_core::IeType::CreatePdr), dl_create_pdr);
    pfcp_core::encode_ie(
        mod_ies, static_cast<std::uint16_t>(pfcp_core::IeType::CreateFar), dl_create_far);

    pfcp_core::Header mod_header;
    mod_header.has_seid = true;
    mod_header.seid = up_seid;
    mod_header.message_type = pfcp_core::MessageType::SessionModificationRequest;
    mod_header.sequence_number = pfcp_peer.allocate_sequence_number();
    auto mod_pdu = pfcp_core::encode_header(mod_header, static_cast<std::uint16_t>(mod_ies.size()));
    mod_pdu.insert(mod_pdu.end(), mod_ies.begin(), mod_ies.end());

    const boost::asio::ip::udp::endpoint upf_endpoint(boost::asio::ip::make_address(upf_ip),
                                                      pfcp_core::kPfcpPort);
    const std::string description = std::string("Session Modification (") + label + " real DL FAR)";
    const auto mod_resp_ies = pfcp_peer.send_request_and_await_response(
        upf_endpoint,
        mod_pdu,
        pfcp_core::MessageType::SessionModificationResponse,
        mod_header.sequence_number,
        description.c_str());
    const auto decoded_mod_resp_ies =
        mod_resp_ies.has_value() ? pfcp_core::decode_ies(*mod_resp_ies) : std::nullopt;
    const auto* mod_cause_ie =
        decoded_mod_resp_ies.has_value()
            ? pfcp_core::find_ie(*decoded_mod_resp_ies,
                                 static_cast<std::uint16_t>(pfcp_core::IeType::Cause))
            : nullptr;
    const auto mod_cause =
        mod_cause_ie != nullptr ? pfcp_core::decode_cause(mod_cause_ie->value) : std::nullopt;
    if (!mod_cause.has_value() || *mod_cause != pfcp_core::Cause::RequestAccepted) {
        return std::string("UPF rejected the real PFCP Session Modification for ") + label +
               "'s new downlink FAR (UP F-SEID=" + std::to_string(up_seid) + ")";
    }
    spdlog::info("smf: {} real N4 Session Modification succeeded (UP F-SEID={:#x}, new gNB "
                 "TEID={:#x}) -- real downlink GTP-U tunnel now points at the new gNB",
                 label,
                 up_seid,
                 gnb_teid);
    return std::nullopt;
}

// ADR-0308: ambr_to_kbps lives in nfs/smf/src/ambr.{hpp,cpp} so it is testable without
// linking the whole SMF binary.
std::optional<N4EstablishmentResult>
perform_n4_session_establishment(smf::PfcpPeer& pfcp_peer,
                                 const std::string& upf_ip,
                                 std::uint8_t pdu_session_id,
                                 std::optional<std::uint64_t> granted_total_volume_octets,
                                 // ADR-0308: PCF's own authorised session AMBR, so UPF actually
                                 // ENFORCES the throttle PCF decided. Absent = no QER sent, which
                                 // is exactly the previous behaviour.
                                 const std::optional<std::string>& ambr_uplink = std::nullopt,
                                 const std::optional<std::string>& ambr_downlink = std::nullopt) {
    const boost::asio::ip::udp::endpoint upf_endpoint(boost::asio::ip::make_address(upf_ip),
                                                      pfcp_core::kPfcpPort);

    static std::atomic<std::uint64_t> next_cp_seid{1};
    const std::uint64_t cp_seid = next_cp_seid++;

    std::vector<std::uint8_t> pdi;
    pfcp_core::encode_ie(pdi,
                         static_cast<std::uint16_t>(pfcp_core::IeType::SourceInterface),
                         pfcp_core::encode_source_interface(pfcp_core::InterfaceValue::Access));
    pfcp_core::encode_ie(pdi,
                         static_cast<std::uint16_t>(pfcp_core::IeType::FTeid),
                         pfcp_core::encode_f_teid_choose_ipv4());

    std::vector<std::uint8_t> create_pdr;
    pfcp_core::encode_ie(create_pdr,
                         static_cast<std::uint16_t>(pfcp_core::IeType::PdrId),
                         pfcp_core::encode_pdr_id(1));
    pfcp_core::encode_ie(create_pdr,
                         static_cast<std::uint16_t>(pfcp_core::IeType::Precedence),
                         pfcp_core::encode_precedence(100));
    pfcp_core::encode_ie(create_pdr, static_cast<std::uint16_t>(pfcp_core::IeType::Pdi), pdi);
    pfcp_core::encode_ie(create_pdr,
                         static_cast<std::uint16_t>(pfcp_core::IeType::FarId),
                         pfcp_core::encode_far_id(1));
    if (granted_total_volume_octets.has_value()) {
        pfcp_core::encode_ie(create_pdr,
                             static_cast<std::uint16_t>(pfcp_core::IeType::UrrId),
                             pfcp_core::encode_urr_id(kUrrId));
    }

    std::vector<std::uint8_t> forwarding_parameters;
    pfcp_core::encode_ie(forwarding_parameters,
                         static_cast<std::uint16_t>(pfcp_core::IeType::DestinationInterface),
                         pfcp_core::encode_destination_interface(pfcp_core::InterfaceValue::Core));

    std::vector<std::uint8_t> create_far;
    pfcp_core::encode_ie(create_far,
                         static_cast<std::uint16_t>(pfcp_core::IeType::FarId),
                         pfcp_core::encode_far_id(1));
    pfcp_core::encode_ie(create_far,
                         static_cast<std::uint16_t>(pfcp_core::IeType::ApplyAction),
                         pfcp_core::encode_apply_action_forward());
    pfcp_core::encode_ie(create_far,
                         static_cast<std::uint16_t>(pfcp_core::IeType::ForwardingParameters),
                         forwarding_parameters);

    pfcp_core::FSeid cp_f_seid;
    cp_f_seid.seid = cp_seid;
    cp_f_seid.ipv4 = kSmfNodeIpv4;

    std::vector<std::uint8_t> ies;
    pfcp_core::encode_ie(ies,
                         static_cast<std::uint16_t>(pfcp_core::IeType::NodeId),
                         pfcp_core::encode_node_id_ipv4(kSmfNodeIpv4));
    pfcp_core::encode_ie(ies,
                         static_cast<std::uint16_t>(pfcp_core::IeType::FSeid),
                         pfcp_core::encode_f_seid_ipv4(cp_f_seid));
    pfcp_core::encode_ie(ies, static_cast<std::uint16_t>(pfcp_core::IeType::CreatePdr), create_pdr);
    pfcp_core::encode_ie(ies, static_cast<std::uint16_t>(pfcp_core::IeType::CreateFar), create_far);

    if (granted_total_volume_octets.has_value()) {
        const auto volume_threshold_octets =
            static_cast<std::uint64_t>(static_cast<double>(*granted_total_volume_octets) * 0.9);

        std::vector<std::uint8_t> create_urr;
        pfcp_core::encode_ie(create_urr,
                             static_cast<std::uint16_t>(pfcp_core::IeType::UrrId),
                             pfcp_core::encode_urr_id(kUrrId));
        pfcp_core::encode_ie(create_urr,
                             static_cast<std::uint16_t>(pfcp_core::IeType::MeasurementMethod),
                             pfcp_core::encode_measurement_method_volume());
        pfcp_core::encode_ie(create_urr,
                             static_cast<std::uint16_t>(pfcp_core::IeType::ReportingTriggers),
                             pfcp_core::encode_reporting_triggers_volume());
        pfcp_core::encode_ie(create_urr,
                             static_cast<std::uint16_t>(pfcp_core::IeType::VolumeThreshold),
                             pfcp_core::encode_volume_total(volume_threshold_octets));
        pfcp_core::encode_ie(create_urr,
                             static_cast<std::uint16_t>(pfcp_core::IeType::VolumeQuota),
                             pfcp_core::encode_volume_total(*granted_total_volume_octets));
        pfcp_core::encode_ie(
            ies, static_cast<std::uint16_t>(pfcp_core::IeType::CreateUrr), create_urr);

        spdlog::info("smf: provisioning URR {} for pduSessionId {}: threshold={} octets, "
                     "quota={} octets",
                     kUrrId,
                     pdu_session_id,
                     volume_threshold_octets,
                     *granted_total_volume_octets);
    }

    // ADR-0308: the QER that makes PCF's throttle real on the user plane.
    //
    // README listed tiered/fair-use throttling as "decided, not enforced": PCF's `authSessAmbr`
    // reached the UE in the NAS Establishment Accept and was recorded, but never reached UPF, so
    // nothing rate-limited anything. UPF has had real QER enforcement in its datapath since
    // ADR-0071 (`register_qer` with an MBR); SMF simply never sent one. This closes that.
    //
    // Gate status is OPEN/OPEN: this QER exists to apply a RATE limit, not to block traffic.
    // Closing a gate here would drop the session's packets entirely, which is a different policy
    // decision (and one PCF expresses differently), so it is not inferred from an AMBR.
    if (ambr_uplink.has_value() || ambr_downlink.has_value()) {
        const auto ul_kbps =
            ambr_uplink.has_value() ? smf::ambr_to_kbps(*ambr_uplink) : std::nullopt;
        const auto dl_kbps =
            ambr_downlink.has_value() ? smf::ambr_to_kbps(*ambr_downlink) : std::nullopt;
        if (!ul_kbps.has_value() && !dl_kbps.has_value()) {
            spdlog::warn("smf: PCF's authSessAmbr (ul={}, dl={}) is not a rate this build can "
                         "parse -- NO QER sent, so the session is UNTHROTTLED rather than "
                         "throttled by a guessed value",
                         ambr_uplink.value_or("<none>"),
                         ambr_downlink.value_or("<none>"));
        } else {
            std::vector<std::uint8_t> create_qer;
            pfcp_core::encode_ie(create_qer,
                                 static_cast<std::uint16_t>(pfcp_core::IeType::QerId),
                                 pfcp_core::encode_qer_id(kQerId));
            pfcp_core::GateStatus gate{};
            gate.ul_closed = false;
            gate.dl_closed = false;
            pfcp_core::encode_ie(create_qer,
                                 static_cast<std::uint16_t>(pfcp_core::IeType::GateStatus),
                                 pfcp_core::encode_gate_status(gate));
            pfcp_core::Mbr mbr{};
            mbr.ul_kbps = ul_kbps.value_or(0);
            mbr.dl_kbps = dl_kbps.value_or(0);
            pfcp_core::encode_ie(create_qer,
                                 static_cast<std::uint16_t>(pfcp_core::IeType::Mbr),
                                 pfcp_core::encode_mbr(mbr));
            pfcp_core::encode_ie(
                ies, static_cast<std::uint16_t>(pfcp_core::IeType::CreateQer), create_qer);
            spdlog::info("smf: provisioning QER {} for pduSessionId {}: MBR ul={} kbps dl={} kbps "
                         "(from PCF authSessAmbr ul='{}' dl='{}')",
                         kQerId,
                         pdu_session_id,
                         mbr.ul_kbps,
                         mbr.dl_kbps,
                         ambr_uplink.value_or("<none>"),
                         ambr_downlink.value_or("<none>"));
        }
    }

    pfcp_core::Header req_header;
    // Sx Session Establishment Request always has S=1 with SEID=0 -- UP's own SEID for this
    // session doesn't exist yet (TS 29.244 §7.2.2.4.2's explicit list of when SEID=0 is used).
    req_header.has_seid = true;
    req_header.seid = 0;
    req_header.message_type = pfcp_core::MessageType::SessionEstablishmentRequest;
    req_header.sequence_number = pfcp_peer.allocate_sequence_number();

    auto pdu = pfcp_core::encode_header(req_header, static_cast<std::uint16_t>(ies.size()));
    pdu.insert(pdu.end(), ies.begin(), ies.end());

    const auto resp_ie_bytes = pfcp_peer.send_request_and_await_response(
        upf_endpoint,
        pdu,
        pfcp_core::MessageType::SessionEstablishmentResponse,
        req_header.sequence_number,
        "Session Establishment");
    if (!resp_ie_bytes.has_value()) {
        return std::nullopt;
    }
    const auto resp_ies = pfcp_core::decode_ies(*resp_ie_bytes);
    const auto* cause_ie =
        resp_ies.has_value()
            ? pfcp_core::find_ie(*resp_ies, static_cast<std::uint16_t>(pfcp_core::IeType::Cause))
            : nullptr;
    const auto cause =
        cause_ie != nullptr ? pfcp_core::decode_cause(cause_ie->value) : std::nullopt;
    if (!cause.has_value() || *cause != pfcp_core::Cause::RequestAccepted) {
        spdlog::warn("smf: UPF rejected N4 Session Establishment for pduSessionId {} (cause={})",
                     pdu_session_id,
                     cause.has_value() ? static_cast<int>(*cause) : -1);
        return std::nullopt;
    }

    std::optional<pfcp_core::FSeid> up_f_seid;
    if (const auto* up_f_seid_ie =
            pfcp_core::find_ie(*resp_ies, static_cast<std::uint16_t>(pfcp_core::IeType::FSeid));
        up_f_seid_ie != nullptr) {
        up_f_seid = pfcp_core::decode_f_seid_ipv4(up_f_seid_ie->value);
    }
    std::optional<std::uint32_t> allocated_teid;
    std::optional<std::array<std::uint8_t, 4>> allocated_ipv4;
    if (const auto* created_pdr_ie = pfcp_core::find_ie(
            *resp_ies, static_cast<std::uint16_t>(pfcp_core::IeType::CreatedPdr));
        created_pdr_ie != nullptr) {
        if (const auto created_pdr_ies = pfcp_core::decode_ies(created_pdr_ie->value);
            created_pdr_ies.has_value()) {
            if (const auto* f_teid_ie = pfcp_core::find_ie(
                    *created_pdr_ies, static_cast<std::uint16_t>(pfcp_core::IeType::FTeid));
                f_teid_ie != nullptr) {
                if (const auto allocated =
                        pfcp_core::decode_f_teid_allocated_ipv4(f_teid_ie->value);
                    allocated.has_value()) {
                    allocated_teid = allocated->teid;
                    allocated_ipv4 = allocated->ipv4;
                }
            }
        }
    }

    spdlog::info("smf: N4 Session Establishment succeeded for pduSessionId {}, UPF F-SEID={:#x}, "
                 "allocated uplink F-TEID={:#x}",
                 pdu_session_id,
                 up_f_seid.has_value() ? up_f_seid->seid : 0,
                 allocated_teid.value_or(0));
    if (!up_f_seid.has_value()) {
        // Real spec: a successful (Cause=RequestAccepted) Establishment Response always carries
        // the UP F-SEID -- this is a malformed-but-accepted response, not a normal outcome.
        // Establishment itself already succeeded (logged above), but Stage 5's later real Session
        // Modification has no way to address this session without it -- disclosed as std::nullopt
        // here rather than fabricating a SEID.
        spdlog::warn("smf: N4 Session Establishment response for pduSessionId {} had no UP F-SEID, "
                     "quota re-authorization won't be able to reach UPF for this session",
                     pdu_session_id);
        return std::nullopt;
    }
    return N4EstablishmentResult{cp_seid, up_f_seid->seid, allocated_teid, allocated_ipv4};
}

// Real Nchf_ConvergedCharging_Create (TS 32.291, N40, ADR-0044), SMF's side -- see this file's own
// header for the approved Phase 4 scope. Sends only the 3 mandatory ChargingDataRequest fields
// plus subscriberIdentifier; pDUSessionChargingInformation is deliberately left unset, matching
// nfs/chf/src/main.cpp's own disclosed "no real rating engine yet" scope -- nothing on the CHF
// side would use richer charging information yet, so sending it would be padding, not real
// content. invocationSequenceNumber is always 1: this is the first (and, this stage, only)
// charging data invocation SMF ever sends for a given PDU session -- see chf's own file header for
// why the response doesn't get independently sequenced either. Best-effort: see file header.
// Returns the allocated ChargingDataRef (parsed from CHF's `location` header, same extraction
// pattern already used for PCF's smPolicyId above) plus, if CHF granted one, the real
// GrantedUnit.totalVolume octet count (ADR-0050, Stage 1) -- so perform_n4_session_establishment
// can provision a real Create URR with that exact quota rather than a fabricated one.
struct ChargingDataCreateResult {
    std::string charging_data_ref;
    std::optional<std::uint64_t> granted_total_volume_octets;
};

std::optional<ChargingDataCreateResult>
perform_n40_charging_data_create(sbi_core::http2::Client& chf_client,
                                 sbi_core::OAuth2Client& chf_oauth,
                                 const std::string& chf_base,
                                 const std::string& smf_instance_id,
                                 const std::string& supi,
                                 std::uint8_t pdu_session_id) {
    auto token = chf_oauth.get_bearer_token();
    if (!token.has_value()) {
        spdlog::warn("smf: could not obtain a token for CHF, skipping Nchf_ConvergedCharging_"
                     "Create for pduSessionId {}: {}",
                     pdu_session_id,
                     token.error());
        return std::nullopt;
    }

    sbi_gen::NFIdentification_Nchf_ConvergedCharging nf_id{};
    nf_id.nFName = smf_instance_id;
    nf_id.nFIPv4Address = "127.0.0.1";
    nf_id.nodeFunctionality.value = sbi_gen::NodeFunctionality_Nchf_ConvergedCharging::SMF;

    sbi_gen::ChargingDataRequest_Nchf_ConvergedCharging chf_req{};
    chf_req.nfConsumerIdentification = nf_id;
    chf_req.invocationTimeStamp = sbi_core::format_rfc3339(std::chrono::system_clock::now());
    chf_req.invocationSequenceNumber = 1;
    chf_req.subscriberIdentifier = supi;
    // ADR-0048: a real online-charging quota request. ratingGroup is TS 32.291's one mandatory
    // field on MultipleUnitUsage (confirmed directly against the vendored YAML's `required:`
    // block); requestedUnit is deliberately omitted -- this build has no real traffic-volume
    // estimator to request against, so CHF grants a full quota from its own rate-plan lookup
    // rather than SMF requesting a specific amount (see nfs/chf/src/main.cpp's own comment).
    sbi_gen::MultipleUnitUsage_Nchf_ConvergedCharging unit_usage{};
    unit_usage.ratingGroup = kDefaultRatingGroup;
    chf_req.multipleUnitUsage =
        std::vector<sbi_gen::MultipleUnitUsage_Nchf_ConvergedCharging>{unit_usage};

    sbi_core::http2::ClientRequest chf_http_req;
    chf_http_req.method = "POST";
    chf_http_req.url = chf_base + "/nchf-convergedcharging/v3/chargingdata";
    chf_http_req.headers.emplace("content-type", "application/json");
    chf_http_req.headers.emplace("authorization", "Bearer " + *token);
    chf_http_req.body = json(chf_req).dump();

    auto chf_resp = chf_client.send(chf_http_req);
    if (!chf_resp.has_value()) {
        spdlog::warn("smf: could not reach CHF for Nchf_ConvergedCharging_Create, pduSessionId "
                     "{}: {}",
                     pdu_session_id,
                     chf_resp.error());
        return std::nullopt;
    }
    if (chf_resp->status != 201) {
        spdlog::warn("smf: CHF Nchf_ConvergedCharging_Create returned unexpected status {} for "
                     "pduSessionId {}",
                     chf_resp->status,
                     pdu_session_id);
        return std::nullopt;
    }
    std::string charging_data_ref;
    if (const auto location_it = chf_resp->headers.find("location");
        location_it != chf_resp->headers.end()) {
        const auto& location = location_it->second;
        charging_data_ref = location.substr(location.find_last_of('/') + 1);
    }
    if (charging_data_ref.empty()) {
        spdlog::warn("smf: CHF Nchf_ConvergedCharging_Create succeeded but returned no usable "
                     "location header for pduSessionId {}, Release will not be possible for this "
                     "session",
                     pdu_session_id);
        return std::nullopt;
    }
    // ADR-0050 Stage 1: extract the real granted quota (if any) so N4 Session Establishment can
    // provision a real Create URR with it. Best-effort parse -- an empty/malformed body still
    // leaves the ChargingDataRef usable, just with no URR provisioned this call (same "grants
    // nothing" fallback ADR-0048 already established for a catalog with no matching offering).
    std::optional<std::uint64_t> granted_total_volume_octets;
    try {
        const auto resp_body =
            json::parse(chf_resp->body).get<sbi_gen::ChargingDataResponse_Nchf_ConvergedCharging>();
        if (resp_body.multipleUnitInformation.has_value() &&
            !resp_body.multipleUnitInformation->empty() &&
            (*resp_body.multipleUnitInformation)[0].grantedUnit.has_value() &&
            (*resp_body.multipleUnitInformation)[0].grantedUnit->totalVolume.has_value()) {
            granted_total_volume_octets =
                *(*resp_body.multipleUnitInformation)[0].grantedUnit->totalVolume;
        }
    } catch (const json::exception& e) {
        spdlog::warn("smf: could not parse CHF's ChargingDataResponse body for pduSessionId {}: {}",
                     pdu_session_id,
                     e.what());
    }

    spdlog::info("smf: Nchf_ConvergedCharging_Create succeeded for pduSessionId {}, "
                 "ChargingDataRef={}, granted total volume={}",
                 pdu_session_id,
                 charging_data_ref,
                 granted_total_volume_octets.has_value()
                     ? std::to_string(*granted_total_volume_octets) + " octets"
                     : "none");
    return ChargingDataCreateResult{charging_data_ref, granted_total_volume_octets};
}

// Real Nchf_ConvergedCharging_Release (TS 32.291, N40, ADR-0046), SMF's side.
// POST /chargingdata/{ChargingDataRef}/release, same real spec shape as Create's request body
// (ChargingDataRequest) -- sent with the same minimal 3-mandatory-fields-plus-subscriberIdentifier
// content as Create, since there's no richer charging information collected between Create and
// Release in this build either. Best-effort: matches DeleteSMPolicy's discipline in
// ReleaseSMContext's handler -- local session release must not get stuck on CHF being unreachable.
//
// `invocation_sequence_number` is real, caller-supplied (via ChargingDataInvocationSeqStore), not
// a hardcoded literal -- see that store's own comment for the real TS 32.291 "strictly increasing
// per invocation" violation this closes (a session that had at least one Update before Release
// would previously send invocationSequenceNumber=2 for Release even after Update already used 2,
// 3, ... for the same ChargingDataRef).
bool perform_n40_charging_data_release(sbi_core::http2::Client& chf_client,
                                       sbi_core::OAuth2Client& chf_oauth,
                                       const std::string& chf_base,
                                       const std::string& smf_instance_id,
                                       const std::string& supi,
                                       const std::string& charging_data_ref,
                                       std::int64_t invocation_sequence_number) {
    auto token = chf_oauth.get_bearer_token();
    if (!token.has_value()) {
        spdlog::warn("smf: could not obtain a token for CHF, skipping Nchf_ConvergedCharging_"
                     "Release for ChargingDataRef={}: {}",
                     charging_data_ref,
                     token.error());
        return false;
    }

    sbi_gen::NFIdentification_Nchf_ConvergedCharging nf_id{};
    nf_id.nFName = smf_instance_id;
    nf_id.nFIPv4Address = "127.0.0.1";
    nf_id.nodeFunctionality.value = sbi_gen::NodeFunctionality_Nchf_ConvergedCharging::SMF;

    sbi_gen::ChargingDataRequest_Nchf_ConvergedCharging chf_req{};
    chf_req.nfConsumerIdentification = nf_id;
    chf_req.invocationTimeStamp = sbi_core::format_rfc3339(std::chrono::system_clock::now());
    chf_req.invocationSequenceNumber = invocation_sequence_number;
    chf_req.subscriberIdentifier = supi;

    sbi_core::http2::ClientRequest chf_http_req;
    chf_http_req.method = "POST";
    chf_http_req.url =
        chf_base + "/nchf-convergedcharging/v3/chargingdata/" + charging_data_ref + "/release";
    chf_http_req.headers.emplace("content-type", "application/json");
    chf_http_req.headers.emplace("authorization", "Bearer " + *token);
    chf_http_req.body = json(chf_req).dump();

    auto chf_resp = chf_client.send(chf_http_req);
    if (!chf_resp.has_value()) {
        spdlog::warn("smf: could not reach CHF for Nchf_ConvergedCharging_Release, "
                     "ChargingDataRef={}: {}",
                     charging_data_ref,
                     chf_resp.error());
        return false;
    }
    if (chf_resp->status != 204) {
        spdlog::warn("smf: CHF Nchf_ConvergedCharging_Release returned unexpected status {} for "
                     "ChargingDataRef={}",
                     chf_resp->status,
                     charging_data_ref);
        return false;
    }
    spdlog::info("smf: Nchf_ConvergedCharging_Release succeeded for ChargingDataRef={}",
                 charging_data_ref);
    return true;
}

// Real Nchf_ConvergedCharging_Update (TS 32.291, N40, ADR-0050 Stage 3), SMF's side. POST
// /chargingdata/{ChargingDataRef}/update -- confirmed directly against the real, vendored
// TS32291_Nchf_ConvergedCharging.yaml (the path exists verbatim, sharing Create's own
// ChargingDataRequest/ChargingDataResponse schemas). Reports real consumed usage via
// multipleUnitUsage[0].usedUnitContainer[0]: MultipleUnitUsage's one mandatory field is
// ratingGroup (confirmed from the same YAML's `required:` block, same discipline as Create's own
// comment); UsedUnitContainer's one mandatory field is localSequenceNumber (confirmed
// independently), populated here with the real UR-SEQN this Usage Report carried (TS 29.244's own
// per-URR report counter -- this build has no separate CHF-facing usage-report sequence concept
// of its own, and reusing it is a real, traceable value rather than an arbitrary counter).
//
// invocation_sequence_number is real, caller-supplied (via ChargingDataInvocationSeqStore, shared
// with Create and Release) -- the interaction this comment used to flag as unfixed (Release's own
// hardcoded literal 2 potentially colliding with an Update's real counter) is closed; see
// ChargingDataInvocationSeqStore's own comment.
//
// ADR-0050 Stage 5: also returns the real re-authorized GrantedUnit.totalVolume (when CHF's real
// Update endpoint, now built, returns one) -- the exact grant Stage 5 needs to compute the new,
// absolute Volume Threshold/Volume Quota values for a follow-on Session Modification Request.
struct ChargingDataUpdateResult {
    std::optional<std::uint64_t> granted_total_volume_octets;
};

std::optional<ChargingDataUpdateResult>
perform_n40_charging_data_update(sbi_core::http2::Client& chf_client,
                                 sbi_core::OAuth2Client& chf_oauth,
                                 const std::string& chf_base,
                                 const std::string& smf_instance_id,
                                 const std::string& supi,
                                 const std::string& charging_data_ref,
                                 std::int64_t invocation_sequence_number,
                                 std::int64_t rating_group,
                                 std::int64_t local_sequence_number,
                                 std::uint64_t used_total_volume_octets) {
    auto token = chf_oauth.get_bearer_token();
    if (!token.has_value()) {
        spdlog::warn("smf: could not obtain a token for CHF, skipping Nchf_ConvergedCharging_"
                     "Update for ChargingDataRef={}: {}",
                     charging_data_ref,
                     token.error());
        return std::nullopt;
    }

    sbi_gen::NFIdentification_Nchf_ConvergedCharging nf_id{};
    nf_id.nFName = smf_instance_id;
    nf_id.nFIPv4Address = "127.0.0.1";
    nf_id.nodeFunctionality.value = sbi_gen::NodeFunctionality_Nchf_ConvergedCharging::SMF;

    sbi_gen::UsedUnitContainer_Nchf_ConvergedCharging used_unit{};
    used_unit.localSequenceNumber = local_sequence_number;
    used_unit.totalVolume = static_cast<std::int64_t>(used_total_volume_octets);

    sbi_gen::MultipleUnitUsage_Nchf_ConvergedCharging unit_usage{};
    unit_usage.ratingGroup = rating_group;
    unit_usage.usedUnitContainer =
        std::vector<sbi_gen::UsedUnitContainer_Nchf_ConvergedCharging>{used_unit};

    sbi_gen::ChargingDataRequest_Nchf_ConvergedCharging chf_req{};
    chf_req.nfConsumerIdentification = nf_id;
    chf_req.invocationTimeStamp = sbi_core::format_rfc3339(std::chrono::system_clock::now());
    chf_req.invocationSequenceNumber = invocation_sequence_number;
    chf_req.subscriberIdentifier = supi;
    chf_req.multipleUnitUsage =
        std::vector<sbi_gen::MultipleUnitUsage_Nchf_ConvergedCharging>{unit_usage};

    sbi_core::http2::ClientRequest chf_http_req;
    chf_http_req.method = "POST";
    chf_http_req.url =
        chf_base + "/nchf-convergedcharging/v3/chargingdata/" + charging_data_ref + "/update";
    chf_http_req.headers.emplace("content-type", "application/json");
    chf_http_req.headers.emplace("authorization", "Bearer " + *token);
    chf_http_req.body = json(chf_req).dump();

    auto chf_resp = chf_client.send(chf_http_req);
    if (!chf_resp.has_value()) {
        spdlog::warn("smf: could not reach CHF for Nchf_ConvergedCharging_Update, "
                     "ChargingDataRef={}: {}",
                     charging_data_ref,
                     chf_resp.error());
        return std::nullopt;
    }
    if (chf_resp->status != 200) {
        spdlog::warn("smf: CHF Nchf_ConvergedCharging_Update returned unexpected status {} for "
                     "ChargingDataRef={}",
                     chf_resp->status,
                     charging_data_ref);
        return std::nullopt;
    }

    // Same best-effort parse discipline as perform_n40_charging_data_create's own grant parsing --
    // an empty/malformed body still means the Update itself succeeded (logged below), just with no
    // re-authorized quota to push to UPF this call.
    std::optional<std::uint64_t> granted_total_volume_octets;
    try {
        const auto resp_body =
            json::parse(chf_resp->body).get<sbi_gen::ChargingDataResponse_Nchf_ConvergedCharging>();
        if (resp_body.multipleUnitInformation.has_value() &&
            !resp_body.multipleUnitInformation->empty() &&
            (*resp_body.multipleUnitInformation)[0].grantedUnit.has_value() &&
            (*resp_body.multipleUnitInformation)[0].grantedUnit->totalVolume.has_value()) {
            granted_total_volume_octets =
                *(*resp_body.multipleUnitInformation)[0].grantedUnit->totalVolume;
        }
    } catch (const json::exception& e) {
        spdlog::warn(
            "smf: could not parse CHF's ChargingDataResponse body for ChargingDataRef={}: {}",
            charging_data_ref,
            e.what());
    }

    spdlog::info("smf: Nchf_ConvergedCharging_Update succeeded for ChargingDataRef={}, reported {} "
                 "octets used, re-authorized {}",
                 charging_data_ref,
                 used_total_volume_octets,
                 granted_total_volume_octets.has_value()
                     ? std::to_string(*granted_total_volume_octets) + " octets"
                     : "nothing");
    return ChargingDataUpdateResult{granted_total_volume_octets};
}

// Real Sx Session Modification Request (TS 29.244 §7.5.4, ADR-0050 Stage 5), SMF's side. Pushes a
// real, updated Volume Threshold/Volume Quota for the session's already-created URR via an Update
// URR IE (real type=13, confirmed against TS 29.244 Table 7.5.4.4-1) -- only URR ID (mandatory)
// plus the two fields actually being changed are sent, matching that table's own "present if X
// needs to be modified" convention (Measurement Method/Reporting Triggers are omitted: unchanged
// since Create). Header SEID is `up_seid` (the UP function's own F-SEID for this session, per this
// file's own established addressing-rule comment), not `cp_seid`.
bool perform_n4_session_modification_update_urr(smf::PfcpPeer& pfcp_peer,
                                                const std::string& upf_ip,
                                                std::uint64_t up_seid,
                                                std::uint32_t urr_id,
                                                std::uint64_t new_volume_threshold_octets,
                                                std::uint64_t new_volume_quota_octets) {
    const boost::asio::ip::udp::endpoint upf_endpoint(boost::asio::ip::make_address(upf_ip),
                                                      pfcp_core::kPfcpPort);

    std::vector<std::uint8_t> update_urr;
    pfcp_core::encode_ie(update_urr,
                         static_cast<std::uint16_t>(pfcp_core::IeType::UrrId),
                         pfcp_core::encode_urr_id(urr_id));
    pfcp_core::encode_ie(update_urr,
                         static_cast<std::uint16_t>(pfcp_core::IeType::VolumeThreshold),
                         pfcp_core::encode_volume_total(new_volume_threshold_octets));
    pfcp_core::encode_ie(update_urr,
                         static_cast<std::uint16_t>(pfcp_core::IeType::VolumeQuota),
                         pfcp_core::encode_volume_total(new_volume_quota_octets));

    std::vector<std::uint8_t> ies;
    pfcp_core::encode_ie(ies, static_cast<std::uint16_t>(pfcp_core::IeType::UpdateUrr), update_urr);

    pfcp_core::Header req_header;
    req_header.has_seid = true;
    req_header.seid = up_seid;
    req_header.message_type = pfcp_core::MessageType::SessionModificationRequest;
    req_header.sequence_number = pfcp_peer.allocate_sequence_number();

    auto pdu = pfcp_core::encode_header(req_header, static_cast<std::uint16_t>(ies.size()));
    pdu.insert(pdu.end(), ies.begin(), ies.end());

    const auto resp_ie_bytes = pfcp_peer.send_request_and_await_response(
        upf_endpoint,
        pdu,
        pfcp_core::MessageType::SessionModificationResponse,
        req_header.sequence_number,
        "Session Modification");
    if (!resp_ie_bytes.has_value()) {
        return false;
    }
    const auto resp_ies = pfcp_core::decode_ies(*resp_ie_bytes);
    const auto* cause_ie =
        resp_ies.has_value()
            ? pfcp_core::find_ie(*resp_ies, static_cast<std::uint16_t>(pfcp_core::IeType::Cause))
            : nullptr;
    const auto cause =
        cause_ie != nullptr ? pfcp_core::decode_cause(cause_ie->value) : std::nullopt;
    if (!cause.has_value() || *cause != pfcp_core::Cause::RequestAccepted) {
        spdlog::warn("smf: UPF rejected N4 Session Modification for URR {} (UP F-SEID={:#x}, "
                     "cause={})",
                     urr_id,
                     up_seid,
                     cause.has_value() ? static_cast<int>(*cause) : -1);
        return false;
    }
    spdlog::info("smf: N4 Session Modification succeeded for URR {} (UP F-SEID={:#x}): "
                 "threshold={} octets, quota={} octets",
                 urr_id,
                 up_seid,
                 new_volume_threshold_octets,
                 new_volume_quota_octets);
    return true;
}

// ADR-0279. 0x45, from simulators/ransim/vendor/UERANSIM/src/lib/nas/enums.hpp's
// ESmCause::INSUFFICIENT_RESOURCES_FOR_SPECIFIC_SLICE (0b01000101) -- the same real source
// nas_5gsm_codec.cpp reads its message types from.
constexpr std::uint8_t kSmCauseInsufficientResourcesForSlice = 0x45;

// The PTI the UE used, so the reject can echo it. Re-extracts the n1SmMsg part rather than
// hoisting the whole (much later) Accept-building block up here: this runs only on the refusal
// path, and moving that block would restructure the success path for the sake of an error one.
// A PTI of 0 is what the UE sent if there is no decodable request, which is also what the spec's
// "no procedure transaction identity assigned" value is.
std::optional<std::uint8_t> extract_request_pti(const sbi_core::http2::Request& req) {
    const auto content_type_it = req.headers.find("content-type");
    if (content_type_it == req.headers.end() ||
        !sbi_core::multipart::is_multipart_related(content_type_it->second)) {
        return std::nullopt;
    }
    auto parts = sbi_core::multipart::parse(content_type_it->second, req.body);
    if (!parts.has_value()) {
        return std::nullopt;
    }
    for (const auto& part : *parts) {
        if (!part.content_id.has_value()) {
            continue;
        }
        const std::vector<std::uint8_t> bytes(part.body.begin(), part.body.end());
        const auto info = smf::nas5gsm::decode_establishment_request(bytes);
        if (info.has_value()) {
            return info->pti;
        }
    }
    return std::nullopt;
}

// ADR-0279: distinguishes "this slice is full" from "this slice is not mine".
//
// TS 29.536's AcuFailureReason carries both: EXCEED_MAX_* means the quota is exhausted, while
// SLICE_NOT_FOUND means NSACF holds no admission configuration for that S-NSSAI at all. Only the
// first is a refusal. Treating the second as one would let an NSACF that simply does not manage a
// slice block every session on it -- the same reasoning as the fail-open on an unreachable NSACF,
// applied to an answer rather than to a failure to answer.
//
// Found by a test regression, not by review: the Sx-association gate creates sessions on sst=1
// with no SD, a different S-NSSAI from the configured sst=1/sd=000001, so NSACF correctly answered
// SLICE_NOT_FOUND and a blanket refusal turned that into a 403.
bool acu_failure_is_a_real_refusal(const std::string& body) {
    try {
        const auto parsed = json::parse(body);
        if (!parsed.contains("acuFailureList") || !parsed.at("acuFailureList").is_array()) {
            return false;
        }
        for (const auto& item : parsed.at("acuFailureList")) {
            if (!item.contains("reason")) {
                continue;
            }
            if (item.at("reason").get<std::string>().rfind("EXCEED_MAX", 0) == 0) {
                return true;
            }
        }
    } catch (const json::exception&) {
        // An unparseable failure list is not evidence of a refusal.
        return false;
    }
    return false;
}

// ADR-0279: ask NSACF whether this PDU session may be established. Returns false ONLY on a real
// refusal -- unreachable, no token, or an unexpected status all admit, the same deliberate
// fail-open AMF's own report_ue_to_nsacf documents and for the same reason: NSAC is an operator
// quota mechanism with no authority over a session it could not evaluate.
bool admit_pdu_session_with_nsacf(sbi_core::http2::Client& nsacf_client,
                                  sbi_core::OAuth2Client& nsacf_oauth,
                                  const std::string& nsacf_base_url,
                                  const std::string& supi,
                                  std::int32_t pdu_session_id,
                                  const sbi_gen::Snssai& snssai) {
    if (nsacf_base_url.empty()) {
        return true;
    }
    const auto token = nsacf_oauth.get_bearer_token();
    if (!token.has_value()) {
        spdlog::error("smf: could not obtain an NSACF token for slice admission control: {}",
                      token.error());
        return true;
    }

    json snssai_json{{"sst", snssai.sst}};
    if (snssai.sd.has_value()) {
        snssai_json["sd"] = *snssai.sd;
    }
    json request;
    request["pduACRequestInfo"] = json::array(
        {json{{"supi", supi},
              {"anType", "3GPP_ACCESS"},
              {"pduSessionId", pdu_session_id},
              {"acuOperationList",
               json::array({json{{"updateFlag", "INCREASE"}, {"snssai", snssai_json}}})}}});

    sbi_core::http2::ClientRequest http_req;
    http_req.method = "POST";
    http_req.url = nsacf_base_url + "/nnsacf-nsac/v1/slices/pdus";
    http_req.headers.emplace("content-type", "application/json");
    http_req.headers.emplace("authorization", "Bearer " + *token);
    http_req.body = request.dump();

    auto resp = nsacf_client.send(http_req);
    if (!resp.has_value()) {
        spdlog::warn("smf: NSACF NumOfPDUsUpdate call failed for SUPI {} pduSessionId {}: {} -- "
                     "admitting, see this function's own fail-open note",
                     supi,
                     pdu_session_id,
                     resp.error());
        return true;
    }
    if (resp->status == 204) {
        spdlog::info("smf: NSACF admitted pduSessionId {} for SUPI {} on slice sst={}",
                     pdu_session_id,
                     supi,
                     snssai.sst);
        return true;
    }
    if (resp->status == 200) {
        // Only an EXCEED_MAX_* reason is a refusal -- see acu_failure_is_a_real_refusal.
        if (acu_failure_is_a_real_refusal(resp->body)) {
            spdlog::warn("smf: NSACF REFUSED pduSessionId {} for SUPI {}: {}",
                         pdu_session_id,
                         supi,
                         resp->body);
            return false;
        }
        spdlog::info("smf: NSACF reported a non-quota ACU failure for pduSessionId {} ({}) -- "
                     "admitting, it holds no quota for this slice",
                     pdu_session_id,
                     resp->body);
        return true;
    }
    spdlog::warn("smf: NSACF NumOfPDUsUpdate returned unexpected status {} for SUPI {} -- "
                 "admitting, see this function's own fail-open note",
                 resp->status,
                 supi);
    return true;
}

// ADR-0277: report an established/released PDU session to NSACF (Nnsacf_NSAC NumOfPDUsUpdate).
//
// Keyed per (SUPI, pduSessionId) on NSACF's side, which is why both are sent: one UE may hold
// several sessions on the same slice, and NSAC counts sessions, not UEs, on this service.
void report_pdu_session_to_nsacf(sbi_core::http2::Client& nsacf_client,
                                 sbi_core::OAuth2Client& nsacf_oauth,
                                 const std::string& nsacf_base_url,
                                 const std::string& supi,
                                 std::int64_t pdu_session_id,
                                 std::int64_t sst,
                                 const std::optional<std::string>& sd,
                                 bool increase) {
    if (nsacf_base_url.empty() || supi.empty()) {
        return;
    }
    const auto token = nsacf_oauth.get_bearer_token();
    if (!token.has_value()) {
        spdlog::error("smf: could not obtain an NSACF token for slice admission control: {}",
                      token.error());
        return;
    }

    json snssai{{"sst", sst}};
    if (sd.has_value()) {
        snssai["sd"] = *sd;
    }
    json request;
    request["pduACRequestInfo"] =
        json::array({json{{"supi", supi},
                          {"anType", "3GPP_ACCESS"},
                          {"pduSessionId", pdu_session_id},
                          {"acuOperationList",
                           json::array({json{{"updateFlag", increase ? "INCREASE" : "DECREASE"},
                                             {"snssai", snssai}}})}}});

    sbi_core::http2::ClientRequest http_req;
    http_req.method = "POST";
    http_req.url = nsacf_base_url + "/nnsacf-nsac/v1/slices/pdus";
    http_req.headers.emplace("content-type", "application/json");
    http_req.headers.emplace("authorization", "Bearer " + *token);
    http_req.body = request.dump();

    auto resp = nsacf_client.send(http_req);
    if (!resp.has_value()) {
        spdlog::warn("smf: NSACF NumOfPDUsUpdate call failed for SUPI {} pduSessionId {}: {}",
                     supi,
                     pdu_session_id,
                     resp.error());
        return;
    }
    if (resp->status == 204) {
        spdlog::info("smf: NSACF admitted pduSessionId {} for SUPI {} on slice sst={}",
                     pdu_session_id,
                     supi,
                     sst);
        return;
    }
    if (resp->status == 200) {
        spdlog::warn("smf: NSACF REJECTED pduSessionId {} for SUPI {}: {} -- this session is NOT "
                     "being torn down (enforcement is a separate increment, ADR-0277)",
                     pdu_session_id,
                     supi,
                     resp->body);
        return;
    }
    spdlog::warn(
        "smf: NSACF NumOfPDUsUpdate returned unexpected status {} for SUPI {}", resp->status, supi);
}

} // namespace

int main() {
    const auto config = nf_config::load("smf", CONFIG_DIR);
    const auto port = nf_config::require<unsigned short>(config, "port");
    const auto metrics_bind_address =
        nf_config::require<std::string>(config, "metrics_bind_address");
    const auto nrf_base_url =
        nf_config::require<std::string>(config, "nrf_base_url", "SMF_NRF_BASE_URL");
    const auto self_base_url =
        nf_config::require<std::string>(config, "self_base_url", "SMF_SELF_BASE_URL");
    const auto pcf_base_url =
        nf_config::require<std::string>(config, "pcf_base_url", "SMF_PCF_BASE_URL");
    // ADR-0277: NSACF, for Network Slice Admission Control on the number of PDU sessions
    // (TS 23.501 §5.15.11, TS 23.502 §4.3.2.2.1).
    const auto nsacf_base_url =
        nf_config::require<std::string>(config, "nsacf_base_url", "SMF_NSACF_BASE_URL");
    const auto amf_base_url =
        nf_config::require<std::string>(config, "amf_base_url", "SMF_AMF_BASE_URL");
    const auto chf_base_url =
        nf_config::require<std::string>(config, "chf_base_url", "SMF_CHF_BASE_URL");

    sbi_core::init_logging("smf");
    sbi_core::init_tracing("smf");
    sbi_core::init_metrics(metrics_bind_address);

    const std::string smf_instance_id = sbi_core::generate_uuid_v4();
    spdlog::info("smf: starting, nfInstanceId={}", smf_instance_id);

    sbi_core::http2::TlsConfig server_tls{
        .cert_path = CERTS_DIR "/smf/cert.pem",
        .key_path = CERTS_DIR "/smf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };

    sbi_core::jwt::Verifier verifier(CERTS_DIR "/nrf-jwt/public.pem", kNrfInstanceId);

    // ADR-0049 (quota-consumption-tracking turn), Stage 0: SMF's one persistent, bidirectional
    // PFCP peer -- replaces the old per-call ephemeral sockets so UPF can reach SMF directly for
    // an unsolicited Sx Session Report Request. Handler installed further down (ADR-0050 Stage 3),
    // once its own dependencies (a dedicated CHF client, CpSeidSessionStore) exist -- pfcp_peer
    // itself must be constructed first regardless, since the handler needs to capture it by
    // reference (see PfcpPeer's own two-phase-construction comment for why this can't be a
    // constructor parameter).
    smf::PfcpPeer pfcp_peer;

    // SMF's own client identity + token source for calling PCF -- separate http2::Client/
    // OAuth2Client from run_nrf_lifecycle's (which runs on its own thread; this one is only ever
    // touched from route handlers, which all run on ioc's single thread -- see
    // docs/DECISIONS.md ADR-0027, which established this exact pattern for AUSF calling UDM).
    sbi_core::http2::TlsConfig pcf_client_tls{
        .cert_path = CERTS_DIR "/smf/cert.pem",
        .key_path = CERTS_DIR "/smf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client pcf_client(std::move(pcf_client_tls));
    sbi_core::OAuth2Client pcf_oauth(
        pcf_client, nrf_base_url + "/oauth2/token", smf_instance_id, "npcf-smpolicycontrol", "PCF");

    // ADR-0277: SMF's own client for Nnsacf_NSAC, same one-client-per-peer pattern as PCF's above.
    sbi_core::http2::TlsConfig nsacf_client_tls{
        .cert_path = CERTS_DIR "/smf/cert.pem",
        .key_path = CERTS_DIR "/smf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client nsacf_client(std::move(nsacf_client_tls));
    sbi_core::OAuth2Client nsacf_oauth(
        nsacf_client, nrf_base_url + "/oauth2/token", smf_instance_id, "nnsacf-nsac", "NSACF");

    // SMF's own client identity + token source for calling AMF's Namf_Communication
    // N1N2MessageTransfer (TS29518_Namf_Communication.yaml) -- the real mechanism for delivering
    // the PDU Session Establishment Accept back to the UE (ADR-0038), same one-client-per-NF
    // pattern as pcf_client above.
    sbi_core::http2::TlsConfig amf_client_tls{
        .cert_path = CERTS_DIR "/smf/cert.pem",
        .key_path = CERTS_DIR "/smf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client amf_client(std::move(amf_client_tls));
    sbi_core::OAuth2Client amf_oauth(
        amf_client, nrf_base_url + "/oauth2/token", smf_instance_id, "namf-comm", "AMF");

    // SMF's own client identity + token source for calling CHF's Nchf_ConvergedCharging (N40,
    // ADR-0044) -- same one-client-per-NF pattern as pcf_client/amf_client above.
    sbi_core::http2::TlsConfig chf_client_tls{
        .cert_path = CERTS_DIR "/smf/cert.pem",
        .key_path = CERTS_DIR "/smf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client chf_client(std::move(chf_client_tls));
    sbi_core::OAuth2Client chf_oauth(chf_client,
                                     nrf_base_url + "/oauth2/token",
                                     smf_instance_id,
                                     "nchf-convergedcharging",
                                     "CHF");

    // ADR-0050 Stage 3: a SEPARATE, dedicated CHF client for the Session Report handler below --
    // that handler runs on PfcpPeer's own receive thread, not the HTTP/2 server's ioc thread that
    // `chf_client` above is only safe to touch from (same one-client-per-thread discipline this
    // file's own comments already document for pcf_client/amf_client/chf_client, and the same
    // reasoning Stage 0's ADR text already used for AMF's own NGAP-thread AUSF client).
    sbi_core::http2::TlsConfig chf_report_client_tls{
        .cert_path = CERTS_DIR "/smf/cert.pem",
        .key_path = CERTS_DIR "/smf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client chf_report_client(std::move(chf_report_client_tls));
    sbi_core::OAuth2Client chf_report_oauth(chf_report_client,
                                            nrf_base_url + "/oauth2/token",
                                            smf_instance_id,
                                            "nchf-convergedcharging",
                                            "CHF");

    // ADR-0050 Stage 3: resolves a real Session Report Request's header SEID back to the session
    // it belongs to -- populated by CreateSMContext's handler, read here.
    CpSeidSessionStore cp_seid_sessions;
    // Pending-items cleanup turn: the real, single per-ChargingDataRef invocation-sequence
    // counter, shared by Create/Update/Release -- see its own class comment.
    ChargingDataInvocationSeqStore charging_data_invocation_seq;

    // ADR-0050 Stage 3/5: the real handler. Decodes the real Usage Report content, looks the
    // session up, calls a real Nchf_ConvergedCharging_Update, and (Stage 5) if CHF re-authorized a
    // fresh grant, pushes it to UPF via a real Session Modification Request. Acks the Sx Session
    // Report Request immediately, before any of that -- PFCP acknowledgment is a different
    // protocol layer from the SBI/N40 work below.
    //
    // ADR-0050 Stage 5's real, load-bearing constraint: the CHF call and the Session Modification
    // call are both real, blocking network round-trips, and this handler itself runs synchronously
    // on PfcpPeer's own receive thread (see pfcp_peer.cpp's receive_loop -- the handler is invoked
    // in-line, not on a separate dispatch thread). Session Modification's response can only ever be
    // delivered BY that same receive thread; calling send_request_and_await_response for it
    // directly from here would deadlock the thread against itself. Both calls are therefore handed
    // off to a detached std::thread. Captured references (pfcp_peer, chf_report_client/oauth,
    // smf_instance_id) are all main()'s own locals, alive for this process's entire lifetime (it
    // never terminates, same disclosed simplification as every other NF in this project) -- safe to
    // capture by reference into a detached thread for that reason, not despite it.
    pfcp_peer.set_session_report_handler([&pfcp_peer,
                                          &cp_seid_sessions,
                                          &charging_data_invocation_seq,
                                          &chf_report_client,
                                          &chf_report_oauth,
                                          &chf_base_url,
                                          &smf_instance_id](
                                             const pfcp_core::Header& header,
                                             const std::vector<std::uint8_t>& ie_bytes,
                                             const boost::asio::ip::udp::endpoint& sender) {
        spdlog::info("smf: received real Sx Session Report Request from {} (seq={})",
                     sender.address().to_string(),
                     header.sequence_number);

        std::vector<std::uint8_t> ack_ies;
        pfcp_core::encode_ie(ack_ies,
                             static_cast<std::uint16_t>(pfcp_core::IeType::Cause),
                             pfcp_core::encode_cause(pfcp_core::Cause::RequestAccepted));
        pfcp_core::Header ack_header;
        ack_header.has_seid = true;
        ack_header.seid = header.seid;
        ack_header.message_type = pfcp_core::MessageType::SessionReportResponse;
        ack_header.sequence_number = header.sequence_number;
        auto ack_pdu =
            pfcp_core::encode_header(ack_header, static_cast<std::uint16_t>(ack_ies.size()));
        ack_pdu.insert(ack_pdu.end(), ack_ies.begin(), ack_ies.end());
        pfcp_peer.send_fire_and_forget(sender, ack_pdu);

        // Decoding is fast/non-blocking -- safe to keep inline. Only the real network I/O
        // below is handed off.
        const auto ies = pfcp_core::decode_ies(ie_bytes);
        const auto* report_type_ie =
            ies.has_value() ? pfcp_core::find_ie(
                                  *ies, static_cast<std::uint16_t>(pfcp_core::IeType::ReportType))
                            : nullptr;
        const auto* usage_report_ie =
            ies.has_value() ? pfcp_core::find_ie(
                                  *ies, static_cast<std::uint16_t>(pfcp_core::IeType::UsageReport))
                            : nullptr;
        if (report_type_ie == nullptr ||
            !pfcp_core::decode_report_type_has_usage_report(report_type_ie->value) ||
            usage_report_ie == nullptr) {
            spdlog::warn("smf: Sx Session Report Request from {} has no real Usage Report "
                         "content, no CHF Update call",
                         sender.address().to_string());
            return;
        }
        const auto usage_ies = pfcp_core::decode_ies(usage_report_ie->value);
        const auto* urr_id_ie =
            usage_ies.has_value()
                ? pfcp_core::find_ie(*usage_ies,
                                     static_cast<std::uint16_t>(pfcp_core::IeType::UrrId))
                : nullptr;
        const auto* ur_seqn_ie =
            usage_ies.has_value()
                ? pfcp_core::find_ie(*usage_ies,
                                     static_cast<std::uint16_t>(pfcp_core::IeType::UrSeqn))
                : nullptr;
        const auto* volume_ie =
            usage_ies.has_value()
                ? pfcp_core::find_ie(
                      *usage_ies, static_cast<std::uint16_t>(pfcp_core::IeType::VolumeMeasurement))
                : nullptr;
        const auto ur_seqn =
            ur_seqn_ie != nullptr ? pfcp_core::decode_ur_seqn(ur_seqn_ie->value) : std::nullopt;
        const auto used_volume =
            volume_ie != nullptr ? pfcp_core::decode_volume_total(volume_ie->value) : std::nullopt;
        // URR ID itself is decoded (urr_id_ie) only to confirm the report is well-formed -- this
        // build's Update call reports usage against the session's one fixed rating group
        // (kDefaultRatingGroup, same simplification Create already carries), not a per-URR-ID
        // rating group lookup that doesn't exist in this codebase.
        if (urr_id_ie == nullptr || !ur_seqn.has_value() || !used_volume.has_value()) {
            spdlog::warn("smf: Sx Session Report Request from {} has a malformed Usage Report, "
                         "no CHF Update call",
                         sender.address().to_string());
            return;
        }
        const auto session = cp_seid_sessions.get(header.seid);
        if (!session.has_value()) {
            spdlog::warn("smf: Sx Session Report Request references unknown SEID {:#x}, no CHF "
                         "Update call",
                         header.seid);
            return;
        }
        const CpSeidSessionInfo info = *session;
        const std::int64_t invocation_seq =
            charging_data_invocation_seq.get_and_advance(info.charging_data_ref);
        const std::uint32_t ur_seqn_value = *ur_seqn;
        const std::uint64_t used_volume_value = *used_volume;

        std::thread([&pfcp_peer,
                     &chf_report_client,
                     &chf_report_oauth,
                     &chf_base_url,
                     &smf_instance_id,
                     info,
                     invocation_seq,
                     ur_seqn_value,
                     used_volume_value]() {
            const auto update_result =
                perform_n40_charging_data_update(chf_report_client,
                                                 chf_report_oauth,
                                                 chf_base_url,
                                                 smf_instance_id,
                                                 info.supi,
                                                 info.charging_data_ref,
                                                 invocation_seq,
                                                 kDefaultRatingGroup,
                                                 static_cast<std::int64_t>(ur_seqn_value),
                                                 used_volume_value);
            if (!update_result.has_value() ||
                !update_result->granted_total_volume_octets.has_value()) {
                return;
            }
            // ADR-0050 Stage 5: the new Volume Threshold/Volume Quota are computed relative to
            // this report's own real cumulative Volume Measurement -- the same technique TS
            // 29.244 §5.2.2.2.1 NOTE 3 itself describes for online charging -- rather than
            // resetting to a fresh small window, and the same 90%/100% ratio Stage 1's Create
            // URR already uses. The next crossing is where the newly re-authorized budget
            // itself runs out, not an arbitrary new baseline.
            const std::uint64_t new_grant = *update_result->granted_total_volume_octets;
            const std::uint64_t new_quota = used_volume_value + new_grant;
            const std::uint64_t new_threshold =
                used_volume_value +
                static_cast<std::uint64_t>(static_cast<double>(new_grant) * 0.9);
            perform_n4_session_modification_update_urr(
                pfcp_peer, info.upf_ip, info.up_seid, kUrrId, new_threshold, new_quota);
        }).detach();
    });

    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #107, ADR-0087): real Sx Node Report
    // Request handling (TS 29.244 §7.4.5.1/§7.4.5.2). Real, disclosed scope: this handler decodes
    // and acknowledges the report (real Cause=RequestAccepted, no Offending IE support -- same
    // precedent as UPF's own PFD Management/Association Update handling this session already
    // established) but does not yet act on it (e.g. triggering a real N4 re-association or marking
    // the reported remote GTP-U peer unreachable) -- this project has no other real consumer for a
    // User Plane Path Failure Report yet, and no live UPF-side trigger sends this message in this
    // lab either (see nfs/upf/src/main.cpp's own build_node_report_request_ies comment). Simple
    // enough to stay inline on PfcpPeer's own receive thread, unlike the Session Report handler
    // above -- no further network I/O follows.
    pfcp_peer.set_node_report_handler([&pfcp_peer](const pfcp_core::Header& header,
                                                   const std::vector<std::uint8_t>& ie_bytes,
                                                   const boost::asio::ip::udp::endpoint& sender) {
        spdlog::info("smf: received real Sx Node Report Request from {} (seq={})",
                     sender.address().to_string(),
                     header.sequence_number);

        const auto ies = pfcp_core::decode_ies(ie_bytes);
        const auto* node_id_ie =
            ies.has_value()
                ? pfcp_core::find_ie(*ies, static_cast<std::uint16_t>(pfcp_core::IeType::NodeId))
                : nullptr;
        const auto node_id = node_id_ie != nullptr
                                 ? pfcp_core::decode_node_id_ipv4(node_id_ie->value)
                                 : std::nullopt;
        const auto* failure_report_ie =
            ies.has_value() ? pfcp_core::find_ie(*ies,
                                                 static_cast<std::uint16_t>(
                                                     pfcp_core::IeType::UserPlanePathFailureReport))
                            : nullptr;
        if (failure_report_ie != nullptr) {
            const auto failure_ies = pfcp_core::decode_ies(failure_report_ie->value);
            const auto* peer_ie = failure_ies.has_value()
                                      ? pfcp_core::find_ie(*failure_ies,
                                                           static_cast<std::uint16_t>(
                                                               pfcp_core::IeType::RemoteGtpuPeer))
                                      : nullptr;
            const auto peer_ipv4 = peer_ie != nullptr
                                       ? pfcp_core::decode_remote_gtpu_peer_ipv4(peer_ie->value)
                                       : std::nullopt;
            if (peer_ipv4.has_value()) {
                spdlog::warn(
                    "smf: real User Plane Path Failure Report from Node ID {}.{}.{}.{} -- remote "
                    "GTP-U peer {}.{}.{}.{} unreachable (real, disclosed gap: not yet acted on)",
                    node_id.has_value() ? (*node_id)[0] : 0,
                    node_id.has_value() ? (*node_id)[1] : 0,
                    node_id.has_value() ? (*node_id)[2] : 0,
                    node_id.has_value() ? (*node_id)[3] : 0,
                    (*peer_ipv4)[0],
                    (*peer_ipv4)[1],
                    (*peer_ipv4)[2],
                    (*peer_ipv4)[3]);
            }
        }

        std::vector<std::uint8_t> resp_ies;
        pfcp_core::encode_ie(resp_ies,
                             static_cast<std::uint16_t>(pfcp_core::IeType::NodeId),
                             pfcp_core::encode_node_id_ipv4(kSmfNodeIpv4));
        pfcp_core::encode_ie(resp_ies,
                             static_cast<std::uint16_t>(pfcp_core::IeType::Cause),
                             pfcp_core::encode_cause(pfcp_core::Cause::RequestAccepted));
        pfcp_core::Header resp_header;
        resp_header.has_seid = false;
        resp_header.message_type = pfcp_core::MessageType::NodeReportResponse;
        resp_header.sequence_number = header.sequence_number;
        auto resp_pdu =
            pfcp_core::encode_header(resp_header, static_cast<std::uint16_t>(resp_ies.size()));
        resp_pdu.insert(resp_pdu.end(), resp_ies.begin(), resp_ies.end());
        pfcp_peer.send_fire_and_forget(sender, resp_pdu);
    });

    smf::SmContextStore sm_contexts;
    UpfEndpointStore upf_endpoint_store;
    // ADR-0201: Nsmf_EventExposure's subscription resource.
    smf::EventSubscriptionStore event_subs;

    auto meter = sbi_core::get_meter("smf");
    auto create_counter =
        meter->CreateUInt64Counter("smf_create_sm_context_total", "Total CreateSMContext calls");
    auto retrieve_counter = meter->CreateUInt64Counter("smf_retrieve_sm_context_total",
                                                       "Total RetrieveSMContext calls");
    auto update_counter =
        meter->CreateUInt64Counter("smf_update_sm_context_total", "Total UpdateSMContext calls");
    auto release_counter =
        meter->CreateUInt64Counter("smf_release_sm_context_total", "Total ReleaseSMContext calls");
    auto pcf_sm_policy_create_counter = meter->CreateUInt64Counter(
        "smf_pcf_sm_policy_create_total", "Total successful CreateSMPolicy calls to PCF");
    auto pcf_sm_policy_delete_counter =
        meter->CreateUInt64Counter("smf_pcf_sm_policy_delete_total",
                                   "Total successful (best-effort) DeleteSMPolicy calls to PCF");
    auto n1n2_transfer_counter =
        meter->CreateUInt64Counter("smf_n1n2_message_transfer_total",
                                   "Total successful AMF N1N2MessageTransfer calls delivering a "
                                   "PDU Session Establishment Accept");
    auto chf_charging_data_create_counter = meter->CreateUInt64Counter(
        "smf_chf_charging_data_create_total",
        "Total successful (best-effort) Nchf_ConvergedCharging_Create calls to CHF");
    auto chf_charging_data_release_counter = meter->CreateUInt64Counter(
        "smf_chf_charging_data_release_total",
        "Total successful (best-effort) Nchf_ConvergedCharging_Release calls to CHF");
    // ADR-0201: Nsmf_EventExposure + Nsmf_NIDD counters.
    auto event_sub_create_counter =
        meter->CreateUInt64Counter("smf_event_exposure_create_subscription_total",
                                   "Total Nsmf_EventExposure CreateIndividualSubcription calls");
    auto event_sub_get_counter =
        meter->CreateUInt64Counter("smf_event_exposure_get_subscription_total",
                                   "Total Nsmf_EventExposure GetIndividualSubcription calls");
    auto event_sub_replace_counter =
        meter->CreateUInt64Counter("smf_event_exposure_replace_subscription_total",
                                   "Total Nsmf_EventExposure ReplaceIndividualSubcription calls");
    auto event_sub_delete_counter =
        meter->CreateUInt64Counter("smf_event_exposure_delete_subscription_total",
                                   "Total Nsmf_EventExposure DeleteIndividualSubcription calls");
    auto nidd_deliver_counter =
        meter->CreateUInt64Counter("smf_nidd_deliver_total", "Total Nsmf_NIDD Deliver calls");
    // ADR-0257: the mobile-ORIGINATED half of small-data-over-NAS, mirroring Nsmf_NIDD's own
    // mobile-terminated Deliver above.
    auto send_mo_data_counter = meter->CreateUInt64Counter("smf_send_mo_data_total",
                                                           "Total Nsmf_PDUSession SendMoData "
                                                           "calls");
    auto transfer_mo_data_counter = meter->CreateUInt64Counter(
        "smf_transfer_mo_data_total", "Total Nsmf_PDUSession TransferMoData calls");

    // ADR-0262: the real TS 28.552 clause 5.3.1.3/5.3.1.4/5.3.1.5 measurements, which is what
    // makes the TS 28.554 PDU-session KPIs computable at all (they are ratios of exactly these
    // three). Spec measurement names are cited in the help text; the metric names keep this
    // project's own `smf_*_total` exporter namespace rather than inventing a second one.
    //
    // The label sets are ASYMMETRIC, and that is the spec's doing, not an oversight here:
    // Req/Succ are filtered per PLMN *and* S-NSSAI with a subcounter per request type
    // (§5.3.1.3(c), §5.3.1.4(c)); Fail is filtered per PLMN *only*, with a subcounter per
    // rejection cause (§5.3.1.5(c)).
    // ADR-0286: how often PCF actually pushed a policy change to this SMF -- the N28 chain's own
    // observable proof that it is live rather than merely wired.
    auto pcf_policy_update_counter = meter->CreateUInt64Counter(
        "smf_pcf_policy_updates_total",
        "SM policy decisions pushed by PCF to SMF's notificationUri (N28/Sy chain)");
    auto pdu_session_creation_req_counter = meter->CreateUInt64Counter(
        "smf_sm_pdu_session_creation_req_total",
        "TS 28.552 5.3.1.3 SM.PduSessionCreationReq -- PDU sessions requested to be created");
    auto pdu_session_creation_succ_counter = meter->CreateUInt64Counter(
        "smf_sm_pdu_session_creation_succ_total",
        "TS 28.552 5.3.1.4 SM.PduSessionCreationSucc -- PDU sessions successfully created");
    auto pdu_session_creation_fail_counter = meter->CreateUInt64Counter(
        "smf_sm_pdu_session_creation_fail_total",
        "TS 28.552 5.3.1.5 SM.PduSessionCreationFail -- PDU sessions failed to be created");

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

    // ADR-0262: the real handler is a named lambda so the TS 28.552 §5.3.1.3-5
    // measurements can be taken around it by classifying its own response, rather than
    // by threading counters through every one of its exit paths.
    const auto create_sm_context_inner =

        [&verifier,
         &sm_contexts,
         &create_counter,
         &pcf_client,
         &pcf_oauth,
         &pcf_base_url,
         &self_base_url,
         &pcf_sm_policy_create_counter,
         &amf_client,
         &amf_oauth,
         &amf_base_url,
         &upf_endpoint_store,
         &n1n2_transfer_counter,
         &chf_client,
         &chf_oauth,
         &chf_base_url,
         &chf_charging_data_create_counter,
         &smf_instance_id,
         &pfcp_peer,
         &cp_seid_sessions,
         &nsacf_client,
         &nsacf_oauth,
         &nsacf_base_url,
         &charging_data_invocation_seq](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_multipart_json_body<
                sbi_gen::SmContextCreateData_Nsmf_PDUSession>(req, err);
            if (!body.has_value()) {
                return err;
            }
            // Disclosed simplification (see file header): this build's PCF wiring has nothing to
            // fall back to without these, even though SmContextCreateData's schema allows them to
            // be absent for edge cases (e.g. unauthenticated SUPI) this build doesn't model.
            if (!body->supi.has_value() || !body->pduSessionId.has_value() ||
                !body->dnn.has_value() || !body->sNssai.has_value()) {
                return sbi_core::http2::problem_response(
                    400,
                    "Missing mandatory IE",
                    "This build requires supi, pduSessionId, dnn, and sNssai to establish an SM "
                    "Policy Association with PCF");
            }

            // ADR-0279: Network Slice Admission Control, BEFORE any state exists.
            //
            // Placement is the same lesson ADR-0278 learned on the AMF side: this must precede the
            // N4 session, the PCF SM Policy Association and the SM context itself, because a
            // refused session must leave nothing behind to unwind. TS 23.501 §5.15.11 controls how
            // many PDU sessions may BE established on a slice, so the decision belongs before the
            // establishment, not after the 201.
            if (!admit_pdu_session_with_nsacf(nsacf_client,
                                              nsacf_oauth,
                                              nsacf_base_url,
                                              *body->supi,
                                              *body->pduSessionId,
                                              *body->sNssai)) {
                // The UE is owed a real NAS reason, not just an HTTP status. TS 29.502's
                // SmContextCreateError is the schema that can carry one (`n1SmMsg`), which is why
                // this operation departs from this file's own generic-ProblemDetails convention --
                // see the file header and ADR-0279 for why the exception is confined to here.
                const std::uint8_t pti = extract_request_pti(req).value_or(0);
                const auto reject = smf::nas5gsm::encode_establishment_reject(
                    static_cast<std::uint8_t>(*body->pduSessionId),
                    pti,
                    kSmCauseInsufficientResourcesForSlice);

                sbi_gen::ExtProblemDetails_Nsmf_PDUSession problem{};
                problem.status = 403;
                problem.title = "Slice admission control refused this PDU session";
                problem.detail = "NSACF refused S-NSSAI sst=" + std::to_string(body->sNssai->sst) +
                                 " for SUPI " + *body->supi;
                sbi_gen::SmContextCreateError error{};
                error.error = problem;
                sbi_gen::RefToBinaryData n1_ref{};
                n1_ref.contentId = "n1SmMsg";
                error.n1SmMsg = n1_ref;

                sbi_core::multipart::Part json_part;
                json_part.content_type = "application/problem+json";
                json_part.body = json(error).dump();
                sbi_core::multipart::Part n1_part;
                n1_part.content_type = "application/vnd.3gpp.5gnas";
                n1_part.content_id = "n1SmMsg";
                n1_part.body = std::string(reject.begin(), reject.end());
                const auto encoded = sbi_core::multipart::encode({json_part, n1_part});

                spdlog::warn("smf: PDU SESSION REFUSED for SUPI {} pduSessionId {} -- NSACF "
                             "refused the slice (5GSM cause {:#x})",
                             *body->supi,
                             *body->pduSessionId,
                             kSmCauseInsufficientResourcesForSlice);

                sbi_core::http2::Response resp;
                resp.status = 403;
                resp.headers.emplace("content-type", encoded.content_type_header);
                resp.body = encoded.body;
                return resp;
            }

            const auto sm_context_ref = sm_contexts.create(json::object());
            create_counter->Add(1);

            auto token = pcf_oauth.get_bearer_token();
            if (!token.has_value()) {
                sm_contexts.remove(sm_context_ref);
                return sbi_core::http2::problem_response(500,
                                                         "Internal Server Error",
                                                         "SMF could not obtain a token for PCF: " +
                                                             token.error());
            }

            sbi_gen::SmPolicyContextData pcf_req{};
            pcf_req.supi = *body->supi;
            pcf_req.pduSessionId = *body->pduSessionId;
            // PduSessionType is negotiated inside the NAS SM message (n1SmMsg, an opaque binary
            // blob this build never decodes) -- not available from SmContextCreateData at all.
            // Disclosed fixed default, not the UE's real requested type -- see file header.
            pcf_req.pduSessionType.value = sbi_gen::PduSessionType::IPV4;
            pcf_req.dnn = *body->dnn;
            pcf_req.notificationUri = self_base_url + std::string(kApiRoot) + "/sm-contexts/" +
                                      sm_context_ref + "/pcf-notify";
            pcf_req.sliceInfo = *body->sNssai;

            sbi_core::http2::ClientRequest pcf_http_req;
            pcf_http_req.method = "POST";
            pcf_http_req.url = pcf_base_url + "/npcf-smpolicycontrol/v1/sm-policies";
            pcf_http_req.headers.emplace("content-type", "application/json");
            pcf_http_req.headers.emplace("authorization", "Bearer " + *token);
            pcf_http_req.body = json(pcf_req).dump();

            auto pcf_resp = pcf_client.send(pcf_http_req);
            if (!pcf_resp.has_value()) {
                sm_contexts.remove(sm_context_ref);
                return sbi_core::http2::problem_response(
                    500,
                    "Internal Server Error",
                    "SMF could not reach PCF to establish an SM Policy Association: " +
                        pcf_resp.error());
            }
            if (pcf_resp->status != 201) {
                sm_contexts.remove(sm_context_ref);
                return sbi_core::http2::problem_response(
                    500,
                    "Internal Server Error",
                    "PCF CreateSMPolicy returned unexpected status " +
                        std::to_string(pcf_resp->status));
            }

            sbi_gen::SmPolicyDecision decision;
            try {
                decision = json::parse(pcf_resp->body).get<sbi_gen::SmPolicyDecision>();
            } catch (const json::exception& e) {
                sm_contexts.remove(sm_context_ref);
                return sbi_core::http2::problem_response(
                    500,
                    "Internal Server Error",
                    "PCF returned a malformed SmPolicyDecision: " + std::string(e.what()));
            }
            std::string sm_policy_id;
            if (const auto location_it = pcf_resp->headers.find("location");
                location_it != pcf_resp->headers.end()) {
                const auto& location = location_it->second;
                sm_policy_id = location.substr(location.find_last_of('/') + 1);
            }
            // Not exposed in SmContextCreatedData -- TS29502 has no field for it (matches
            // n2SmInfo's own unpopulated state, see file header); kept internally so
            // ReleaseSMContext can tear the association down again. `supi` is stored here too
            // (ADR-0046) so ReleaseSMContext can populate Nchf_ConvergedCharging_Release's
            // subscriberIdentifier without re-deriving it from anywhere else.
            sm_contexts.update(sm_context_ref,
                               json{{"smPolicyId", sm_policy_id},
                                    {"policy", json(decision)},
                                    {"supi", *body->supi}});
            pcf_sm_policy_create_counter->Add(1);

            // N40, ADR-0044/ADR-0046/ADR-0050: real Nchf_ConvergedCharging_Create, moved ahead of
            // N4 Session Establishment (was previously called after it) -- TS 29.244 Annex
            // C.2.1.1's real online-charging call flow requests credit from the charging function
            // BEFORE provisioning the UP function with the resulting quota (its own steps 1 then
            // 2), so the granted quota is known in time to include a real Create URR in the same
            // N4 Session Establishment Request below, not a separate follow-up call. Best-effort:
            // failure is logged, not fatal to CreateSMContext's own 201 response.
            std::optional<std::uint64_t> granted_total_volume_octets;
            std::string charging_data_ref;
            if (const auto charging_result = perform_n40_charging_data_create(
                    chf_client,
                    chf_oauth,
                    chf_base_url,
                    smf_instance_id,
                    *body->supi,
                    static_cast<std::uint8_t>(*body->pduSessionId));
                charging_result.has_value()) {
                chf_charging_data_create_counter->Add(1);
                granted_total_volume_octets = charging_result->granted_total_volume_octets;
                charging_data_ref = charging_result->charging_data_ref;
                // Pending-items cleanup turn: seed the real invocation-sequence counter for this
                // ChargingDataRef unconditionally (not just when a grant/URR exists) -- Release can
                // still be called on a no-grant session, and needs a real, non-colliding sequence
                // number too.
                charging_data_invocation_seq.put(charging_data_ref);
                if (auto stored = sm_contexts.get(sm_context_ref); stored.has_value()) {
                    (*stored)["chargingDataRef"] = charging_result->charging_data_ref;
                    sm_contexts.update(sm_context_ref, *stored);
                }
            }

            // ADR-0042/ADR-0050: real N4 Session Establishment with UPF, using the Sx Association
            // Stage 2 already proved (run_pfcp_lifecycle populates upf_endpoint_store once, at
            // startup), now also provisioning a real Create URR (Stage 1) if CHF granted a quota
            // above. Best-effort, matching the N1N2MessageTransfer call below: no real datapath
            // exists yet for anything beyond decapsulation (Phase 3), so a failure here is
            // disclosed via a log line, not fatal to this response -- the real gap it would block
            // on (no UPF discovered yet) shouldn't also block CreateSMContext's own already-real
            // PCF/AMF/CHF work.
            // ADR-0308: PCF's authorised session AMBR, read BEFORE the N4 establishment so it can
            // be installed as a real QER. It was previously read further down, only to build the
            // NAS Establishment Accept -- which told the UE its rate limit while UPF enforced
            // nothing. Extracted once here and reused for the NAS message below, so the value the
            // UE is told and the value UPF enforces cannot drift apart.
            std::optional<std::string> policy_ambr_ul;
            std::optional<std::string> policy_ambr_dl;
            if (decision.sessRules.has_value() && decision.sessRules->is_object() &&
                !decision.sessRules->empty()) {
                try {
                    const auto rule =
                        decision.sessRules->begin().value().get<sbi_gen::SessionRule>();
                    if (rule.authSessAmbr.has_value()) {
                        policy_ambr_ul = rule.authSessAmbr->uplink;
                        policy_ambr_dl = rule.authSessAmbr->downlink;
                    }
                } catch (const json::exception&) {
                    // Malformed sessRules -- left absent, so no QER is sent and the NAS path falls
                    // back to its own documented defaults, exactly as before.
                }
            }

            if (const auto upf_ip = upf_endpoint_store.get(); upf_ip.has_value()) {
                const auto n4_result =
                    perform_n4_session_establishment(pfcp_peer,
                                                     *upf_ip,
                                                     static_cast<std::uint8_t>(*body->pduSessionId),
                                                     granted_total_volume_octets,
                                                     policy_ambr_ul,
                                                     policy_ambr_dl);
                // ADR-0050 Stage 3/5: only register a session for later Usage Report handling if a
                // real URR was actually provisioned above -- a session with no granted quota can
                // never produce a Session Report Request in the first place (UPF only counts/
                // reports against TEIDs a Create URR was registered for, see nfs/upf/src/main.cpp).
                if (n4_result.has_value() && granted_total_volume_octets.has_value() &&
                    !charging_data_ref.empty()) {
                    cp_seid_sessions.put(
                        n4_result->cp_seid,
                        CpSeidSessionInfo{
                            *body->supi, charging_data_ref, n4_result->up_seid, *upf_ip});
                }
                // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #101, ADR-0092): persisted
                // unconditionally (not gated on a granted quota like cp_seid_sessions above --
                // PATH_SWITCH_REQ can arrive for any established session, charged or not), so
                // handle_update_sm_context's own PATH_SWITCH_REQ branch can address a real Session
                // Modification back to UPF and build a real uL-NGU-UP-TNLInformation.
                if (n4_result.has_value()) {
                    if (auto stored = sm_contexts.get(sm_context_ref); stored.has_value()) {
                        (*stored)["upSeid"] = n4_result->up_seid;
                        (*stored)["upfIp"] = *upf_ip;
                        if (n4_result->ul_teid.has_value()) {
                            (*stored)["ulTeid"] = *n4_result->ul_teid;
                        }
                        if (n4_result->ul_ipv4.has_value()) {
                            (*stored)["ulIpv4"] = std::vector<std::uint8_t>(
                                n4_result->ul_ipv4->begin(), n4_result->ul_ipv4->end());
                        }
                        sm_contexts.update(sm_context_ref, *stored);
                    }
                }
            } else {
                spdlog::warn("smf: no UPF Sx Association established yet, skipping N4 Session "
                             "Establishment for pduSessionId {}",
                             *body->pduSessionId);
            }

            // ADR-0038: the real TS 23.502 §4.3.2.2.1 step 11 -- SMF decodes the UE's actual PDU
            // Session Establishment Request (forwarded by AMF as n1SmMsg, ADR-0038, not the
            // opaque-and-dropped gap ADR-0036 disclosed), builds a real Accept using PCF's actual
            // QoS decision above (not fabricated), and delivers it to the UE via AMF's
            // Namf_Communication N1N2MessageTransfer -- the real mechanism (TS29518_
            // Namf_Communication.yaml), not a field on this response. Best-effort: any failure
            // here is logged, not fatal to CreateSMContext's own 201 -- matches TS 23.502's real
            // procedure, where N1N2MessageTransfer happens after CreateSMContext already returned.
            if (body->n1SmMsg.has_value()) {
                std::vector<std::uint8_t> n1_sm_bytes;
                bool found_n1_sm = false;
                if (const auto content_type_it = req.headers.find("content-type");
                    content_type_it != req.headers.end()) {
                    if (auto parts = sbi_core::multipart::parse(content_type_it->second, req.body);
                        parts.has_value()) {
                        for (const auto& part : *parts) {
                            if (part.content_id.has_value() &&
                                *part.content_id == body->n1SmMsg->contentId) {
                                n1_sm_bytes.assign(part.body.begin(), part.body.end());
                                found_n1_sm = true;
                                break;
                            }
                        }
                    }
                }
                const auto req_info = found_n1_sm
                                          ? smf::nas5gsm::decode_establishment_request(n1_sm_bytes)
                                          : std::nullopt;
                if (!req_info.has_value()) {
                    spdlog::warn(
                        "smf: SUPI {} pduSessionId {} -- n1SmMsg referenced but its binary "
                        "part was missing or not a PDU Session Establishment Request, no "
                        "Accept sent",
                        *body->supi,
                        *body->pduSessionId);
                } else {
                    // Sourced from PCF's real SmPolicyDecision.sessRules (built above), not
                    // fabricated -- falls back to nas_5gsm_codec's own disclosed defaults only if
                    // PCF returned no session rule at all.
                    // ADR-0308: the SAME values installed as a QER above, so the rate the UE is
                    // told and the rate UPF enforces are one decision rather than two reads that
                    // could diverge.
                    std::string ambr_ul = policy_ambr_ul.value_or("1 Mbps");
                    std::string ambr_dl = policy_ambr_dl.value_or("1 Mbps");
                    std::uint8_t qfi = 1;
                    if (decision.sessRules.has_value() && decision.sessRules->is_object() &&
                        !decision.sessRules->empty()) {
                        try {
                            const auto rule =
                                decision.sessRules->begin().value().get<sbi_gen::SessionRule>();
                            if (rule.authDefQos.has_value() && rule.authDefQos->n5qi.has_value()) {
                                qfi = static_cast<std::uint8_t>(*rule.authDefQos->n5qi & 0x3F);
                            }
                        } catch (const json::exception&) {
                            // Malformed sessRules entry -- fall back to the defaults above,
                            // disclosed via the log line below rather than silently swallowed.
                        }
                    }

                    const auto accept_bytes = smf::nas5gsm::encode_establishment_accept(
                        req_info->pdu_session_id, req_info->pti, ambr_ul, ambr_dl, qfi);

                    auto amf_token = amf_oauth.get_bearer_token();
                    if (!amf_token.has_value()) {
                        spdlog::warn("smf: could not obtain AMF token, PDU Session Establishment "
                                     "Accept not delivered for SUPI {}: {}",
                                     *body->supi,
                                     amf_token.error());
                    } else {
                        sbi_gen::N1N2MessageTransferReqData n1n2_req{};
                        sbi_gen::N1MessageContainer n1_container{};
                        n1_container.n1MessageClass.value = sbi_gen::N1MessageClass::SM;
                        sbi_gen::RefToBinaryData n1_content_ref{};
                        n1_content_ref.contentId = "n1Message";
                        n1_container.n1MessageContent = n1_content_ref;
                        n1n2_req.n1MessageContainer = n1_container;
                        n1n2_req.pduSessionId = body->pduSessionId;

                        sbi_core::multipart::Part n1n2_json_part;
                        n1n2_json_part.content_type = "application/json";
                        n1n2_json_part.body = json(n1n2_req).dump();
                        sbi_core::multipart::Part n1n2_bin_part;
                        n1n2_bin_part.content_type = "application/vnd.3gpp.5gnas";
                        n1n2_bin_part.content_id = "n1Message";
                        n1n2_bin_part.body.assign(accept_bytes.begin(), accept_bytes.end());
                        const auto n1n2_encoded =
                            sbi_core::multipart::encode({n1n2_json_part, n1n2_bin_part});

                        sbi_core::http2::ClientRequest amf_http_req;
                        amf_http_req.method = "POST";
                        amf_http_req.url = amf_base_url + "/namf-comm/v1/ue-contexts/" +
                                           *body->supi + "/n1-n2-messages";
                        amf_http_req.headers.emplace("content-type",
                                                     n1n2_encoded.content_type_header);
                        amf_http_req.headers.emplace("authorization", "Bearer " + *amf_token);
                        amf_http_req.body = n1n2_encoded.body;

                        auto amf_resp = amf_client.send(amf_http_req);
                        if (!amf_resp.has_value() ||
                            (amf_resp->status != 200 && amf_resp->status != 202)) {
                            spdlog::warn(
                                "smf: AMF N1N2MessageTransfer call failed for SUPI {} -- PDU "
                                "Session Establishment Accept not delivered to the UE: {}",
                                *body->supi,
                                amf_resp.has_value() ? std::to_string(amf_resp->status)
                                                     : amf_resp.error());
                        } else {
                            n1n2_transfer_counter->Add(1);
                            spdlog::info("smf: PDU Session Establishment Accept delivered to AMF "
                                         "for SUPI {}, pduSessionId {}",
                                         *body->supi,
                                         *body->pduSessionId);
                        }
                    }
                }
            }

            sbi_gen::SmContextCreatedData_Nsmf_PDUSession resp_data;
            resp_data.pduSessionId = body->pduSessionId;
            resp_data.sNssai = body->sNssai;
            json j = resp_data;
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kApiRoot) + "/sm-contexts/" + sm_context_ref);
            resp.body = j.dump();
            return resp;
        };

    // ADR-0262: TS 28.552 clause 5.3.1.3-5, taken around the real handler.
    //
    // Where "receipt" begins, stated as a reading rather than left implicit: §5.3.1.3(c) says
    // "on receipt by the SMF from AMF of Nsmf_PDUSession_CreateSMContext Request". A request that
    // fails the OAuth2 check is not a CreateSMContext Request from AMF -- it is an unauthenticated
    // caller -- so 401 is counted as neither a request nor a failure. Everything past
    // authentication is counted, including a request whose body will not parse.
    //
    // This also FIXES a real defect rather than only adding counters: the pre-existing
    // `smf_create_sm_context_total` increments AFTER validation, so it silently under-counts every
    // request rejected during validation and never matched §5.3.1.3's own definition. It is left
    // in place, unchanged, as the "reached the point of allocating an SM context ref" counter it
    // actually is -- renaming or repurposing it would break existing dashboards -- and the
    // spec-named counter above is the one that means what 28.552 says.
    server.add_route(
        "POST",
        std::string(kApiRoot) + "/sm-contexts",
        [create_sm_context_inner,
         &pdu_session_creation_req_counter,
         &pdu_session_creation_succ_counter,
         &pdu_session_creation_fail_counter,
         &nsacf_client,
         &nsacf_oauth,
         &nsacf_base_url](const sbi_core::http2::Request& req) {
            auto resp = create_sm_context_inner(req);
            if (resp.status == 401) {
                return resp;
            }

            // Filter/subcounter values come from the request itself. `requestType` is OPTIONAL in
            // SmContextCreateData (checked against the YAML, not assumed), so an absent one is
            // reported as the literal "absent" rather than defaulted to a real RequestType value
            // this project would then be inventing.
            std::string plmn = "unknown";
            std::string snssai = "unknown";
            std::string supi_for_nsac;
            std::int64_t pdu_session_id_for_nsac = 0;
            std::int64_t sst_for_nsac = 0;
            std::optional<std::string> sd_for_nsac;
            std::string req_type = "absent";
            const auto content_type_it = req.headers.find("content-type");
            const auto parts =
                content_type_it != req.headers.end() &&
                        sbi_core::multipart::is_multipart_related(content_type_it->second)
                    ? sbi_core::multipart::parse(content_type_it->second, req.body)
                    : tl::expected<std::vector<sbi_core::multipart::Part>, std::string>(
                          tl::unexpect, "not multipart");
            if (parts.has_value() && !parts->empty()) {
                try {
                    const auto body = json::parse((*parts)[0].body);
                    if (body.contains("servingNetwork")) {
                        const auto& sn = body.at("servingNetwork");
                        if (sn.contains("mcc") && sn.contains("mnc")) {
                            plmn = sn.at("mcc").get<std::string>() + "-" +
                                   sn.at("mnc").get<std::string>();
                        }
                    }
                    if (body.contains("sNssai")) {
                        const auto& sn = body.at("sNssai");
                        snssai = std::to_string(sn.value("sst", 0));
                        if (sn.contains("sd")) {
                            snssai += "-" + sn.at("sd").get<std::string>();
                        }
                    }
                    if (body.contains("requestType")) {
                        req_type = body.at("requestType").get<std::string>();
                    }
                    // ADR-0277: the same parsed body already gives NSAC everything it needs, so
                    // no second parse is added for it.
                    if (body.contains("supi")) {
                        supi_for_nsac = body.at("supi").get<std::string>();
                    }
                    if (body.contains("pduSessionId")) {
                        pdu_session_id_for_nsac = body.at("pduSessionId").get<std::int64_t>();
                    }
                    if (body.contains("sNssai")) {
                        const auto& sn = body.at("sNssai");
                        sst_for_nsac = sn.value("sst", 0);
                        if (sn.contains("sd")) {
                            sd_for_nsac = sn.at("sd").get<std::string>();
                        }
                    }
                } catch (const nlohmann::json::exception&) {
                    // Leave the "unknown" defaults: an unparseable body is still a received
                    // request per §5.3.1.3(c), and is counted as one below.
                }
            }

            pdu_session_creation_req_counter->Add(
                1, {{"plmn", plmn}, {"snssai", snssai}, {"reqType", req_type}});
            if (resp.status == 201) {
                pdu_session_creation_succ_counter->Add(
                    1, {{"plmn", plmn}, {"snssai", snssai}, {"reqType", req_type}});

                // ADR-0277: TS 23.501 §5.15.11 / TS 23.502 §4.3.2.2.1 -- a PDU session was just
                // ESTABLISHED, which is what NSAC counts on the PDU-number side. Reported here,
                // after the 201, and deliberately NOT used to refuse the session: refusing means
                // failing the establishment back to the UE and unwinding the N4 session and the
                // PCF association already created, a behaviour change to the procedure rather than
                // a call. Same disclosed boundary as AMF's own UE-count report.
                report_pdu_session_to_nsacf(nsacf_client,
                                            nsacf_oauth,
                                            nsacf_base_url,
                                            supi_for_nsac,
                                            pdu_session_id_for_nsac,
                                            sst_for_nsac,
                                            sd_for_nsac,
                                            /*increase=*/true);
            } else {
                // §5.3.1.5's subcounter is "per rejection cause". This build's rejection cause is
                // the ProblemDetails `title` it already returns -- a real value carried on the
                // wire, not a parallel taxonomy invented for metrics.
                std::string cause = "status-" + std::to_string(resp.status);
                try {
                    const auto problem = json::parse(resp.body);
                    if (problem.contains("title")) {
                        cause = problem.at("title").get<std::string>();
                    }
                } catch (const nlohmann::json::exception&) {
                }
                pdu_session_creation_fail_counter->Add(1, {{"plmn", plmn}, {"cause", cause}});
            }
            return resp;
        });

    // ADR-0286 (the user's standing N28/Sy directive, SMF half): the callback SMF has been
    // ADVERTISING to PCF since ADR-0038 and never implemented.
    //
    // SMF supplies `notificationUri` = .../sm-contexts/{ref}/pcf-notify when it creates the SM
    // Policy Association, and until now nothing served that path -- PCF's own file header records
    // the same gap from its side ("neither AMF nor SMF implements the receiver"). So a policy
    // change PCF decided, including one driven by a CHF spending-limit status change over N28,
    // had nowhere to land. This is that receiver.
    //
    // What it does NOT do, deliberately: invent enforcement semantics. The decision PCF sends is
    // recorded against the SM context and logged; translating a specific decision into a PFCP
    // re-authorisation is a separate increment with its own N4 work, and is named in ADR-0286
    // rather than half-done here.
    server.add_route(
        "POST",
        std::string(kApiRoot) + "/sm-contexts/{smContextRef}/pcf-notify",
        [&sm_contexts, &pcf_policy_update_counter](const sbi_core::http2::Request& req) {
            // No bearer check: this is a notification endpoint PCF calls back on a URI SMF itself
            // supplied, matching how every other callback receiver in this project is reached.
            const auto ref = req.path_params.at("smContextRef");
            auto stored = sm_contexts.get(ref);
            if (!stored.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No SM context " + ref + " for this policy notification");
            }

            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::SmPolicyNotification>(req, err);
            if (!body.has_value()) {
                return err;
            }

            if (body->smPolicyDecision.has_value()) {
                auto updated = *stored;
                // Kept under its own key rather than overwriting the decision captured at
                // establishment: an operator debugging a live session needs to see both what was
                // decided originally and what changed it.
                updated["pcfPolicyUpdate"] = json(*body->smPolicyDecision);
                sm_contexts.update(ref, updated);
                pcf_policy_update_counter->Add(1);
                spdlog::info("smf: PCF pushed an updated SM policy decision for smContextRef {} "
                             "-- recorded against the session",
                             ref);
            } else {
                spdlog::info("smf: PCF notification for smContextRef {} carried no "
                             "smPolicyDecision -- nothing to apply",
                             ref);
            }

            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/sm-contexts/{smContextRef}/retrieve",
        [&verifier, &sm_contexts, &retrieve_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto sm_context_ref = req.path_params.at("smContextRef");
            if (!sm_contexts.get(sm_context_ref).has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No SM context with ref " + sm_context_ref);
            }
            // SmContextRetrieveData is optional per spec (required: false) -- an empty body is
            // valid, not a parse error.
            if (!req.body.empty()) {
                sbi_core::http2::Response err;
                auto body =
                    sbi_core::http2::parse_json_body<sbi_gen::SmContextRetrieveData>(req, err);
                if (!body.has_value()) {
                    return err;
                }
            }
            retrieve_counter->Add(1);
            // Disclosed simplification: ueEpsPdnConnection (mandatory per spec) is an opaque
            // base64 container with nothing real behind it in this build (no EPS interworking
            // state exists) -- emitted as an empty string, a schema-valid but empty value.
            sbi_gen::SmContextRetrievedData resp_data;
            resp_data.ueEpsPdnConnection = "";
            json j = resp_data;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/sm-contexts/{smContextRef}/modify",
        [&verifier, &sm_contexts, &update_counter, &pfcp_peer](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            // Real, both-real-shapes content negotiation (TS29502_Nsmf_PDUSession.yaml's own
            // UpdateSmContext requestBody: application/json "message without binary body part" OR
            // multipart/related "message with binary body part(s)") -- unlike CreateSMContext,
            // which is multipart-ONLY, most real Updates (upCnxState-only, etc.) carry no N2SmInfo
            // and use plain JSON; gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #101,
            // ADR-0092) is the first real reason this handler ever needs the multipart branch.
            std::optional<sbi_gen::SmContextUpdateData_Nsmf_PDUSession> body;
            std::vector<std::uint8_t> n2_sm_info_bytes;
            bool has_n2_sm_info_bytes = false;
            const auto content_type_it = req.headers.find("content-type");
            const bool is_multipart =
                content_type_it != req.headers.end() &&
                sbi_core::multipart::is_multipart_related(content_type_it->second);
            if (is_multipart) {
                auto parts = sbi_core::multipart::parse(content_type_it->second, req.body);
                if (!parts.has_value() || parts->empty()) {
                    return sbi_core::http2::problem_response(
                        400, "Malformed multipart body", "no parts found");
                }
                try {
                    body = json::parse((*parts)[0].body)
                               .get<sbi_gen::SmContextUpdateData_Nsmf_PDUSession>();
                } catch (const json::exception& e) {
                    return sbi_core::http2::problem_response(
                        400, "Missing or invalid mandatory IE", e.what());
                }
                if (body->n2SmInfo.has_value()) {
                    for (const auto& part : *parts) {
                        if (part.content_id.has_value() &&
                            *part.content_id == body->n2SmInfo->contentId) {
                            n2_sm_info_bytes.assign(part.body.begin(), part.body.end());
                            has_n2_sm_info_bytes = true;
                            break;
                        }
                    }
                }
            } else {
                sbi_core::http2::Response err;
                body =
                    sbi_core::http2::parse_json_body<sbi_gen::SmContextUpdateData_Nsmf_PDUSession>(
                        req, err);
                if (!body.has_value()) {
                    return err;
                }
            }
            const auto sm_context_ref = req.path_params.at("smContextRef");
            const auto stored_ctx = sm_contexts.get(sm_context_ref);
            if (!stored_ctx.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No SM context with ref " + sm_context_ref);
            }
            update_counter->Add(1);

            // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #101, ADR-0092): real
            // PATH_SWITCH_REQ handling -- the one N2SmInfoType this project closes real, live
            // datapath behavior for (see this handler's own real, disclosed scope in the file
            // header: the other 20 real N2SmInfoType values remain a real, open gap).
            // Gap-closure (ADR-0249): HANDOVER_REQUIRED -- the N2-handover PREPARATION step
            // (TS 23.502 §4.9.1.3, step 3). AMF relays the source gNB's Handover Required to SMF,
            // and SMF answers with the N2 SM info the TARGET gNB needs to set the session up:
            // a real PDUSessionResourceSetupRequestTransfer carrying UPF's own REAL N3 uplink
            // F-TEID. This is what removes nfs/amf's long-disclosed placeholder
            // (TEID=0 / 0.0.0.0) -- AMF was fabricating that tunnel precisely because it had
            // never asked SMF for the real one. The F-TEID here is not new state: it is the same
            // ulTeid/ulIpv4 UPF allocated at Session Establishment and SMF has persisted since
            // ADR-0092.
            if (body->n2SmInfoType.has_value() &&
                body->n2SmInfoType->value == sbi_gen::N2SmInfoType::HANDOVER_REQUIRED) {
                if (!stored_ctx->contains("ulTeid") || !stored_ctx->contains("ulIpv4")) {
                    return sbi_core::http2::problem_response(
                        500,
                        "Internal Server Error",
                        "No real UPF N3 uplink F-TEID on record for this SM context -- cannot "
                        "build a real PDUSessionResourceSetupRequestTransfer for handover");
                }
                const auto ul_teid = stored_ctx->at("ulTeid").get<std::uint32_t>();
                const auto ul_ipv4 = stored_ctx->at("ulIpv4").get<std::vector<std::uint8_t>>();
                if (ul_ipv4.size() != 4) {
                    return sbi_core::http2::problem_response(
                        500,
                        "Internal Server Error",
                        "Stored UPF N3 address is not a real 4-octet IPv4 address");
                }

                PDUSessionResourceSetupRequestTransfer_t transfer{};

                UPTransportLayerInformation_t ul_info{};
                ul_info.present = UPTransportLayerInformation_PR_gTPTunnel;
                auto* gtp_tunnel = static_cast<GTPTunnel_t*>(std::calloc(1, sizeof(GTPTunnel_t)));
                const std::uint8_t teid_bytes[4] = {
                    static_cast<std::uint8_t>((ul_teid >> 24) & 0xFF),
                    static_cast<std::uint8_t>((ul_teid >> 16) & 0xFF),
                    static_cast<std::uint8_t>((ul_teid >> 8) & 0xFF),
                    static_cast<std::uint8_t>(ul_teid & 0xFF)};
                gtp_tunnel->gTP_TEID.buf = static_cast<std::uint8_t*>(std::malloc(4));
                std::memcpy(gtp_tunnel->gTP_TEID.buf, teid_bytes, 4);
                gtp_tunnel->gTP_TEID.size = 4;
                gtp_tunnel->transportLayerAddress.buf = static_cast<std::uint8_t*>(std::malloc(4));
                std::memcpy(gtp_tunnel->transportLayerAddress.buf, ul_ipv4.data(), 4);
                gtp_tunnel->transportLayerAddress.size = 4;
                gtp_tunnel->transportLayerAddress.bits_unused = 0;
                ul_info.choice.gTPTunnel = gtp_tunnel;
                ::ngap::add_ie(transfer.protocolIEs,
                               ::ngap::make_ie(139 /* id-UL-NGU-UP-TNLInformation */,
                                               Criticality_reject,
                                               &asn_DEF_UPTransportLayerInformation,
                                               &ul_info));
                ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_UPTransportLayerInformation, &ul_info);

                PDUSessionType_t pdu_type = PDUSessionType_ipv4;
                ::ngap::add_ie(transfer.protocolIEs,
                               ::ngap::make_ie(134 /* id-PDUSessionType */,
                                               Criticality_reject,
                                               &asn_DEF_PDUSessionType,
                                               &pdu_type));

                // Same real QoS flow this project already uses on its establishment path: QFI=1,
                // non-dynamic 5QI=9 (the real 3GPP non-GBR default, TS 23.501 Table 5.7.4-1),
                // ARP 8 / shall-not-trigger-pre-emption / not-pre-emptable. Real, disclosed
                // simplification carried over unchanged, not new scope: this project has no real
                // per-subscriber QoS profile source to derive a different flow from.
                QosFlowSetupRequestList_t qos_list{};
                auto* qos_item = static_cast<QosFlowSetupRequestItem_t*>(
                    std::calloc(1, sizeof(QosFlowSetupRequestItem_t)));
                qos_item->qosFlowIdentifier = 1;
                qos_item->qosFlowLevelQosParameters.qosCharacteristics.present =
                    QosCharacteristics_PR_nonDynamic5QI;
                auto* non_dynamic = static_cast<NonDynamic5QIDescriptor_t*>(
                    std::calloc(1, sizeof(NonDynamic5QIDescriptor_t)));
                non_dynamic->fiveQI = 9;
                qos_item->qosFlowLevelQosParameters.qosCharacteristics.choice.nonDynamic5QI =
                    non_dynamic;
                qos_item->qosFlowLevelQosParameters.allocationAndRetentionPriority
                    .priorityLevelARP = 8;
                qos_item->qosFlowLevelQosParameters.allocationAndRetentionPriority
                    .pre_emptionCapability = Pre_emptionCapability_shall_not_trigger_pre_emption;
                qos_item->qosFlowLevelQosParameters.allocationAndRetentionPriority
                    .pre_emptionVulnerability = Pre_emptionVulnerability_not_pre_emptable;
                ASN_SEQUENCE_ADD(&qos_list.list, qos_item);
                ::ngap::add_ie(transfer.protocolIEs,
                               ::ngap::make_ie(136 /* id-QosFlowSetupRequestList */,
                                               Criticality_reject,
                                               &asn_DEF_QosFlowSetupRequestList,
                                               &qos_list));
                ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_QosFlowSetupRequestList, &qos_list);

                const auto setup_bytes = ::ngap::encode_value(
                    &asn_DEF_PDUSessionResourceSetupRequestTransfer, &transfer);
                ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_PDUSessionResourceSetupRequestTransfer,
                                              &transfer);
                if (setup_bytes.empty()) {
                    return sbi_core::http2::problem_response(
                        500,
                        "Internal Server Error",
                        "Failed to PER-encode a real PDUSessionResourceSetupRequestTransfer");
                }
                spdlog::info(
                    "smf: HANDOVER_REQUIRED answered with a real PDUSessionResourceSetupRequest"
                    "Transfer (real UPF N3 uplink TEID={:#x}) -- no placeholder tunnel",
                    ul_teid);

                sbi_gen::SmContextUpdatedData resp_data{};
                resp_data.n2SmInfoType = sbi_gen::N2SmInfoType{};
                resp_data.n2SmInfoType->value = sbi_gen::N2SmInfoType::PDU_RES_SETUP_REQ;
                sbi_gen::RefToBinaryData n2_info_ref{};
                n2_info_ref.contentId = "n2SmInfo";
                resp_data.n2SmInfo = n2_info_ref;

                sbi_core::multipart::Part json_part;
                json_part.content_type = "application/json";
                json_part.body = json(resp_data).dump();
                sbi_core::multipart::Part bin_part;
                bin_part.content_type = "application/vnd.3gpp.ngap";
                bin_part.content_id = "n2SmInfo";
                bin_part.body.assign(setup_bytes.begin(), setup_bytes.end());
                const auto encoded = sbi_core::multipart::encode({json_part, bin_part});

                sbi_core::http2::Response resp;
                resp.status = 200;
                resp.headers.emplace("content-type", encoded.content_type_header);
                resp.body = encoded.body;
                return resp;
            }

            // Gap-closure (ADR-0248): the same real downlink-path switch is driven by TWO real
            // N2SmInfoType values, not one. PATH_SWITCH_REQ is Xn-based mobility (TS 23.502
            // §4.9.1.2); HANDOVER_REQ_ACK is the N2-based handover's own Handover Resource
            // Allocation step (§4.9.1.3), where the TARGET gNB's accepted downlink tunnel comes
            // back to SMF. Both transfers carry the same real `dL-NGU-UP-TNLInformation` field
            // (confirmed by reading both generated ASN.1 structs, not assumed from the names), so
            // the real UPF consequence -- repoint the downlink FAR at the new gNB -- is
            // identical. Only the decode type and the N2 response type differ.
            const bool is_path_switch =
                body->n2SmInfoType.has_value() &&
                body->n2SmInfoType->value == sbi_gen::N2SmInfoType::PATH_SWITCH_REQ;
            const bool is_ho_req_ack =
                body->n2SmInfoType.has_value() &&
                body->n2SmInfoType->value == sbi_gen::N2SmInfoType::HANDOVER_REQ_ACK;
            if ((is_path_switch || is_ho_req_ack) && has_n2_sm_info_bytes) {
                std::array<std::uint8_t, 4> gnb_ipv4{};
                std::uint32_t gnb_teid = 0;
                {
                    // Decode whichever real transfer this N2SmInfoType actually declares, pull the
                    // DL GTP-U tunnel out of it, and free it before going anywhere near the
                    // network -- the two types have different ASN.1 descriptors, so the free must
                    // be paired with the matching one.
                    const UPTransportLayerInformation_t* dl_tnl = nullptr;
                    PathSwitchRequestTransfer_t* ps_transfer = nullptr;
                    HandoverRequestAcknowledgeTransfer_t* ho_transfer = nullptr;
                    if (is_path_switch) {
                        ps_transfer = static_cast<PathSwitchRequestTransfer_t*>(
                            ngap_per_decode(&asn_DEF_PathSwitchRequestTransfer, n2_sm_info_bytes));
                        if (ps_transfer != nullptr) {
                            dl_tnl = &ps_transfer->dL_NGU_UP_TNLInformation;
                        }
                    } else {
                        ho_transfer =
                            static_cast<HandoverRequestAcknowledgeTransfer_t*>(ngap_per_decode(
                                &asn_DEF_HandoverRequestAcknowledgeTransfer, n2_sm_info_bytes));
                        if (ho_transfer != nullptr) {
                            dl_tnl = &ho_transfer->dL_NGU_UP_TNLInformation;
                        }
                    }
                    const char* type_name = is_path_switch ? "PathSwitchRequestTransfer"
                                                           : "HandoverRequestAcknowledgeTransfer";
                    auto free_transfer = [&]() {
                        if (ps_transfer != nullptr) {
                            ASN_STRUCT_FREE(asn_DEF_PathSwitchRequestTransfer, ps_transfer);
                        }
                        if (ho_transfer != nullptr) {
                            ASN_STRUCT_FREE(asn_DEF_HandoverRequestAcknowledgeTransfer,
                                            ho_transfer);
                        }
                    };
                    if (dl_tnl == nullptr) {
                        free_transfer();
                        return sbi_core::http2::problem_response(
                            400,
                            "Bad Request",
                            std::string("n2SmInfo is not a valid ") + type_name);
                    }
                    // ADR-0259: the same read_gtp_tunnel the new N2SmInfoType branches use.
                    const auto endpoint = read_gtp_tunnel(dl_tnl);
                    if (!endpoint.has_value()) {
                        free_transfer();
                        return sbi_core::http2::problem_response(
                            400,
                            "Bad Request",
                            std::string(type_name) +
                                " has no real IPv4 GTP-U dL-NGU-UP-TNLInformation");
                    }
                    gnb_ipv4 = endpoint->ipv4;
                    gnb_teid = endpoint->teid;
                    free_transfer();
                }

                if (!stored_ctx->contains("upSeid") || !stored_ctx->contains("upfIp")) {
                    return sbi_core::http2::problem_response(
                        500,
                        "Internal Server Error",
                        "No real N4 session on record for this SM context (established before "
                        "UPF was reachable) -- cannot address a real PFCP Session Modification");
                }
                const std::uint64_t up_seid = stored_ctx->at("upSeid").get<std::uint64_t>();
                const std::string upf_ip = stored_ctx->at("upfIp").get<std::string>();

                // ADR-0259: the real PFCP Session Modification that repoints the downlink FAR
                // now lives in install_downlink_far -- identical behaviour, one implementation
                // shared with the N2SmInfoType values added below, rather than five copies.
                const auto dl_far_error =
                    install_downlink_far(pfcp_peer,
                                         up_seid,
                                         upf_ip,
                                         gnb_teid,
                                         gnb_ipv4,
                                         is_path_switch ? "PATH_SWITCH_REQ" : "HANDOVER_REQ_ACK");
                if (dl_far_error.has_value()) {
                    return sbi_core::http2::problem_response(
                        500, "Internal Server Error", *dl_far_error);
                }

                // Real PathSwitchRequestAcknowledgeTransfer (§9.3.4.9): uL-NGU-UP-TNLInformation
                // is UPF's own real, previously-allocated N3 receive F-TEID (persisted at Session
                // Establishment, ADR-0092) -- not the all-OPTIONAL-fields-empty placeholder
                // ADR-0090 disclosed as this project's own real, previously-open gap.
                PathSwitchRequestAcknowledgeTransfer_t ack_transfer{};
                if (is_path_switch && stored_ctx->contains("ulTeid") &&
                    stored_ctx->contains("ulIpv4")) {
                    auto* ul_gtp_tunnel =
                        static_cast<GTPTunnel_t*>(std::calloc(1, sizeof(GTPTunnel_t)));
                    const auto ul_teid = stored_ctx->at("ulTeid").get<std::uint32_t>();
                    const auto ul_ipv4 = stored_ctx->at("ulIpv4").get<std::vector<std::uint8_t>>();
                    const std::uint8_t teid_bytes[4] = {
                        static_cast<std::uint8_t>((ul_teid >> 24) & 0xFF),
                        static_cast<std::uint8_t>((ul_teid >> 16) & 0xFF),
                        static_cast<std::uint8_t>((ul_teid >> 8) & 0xFF),
                        static_cast<std::uint8_t>(ul_teid & 0xFF)};
                    ul_gtp_tunnel->gTP_TEID.buf = static_cast<std::uint8_t*>(std::malloc(4));
                    std::memcpy(ul_gtp_tunnel->gTP_TEID.buf, teid_bytes, 4);
                    ul_gtp_tunnel->gTP_TEID.size = 4;
                    ul_gtp_tunnel->transportLayerAddress.buf =
                        static_cast<std::uint8_t*>(std::malloc(4));
                    std::memcpy(ul_gtp_tunnel->transportLayerAddress.buf, ul_ipv4.data(), 4);
                    ul_gtp_tunnel->transportLayerAddress.size = 4;
                    ul_gtp_tunnel->transportLayerAddress.bits_unused = 0;
                    ack_transfer.uL_NGU_UP_TNLInformation =
                        static_cast<UPTransportLayerInformation_t*>(
                            std::calloc(1, sizeof(UPTransportLayerInformation_t)));
                    ack_transfer.uL_NGU_UP_TNLInformation->present =
                        UPTransportLayerInformation_PR_gTPTunnel;
                    ack_transfer.uL_NGU_UP_TNLInformation->choice.gTPTunnel = ul_gtp_tunnel;
                } else if (is_path_switch) {
                    spdlog::warn(
                        "smf: no real UPF N3 uplink F-TEID on record for UP F-SEID={:#x} -- "
                        "PathSwitchRequestAcknowledgeTransfer sent with uL-NGU-UP-TNLInformation "
                        "absent (a real, disclosed gap in this specific session's own Establishment"
                        " history, not new scope)",
                        up_seid);
                }

                // ADR-0248: the two N2SmInfoTypes answer with DIFFERENT real transfers. Xn-based
                // PATH_SWITCH_REQ is acknowledged with PathSwitchRequestAcknowledgeTransfer
                // (§9.3.4.10); N2-based HANDOVER_REQ_ACK is answered with HandoverCommandTransfer
                // (§9.3.4.2), which AMF relays to the SOURCE gNB as the Handover Command. Real,
                // disclosed scope: every field of HandoverCommandTransfer is OPTIONAL in the real
                // ASN.1, and this project sets none of them -- specifically NO
                // dLForwardingUP_TNLInformation, because indirect data forwarding (a real
                // TS 23.502 §4.9.1.3 option requiring a second forwarding UPF tunnel) is NOT
                // implemented here. An empty HandoverCommandTransfer is spec-legal and means
                // exactly "handover accepted, no data forwarding" -- it is not a placeholder
                // standing in for a value we failed to compute.
                std::vector<std::uint8_t> ack_bytes;
                if (is_path_switch) {
                    ack_bytes = ngap_per_encode(&asn_DEF_PathSwitchRequestAcknowledgeTransfer,
                                                &ack_transfer);
                    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_PathSwitchRequestAcknowledgeTransfer,
                                                  &ack_transfer);
                } else {
                    HandoverCommandTransfer_t ho_cmd{};
                    ack_bytes = ngap_per_encode(&asn_DEF_HandoverCommandTransfer, &ho_cmd);
                    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_HandoverCommandTransfer, &ho_cmd);
                }
                if (ack_bytes.empty()) {
                    return sbi_core::http2::problem_response(
                        500,
                        "Internal Server Error",
                        std::string("Failed to PER-encode a real ") +
                            (is_path_switch ? "PathSwitchRequestAcknowledgeTransfer"
                                            : "HandoverCommandTransfer"));
                }

                sbi_gen::SmContextUpdatedData resp_data{};
                resp_data.n2SmInfoType = sbi_gen::N2SmInfoType{};
                resp_data.n2SmInfoType->value = is_path_switch
                                                    ? sbi_gen::N2SmInfoType::PATH_SWITCH_REQ_ACK
                                                    : sbi_gen::N2SmInfoType::HANDOVER_CMD;
                sbi_gen::RefToBinaryData n2_info_ref{};
                n2_info_ref.contentId = "n2SmInfo";
                resp_data.n2SmInfo = n2_info_ref;

                sbi_core::multipart::Part json_part;
                json_part.content_type = "application/json";
                json_part.body = json(resp_data).dump();
                sbi_core::multipart::Part bin_part;
                bin_part.content_type = "application/vnd.3gpp.ngap";
                bin_part.content_id = "n2SmInfo";
                bin_part.body.assign(ack_bytes.begin(), ack_bytes.end());
                const auto encoded = sbi_core::multipart::encode({json_part, bin_part});

                sbi_core::http2::Response resp;
                resp.status = 200;
                resp.headers.emplace("content-type", encoded.content_type_header);
                resp.body = encoded.body;
                return resp;
            }

            // ADR-0259: three more real N2SmInfoType values. All three hand SMF a new NG-RAN
            // DOWNLINK endpoint, so all three have exactly the real UPF consequence ADR-0092
            // built for PATH_SWITCH_REQ -- repoint the downlink FAR. That code is factored into
            // install_downlink_far rather than copied three more times. What differs per value is
            // the transfer it arrives in, where the tunnel sits inside it, and what SMF answers:
            //
            //   PDU_RES_SETUP_RSP  PDUSessionResourceSetupResponseTransfer    dLQosFlowPerTNL  204
            //   PDU_RES_MOD_RSP    PDUSessionResourceModifyResponseTransfer   dL_NGU_UP_TNL*   204
            //   PDU_RES_MOD_IND    PDUSessionResourceModifyIndicationTransfer dLQosFlowPerTNL  200
            //
            // (*) OPTIONAL in the real ASN.1 -- read from the generated struct, not assumed.
            // Absent means the modification changed no downlink endpoint, which is a legal
            // answer: SMF acknowledges without touching UPF rather than rejecting.
            //
            // PDU_RES_MOD_IND is the only one of the three that owes an N2 answer:
            // PDUSessionResourceModifyConfirmTransfer (TS 38.413 §9.3.4.6), whose
            // qosFlowModifyConfirmList and uLNGU-UP-TNLInformation are both MANDATORY -- the QFIs
            // are echoed from the indication's own associatedQosFlowList (SMF confirms the flows
            // NG-RAN actually named, it does not invent a set), and the uplink tunnel is UPF's
            // own real N3 F-TEID, the same one PATH_SWITCH_REQ_ACK returns.
            const bool is_setup_rsp =
                body->n2SmInfoType.has_value() &&
                body->n2SmInfoType->value == sbi_gen::N2SmInfoType::PDU_RES_SETUP_RSP;
            const bool is_mod_rsp =
                body->n2SmInfoType.has_value() &&
                body->n2SmInfoType->value == sbi_gen::N2SmInfoType::PDU_RES_MOD_RSP;
            const bool is_mod_ind =
                body->n2SmInfoType.has_value() &&
                body->n2SmInfoType->value == sbi_gen::N2SmInfoType::PDU_RES_MOD_IND;
            if ((is_setup_rsp || is_mod_rsp || is_mod_ind) && has_n2_sm_info_bytes) {
                const char* type_name = is_setup_rsp ? "PDUSessionResourceSetupResponseTransfer"
                                        : is_mod_rsp ? "PDUSessionResourceModifyResponseTransfer"
                                                     : "PDUSessionResourceModifyIndicationTransfer";
                std::optional<GtpTunnelEndpoint> dl_endpoint;
                std::vector<long> confirmed_qfis;
                {
                    PDUSessionResourceSetupResponseTransfer_t* setup_rsp = nullptr;
                    PDUSessionResourceModifyResponseTransfer_t* mod_rsp = nullptr;
                    PDUSessionResourceModifyIndicationTransfer_t* mod_ind = nullptr;
                    const QosFlowPerTNLInformation_t* per_tnl = nullptr;
                    const UPTransportLayerInformation_t* bare_tnl = nullptr;
                    bool decode_failed = false;
                    if (is_setup_rsp) {
                        setup_rsp = static_cast<PDUSessionResourceSetupResponseTransfer_t*>(
                            ngap_per_decode(&asn_DEF_PDUSessionResourceSetupResponseTransfer,
                                            n2_sm_info_bytes));
                        decode_failed = setup_rsp == nullptr;
                        if (setup_rsp != nullptr) {
                            per_tnl = &setup_rsp->dLQosFlowPerTNLInformation;
                        }
                    } else if (is_mod_rsp) {
                        mod_rsp = static_cast<PDUSessionResourceModifyResponseTransfer_t*>(
                            ngap_per_decode(&asn_DEF_PDUSessionResourceModifyResponseTransfer,
                                            n2_sm_info_bytes));
                        decode_failed = mod_rsp == nullptr;
                        if (mod_rsp != nullptr) {
                            bare_tnl = mod_rsp->dL_NGU_UP_TNLInformation;
                        }
                    } else {
                        mod_ind = static_cast<PDUSessionResourceModifyIndicationTransfer_t*>(
                            ngap_per_decode(&asn_DEF_PDUSessionResourceModifyIndicationTransfer,
                                            n2_sm_info_bytes));
                        decode_failed = mod_ind == nullptr;
                        if (mod_ind != nullptr) {
                            per_tnl = &mod_ind->dLQosFlowPerTNLInformation;
                        }
                    }
                    auto free_transfer = [&]() {
                        if (setup_rsp != nullptr) {
                            ASN_STRUCT_FREE(asn_DEF_PDUSessionResourceSetupResponseTransfer,
                                            setup_rsp);
                        }
                        if (mod_rsp != nullptr) {
                            ASN_STRUCT_FREE(asn_DEF_PDUSessionResourceModifyResponseTransfer,
                                            mod_rsp);
                        }
                        if (mod_ind != nullptr) {
                            ASN_STRUCT_FREE(asn_DEF_PDUSessionResourceModifyIndicationTransfer,
                                            mod_ind);
                        }
                    };
                    if (decode_failed) {
                        free_transfer();
                        return sbi_core::http2::problem_response(
                            400,
                            "Bad Request",
                            std::string("n2SmInfo is not a valid ") + type_name);
                    }
                    if (per_tnl != nullptr) {
                        dl_endpoint = read_gtp_tunnel(&per_tnl->uPTransportLayerInformation);
                        if (!dl_endpoint.has_value()) {
                            free_transfer();
                            return sbi_core::http2::problem_response(
                                400,
                                "Bad Request",
                                std::string(type_name) +
                                    "'s dLQosFlowPerTNLInformation carries no real IPv4 GTP-U "
                                    "tunnel");
                        }
                        for (int i = 0; i < per_tnl->associatedQosFlowList.list.count; ++i) {
                            const auto* item = per_tnl->associatedQosFlowList.list.array[i];
                            if (item != nullptr) {
                                confirmed_qfis.push_back(item->qosFlowIdentifier);
                            }
                        }
                    } else if (bare_tnl != nullptr) {
                        // Present-but-malformed is a real error; ABSENT is not (see above).
                        dl_endpoint = read_gtp_tunnel(bare_tnl);
                        if (!dl_endpoint.has_value()) {
                            free_transfer();
                            return sbi_core::http2::problem_response(
                                400,
                                "Bad Request",
                                std::string(type_name) +
                                    "'s dL-NGU-UP-TNLInformation is present but carries no real "
                                    "IPv4 GTP-U tunnel");
                        }
                    }
                    free_transfer();
                }

                if (dl_endpoint.has_value()) {
                    if (!stored_ctx->contains("upSeid") || !stored_ctx->contains("upfIp")) {
                        return sbi_core::http2::problem_response(
                            500,
                            "Internal Server Error",
                            "No real N4 session on record for this SM context (established before "
                            "UPF was reachable) -- cannot address a real PFCP Session "
                            "Modification");
                    }
                    const auto error = install_downlink_far(
                        pfcp_peer,
                        stored_ctx->at("upSeid").get<std::uint64_t>(),
                        stored_ctx->at("upfIp").get<std::string>(),
                        dl_endpoint->teid,
                        dl_endpoint->ipv4,
                        is_setup_rsp ? "PDU_RES_SETUP_RSP"
                                     : (is_mod_rsp ? "PDU_RES_MOD_RSP" : "PDU_RES_MOD_IND"));
                    if (error.has_value()) {
                        return sbi_core::http2::problem_response(
                            500, "Internal Server Error", *error);
                    }
                } else {
                    spdlog::info("smf: {} carried no downlink endpoint -- acknowledged with no "
                                 "UPF change, which is what an absent OPTIONAL "
                                 "dL-NGU-UP-TNLInformation means",
                                 type_name);
                }

                if (!is_mod_ind) {
                    sbi_core::http2::Response resp;
                    resp.status = 204;
                    return resp;
                }

                // PDU_RES_MOD_CFM: both of its fields are MANDATORY in the real ASN.1.
                if (!stored_ctx->contains("ulTeid") || !stored_ctx->contains("ulIpv4")) {
                    return sbi_core::http2::problem_response(
                        500,
                        "Internal Server Error",
                        "No real UPF N3 uplink F-TEID on record for this SM context -- "
                        "PDUSessionResourceModifyConfirmTransfer's uLNGU-UP-TNLInformation is "
                        "mandatory and this build will not fabricate one");
                }
                const auto ul_teid = stored_ctx->at("ulTeid").get<std::uint32_t>();
                const auto ul_ipv4 = stored_ctx->at("ulIpv4").get<std::vector<std::uint8_t>>();
                if (ul_ipv4.size() != 4) {
                    return sbi_core::http2::problem_response(
                        500,
                        "Internal Server Error",
                        "Stored UPF N3 address is not a real 4-octet IPv4 address");
                }

                PDUSessionResourceModifyConfirmTransfer_t confirm{};
                for (const auto qfi : confirmed_qfis) {
                    auto* item = static_cast<QosFlowModifyConfirmItem_t*>(
                        std::calloc(1, sizeof(QosFlowModifyConfirmItem_t)));
                    item->qosFlowIdentifier = qfi;
                    ASN_SEQUENCE_ADD(&confirm.qosFlowModifyConfirmList.list, item);
                }
                confirm.uLNGU_UP_TNLInformation.present = UPTransportLayerInformation_PR_gTPTunnel;
                auto* ul_gtp_tunnel =
                    static_cast<GTPTunnel_t*>(std::calloc(1, sizeof(GTPTunnel_t)));
                const std::uint8_t ul_teid_bytes[4] = {
                    static_cast<std::uint8_t>((ul_teid >> 24) & 0xFF),
                    static_cast<std::uint8_t>((ul_teid >> 16) & 0xFF),
                    static_cast<std::uint8_t>((ul_teid >> 8) & 0xFF),
                    static_cast<std::uint8_t>(ul_teid & 0xFF)};
                ul_gtp_tunnel->gTP_TEID.buf = static_cast<std::uint8_t*>(std::malloc(4));
                std::memcpy(ul_gtp_tunnel->gTP_TEID.buf, ul_teid_bytes, 4);
                ul_gtp_tunnel->gTP_TEID.size = 4;
                ul_gtp_tunnel->transportLayerAddress.buf =
                    static_cast<std::uint8_t*>(std::malloc(4));
                std::memcpy(ul_gtp_tunnel->transportLayerAddress.buf, ul_ipv4.data(), 4);
                ul_gtp_tunnel->transportLayerAddress.size = 4;
                ul_gtp_tunnel->transportLayerAddress.bits_unused = 0;
                confirm.uLNGU_UP_TNLInformation.choice.gTPTunnel = ul_gtp_tunnel;

                const auto confirm_bytes =
                    ngap_per_encode(&asn_DEF_PDUSessionResourceModifyConfirmTransfer, &confirm);
                ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_PDUSessionResourceModifyConfirmTransfer,
                                              &confirm);
                if (confirm_bytes.empty()) {
                    return sbi_core::http2::problem_response(
                        500,
                        "Internal Server Error",
                        "Failed to PER-encode a real PDUSessionResourceModifyConfirmTransfer");
                }

                sbi_gen::SmContextUpdatedData resp_data{};
                resp_data.n2SmInfoType = sbi_gen::N2SmInfoType{};
                resp_data.n2SmInfoType->value = sbi_gen::N2SmInfoType::PDU_RES_MOD_CFM;
                sbi_gen::RefToBinaryData n2_info_ref{};
                n2_info_ref.contentId = "n2SmInfo";
                resp_data.n2SmInfo = n2_info_ref;

                sbi_core::multipart::Part json_part;
                json_part.content_type = "application/json";
                json_part.body = json(resp_data).dump();
                sbi_core::multipart::Part bin_part;
                bin_part.content_type = "application/vnd.3gpp.ngap";
                bin_part.content_id = "n2SmInfo";
                bin_part.body.assign(confirm_bytes.begin(), confirm_bytes.end());
                const auto encoded = sbi_core::multipart::encode({json_part, bin_part});

                sbi_core::http2::Response resp;
                resp.status = 200;
                resp.headers.emplace("content-type", encoded.content_type_header);
                resp.body = encoded.body;
                return resp;
            }

            // ADR-0261: handover cancellation reaches SMF as hoState=CANCELLED, a real field
            // of SmContextUpdateData -- not an N2SmInfoType, which is why it is checked before the
            // N2 dispatch below rather than inside it.
            //
            // Real, disclosed, and the reason matters: there is nothing in UPF to release here.
            // ADR-0249's HANDOVER_REQUIRED answer allocates NO new UPF resource -- it returns the
            // SAME N3 uplink F-TEID allocated at session establishment -- so the target-side
            // reservation a production SMF would tear down at this point does not exist in this
            // build. SMF therefore records the cancellation truthfully and answers, rather than
            // performing a PFCP modification that would have nothing to undo.
            if (body->hoState.has_value() && body->hoState->value == sbi_gen::HoState::CANCELLED) {
                auto cancelled_ctx = *stored_ctx;
                cancelled_ctx["hoState"] = sbi_gen::HoState::CANCELLED;
                sm_contexts.update(sm_context_ref, cancelled_ctx);
                spdlog::info("smf: handover CANCELLED for smContextRef {} -- recorded; no UPF "
                             "resource was reserved for the target, so none is released",
                             sm_context_ref);

                sbi_gen::SmContextUpdatedData resp_data{};
                resp_data.hoState = sbi_gen::HoState{};
                resp_data.hoState->value = sbi_gen::HoState::CANCELLED;
                sbi_core::http2::Response resp;
                resp.status = 200;
                resp.headers.emplace("content-type", "application/json");
                resp.body = json(resp_data).dump();
                return resp;
            }

            // ADR-0271: handover completion reaches SMF the same way cancellation does -- as a
            // real `hoState` value, not an N2SmInfoType -- so it is checked here beside CANCELLED
            // rather than inside the N2 dispatch below. TS 23.502 §4.9.1.3.3: AMF sends this once
            // the UE has actually arrived at the target gNB.
            //
            // Real, disclosed, and the reason matters as much as it did for CANCELLED. A
            // production SMF does two things at this point: switch the downlink path if it had
            // not already, and release any indirect data forwarding tunnel. Neither applies to
            // this build, for reasons that are already recorded rather than convenient: the
            // downlink FAR was really repointed at the target's tunnel back at HANDOVER_REQ_ACK
            // (ADR-0248/ADR-0270, confirmed by UPF's own log), and indirect forwarding is not
            // implemented at all -- SMF's HandoverCommandTransfer deliberately carries no
            // dLForwardingUP-TNLInformation (ADR-0248), so there is no forwarding tunnel in
            // existence to tear down. SMF therefore records the completion truthfully and
            // answers, rather than performing a PFCP modification with nothing to change.
            if (body->hoState.has_value() && body->hoState->value == sbi_gen::HoState::COMPLETED) {
                auto completed_ctx = *stored_ctx;
                completed_ctx["hoState"] = sbi_gen::HoState::COMPLETED;
                sm_contexts.update(sm_context_ref, completed_ctx);
                spdlog::info("smf: handover COMPLETED for smContextRef {} -- recorded; the "
                             "downlink FAR already points at the target gNB (HANDOVER_REQ_ACK) and "
                             "this build has no indirect forwarding tunnel to release",
                             sm_context_ref);

                sbi_gen::SmContextUpdatedData resp_data{};
                resp_data.hoState = sbi_gen::HoState{};
                resp_data.hoState->value = sbi_gen::HoState::COMPLETED;
                sbi_core::http2::Response resp;
                resp.status = 200;
                resp.headers.emplace("content-type", "application/json");
                resp.body = json(resp_data).dump();
                return resp;
            }

            // ADR-0260: the remainder of TS 29.502's 26 N2SmInfoType values, so the enum is fully
            // accounted for rather than falling through to a blanket 204 that acknowledged
            // anything at all.
            //
            // Ten more values really arrive at SMF from AMF. Every one decodes its own real
            // transfer -- which is what turns a malformed peer message into a 400 instead of a
            // silently accepted 204 -- and three of them owe a real N2 answer back.
            static const AckOnlyN2SmInfo kAckOnlyN2SmInfos[] = {
                {&sbi_gen::N2SmInfoType::PDU_RES_SETUP_FAIL,
                 &asn_DEF_PDUSessionResourceSetupUnsuccessfulTransfer,
                 "PDUSessionResourceSetupUnsuccessfulTransfer"},
                {&sbi_gen::N2SmInfoType::PDU_RES_MOD_FAIL,
                 &asn_DEF_PDUSessionResourceModifyUnsuccessfulTransfer,
                 "PDUSessionResourceModifyUnsuccessfulTransfer"},
                {&sbi_gen::N2SmInfoType::PDU_RES_REL_RSP,
                 &asn_DEF_PDUSessionResourceReleaseResponseTransfer,
                 "PDUSessionResourceReleaseResponseTransfer"},
                {&sbi_gen::N2SmInfoType::PDU_RES_NTY,
                 &asn_DEF_PDUSessionResourceNotifyTransfer,
                 "PDUSessionResourceNotifyTransfer"},
                {&sbi_gen::N2SmInfoType::PDU_RES_NTY_REL,
                 &asn_DEF_PDUSessionResourceNotifyReleasedTransfer,
                 "PDUSessionResourceNotifyReleasedTransfer"},
                {&sbi_gen::N2SmInfoType::SECONDARY_RAT_USAGE,
                 &asn_DEF_SecondaryRATDataUsageReportTransfer,
                 "SecondaryRATDataUsageReportTransfer"},
                {&sbi_gen::N2SmInfoType::UE_CONTEXT_SUSPEND_REQ,
                 &asn_DEF_UEContextSuspendRequestTransfer,
                 "UEContextSuspendRequestTransfer"},
            };
            if (body->n2SmInfoType.has_value()) {
                for (const auto& entry : kAckOnlyN2SmInfos) {
                    if (body->n2SmInfoType->value != *entry.type_value) {
                        continue;
                    }
                    if (has_n2_sm_info_bytes) {
                        void* decoded = ngap_per_decode(entry.descriptor, n2_sm_info_bytes);
                        if (decoded == nullptr) {
                            return sbi_core::http2::problem_response(
                                400,
                                "Bad Request",
                                std::string("n2SmInfo is not a valid ") + entry.transfer_name);
                        }
                        ASN_STRUCT_FREE(*entry.descriptor, decoded);
                    }
                    // Real, disclosed: SMF validates and records these, and does not act further
                    // on them. There is no per-QoS-flow rule state in this build for PDU_RES_NTY's
                    // notify/released lists to modify, no charging path consuming
                    // SECONDARY_RAT_USAGE's usage report, and no RRC-Inactive suspend state for
                    // UE_CONTEXT_SUSPEND_REQ to enter. Each is a real, separate gap, named here
                    // rather than implied by a 204 that looks like success.
                    spdlog::info("smf: {} ({}) accepted and validated for smContextRef {}",
                                 body->n2SmInfoType->value,
                                 entry.transfer_name,
                                 sm_context_ref);
                    sbi_core::http2::Response resp;
                    resp.status = 204;
                    return resp;
                }
            }

            // The three that owe a real N2 answer. Each echoes the cause NG-RAN reported rather
            // than inventing one -- see copy_cause for the single case that cannot be echoed.
            //
            //   PATH_SWITCH_SETUP_FAIL  -> PATH_SWITCH_REQ_FAIL
            //   (PathSwitchRequestUnsuccessfulTransfer) HANDOVER_RES_ALLOC_FAIL ->
            //   HANDOVER_PREP_FAIL    (HandoverPreparationUnsuccessfulTransfer)
            //   UE_CONTEXT_RESUME_REQ   -> UE_CONTEXT_RESUME_RSP (UEContextResumeResponseTransfer)
            const bool is_ps_setup_fail =
                body->n2SmInfoType.has_value() &&
                body->n2SmInfoType->value == sbi_gen::N2SmInfoType::PATH_SWITCH_SETUP_FAIL;
            const bool is_ho_alloc_fail =
                body->n2SmInfoType.has_value() &&
                body->n2SmInfoType->value == sbi_gen::N2SmInfoType::HANDOVER_RES_ALLOC_FAIL;
            const bool is_resume_req =
                body->n2SmInfoType.has_value() &&
                body->n2SmInfoType->value == sbi_gen::N2SmInfoType::UE_CONTEXT_RESUME_REQ;
            if (is_ps_setup_fail || is_ho_alloc_fail || is_resume_req) {
                if (!has_n2_sm_info_bytes) {
                    return sbi_core::http2::problem_response(
                        400,
                        "Bad Request",
                        body->n2SmInfoType->value +
                            " requires an n2SmInfo binary part and none was sent");
                }
                std::vector<std::uint8_t> answer_bytes;
                std::string answer_type;
                if (is_resume_req) {
                    auto* req_transfer = static_cast<UEContextResumeRequestTransfer_t*>(
                        ngap_per_decode(&asn_DEF_UEContextResumeRequestTransfer, n2_sm_info_bytes));
                    if (req_transfer == nullptr) {
                        return sbi_core::http2::problem_response(
                            400,
                            "Bad Request",
                            "n2SmInfo is not a valid UEContextResumeRequestTransfer");
                    }
                    const int failed_count =
                        req_transfer->qosFlowFailedToResumeList != nullptr
                            ? req_transfer->qosFlowFailedToResumeList->list.count
                            : 0;
                    ASN_STRUCT_FREE(asn_DEF_UEContextResumeRequestTransfer, req_transfer);
                    // Every field of UEContextResumeResponseTransfer is OPTIONAL. An empty one is
                    // spec-legal and means "all flows resumed" -- it is not a placeholder standing
                    // in for a value SMF failed to compute. SMF has no per-flow resume state to
                    // report a failure from, which is why the list is omitted rather than guessed.
                    UEContextResumeResponseTransfer_t rsp_transfer{};
                    answer_bytes =
                        ngap_per_encode(&asn_DEF_UEContextResumeResponseTransfer, &rsp_transfer);
                    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_UEContextResumeResponseTransfer,
                                                  &rsp_transfer);
                    answer_type = sbi_gen::N2SmInfoType::UE_CONTEXT_RESUME_RSP;
                    spdlog::info(
                        "smf: UE_CONTEXT_RESUME_REQ for smContextRef {} ({} flow(s) NG-RAN "
                        "could not resume) -- answering UE_CONTEXT_RESUME_RSP",
                        sm_context_ref,
                        failed_count);
                } else {
                    Cause_t reported_cause{};
                    bool cause_read = false;
                    if (is_ps_setup_fail) {
                        auto* fail =
                            static_cast<PathSwitchRequestSetupFailedTransfer_t*>(ngap_per_decode(
                                &asn_DEF_PathSwitchRequestSetupFailedTransfer, n2_sm_info_bytes));
                        if (fail == nullptr) {
                            return sbi_core::http2::problem_response(
                                400,
                                "Bad Request",
                                "n2SmInfo is not a valid PathSwitchRequestSetupFailedTransfer");
                        }
                        cause_read = copy_cause(fail->cause, reported_cause);
                        ASN_STRUCT_FREE(asn_DEF_PathSwitchRequestSetupFailedTransfer, fail);
                    } else {
                        auto* fail = static_cast<HandoverResourceAllocationUnsuccessfulTransfer_t*>(
                            ngap_per_decode(&asn_DEF_HandoverResourceAllocationUnsuccessfulTransfer,
                                            n2_sm_info_bytes));
                        if (fail == nullptr) {
                            return sbi_core::http2::problem_response(
                                400,
                                "Bad Request",
                                "n2SmInfo is not a valid "
                                "HandoverResourceAllocationUnsuccessfulTransfer");
                        }
                        cause_read = copy_cause(fail->cause, reported_cause);
                        ASN_STRUCT_FREE(asn_DEF_HandoverResourceAllocationUnsuccessfulTransfer,
                                        fail);
                    }
                    if (!cause_read) {
                        return sbi_core::http2::problem_response(
                            500,
                            "Internal Server Error",
                            "NG-RAN reported an extension-coded Cause, which this build will not "
                            "echo (echoing it would require deep-copying an unbounded extension "
                            "container) and will not replace with a cause of its own");
                    }
                    if (is_ps_setup_fail) {
                        PathSwitchRequestUnsuccessfulTransfer_t unsucc{};
                        unsucc.cause = reported_cause;
                        answer_bytes = ngap_per_encode(
                            &asn_DEF_PathSwitchRequestUnsuccessfulTransfer, &unsucc);
                        answer_type = sbi_gen::N2SmInfoType::PATH_SWITCH_REQ_FAIL;
                    } else {
                        HandoverPreparationUnsuccessfulTransfer_t unsucc{};
                        unsucc.cause = reported_cause;
                        answer_bytes = ngap_per_encode(
                            &asn_DEF_HandoverPreparationUnsuccessfulTransfer, &unsucc);
                        answer_type = sbi_gen::N2SmInfoType::HANDOVER_PREP_FAIL;
                    }
                    // No ASN_STRUCT_FREE_CONTENTS_ONLY on the outgoing transfer: its only field is
                    // the value-copied Cause above, which owns no heap memory (see copy_cause).
                    spdlog::info("smf: {} for smContextRef {} -- answering {} with NG-RAN's own "
                                 "reported cause (present={})",
                                 body->n2SmInfoType->value,
                                 sm_context_ref,
                                 answer_type,
                                 static_cast<int>(reported_cause.present));
                }
                if (answer_bytes.empty()) {
                    return sbi_core::http2::problem_response(500,
                                                             "Internal Server Error",
                                                             "Failed to PER-encode a real " +
                                                                 answer_type + " transfer");
                }

                sbi_gen::SmContextUpdatedData resp_data{};
                resp_data.n2SmInfoType = sbi_gen::N2SmInfoType{};
                resp_data.n2SmInfoType->value = answer_type;
                sbi_gen::RefToBinaryData n2_info_ref{};
                n2_info_ref.contentId = "n2SmInfo";
                resp_data.n2SmInfo = n2_info_ref;

                sbi_core::multipart::Part json_part;
                json_part.content_type = "application/json";
                json_part.body = json(resp_data).dump();
                sbi_core::multipart::Part bin_part;
                bin_part.content_type = "application/vnd.3gpp.ngap";
                bin_part.content_id = "n2SmInfo";
                bin_part.body.assign(answer_bytes.begin(), answer_bytes.end());
                const auto encoded = sbi_core::multipart::encode({json_part, bin_part});

                sbi_core::http2::Response resp;
                resp.status = 200;
                resp.headers.emplace("content-type", encoded.content_type_header);
                resp.body = encoded.body;
                return resp;
            }

            // The remaining ten values name transfers TS 38.413 defines as NG-RAN-bound -- ones
            // SMF PRODUCES and sends towards the RAN, not ones it can be asked to process. Stated
            // honestly: TS 29.502 does not itself tabulate a direction per N2SmInfoType value, so
            // this is a reading of TS 38.413's own definition of who builds each transfer, not a
            // quoted rule. A 400 is still strictly better than the blanket 204 that stood here
            // before, which acknowledged an impossible request as though it had been processed.
            static const std::string* const kSmfOriginatedN2SmInfos[] = {
                &sbi_gen::N2SmInfoType::PDU_RES_SETUP_REQ,
                &sbi_gen::N2SmInfoType::PDU_RES_REL_CMD,
                &sbi_gen::N2SmInfoType::PDU_RES_MOD_REQ,
                &sbi_gen::N2SmInfoType::PDU_RES_MOD_CFM,
                &sbi_gen::N2SmInfoType::PDU_RES_MOD_IND_FAIL,
                &sbi_gen::N2SmInfoType::PATH_SWITCH_REQ_ACK,
                &sbi_gen::N2SmInfoType::PATH_SWITCH_REQ_FAIL,
                &sbi_gen::N2SmInfoType::HANDOVER_CMD,
                &sbi_gen::N2SmInfoType::HANDOVER_PREP_FAIL,
                &sbi_gen::N2SmInfoType::UE_CONTEXT_RESUME_RSP,
            };
            if (body->n2SmInfoType.has_value()) {
                for (const auto* originated : kSmfOriginatedN2SmInfos) {
                    if (body->n2SmInfoType->value == *originated) {
                        return sbi_core::http2::problem_response(
                            400,
                            "Bad Request",
                            body->n2SmInfoType->value +
                                " is an SMF-originated N2 SM information type (SMF builds this "
                                "transfer and sends it towards NG-RAN); it is not something SMF "
                                "can be asked to process in an UpdateSMContext request");
                    }
                }
            }

            // Disclosed simplification: acknowledges every other N2SmInfoType/plain update (204)
            // rather than fabricating SmContextUpdatedData content (EBI allocation, other N1/N2
            // info, ...) with no real PCF/UPF backing it yet -- see file header for the full,
            // real, disclosed scope (only PATH_SWITCH_REQ above has real datapath behavior).
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/sm-contexts/{smContextRef}/release",
        [&verifier,
         &sm_contexts,
         &release_counter,
         &pcf_client,
         &pcf_oauth,
         &pcf_base_url,
         &pcf_sm_policy_delete_counter,
         &chf_client,
         &chf_oauth,
         &chf_base_url,
         &chf_charging_data_release_counter,
         &smf_instance_id,
         &charging_data_invocation_seq](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto sm_context_ref = req.path_params.at("smContextRef");
            auto stored = sm_contexts.get(sm_context_ref);
            if (!stored.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No SM context with ref " + sm_context_ref);
            }
            // SmContextReleaseData is optional per spec (required: false).
            if (!req.body.empty()) {
                sbi_core::http2::Response err;
                auto body =
                    sbi_core::http2::parse_json_body<sbi_gen::SmContextReleaseData_Nsmf_PDUSession>(
                        req, err);
                if (!body.has_value()) {
                    return err;
                }
            }

            // Best-effort DeleteSMPolicy -- see file header for why this doesn't gate local
            // release the way CreateSMContext's PCF call gates creation.
            if (stored->contains("smPolicyId")) {
                const auto sm_policy_id = (*stored)["smPolicyId"].get<std::string>();
                if (!sm_policy_id.empty()) {
                    auto token = pcf_oauth.get_bearer_token();
                    if (!token.has_value()) {
                        spdlog::warn(
                            "smf: could not obtain a PCF token for best-effort DeleteSMPolicy "
                            "(smPolicyId={}): {}",
                            sm_policy_id,
                            token.error());
                    } else {
                        sbi_core::http2::ClientRequest pcf_http_req;
                        pcf_http_req.method = "POST";
                        pcf_http_req.url = pcf_base_url + "/npcf-smpolicycontrol/v1/sm-policies/" +
                                           sm_policy_id + "/delete";
                        pcf_http_req.headers.emplace("content-type", "application/json");
                        pcf_http_req.headers.emplace("authorization", "Bearer " + *token);
                        pcf_http_req.body = json::object().dump();
                        auto pcf_resp = pcf_client.send(pcf_http_req);
                        if (pcf_resp.has_value() && pcf_resp->status == 204) {
                            pcf_sm_policy_delete_counter->Add(1);
                        } else {
                            spdlog::warn("smf: best-effort DeleteSMPolicy failed for smPolicyId={}",
                                         sm_policy_id);
                        }
                    }
                }
            }

            // Best-effort Nchf_ConvergedCharging_Release (ADR-0046) -- same discipline as
            // DeleteSMPolicy directly above: local release must not block on CHF being
            // unreachable. Needs both chargingDataRef (from Create's response, ADR-0044) and supi
            // (stored alongside smPolicyId above) -- if either is missing (e.g. Create's own N40
            // call never succeeded for this session), there's nothing valid to release, skipped
            // rather than sent with a fabricated ref.
            if (stored->contains("chargingDataRef") && stored->contains("supi")) {
                const auto charging_data_ref = (*stored)["chargingDataRef"].get<std::string>();
                const auto supi = (*stored)["supi"].get<std::string>();
                if (!charging_data_ref.empty() &&
                    perform_n40_charging_data_release(
                        chf_client,
                        chf_oauth,
                        chf_base_url,
                        smf_instance_id,
                        supi,
                        charging_data_ref,
                        charging_data_invocation_seq.get_and_advance(charging_data_ref))) {
                    chf_charging_data_release_counter->Add(1);
                }
            }

            sm_contexts.remove(sm_context_ref);
            release_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // ---- TS29508_Nsmf_EventExposure.yaml (ADR-0201) ----

    server.add_route(
        "POST",
        std::string(kEventExposureApiRoot) + "/subscriptions",
        [&verifier, &event_subs, &event_sub_create_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::NsmfEventExposure>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto sub_id = event_subs.create(*body);
            event_sub_create_counter->Add(1);
            body->subId = sub_id;
            json j = *body;
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kEventExposureApiRoot) + "/subscriptions/" + sub_id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kEventExposureApiRoot) + "/subscriptions/{subId}",
        [&verifier, &event_subs, &event_sub_get_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto sub_id = req.path_params.at("subId");
            auto stored = event_subs.get(sub_id);
            if (!stored.has_value()) {
                return sbi_core::http2::problem_response(404, "Not Found", "No such subscription");
            }
            event_sub_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, stored->dump());
        });

    server.add_route(
        "PUT",
        std::string(kEventExposureApiRoot) + "/subscriptions/{subId}",
        [&verifier, &event_subs, &event_sub_replace_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto sub_id = req.path_params.at("subId");
            if (!event_subs.get(sub_id).has_value()) {
                return sbi_core::http2::problem_response(404, "Not Found", "No such subscription");
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::NsmfEventExposure>(req, err);
            if (!body.has_value()) {
                return err;
            }
            body->subId = sub_id;
            event_subs.update(sub_id, *body);
            event_sub_replace_counter->Add(1);
            json j = *body;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "DELETE",
        std::string(kEventExposureApiRoot) + "/subscriptions/{subId}",
        [&verifier, &event_subs, &event_sub_delete_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto sub_id = req.path_params.at("subId");
            if (!event_subs.get(sub_id).has_value()) {
                return sbi_core::http2::problem_response(404, "Not Found", "No such subscription");
            }
            event_subs.remove(sub_id);
            event_sub_delete_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // ---- TS29542_Nsmf_NIDD.yaml (ADR-0201) ----

    server.add_route(
        "POST",
        std::string(kNiddApiRoot) + "/pdu-sessions/{pduSessionRef}/deliver",
        [&verifier, &sm_contexts, &nidd_deliver_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_multipart_json_body<sbi_gen::DeliverReqData_Nsmf_NIDD>(req,
                                                                                              err);
            if (!body.has_value()) {
                return err;
            }
            // Disclosed simplification (see file header): pduSessionRef is treated as referring
            // to this project's own smContextRef id space -- there is no separate real
            // "pdu-sessions" resource anywhere else in this build.
            const auto pdu_session_ref = req.path_params.at("pduSessionRef");
            if (!sm_contexts.get(pdu_session_ref).has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such PDU session: " + pdu_session_ref);
            }
            nidd_deliver_counter->Add(1);
            // Disclosed simplification: no real NAS/5G-SM NIDD delivery pipeline exists to
            // actually push the MT data to a UE, same class of gap as every other NAS-adjacent
            // simplification in this file.
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // ---- ADR-0257: SendMoData / TransferMoData (TS29502_Nsmf_PDUSession.yaml) ----
    // The last two genuinely-absent paths in ADR-0252's whole-project audit. Deferred at SMF's
    // first turn (ADR-0032) as "peripheral to Phase 2" -- a scoping call, not a spec blocker, and
    // closed here.
    //
    // Both are the mobile-ORIGINATED mirror of Nsmf_NIDD's own mobile-terminated Deliver
    // implemented just above, and are implemented the same way for the same reasons. Both are
    // multipart/related-ONLY in the real spec (no application/json alternative exists for either),
    // and both define exactly one success response: 204, no body.
    //
    // Real, disclosed simplification, identical in class to Deliver's own and to every other
    // NAS-adjacent gap in this file: the binary `moData` part is parsed and its presence validated
    // by the multipart codec, but there is no real onward path to a DN or to NEF's own
    // Nnef_SMContext for it to be forwarded to. The operation accepts and validates; it does not
    // pretend to deliver. `moExpDataCounter`/`ueLocation` are likewise accepted and not acted on.

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/sm-contexts/{smContextRef}/send-mo-data",
        [&verifier, &sm_contexts, &send_mo_data_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_multipart_json_body<sbi_gen::SendMoDataReqData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto sm_context_ref = req.path_params.at("smContextRef");
            if (!sm_contexts.get(sm_context_ref).has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such SM context: " + sm_context_ref);
            }
            send_mo_data_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/pdu-sessions/{pduSessionRef}/transfer-mo-data",
        [&verifier, &sm_contexts, &transfer_mo_data_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_multipart_json_body<sbi_gen::TransferMoDataReqData>(
                req, err);
            if (!body.has_value()) {
                return err;
            }
            // Same disclosed identifier simplification Nsmf_NIDD's own Deliver makes: this build
            // has no separate real `/pdu-sessions` resource, so pduSessionRef is resolved against
            // the smContextRef id space. Stated here rather than inherited silently.
            const auto pdu_session_ref = req.path_params.at("pduSessionRef");
            if (!sm_contexts.get(pdu_session_ref).has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such PDU session: " + pdu_session_ref);
            }
            transfer_mo_data_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    std::thread(run_nrf_lifecycle, smf_instance_id, nrf_base_url).detach();
    std::thread(run_pfcp_lifecycle,
                smf_instance_id,
                nrf_base_url,
                std::ref(upf_endpoint_store),
                std::ref(pfcp_peer))
        .detach();

    server.start();
    spdlog::info("smf: listening on https://0.0.0.0:{} (TLS 1.3 + mTLS)", port);
    spdlog::info("smf: Prometheus metrics at http://{}/metrics", metrics_bind_address);
    sbi_core::run_multi_threaded(ioc);
    return 0;
}
