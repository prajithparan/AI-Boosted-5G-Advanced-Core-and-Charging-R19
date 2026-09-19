// nfs/amf: AMF (Access and Mobility Management Function), Namf_Communication surface.
// Source: specs/5G_APIs-REL-19/TS29518_Namf_Communication.yaml (commit
// bca84b60a37773133bcae97e5c6c0d10a93b47b6). Phase 2's second NF (PROMPT.md/CLAUDE.md order:
// NRF -> AMF -> SMF -> UDM -> UDR -> AUSF -> PCF).
//
// In scope: every Namf_Communication operationId -- ReleaseUEContext, EBIAssignment,
// UEContextTransfer, RegistrationStatusUpdate, N1N2MessageTransfer, N1N2MessageSubscribe,
// N1N2MessageUnSubscribe, NonUeN2MessageTransfer, NonUeN2InfoSubscribe, NonUeN2InfoUnSubscribe,
// AMFStatusChangeSubscribe, AMFStatusChangeUnSubscribe, AMFStatusChangeSubscribeModfy (all
// application/json-capable, built first), plus CreateUEContext, RelocateUEContext,
// CancelRelocateUEContext (multipart/related-ONLY per spec -- initially deferred, then built once
// sbi_core::multipart landed; see docs/DECISIONS.md ADR-0020).
//
// Disclosed simplifications, real and not hidden:
// - CreateUEContext/RelocateUEContext are the inter-AMF mobility/handover operations
//   (UeContextCreateData/UeContextRelocateData mandate a real source AMF's N2/NGAP payload,
//   NgRanTargetId, etc.) -- this lab has exactly one AMF and no NGAP stack, so these are
//   implemented per-spec (real field validation, real store writes) but their N2 content fields
//   are stub placeholders (see the handlers below), not real inter-AMF state transfer.
// - Subscriptions (N1N2Message*, NonUeN2Info*, AMFStatusChange*) are created/removed for real, but
//   notification DELIVERY is not implemented -- there is no trigger path yet (no NGAP/N2, no real
//   UE, no multi-AMF deployment) that would ever fire one.
// - Error responses use the generic ProblemDetails shape (sbi_core::ProblemDetails,
//   application/problem+json) rather than each operation's bespoke *Error schema (AssignEbiError,
//   N1N2MessageTransferError, N2InformationTransferError, UeContextCreateError) -- same
//   simplification NRF already uses for its own error paths.
//
// UPDATE (ADR-0199, gap-closure task #158): two more Namf_* APIs, both already in the sbi-codegen
// pilot set but never wired to a route, now are:
// - TS29518_Namf_Location.yaml (api root /namf-loc/v1): all 3
//   operations. ProvidePositioningInfo does real structural validation (RequestPosInfo's own
//   required fields) then a real, disclosed 501 -- same class of gap as LMF's own DetermineLocation
//   (ADR-0191): no real LPP/GNSS/PRU positioning capability exists anywhere in this build.
//   ProvideLocationInfo checks the real UeContextStore (mirrors ReleaseUEContext's own get/404
//   pattern below) -- 404 if the context doesn't exist, else a real but honestly-empty
//   ProvideLocInfo{} 200, because UeContextStore only ever persists {"ueContextId": ...} (see
//   CreateUEContext above and ue_context_store.hpp's own header) -- there is no real RAT-type/
//   location/timezone data anywhere in this AMF to answer from. CancelLocation does real structural
//   validation (CancelPosInfo's required supi/hgmlcCallBackURI/ldrReference) then a real 404 --
//   ProvidePositioningInfo never issues a real ldrReference (it always 501s), so no cancellation
//   request can ever match one; same precedent as LMF/GMLC's own cancel-path gaps.
// - TS29518_Namf_EventExposure.yaml (api root /namf-evts/v1): all 6 operations -- real, full
//   create+modify+delete subscription CRUD for both /subscriptions (individual) and
//   /set-subscriptions (AMF-Set-level bulk) families, backed by two AmfEventSubscriptionStore
//   instances (see subscriptions.hpp). Modify uses real RFC 6902 JSON Patch
//   (nlohmann::json::patch()) against the stored AmfEventSubscription --
//   AmfUpdateEventSubscriptionItem is real op/path/value-shaped per the YAML, so no special-case
//   parsing is needed. Same disclosed notification-delivery gap as this file's other 3 subscription
//   types (see subscriptions.hpp's own header): subscriptions are stored/modified/removed for real,
//   but nothing ever fires one.
//
// UPDATE (ADR-0200, gap-closure task #159): four more Namf_* APIs, none previously in the
// sbi-codegen pilot set at all (real Tier-A gap, now closed):
// - TS29518_Namf_AIoT.yaml (api root /namf-aiot/v1): MessageDelivery, real structural validation
//   of AiotMessageReq (accepts both its real application/json and multipart/related encodings --
//   dispatched on the real Content-Type header, reusing sbi_core::multipart::is_multipart_related),
//   then a real 204 -- disclosed: this lab has no real Ambient IoT device/NG-RAN stack behind it
//   (TS 23.369 is a Tier 3 R19 feature per CLAUDE.md), so "successfully received" is honest but
//   nothing is actually forwarded to a device.
// - TS29518_Namf_MBSCommunication.yaml (api root /namf-mbs-comm/v1): N2MessageTransfer, real
//   multipart/related-only structural validation (MbsN2MessageTransferReqData), then a real 200
//   with result=N2_INFO_TRANSFER_INITIATED -- same disclosed "no real N2 delivery pipeline"
//   simplification as Namf_Communication's own NonUeN2MessageTransfer above.
// - TS29518_Namf_MBSBroadcast.yaml (api root /namf-mbs-bc/v1): all 3 operations, backed by a new
//   MbsBroadcastContextStore (subscriptions.hpp). ContextCreate: real structural validation
//   (multipart-only ContextCreateReqData), assigns a real mbsContextRef, real 201. ContextDelete:
//   real get/404-then-remove/204 (mirrors ReleaseUEContext's own pattern). ContextUpdate: real
//   get/404 against the stored context, then a real 204 -- disclosed: the update body's fields are
//   validated but not merged into stored state, since (same as Create) there is no real N2
//   consumer downstream that would ever read the merged result.
// - TS29518_Namf_MT.yaml (api root /namf-mt/v1): all 3 operations. ProvideDomainSelectionInfo
//   checks the real UeContextStore -- 404 if absent, else an honestly-empty UeContextInfo{} 200
//   (same disclosed no-VoPS/RAT-data gap as Namf_Location's ProvideLocationInfo).
//   EnableUeReachability checks UeContextStore -- 404 if absent, else a real ack (echoes the
//   requested `reachability` back), disclosed: no real paging/reachability state machine exists to
//   actually change UE reachability. EnableGroupReachability does real structural validation
//   (ueInfoList+tmgi) then a real 200 with an honestly-empty ueConnectedList -- no real per-UE
//   reachability tracking exists to populate it from.

#include "sbi_core/http2_client.hpp"
#include "sbi_core/http2_server.hpp"
#include "sbi_core/io_context_pool.hpp"
#include "sbi_core/jwt.hpp"
#include "sbi_core/logging.hpp"
#include "sbi_core/metrics.hpp"
#include "sbi_core/multipart.hpp"
#include "sbi_core/oauth2_client.hpp"
#include "sbi_core/otel.hpp"
#include "sbi_core/problem_details.hpp"
#include "sbi_core/rate_limit.hpp"
#include "sbi_core/sbi_headers.hpp"
#include "sbi_core/uuid.hpp"

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <sw/redis++/redis++.h>
#include <thread>

#include "TS26510_CommonData_grp.hpp"
#include "TS29518_Namf_AIoT.hpp"
#include "TS29518_Namf_MBSBroadcast.hpp"
#include "TS29518_Namf_MBSCommunication.hpp"
#include "TS29518_Namf_MT.hpp"
#include "li_poi.hpp"
#include "nf_config/nf_config.hpp"
#include "ngap_task.hpp"
#include "subscriptions.hpp"
#include "ue_context_store.hpp"
#include "ue_security_context_store.hpp"

namespace {

using nlohmann::json;

#ifndef CERTS_DIR
#error "CERTS_DIR must be defined by CMake (see nfs/amf/CMakeLists.txt)"
#endif
#ifndef CONFIG_DIR
#error "CONFIG_DIR must be defined by CMake (see nfs/amf/CMakeLists.txt)"
#endif

constexpr const char* kNfType = "AMF";
constexpr const char* kApiRoot = "/namf-comm/v1";
// TS29518_Namf_Location.yaml / TS29518_Namf_EventExposure.yaml own real api roots (ADR-0199) --
// distinct from kApiRoot above, confirmed via each YAML's own `servers:` block.
constexpr const char* kLocationApiRoot = "/namf-loc/v1";
constexpr const char* kEventExposureApiRoot = "/namf-evts/v1";
// TS29518_Namf_AIoT.yaml / TS29518_Namf_MBSBroadcast.yaml / TS29518_Namf_MBSCommunication.yaml /
// TS29518_Namf_MT.yaml own real api roots (ADR-0200), confirmed via each YAML's own `servers:`
// block.
constexpr const char* kAiotApiRoot = "/namf-aiot/v1";
constexpr const char* kMbsBroadcastApiRoot = "/namf-mbs-bc/v1";
constexpr const char* kMbsCommApiRoot = "/namf-mbs-comm/v1";
constexpr const char* kMtApiRoot = "/namf-mt/v1";

// Must match nfs/nrf/src/main.cpp's kNrfInstanceId exactly -- see docs/DECISIONS.md ADR-0018 for
// why NRF's identity is a fixed constant rather than randomly generated per run (any NF other than
// NRF itself needs to know NRF's nfInstanceId in advance to construct a working
// sbi_core::jwt::Verifier, since the `iss` claim on every token NRF issues is NRF's own
// nfInstanceId). This is a fixed protocol-identity constant, not a deployment parameter, so
// ADR-0077 (no hardcoded DB URL/config params -- see below) doesn't apply to it.
constexpr const char* kNrfInstanceId = "5ba9a927-1d31-4c8e-8a10-000000000001";

sbi_core::http2::Response
problem_response(int status, const std::string& title, const std::string& detail) {
    auto pd = sbi_core::make_problem_details(status, title, detail);
    json j = pd;
    sbi_core::http2::Response r;
    r.status = status;
    r.headers.emplace("content-type", "application/problem+json");
    r.body = j.dump();
    return r;
}

// Same pattern as nfs/nrf/src/main.cpp's check_bearer -- see that file's comment for why a missing
// Authorization header is not itself a 401 (RegisterNFInstance-equivalent bootstrap security
// alternative: `security: [{}, oAuth2ClientCredentials:[...]]` in the YAML).
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

// Parses req.body as JSON into T (a generated sbi_gen DTO). On success returns the value; on
// failure writes a 400 ProblemDetails into err_out and returns nullopt. Shared across all 13
// routes below rather than repeating the same try/catch 13 times.
template <typename T>
std::optional<T> parse_json_body(const sbi_core::http2::Request& req,
                                 sbi_core::http2::Response& err_out) {
    try {
        return json::parse(req.body).get<T>();
    } catch (const json::parse_error& e) {
        err_out = problem_response(400, "Malformed JSON", e.what());
    } catch (const json::exception& e) {
        err_out = problem_response(400, "Missing or invalid mandatory IE", e.what());
    }
    return std::nullopt;
}

// Same contract as parse_json_body<T>, but for the three operations that are multipart/related-
// ONLY per spec (CreateUEContext, RelocateUEContext, CancelRelocateUEContext -- see
// docs/DECISIONS.md ADR-0020). Extracts and parses the root (jsonData) part; binary parts
// (N2/GTP-C content) are validated as present in the wire format but not otherwise interpreted --
// there is nothing in this build that consumes real NGAP/GTP-C bytes yet.
template <typename T>
std::optional<T> parse_multipart_json_body(const sbi_core::http2::Request& req,
                                           sbi_core::http2::Response& err_out) {
    const auto content_type_it = req.headers.find("content-type");
    if (content_type_it == req.headers.end() ||
        !sbi_core::multipart::is_multipart_related(content_type_it->second)) {
        err_out = problem_response(
            400, "Unsupported Media Type", "This operation requires a multipart/related body");
        return std::nullopt;
    }
    auto parts = sbi_core::multipart::parse(content_type_it->second, req.body);
    if (!parts.has_value()) {
        err_out = problem_response(400, "Malformed multipart body", parts.error());
        return std::nullopt;
    }
    if (parts->empty() || (*parts)[0].content_type.find("application/json") == std::string::npos) {
        err_out = problem_response(
            400, "Malformed multipart body", "first part (jsonData) must be application/json");
        return std::nullopt;
    }
    try {
        return json::parse((*parts)[0].body).get<T>();
    } catch (const json::parse_error& e) {
        err_out = problem_response(400, "Malformed JSON in jsonData part", e.what());
    } catch (const json::exception& e) {
        err_out = problem_response(400, "Missing or invalid mandatory IE", e.what());
    }
    return std::nullopt;
}

// Runs on a dedicated thread, never on the server's io_context -- libs/sbi-core's http2::Client is
// synchronous (docs/DECISIONS.md ADR-0006, disclosed debt), so driving NRF registration/heartbeat
// from the same io_context that serves inbound Namf_Communication requests would stall the server
// while a heartbeat call is in flight. A dedicated thread with its own Client instance is a
// minimal, disclosed resolution -- not the full curl_multi/Asio integration ADR-0006 names as the
// eventual real fix, which remains future work.
void run_nrf_lifecycle(const std::string& amf_instance_id,
                       const std::string& nrf_base,
                       const std::string& advertised_ipv4) {
    sbi_core::http2::TlsConfig client_tls{
        .cert_path = CERTS_DIR "/amf/cert.pem",
        .key_path = CERTS_DIR "/amf/key.pem",
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
        http_client, nrf_base + "/oauth2/token", amf_instance_id, "nnrf-nfm", "NRF");

    constexpr int kHeartbeatSeconds = 30;
    json profile{
        {"nfInstanceId", amf_instance_id},
        {"nfType", kNfType},
        {"nfStatus", "REGISTERED"},
        {"ipv4Addresses", json::array({advertised_ipv4})},
        {"heartBeatTimer", kHeartbeatSeconds},
    };

    while (true) {
        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("amf: OAuth2 token fetch failed: {}", token.error());
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        sbi_core::http2::ClientRequest put_req;
        put_req.method = "PUT";
        put_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + amf_instance_id;
        put_req.headers.emplace("content-type", "application/json");
        put_req.headers.emplace("authorization", "Bearer " + *token);
        put_req.headers.emplace(
            sbi_core::headers::kSenderTimestamp,
            sbi_core::headers::format_sender_timestamp(std::chrono::system_clock::now()));
        put_req.body = profile.dump();

        auto put_resp = http_client.send(put_req);
        if (put_resp.has_value() && (put_resp->status == 200 || put_resp->status == 201)) {
            spdlog::info("amf: registered with NRF (HTTP {})", put_resp->status);
            break;
        }
        spdlog::warn("amf: NRF registration attempt failed, retrying in 5s");
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(kHeartbeatSeconds / 2));

        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("amf: OAuth2 token fetch failed for heartbeat: {}", token.error());
            continue;
        }

        sbi_core::http2::ClientRequest patch_req;
        patch_req.method = "PATCH";
        patch_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + amf_instance_id;
        patch_req.headers.emplace("content-type", "application/json-patch+json");
        patch_req.headers.emplace("authorization", "Bearer " + *token);
        patch_req.body =
            json::array({json{{"op", "replace"}, {"path", "/nfStatus"}, {"value", "REGISTERED"}}})
                .dump();

        auto patch_resp = http_client.send(patch_req);
        if (!patch_resp.has_value() || patch_resp->status != 200) {
            spdlog::warn("amf: heartbeat failed");
        }
    }
}

} // namespace

int main() {
    sbi_core::init_logging("amf");
    sbi_core::init_tracing("amf");

    // ADR-0077 (user-directed, mandatory, project-wide): no DB URL/connection/deployment
    // parameter may be a hardcoded literal default in source -- real values live in the
    // checked-in config/amf.json, with an env var override per key still available for
    // deployment-time substitution (e.g. AMF_REDIS_URL, matching every other NF's own existing
    // override convention).
    const auto config = nf_config::load("amf", CONFIG_DIR);
    const auto port = nf_config::require<unsigned short>(config, "port");
    const auto metrics_bind_address =
        nf_config::require<std::string>(config, "metrics_bind_address");
    const auto nrf_base =
        nf_config::require<std::string>(config, "nrf_base_url", "AMF_NRF_BASE_URL");
    const auto redis_url = nf_config::require<std::string>(config, "redis_url", "AMF_REDIS_URL");
    const auto ngap_bind_address = nf_config::require<std::string>(config, "ngap_bind_address");
    const auto ngap_bind_port = nf_config::require<std::uint16_t>(config, "ngap_bind_port");
    // Real, disclosed lab AMF identity (TS 24.501 §9.11.3.4's own 5G-GUTI structure) -- MUST
    // match ngap_task.cpp's own build_ng_setup_response, which broadcasts this same AMF
    // Region/Set/Pointer to the gNB via NGSetupResponse's own GUAMI (docs/DECISIONS.md ADR-0076).
    // Task #109 (ADR-0273): AMF's peer NF base URLs, previously compile-time literals in
    // ngap_task.cpp/ngap_handover.cpp. Env overrides follow nf_config's existing per-key
    // convention, which is what lets a deployment (or a test) point this AMF at a different SMF
    // without a rebuild.
    amf::ngap::PeerEndpoints peers;
    peers.ausf_base = nf_config::require<std::string>(config, "ausf_base_url", "AMF_AUSF_BASE_URL");
    peers.pcf_base = nf_config::require<std::string>(config, "pcf_base_url", "AMF_PCF_BASE_URL");
    peers.smf_base = nf_config::require<std::string>(config, "smf_base_url", "AMF_SMF_BASE_URL");
    peers.nsacf_base =
        nf_config::require<std::string>(config, "nsacf_base_url", "AMF_NSACF_BASE_URL");
    // AMF's own advertised address. The HOST comes from config (`advertised_ipv4`), the PORT is
    // reused from `port` rather than given a key of its own -- a separate self-port key would have
    // to be hand-synced with the listener's, which is the drift this task removes. The same
    // `advertised_ipv4` feeds the NRF profile's ipv4Addresses below, so the address AMF publishes
    // to NRF and the one it puts in its own callback URIs cannot disagree.
    const auto advertised_ipv4 =
        nf_config::require<std::string>(config, "advertised_ipv4", "AMF_ADVERTISED_IPV4");
    peers.self_base = "https://" + advertised_ipv4 + ":" + std::to_string(port);

    const auto amf_region_id = nf_config::require<std::uint8_t>(config, "amf_region_id");
    const auto amf_set_id = nf_config::require<std::uint16_t>(config, "amf_set_id");
    const auto amf_pointer = nf_config::require<std::uint8_t>(config, "amf_pointer");

    sbi_core::init_metrics(metrics_bind_address);

    const std::string amf_instance_id = sbi_core::generate_uuid_v4();
    spdlog::info("amf: starting, nfInstanceId={}", amf_instance_id);

    sbi_core::http2::TlsConfig server_tls{
        .cert_path = CERTS_DIR "/amf/cert.pem",
        .key_path = CERTS_DIR "/amf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };

    sbi_core::jwt::Verifier verifier(CERTS_DIR "/nrf-jwt/public.pem", kNrfInstanceId);

    amf::UeContextStore ue_contexts;
    amf::ngap::NgapUeRegistry ue_ngap_registry;

    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #100/ADR-0075): real, persistent NAS
    // security context -- see ue_security_context_store.hpp's own header for why this was a
    // real, load-bearing prerequisite for ServiceRequest support. Same real, fail-fast PING
    // discipline every other NF's own Redis connection already uses (e.g. CHF's own).
    auto redis = std::make_shared<sw::redis::Redis>(redis_url);
    redis->ping();
    spdlog::info("amf: connected to Redis/Valkey");
    amf::UeSecurityContextStore ue_security_contexts(redis);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #100, ADR-0090): real cross-association
    // amf_ue_ngap_id -> tmsi index -- see amf_ue_id_index_store.hpp's own header for why
    // PathSwitchRequest needs it. Shares the same Redis connection as ue_security_contexts above.
    amf::AmfUeIdIndexStore amf_ue_id_index(redis);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #100, ADR-0095): real cross-association
    // relay registry a genuine N2-based handover needs -- see gnb_association_registry.hpp's own
    // header for why. In-process only (no Redis backing), matching NgapUeRegistry's own scope:
    // both hold live, non-owning references into associations' own stack frames, meaningless
    // across a restart.
    amf::ngap::GnbAssociationRegistry gnb_associations;
    amf::UeN1N2SubscriptionStore ue_n1n2_subs("n1n2sub-");
    amf::NonUeN2SubscriptionStore non_ue_n2_subs("nonuen2sub-");
    amf::AmfStatusSubscriptionStore amf_status_subs("amfstatussub-");
    // ADR-0199: Namf_EventExposure's two real, separate resource families -- see subscriptions.hpp.
    amf::AmfEventSubscriptionStore amf_event_subs("evtsub-");
    amf::AmfEventSubscriptionStore amf_set_event_subs("setsub-");
    // ADR-0200: Namf_MBSBroadcast's broadcast MBS session contexts.
    amf::MbsBroadcastContextStore mbs_broadcast_contexts("mbsctx-");

    auto meter = sbi_core::get_meter("amf");
    auto release_counter =
        meter->CreateUInt64Counter("amf_release_ue_context_total", "Total ReleaseUEContext calls");
    auto ebi_counter =
        meter->CreateUInt64Counter("amf_ebi_assignment_total", "Total EBIAssignment calls");
    auto transfer_counter = meter->CreateUInt64Counter("amf_ue_context_transfer_total",
                                                       "Total UEContextTransfer calls");
    auto reg_status_counter = meter->CreateUInt64Counter("amf_registration_status_update_total",
                                                         "Total RegistrationStatusUpdate calls");
    auto n1n2_counter = meter->CreateUInt64Counter("amf_n1n2_message_transfer_total",
                                                   "Total N1N2MessageTransfer calls");
    auto non_ue_n2_counter = meter->CreateUInt64Counter("amf_non_ue_n2_message_transfer_total",
                                                        "Total NonUeN2MessageTransfer calls");
    auto create_counter =
        meter->CreateUInt64Counter("amf_create_ue_context_total", "Total CreateUEContext calls");
    auto relocate_counter = meter->CreateUInt64Counter("amf_relocate_ue_context_total",
                                                       "Total RelocateUEContext calls");
    auto cancel_relocate_counter = meter->CreateUInt64Counter(
        "amf_cancel_relocate_ue_context_total", "Total CancelRelocateUEContext calls");
    // ADR-0199: Namf_Location + Namf_EventExposure counters.
    auto provide_pos_info_counter = meter->CreateUInt64Counter(
        "amf_provide_positioning_info_total", "Total ProvidePositioningInfo calls");
    auto provide_loc_info_counter = meter->CreateUInt64Counter("amf_provide_location_info_total",
                                                               "Total ProvideLocationInfo calls");
    auto cancel_location_counter =
        meter->CreateUInt64Counter("amf_cancel_location_total", "Total CancelLocation calls");
    auto event_subscribe_counter = meter->CreateUInt64Counter(
        "amf_event_exposure_subscribe_total", "Total Namf_EventExposure CreateSubscription calls");
    auto set_event_subscribe_counter = meter->CreateUInt64Counter(
        "amf_set_event_exposure_subscribe_total",
        "Total Namf_EventExposure CreateAMFSetLevelBulkSubscription calls");
    // ADR-0200: Namf_AIoT + Namf_MBSBroadcast + Namf_MBSCommunication + Namf_MT counters.
    auto aiot_message_delivery_counter = meter->CreateUInt64Counter(
        "amf_aiot_message_delivery_total", "Total Namf_AIoT MessageDelivery calls");
    auto mbs_context_create_counter = meter->CreateUInt64Counter(
        "amf_mbs_broadcast_context_create_total", "Total Namf_MBSBroadcast ContextCreate calls");
    auto mbs_context_delete_counter = meter->CreateUInt64Counter(
        "amf_mbs_broadcast_context_delete_total", "Total Namf_MBSBroadcast ContextDelete calls");
    auto mbs_context_update_counter = meter->CreateUInt64Counter(
        "amf_mbs_broadcast_context_update_total", "Total Namf_MBSBroadcast ContextUpdate calls");
    auto mbs_n2_message_transfer_counter = meter->CreateUInt64Counter(
        "amf_mbs_n2_message_transfer_total", "Total Namf_MBSCommunication N2MessageTransfer calls");
    auto mt_domain_selection_counter =
        meter->CreateUInt64Counter("amf_mt_provide_domain_selection_info_total",
                                   "Total Namf_MT Provide Domain Selection Info calls");
    auto mt_enable_reachability_counter = meter->CreateUInt64Counter(
        "amf_mt_enable_ue_reachability_total", "Total Namf_MT EnableUeReachability calls");
    auto mt_enable_group_reachability_counter = meter->CreateUInt64Counter(
        "amf_mt_enable_group_reachability_total", "Total Namf_MT EnableGroupReachability calls");

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

    server.add_route(
        "PUT",
        std::string(kApiRoot) + "/ue-contexts/{ueContextId}",
        [&verifier, &ue_contexts, &create_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = parse_multipart_json_body<sbi_gen::UeContextCreateData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_context_id = req.path_params.at("ueContextId");
            // Disclosed simplification: stores only that a context now exists, not the source
            // AMF's full UeContextCreateData -- this lab has one AMF and no real inter-AMF
            // mobility/handover logic, so nothing downstream needs that detail (see file header).
            ue_contexts.put(ue_context_id, json{{"ueContextId", ue_context_id}});
            create_counter->Add(1);

            // Disclosed simplification: targetToSourceData.ngapData.contentId is a placeholder,
            // not a reference to any real attached binary part -- no NGAP stack exists in this
            // build to produce real N2 content for it to point at.
            sbi_gen::UeContextCreatedData resp_data;
            resp_data.ueContext = sbi_gen::UeContext{};
            resp_data.targetToSourceData.ngapData.contentId = "stub-n2-content";
            resp_data.pduSessionList = {};
            json j = resp_data;
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kApiRoot) + "/ue-contexts/" + ue_context_id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/ue-contexts/{ueContextId}/release",
        [&verifier, &ue_contexts, &release_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = parse_json_body<sbi_gen::UEContextRelease>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_context_id = req.path_params.at("ueContextId");
            if (!ue_contexts.get(ue_context_id).has_value()) {
                return problem_response(404, "Not Found", "No UE context with id " + ue_context_id);
            }
            ue_contexts.remove(ue_context_id);
            release_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/ue-contexts/{ueContextId}/assign-ebi",
        [&verifier, &ue_contexts, &ebi_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = parse_json_body<sbi_gen::AssignEbiData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_context_id = req.path_params.at("ueContextId");
            if (!ue_contexts.get(ue_context_id).has_value()) {
                return problem_response(404, "Not Found", "No UE context with id " + ue_context_id);
            }
            ebi_counter->Add(1);
            // Disclosed simplification: no real EPS Bearer Id pool -- always reports zero newly
            // assigned bearers rather than fabricating EBI values with no real bearer behind them.
            sbi_gen::AssignedEbiData resp_data;
            resp_data.pduSessionId = body->pduSessionId;
            resp_data.assignedEbiList = {};
            json j = resp_data;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/ue-contexts/{ueContextId}/transfer",
        [&verifier, &ue_contexts, &transfer_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = parse_json_body<sbi_gen::UeContextTransferReqData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_context_id = req.path_params.at("ueContextId");
            if (!ue_contexts.get(ue_context_id).has_value()) {
                return problem_response(404, "Not Found", "No UE context with id " + ue_context_id);
            }
            transfer_counter->Add(1);
            // Unreachable today (see file header): if it ever runs, only the mandatory
            // ueContext field is populated (all-optional-empty is schema-valid) -- there is no
            // real stored UE context content yet to transfer.
            sbi_gen::UeContextTransferRspData resp_data;
            resp_data.ueContext = sbi_gen::UeContext{};
            json j = resp_data;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/ue-contexts/{ueContextId}/relocate",
        [&verifier, &ue_contexts, &relocate_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = parse_multipart_json_body<sbi_gen::UeContextRelocateData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_context_id = req.path_params.at("ueContextId");
            if (!ue_contexts.get(ue_context_id).has_value()) {
                return problem_response(404, "Not Found", "No UE context with id " + ue_context_id);
            }
            relocate_counter->Add(1);
            sbi_gen::UeContextRelocatedData resp_data;
            resp_data.ueContext = sbi_gen::UeContext{};
            json j = resp_data;
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace(
                "location", std::string(kApiRoot) + "/ue-contexts/" + ue_context_id + "/relocate");
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/ue-contexts/{ueContextId}/cancel-relocate",
        [&verifier, &ue_contexts, &cancel_relocate_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = parse_multipart_json_body<sbi_gen::UeContextCancelRelocateData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_context_id = req.path_params.at("ueContextId");
            if (!ue_contexts.get(ue_context_id).has_value()) {
                return problem_response(404, "Not Found", "No UE context with id " + ue_context_id);
            }
            cancel_relocate_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/ue-contexts/{ueContextId}/transfer-update",
        [&verifier, &ue_contexts, &reg_status_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = parse_json_body<sbi_gen::UeRegStatusUpdateReqData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_context_id = req.path_params.at("ueContextId");
            if (!ue_contexts.get(ue_context_id).has_value()) {
                return problem_response(404, "Not Found", "No UE context with id " + ue_context_id);
            }
            reg_status_counter->Add(1);
            sbi_gen::UeRegStatusUpdateRspData resp_data;
            resp_data.regStatusTransferComplete = true;
            json j = resp_data;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/ue-contexts/{ueContextId}/n1-n2-messages",
        [&verifier, &ue_contexts, &ue_ngap_registry, &n1n2_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_context_id = req.path_params.at("ueContextId");
            if (!ue_contexts.get(ue_context_id).has_value()) {
                return problem_response(404, "Not Found", "No UE context with id " + ue_context_id);
            }

            // Real delivery (ADR-0038) requires a binary N1 message, i.e. multipart/related --
            // the plain application/json alternative the schema also allows (e.g. an N2-only
            // transfer, or a notification with no NAS content) isn't handled by this build, which
            // has no N2 SM info source without a real UPF/N4 (Phase 3) anyway. Disclosed scope
            // narrowing, not silently dropped: rejected with 400, not misinterpreted as JSON.
            sbi_core::http2::Response err;
            auto body = parse_multipart_json_body<sbi_gen::N1N2MessageTransferReqData>(req, err);
            if (!body.has_value()) {
                return err;
            }

            std::optional<std::vector<std::uint8_t>> n1_bytes;
            if (body->n1MessageContainer.has_value()) {
                const auto content_type_it = req.headers.find("content-type");
                if (content_type_it != req.headers.end()) {
                    if (auto parts = sbi_core::multipart::parse(content_type_it->second, req.body);
                        parts.has_value()) {
                        for (const auto& part : *parts) {
                            if (part.content_id.has_value() &&
                                *part.content_id ==
                                    body->n1MessageContainer->n1MessageContent.contentId) {
                                n1_bytes.emplace(part.body.begin(), part.body.end());
                                break;
                            }
                        }
                    }
                }
            }
            if (!n1_bytes.has_value()) {
                return problem_response(400,
                                        "Missing binary part",
                                        "n1MessageContainer referenced but its binary part was "
                                        "not found in the multipart body");
            }
            if (!body->pduSessionId.has_value()) {
                return problem_response(
                    400,
                    "Missing mandatory IE",
                    "This build requires pduSessionId to route the N1 message to a PDU session");
            }

            const auto delivered = ue_ngap_registry.send_dl_nas_transport(
                ue_context_id, static_cast<std::uint8_t>(*body->pduSessionId), *n1_bytes);
            if (!delivered) {
                return problem_response(404,
                                        "Not Found",
                                        "No live NGAP association registered for UE context " +
                                            ue_context_id);
            }

            n1n2_counter->Add(1);
            sbi_gen::N1N2MessageTransferRspData resp_data;
            resp_data.cause.value = sbi_gen::N1N2MessageTransferCause::N1_N2_TRANSFER_INITIATED;
            json j = resp_data;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/ue-contexts/{ueContextId}/n1-n2-messages/subscriptions",
        [&verifier, &ue_n1n2_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = parse_json_body<sbi_gen::UeN1N2InfoSubscriptionCreateData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_context_id = req.path_params.at("ueContextId");
            // Implementation choice, disclosed: NOT gated on the UE context already existing --
            // the schema doesn't mandate that precondition, and gating on it would make this
            // operation permanently untestable until CreateUEContext lands (see file header).
            const auto id = ue_n1n2_subs.create(
                amf::UeN1N2Subscription{.ue_context_id = ue_context_id, .data = *body});
            sbi_gen::UeN1N2InfoSubscriptionCreatedData resp_data;
            resp_data.n1n2NotifySubscriptionId = id;
            json j = resp_data;
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kApiRoot) + "/ue-contexts/" + ue_context_id +
                                     "/n1-n2-messages/subscriptions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route("DELETE",
                     std::string(kApiRoot) +
                         "/ue-contexts/{ueContextId}/n1-n2-messages/subscriptions/{subscriptionId}",
                     [&verifier, &ue_n1n2_subs](const sbi_core::http2::Request& req) {
                         if (auto auth = check_bearer(req, verifier);
                             auth.has_value() && !auth->valid) {
                             return problem_response(401, "Unauthorized", auth->error);
                         }
                         const auto ue_context_id = req.path_params.at("ueContextId");
                         const auto sub_id = req.path_params.at("subscriptionId");
                         auto existing = ue_n1n2_subs.get(sub_id);
                         if (!existing.has_value() || existing->ue_context_id != ue_context_id) {
                             return problem_response(404, "Not Found", "No such N1N2 subscription");
                         }
                         ue_n1n2_subs.remove(sub_id);
                         sbi_core::http2::Response resp;
                         resp.status = 204;
                         return resp;
                     });

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/non-ue-n2-messages/transfer",
        [&verifier, &non_ue_n2_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = parse_json_body<sbi_gen::N2InformationTransferReqData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            non_ue_n2_counter->Add(1);
            // Same "no real N2 delivery pipeline yet" disclosed simplification as
            // N1N2MessageTransfer above.
            sbi_gen::N2InformationTransferRspData resp_data;
            resp_data.result.value =
                sbi_gen::N2InformationTransferResult::N2_INFO_TRANSFER_INITIATED;
            json j = resp_data;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/non-ue-n2-messages/subscriptions",
        [&verifier, &non_ue_n2_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = parse_json_body<sbi_gen::NonUeN2InfoSubscriptionCreateData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto id = non_ue_n2_subs.create(*body);
            sbi_gen::NonUeN2InfoSubscriptionCreatedData resp_data;
            resp_data.n2NotifySubscriptionId = id;
            resp_data.n2InformationClass = body->n2InformationClass;
            json j = resp_data;
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kApiRoot) + "/non-ue-n2-messages/subscriptions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "DELETE",
        std::string(kApiRoot) + "/non-ue-n2-messages/subscriptions/{n2NotifySubscriptionId}",
        [&verifier, &non_ue_n2_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            const auto sub_id = req.path_params.at("n2NotifySubscriptionId");
            if (!non_ue_n2_subs.get(sub_id).has_value()) {
                return problem_response(404, "Not Found", "No such non-UE N2 subscription");
            }
            non_ue_n2_subs.remove(sub_id);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/subscriptions",
        [&verifier, &amf_status_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = parse_json_body<sbi_gen::SubscriptionData_Namf_Communication>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto id = amf_status_subs.create(*body);
            json j = *body; // Response body schema is SubscriptionData itself -- no id field in
                            // it (unlike NRF's SubscriptionData); id travels only via Location.
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location", std::string(kApiRoot) + "/subscriptions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "DELETE",
        std::string(kApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &amf_status_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            const auto sub_id = req.path_params.at("subscriptionId");
            if (!amf_status_subs.get(sub_id).has_value()) {
                return problem_response(404, "Not Found", "No such AMF status subscription");
            }
            amf_status_subs.remove(sub_id);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "PUT",
        std::string(kApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &amf_status_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = parse_json_body<sbi_gen::SubscriptionData_Namf_Communication>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto sub_id = req.path_params.at("subscriptionId");
            if (!amf_status_subs.update(sub_id, *body)) {
                return problem_response(404, "Not Found", "No such AMF status subscription");
            }
            json j = *body;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    // ---- TS29518_Namf_Location.yaml (ADR-0199) ----

    server.add_route("POST",
                     std::string(kLocationApiRoot) + "/{ueContextId}/provide-pos-info",
                     [&verifier, &provide_pos_info_counter](const sbi_core::http2::Request& req) {
                         if (auto auth = check_bearer(req, verifier);
                             auth.has_value() && !auth->valid) {
                             return problem_response(401, "Unauthorized", auth->error);
                         }
                         sbi_core::http2::Response err;
                         auto body = parse_json_body<sbi_gen::RequestPosInfo>(req, err);
                         if (!body.has_value()) {
                             return err;
                         }
                         provide_pos_info_counter->Add(1);
                         // Disclosed gap (same class as LMF's DetermineLocation, ADR-0191): no real
                         // LPP/GNSS/PRU positioning capability exists anywhere in this build to
                         // answer this request from.
                         return problem_response(
                             501,
                             "Not Implemented",
                             "No positioning capability (LPP/GNSS/PRU) implemented in this build");
                     });

    server.add_route(
        "POST",
        std::string(kLocationApiRoot) + "/{ueContextId}/provide-loc-info",
        [&verifier, &ue_contexts, &provide_loc_info_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = parse_json_body<sbi_gen::RequestLocInfo>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_context_id = req.path_params.at("ueContextId");
            if (!ue_contexts.get(ue_context_id).has_value()) {
                return problem_response(404, "Not Found", "No UE context with id " + ue_context_id);
            }
            provide_loc_info_counter->Add(1);
            // Disclosed simplification: an honestly-empty ProvideLocInfo -- UeContextStore only
            // ever persists {"ueContextId": ...} (see CreateUEContext above), so there is no real
            // RAT-type/location/timezone data anywhere in this AMF to populate the response from.
            sbi_gen::ProvideLocInfo resp_data;
            json j = resp_data;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "POST",
        std::string(kLocationApiRoot) + "/{ueContextId}/cancel-pos-info",
        [&verifier, &cancel_location_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = parse_json_body<sbi_gen::CancelPosInfo>(req, err);
            if (!body.has_value()) {
                return err;
            }
            cancel_location_counter->Add(1);
            // Disclosed: ProvidePositioningInfo above always 501s, so no real ldrReference is ever
            // issued -- no cancellation request can ever match one. Same precedent as LMF/GMLC's
            // own cancel-path gaps.
            return problem_response(
                404, "Not Found", "No positioning request with ldrReference " + body->ldrReference);
        });

    // ---- TS29518_Namf_EventExposure.yaml (ADR-0199) ----

    server.add_route(
        "POST",
        std::string(kEventExposureApiRoot) + "/subscriptions",
        [&verifier, &amf_event_subs, &event_subscribe_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = parse_json_body<sbi_gen::AmfCreateEventSubscription>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto id = amf_event_subs.create(body->subscription);
            event_subscribe_counter->Add(1);
            sbi_gen::AmfCreatedEventSubscription resp_data;
            resp_data.subscription = body->subscription;
            resp_data.subscriptionId = id;
            json j = resp_data;
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kEventExposureApiRoot) + "/subscriptions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "PATCH",
        std::string(kEventExposureApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &amf_event_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            const auto sub_id = req.path_params.at("subscriptionId");
            auto existing = amf_event_subs.get(sub_id);
            if (!existing.has_value()) {
                return problem_response(404, "Not Found", "No such event subscription");
            }
            sbi_core::http2::Response err;
            json patch_doc;
            try {
                patch_doc = json::parse(req.body);
            } catch (const std::exception& e) {
                return problem_response(
                    400, "Bad Request", std::string("Invalid JSON: ") + e.what());
            }
            json current = *existing;
            json patched;
            try {
                patched = current.patch(patch_doc);
            } catch (const std::exception& e) {
                return problem_response(
                    400, "Bad Request", std::string("Invalid JSON Patch: ") + e.what());
            }
            sbi_gen::AmfEventSubscription updated;
            try {
                updated = patched.get<sbi_gen::AmfEventSubscription>();
            } catch (const std::exception& e) {
                return problem_response(
                    400, "Bad Request", std::string("Patched document invalid: ") + e.what());
            }
            amf_event_subs.update(sub_id, updated);
            sbi_gen::AmfUpdatedEventSubscription resp_data;
            resp_data.subscription = updated;
            json j = resp_data;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "DELETE",
        std::string(kEventExposureApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &amf_event_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            const auto sub_id = req.path_params.at("subscriptionId");
            if (!amf_event_subs.get(sub_id).has_value()) {
                return problem_response(404, "Not Found", "No such event subscription");
            }
            amf_event_subs.remove(sub_id);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "POST",
        std::string(kEventExposureApiRoot) + "/set-subscriptions",
        [&verifier, &amf_set_event_subs, &set_event_subscribe_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = parse_json_body<sbi_gen::AmfCreateEventSubscription>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto id = amf_set_event_subs.create(body->subscription);
            set_event_subscribe_counter->Add(1);
            sbi_gen::AmfCreatedEventSubscription resp_data;
            resp_data.subscription = body->subscription;
            resp_data.subscriptionId = id;
            json j = resp_data;
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kEventExposureApiRoot) + "/set-subscriptions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "PATCH",
        std::string(kEventExposureApiRoot) + "/set-subscriptions/{subscriptionId}",
        [&verifier, &amf_set_event_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            const auto sub_id = req.path_params.at("subscriptionId");
            auto existing = amf_set_event_subs.get(sub_id);
            if (!existing.has_value()) {
                return problem_response(
                    404, "Not Found", "No such AMF-Set-level event subscription");
            }
            sbi_core::http2::Response err;
            json patch_doc;
            try {
                patch_doc = json::parse(req.body);
            } catch (const std::exception& e) {
                return problem_response(
                    400, "Bad Request", std::string("Invalid JSON: ") + e.what());
            }
            json current = *existing;
            json patched;
            try {
                patched = current.patch(patch_doc);
            } catch (const std::exception& e) {
                return problem_response(
                    400, "Bad Request", std::string("Invalid JSON Patch: ") + e.what());
            }
            sbi_gen::AmfEventSubscription updated;
            try {
                updated = patched.get<sbi_gen::AmfEventSubscription>();
            } catch (const std::exception& e) {
                return problem_response(
                    400, "Bad Request", std::string("Patched document invalid: ") + e.what());
            }
            amf_set_event_subs.update(sub_id, updated);
            sbi_gen::AmfUpdatedEventSubscription resp_data;
            resp_data.subscription = updated;
            json j = resp_data;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route("DELETE",
                     std::string(kEventExposureApiRoot) + "/set-subscriptions/{subscriptionId}",
                     [&verifier, &amf_set_event_subs](const sbi_core::http2::Request& req) {
                         if (auto auth = check_bearer(req, verifier);
                             auth.has_value() && !auth->valid) {
                             return problem_response(401, "Unauthorized", auth->error);
                         }
                         const auto sub_id = req.path_params.at("subscriptionId");
                         if (!amf_set_event_subs.get(sub_id).has_value()) {
                             return problem_response(
                                 404, "Not Found", "No such AMF-Set-level event subscription");
                         }
                         amf_set_event_subs.remove(sub_id);
                         sbi_core::http2::Response resp;
                         resp.status = 204;
                         return resp;
                     });

    // ---- TS29518_Namf_AIoT.yaml (ADR-0200) ----

    server.add_route(
        "POST",
        std::string(kAiotApiRoot) + "/transfer",
        [&verifier, &aiot_message_delivery_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            std::optional<sbi_gen::AiotMessageReq> body;
            const auto ct_it = req.headers.find("content-type");
            if (ct_it != req.headers.end() &&
                sbi_core::multipart::is_multipart_related(ct_it->second)) {
                body = parse_multipart_json_body<sbi_gen::AiotMessageReq>(req, err);
            } else {
                body = parse_json_body<sbi_gen::AiotMessageReq>(req, err);
            }
            if (!body.has_value()) {
                return err;
            }
            aiot_message_delivery_counter->Add(1);
            // Disclosed gap: this lab has no real Ambient IoT device/NG-RAN stack (TS 23.369 is a
            // Tier 3 R19 feature per CLAUDE.md) -- the message is structurally validated and
            // accepted, but nothing is actually forwarded to a device.
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // ---- TS29518_Namf_MBSCommunication.yaml (ADR-0200) ----

    server.add_route(
        "POST",
        std::string(kMbsCommApiRoot) + "/n2-messages/transfer",
        [&verifier, &mbs_n2_message_transfer_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = parse_multipart_json_body<sbi_gen::MbsN2MessageTransferReqData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            mbs_n2_message_transfer_counter->Add(1);
            // Same "no real N2 delivery pipeline yet" disclosed simplification as
            // NonUeN2MessageTransfer above.
            sbi_gen::MbsN2MessageTransferRspData resp_data;
            resp_data.result.value =
                sbi_gen::N2InformationTransferResult::N2_INFO_TRANSFER_INITIATED;
            json j = resp_data;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    // ---- TS29518_Namf_MBSBroadcast.yaml (ADR-0200) ----

    server.add_route(
        "POST",
        std::string(kMbsBroadcastApiRoot) + "/mbs-contexts",
        [&verifier, &mbs_broadcast_contexts, &mbs_context_create_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = parse_multipart_json_body<sbi_gen::ContextCreateReqData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto id = mbs_broadcast_contexts.create(*body);
            mbs_context_create_counter->Add(1);
            sbi_gen::ContextCreateRspData resp_data;
            resp_data.mbsSessionId = body->mbsSessionId;
            json j = resp_data;
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kMbsBroadcastApiRoot) + "/mbs-contexts/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "DELETE",
        std::string(kMbsBroadcastApiRoot) + "/mbs-contexts/{mbsContextRef}",
        [&verifier, &mbs_broadcast_contexts, &mbs_context_delete_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            const auto ctx_ref = req.path_params.at("mbsContextRef");
            if (!mbs_broadcast_contexts.get(ctx_ref).has_value()) {
                return problem_response(404, "Not Found", "No such MBS broadcast context");
            }
            mbs_broadcast_contexts.remove(ctx_ref);
            mbs_context_delete_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "POST",
        std::string(kMbsBroadcastApiRoot) + "/mbs-contexts/{mbsContextRef}/update",
        [&verifier, &mbs_broadcast_contexts, &mbs_context_update_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            const auto ctx_ref = req.path_params.at("mbsContextRef");
            if (!mbs_broadcast_contexts.get(ctx_ref).has_value()) {
                return problem_response(404, "Not Found", "No such MBS broadcast context");
            }
            sbi_core::http2::Response err;
            auto body = parse_multipart_json_body<sbi_gen::ContextUpdateReqData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            mbs_context_update_counter->Add(1);
            // Disclosed simplification: the update body's fields are validated but not merged
            // into stored state -- same as ContextCreate, there is no real N2 consumer downstream
            // that would ever read the merged result in this lab build.
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // ---- TS29518_Namf_MT.yaml (ADR-0200) ----

    server.add_route(
        "GET",
        std::string(kMtApiRoot) + "/ue-contexts/{ueContextId}",
        [&verifier, &ue_contexts, &mt_domain_selection_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_context_id = req.path_params.at("ueContextId");
            if (!ue_contexts.get(ue_context_id).has_value()) {
                return problem_response(404, "Not Found", "No UE context with id " + ue_context_id);
            }
            mt_domain_selection_counter->Add(1);
            // Disclosed simplification: honestly-empty UeContextInfo -- same "UeContextStore only
            // ever persists {ueContextId}" gap as Namf_Location's ProvideLocationInfo above.
            sbi_gen::UeContextInfo resp_data;
            json j = resp_data;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "PUT",
        std::string(kMtApiRoot) + "/ue-contexts/{ueContextId}/ue-reachind",
        [&verifier, &ue_contexts, &mt_enable_reachability_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = parse_json_body<sbi_gen::EnableUeReachabilityReqData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_context_id = req.path_params.at("ueContextId");
            if (!ue_contexts.get(ue_context_id).has_value()) {
                return problem_response(404, "Not Found", "No UE context with id " + ue_context_id);
            }
            mt_enable_reachability_counter->Add(1);
            // Disclosed simplification: real ack (echoes the requested reachability back), but no
            // real paging/reachability state machine exists to actually change UE reachability.
            sbi_gen::EnableUeReachabilityRspData resp_data;
            resp_data.reachability = body->reachability;
            json j = resp_data;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "POST",
        std::string(kMtApiRoot) + "/ue-contexts/enable-group-reachability",
        [&verifier, &mt_enable_group_reachability_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = parse_json_body<sbi_gen::EnableGroupReachabilityReqData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            mt_enable_group_reachability_counter->Add(1);
            // Disclosed simplification: honestly-empty ueConnectedList -- no real per-UE
            // reachability tracking exists to populate it from.
            sbi_gen::EnableGroupReachabilityRspData resp_data;
            json j = resp_data;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    std::thread(run_nrf_lifecycle, amf_instance_id, nrf_base, advertised_ipv4).detach();

    // LI IRI-POI (ADR-0377), disabled unless config/amf.json's li_poi.enabled is true. Lives for
    // the process lifetime (main blocks in run_multi_threaded below), so run_ngap_lifecycle's
    // detached thread may hold a raw pointer to it. nullptr keeps the AMF's behaviour unchanged.
    std::unique_ptr<amf::LiPoi> li_poi;
    if (config.contains("li_poi") && config.at("li_poi").value("enabled", false)) {
        const auto& lp = config.at("li_poi");
        amf::LiPoi::Config poi_cfg;
        poi_cfg.x1_bind_address = lp.value("x1_bind_address", std::string{"0.0.0.0"});
        poi_cfg.x1_port = lp.at("x1_port").get<std::uint16_t>();
        poi_cfg.ne_identifier = lp.value("ne_identifier", std::string{"amf-poi"});
        poi_cfg.network_function_id = lp.value("network_function_id", amf_instance_id);
        poi_cfg.interception_point_id =
            lp.value("interception_point_id", std::string{"AMF-IRI-POI-1"});
        poi_cfg.mdf2_host = lp.at("mdf2_host").get<std::string>();
        poi_cfg.mdf2_port = lp.at("mdf2_port").get<std::uint16_t>();
        poi_cfg.mdf2_sni = lp.value("mdf2_sni", std::string{});
        poi_cfg.cert_path = CERTS_DIR "/amf/cert.pem";
        poi_cfg.key_path = CERTS_DIR "/amf/key.pem";
        poi_cfg.ca_path = CERTS_DIR "/ca/ca.crt";
        poi_cfg.x1_keepalive_p1_seconds = lp.value("x1_keepalive_p1_seconds", 60);
        poi_cfg.x1_keepalive_p2_seconds = lp.value("x1_keepalive_p2_seconds", 180);
        poi_cfg.x1_keepalive_p3_seconds = lp.value("x1_keepalive_p3_seconds", 300);
        poi_cfg.x1_allow_deactivate_all = lp.value("x1_allow_deactivate_all", true);
        li_poi = std::make_unique<amf::LiPoi>(std::move(poi_cfg));
        li_poi->start();
    }
    // NGAP/N2 (SCTP), its own dedicated thread -- see docs/DECISIONS.md ADR-0030/ADR-0031.
    // ngap_bind_address/ngap_bind_port default (config/amf.json) to 127.0.0.5:38412, matching
    // simulators/ransim/config/gnb.yaml's pre-agreed AMF target exactly (ADR-0016) -- now a real
    // config value (ADR-0077), not an in-source literal.
    std::thread(amf::ngap::run_ngap_lifecycle,
                ngap_bind_address,
                ngap_bind_port,
                amf_instance_id,
                nrf_base,
                peers,
                std::ref(ue_contexts),
                std::ref(ue_ngap_registry),
                std::ref(ue_security_contexts),
                std::ref(amf_ue_id_index),
                std::ref(gnb_associations),
                amf_region_id,
                amf_set_id,
                amf_pointer,
                li_poi.get())
        .detach();

    server.start();
    spdlog::info("amf: listening on https://0.0.0.0:{} (TLS 1.3 + mTLS)", port);
    spdlog::info("amf: Prometheus metrics at http://{}/metrics", metrics_bind_address);
    sbi_core::run_multi_threaded(ioc);
    return 0;
}
