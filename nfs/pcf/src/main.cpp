// nfs/pcf: PCF (Policy Control Function), Npcf_AMPolicyControl + Npcf_SMPolicyControl services.
// Source: specs/5G_APIs-REL-19/TS29507_Npcf_AMPolicyControl.yaml,
// TS29512_Npcf_SMPolicyControl.yaml (commit bca84b60a37773133bcae97e5c6c0d10a93b47b6). Phase 2's
// seventh and final originally-scoped NF (PROMPT.md/CLAUDE.md order:
// NRF -> AMF -> SMF -> UDM -> UDR -> AUSF -> PCF).
//
// In scope, agreed with the user before implementation -- the two services CLAUDE.md's Phase 2
// end goal actually needs (UE registration -> AMF gets AM policy from PCF; PDU session
// establishment -> SMF gets SM policy from PCF):
// Npcf_AMPolicyControl -- CreateIndividualAMPolicyAssociation, ReadIndividualAMPolicyAssociation,
// DeleteIndividualAMPolicyAssociation, ReportObservedEventTriggersForIndividualAMPolicyAssociation.
// Npcf_SMPolicyControl -- CreateSMPolicy, GetSMPolicy, UpdateSMPolicy, DeleteSMPolicy.
//
// Deliberately deferred, not dropped: both services' callback notifications (PolicyUpdate/
// TerminationNotification pushed BY PCF TO the notificationUri AMF/SMF supplied) -- neither AMF
// nor SMF has a receiver for these yet, same shape as every other proactive/callback flow this
// build has deferred so far. Npcf_UEPolicyControl (URSP), Npcf_EventExposure,
// Npcf_BDTPolicyControl, Npcf_PDTQPolicyControl, Npcf_AMPolicyAuthorization,
// Npcf_MBSPolicyControl/Authorization -- separate PCF sub-services, not needed for the core
// registration/PDU-session flows. Also deferred: actually wiring AMF/SMF to call this PCF -- this
// turn stands up PCF's own API surface + tests standalone, same precedent as UDR's turn
// (ADR-0025) and UDM's Nudm_UEAU turn (ADR-0026) before AUSF called it -- a deliberate future turn
// touching already-committed AMF/SMF code, reviewable on its own. See ADR-0028.
//
// UPDATE (ADR-0080, gap-closure task #103): Npcf_PolicyAuthorization (specs/5G_APIs-REL-19/
// TS29514_Npcf_PolicyAuthorization.yaml) added -- the real AF/IMS-facing interface an IMS AS
// (P-CSCF/VoNR call setup) uses to request media/QoS policy authorization, flagged by
// docs/CAPABILITY_GAP_ANALYSIS.md as a real, high-impact gap both free5GC and open5GS implement.
// PostAppSessions/GetAppSession/ModAppSession/DeleteAppSession/updateEventsSubsc/
// DeleteEventsSubsc/PcscfRestoration all implemented, route-for-route. Real, disclosed
// simplification, same category as the AM/SM policy defaults above: this lab has no real
// PCC-rule/session-rule engine to actually authorize a requested media flow against, so
// CreateAppSession stores the real request and returns a schema-correct AppSessionContext with NO
// ServAuthInfo failure code set -- per the real spec, ServAuthInfo only enumerates FAILURE reasons
// (TP_NOT_KNOWN, TP_EXPIRED, ...); there is no "AUTHORIZED" value, so an absent servAuthInfo IS
// the real, correct "authorized" outcome, not a fabricated approval decision. No real trigger
// exists in this lab for PcscfRestoration's own real use case (a P-CSCF actually restoring and
// needing to terminate stale App Session Contexts), so it acknowledges (204) without any real
// per-UE inventory to search -- same disclosed shape as this file's other "no real trigger source
// yet" gaps. AF-pushed callback notifications (eventNotification/terminationRequest) are, like
// PolicyUpdate/TerminationNotification above, deferred -- no receiver exists on the AF side in
// this lab.
//
// Disclosed simplification, stated up front: real PCF policy decisions are computed from
// subscriber data UDR would hold (Npcf's own UDR client for the policy-data group), which UDR's
// turn (ADR-0025) never implemented (UDR's provisioned-data group is GET-only with nothing to
// provision anyway). So PCF's policy responses here are schema-valid, real objects built from the
// request plus a small fixed default policy (5QI 9 non-GBR, a placeholder ARP priority level not
// sourced from any TS 23.501 table, a fixed 1 Gbps/1 Gbps session AMBR when the request doesn't
// supply one) -- not real subscriber-specific decisioning. Same category of gap as UDM's SDM stub.
//
// UPDATE (ADR-0072, gap-closure: real N28 end-to-end): CreateSMPolicy now does one real piece of
// subscriber-specific decisioning -- fetches the subscriber's real SmPolicyData from UDR
// (TS29519_Policy_Data.yaml), and if `subscSpendingLimits` is real+true for the request's own
// S-NSSAI+DNN, opens a real Nchf_SpendingLimitControl subscription with CHF (real "CHF hosts, PCF
// subscribes" architecture, TS29594) and tracks it so DeleteSMPolicy can unsubscribe and so a real
// statusNotification callback (`/sm-policies/{smPolicyId}/spending-limit-notify`, this project's
// own chosen notifUri path) can receive later CHF-pushed updates. Real, disclosed non-scope: the
// pushed spending-limit status does NOT yet drive any automated PCC/session-rule change -- 3GPP
// itself leaves that mapping operator-defined (PolicyCounterInfo.currentStatus is explicitly a
// free-form, unspecified string per the real spec text), so inventing that business logic here
// would mean fabricating a rule no spec or user decision actually named.
//
// UPDATE (ADR-0204, gap-closure task #163, first PCF slice): Npcf_UEPolicyControl (TS29525,
// CreateIndividualUEPolicyAssociation/ReadIndividualUEPolicyAssociation/
// DeleteIndividualUEPolicyAssociation/ReportObservedEventTriggersForIndividualUEPolicyAssociation)
// and Npcf_EventExposure (TS29523, PostPcEventExposureSubsc/GetPcEventExposureSubsc/
// PutPcEventExposureSubsc/DeletePcEventExposureSubsc) added -- both real Tier-A gaps found by
// ADR-0193's audit, previously named as deferred above and never wired. Disclosed: no real URSP
// (UE Route Selection Policy) or ANDSP generation logic exists in this build, so
// ReportObservedEventTriggersForIndividualUEPolicyAssociation's real, structurally-valid
// PolicyUpdate response always has an absent `uePolicy` -- same "acknowledge the real report, no
// real policy content to hand back" class of gap as Nudm_NIDDAU/Nudm_SSAU (ADR-0202). No real
// event notification delivery pipeline exists for Npcf_EventExposure either, same disclosed
// non-scope as every other Nnf_EventExposure service in this project (Namf/Nsmf/Nupf). Real name
// collisions found and fixed wiring Npcf_UEPolicyControl in: `PolicyAssociationRequest`,
// `PolicyAssociation`, `PolicyAssociationUpdateRequest`, `PolicyUpdate`, `RequestTrigger` (all
// independently declared by the already-wired TS29507_Npcf_AMPolicyControl.yaml too) --
// disambiguated by the codegen to `*_Npcf_AMPolicyControl`/`*_Npcf_UEPolicyControl`; every
// pre-existing bare-name reference to the AM-Policy-Control variant (this file's own AM Policy
// Association routes above, plus `nfs/amf/src/ngap_task.cpp`'s real PCF client call and
// `tests/integration/test_pcf_policy_control.cpp`) updated to the disambiguated name -- a real,
// necessary fix required by this pilot-set change, not a functional change to AM Policy Control's
// own behavior. `TerminationNotification` also collides three ways
// (AM/SM/UE Policy Control) but was never referenced bare anywhere in this codebase, so nothing to
// fix there.
//
// UPDATE (ADR-0205, gap-closure task #163, second PCF slice): Npcf_AMPolicyAuthorization
// (TS29534, 6 ops: PostAppAmContexts/GetAppAmContext/ModAppAmContext/DeleteAppAmContext/
// updateAmEventsSubsc/DeleteAmEventsSubsc), Npcf_MBSPolicyAuthorization (TS29537, 4 ops:
// CreateMBSAppSessionCtxt/GetMBSAppSessionCtxt/ModifyMBSAppSessionCtxt/DeleteMBSAppSessionCtxt),
// and Npcf_MBSPolicyControl (TS29537, 4 ops: CreateMBSPolicy/GetIndMBSPolicy/DeleteIndMBSPolicy/
// UpdateIndMBSPolicy) added -- 3 of the remaining 5 real Tier-A gaps (2 remain:
// Npcf_PDTQPolicyControl, Npcf_BDTPolicyControl). AppAmContextRespData and AmEventsSubscRespData
// are real anyOf-of-two-full-objects shapes tools/sbi-codegen falls back to an opaque
// nlohmann::json typedef for (see TS26510_CommonData_grp.hpp's own "OPAQUE FALLBACK" comment) --
// routes return the real stored representation directly rather than fabricating a merged shape.
// Real name collision found and fixed wiring Npcf_MBSPolicyAuthorization: MbsExtProblemDetails is
// independently declared by the already-wired TS29521_Nbsf_Management.yaml too (an unrelated real
// BSF schema of the same name) -- disambiguated by the codegen; BSF's own pre-existing bare-name
// reference (nfs/bsf/src/main.cpp) updated, a real necessary fix with no functional change to
// BSF's own behavior. Disclosed: no real MBS PCC-rule/QoS decision engine exists in this build, so
// UpdateIndMBSPolicy's accepted mbsPcrts/mbsErrorReport are structurally validated but never
// applied to any real mbsPolicies decision data, same class of gap as this file's own pre-existing
// SM/AM policy fixed-default disclosure above.
//
// UPDATE (ADR-0206, gap-closure task #163, third and final PCF slice): Npcf_PDTQPolicyControl
// (TS29543, 4 ops: CreatePDTQPolicy/GetIndPDTQPolicy/ModifyIndPDTQPolicy/DeleteIndPDTQPolicy) and
// Npcf_BDTPolicyControl (TS29554, 4 ops: CreateBDTPolicy/GetBDTPolicy/UpdateBDTPolicy/
// DeleteBDTPolicy) added -- the final 2 of PCF's own 7 real Tier-A gaps, closing task #163
// entirely. No new name collisions found wiring either file. Disclosed: no real BDT decision
// engine exists in this build, so CreateBDTPolicy's own created resource never has a real
// `bdtPolData` (the set of transfer policies the PCF offers) -- `BdtPolicyData` itself requires a
// real `bdtRefId` and at least one real `TransferPolicy`, neither of which this build can produce,
// so it's honestly omitted (BdtPolicy's own `bdtPolData` is optional) rather than fabricated.
// UpdateBDTPolicy's own consistent, disclosed consequence: a real `400` if the request tries to
// select a transfer policy (`bdtPolData.selTransPolicyId`), since none were ever offered to
// select from -- real `warnNotifReq`/`energyInd`/`notifUri` merge-patch fields on `bdtReqData`
// still apply for real.

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

#include <chrono>
#include <optional>
#include <thread>

#include "TS26510_CommonData_grp.hpp"
#include "TS29537_Npcf_MBSPolicyAuthorization.hpp"
#include "TS29537_Npcf_MBSPolicyControl.hpp"
#include "stores.hpp"

// docs/DECISIONS.md ADR-0077 -- no hardcoded deployment literal in source.
#include "nf_config/nf_config.hpp"

namespace {

using nlohmann::json;

#ifndef CERTS_DIR
#error "CERTS_DIR must be defined by CMake (see nfs/pcf/CMakeLists.txt)"
#endif
#ifndef CONFIG_DIR
#error "CONFIG_DIR must be defined by CMake (see nfs/pcf/CMakeLists.txt)"
#endif

constexpr const char* kNfType = "PCF";
constexpr const char* kAmApiRoot = "/npcf-am-policy-control/v1";
constexpr const char* kSmApiRoot = "/npcf-smpolicycontrol/v1";
// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #103, ADR-0080).
constexpr const char* kPolicyAuthApiRoot = "/npcf-policyauthorization/v1";
// Gap-closure (ADR-0204, task #163). Real api roots confirmed from each YAML's own `servers:`
// block.
constexpr const char* kUePolicyApiRoot = "/npcf-ue-policy-control/v1";
constexpr const char* kEventExposureApiRoot = "/npcf-eventexposure/v1";
// ADR-0205 (gap-closure task #163, second PCF slice). Real api roots confirmed from each YAML's
// own `servers:` block.
constexpr const char* kAmPolicyAuthApiRoot = "/npcf-am-policyauthorization/v1";
constexpr const char* kMbsPolicyAuthApiRoot = "/npcf-mbspolicyauth/v1";
constexpr const char* kMbsPolicyControlApiRoot = "/npcf-mbspolicycontrol/v1";
// ADR-0206 (gap-closure task #163, third and final PCF slice). Real api roots confirmed from
// each YAML's own `servers:` block.
constexpr const char* kPdtqPolicyControlApiRoot = "/npcf-pdtq-policy-control/v1";
constexpr const char* kBdtPolicyControlApiRoot = "/npcf-bdtpolicycontrol/v1";

// ADR-0072 (gap-closure: real N28 end-to-end). PCF's real UDR client (fetches SmPolicyData,
// TS29519_Policy_Data.yaml) and CHF client (Nchf_SpendingLimitControl, TS29594) -- same
// separate-http2::Client-per-target-NF pattern nfs/udm/src/main.cpp's own udr_client already
// established.
constexpr const char* kUdrApiRoot = "/nudr-dr/v2";
constexpr const char* kChfSpendingLimitApiRoot = "/nchf-spendinglimitcontrol/v1";

// Must match nfs/nrf/src/main.cpp's kNrfInstanceId exactly -- see docs/DECISIONS.md ADR-0018.
constexpr const char* kNrfInstanceId = "5ba9a927-1d31-4c8e-8a10-000000000001";

// Same pattern as every other NF's check_bearer -- see nfs/nrf/src/main.cpp's comment for why a
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

// Builds a default AuthorizedDefaultQos: 5QI 9 (non-GBR, per TS 23.501's 5QI table -- the one
// piece of this sourced from a real spec table) with a fixed ARP. The ARP priorityLevel (8) is an
// arbitrary placeholder, NOT sourced from any TS 23.501 table -- disclosed here, not just in the
// file header, since it's the one field in this default with no spec backing at all.
sbi_gen::AuthorizedDefaultQos default_authorized_qos() {
    sbi_gen::AuthorizedDefaultQos qos{};
    qos.n5qi = 9;
    sbi_gen::Arp arp{};
    arp.priorityLevel = 8;
    arp.preemptCap.value = sbi_gen::PreemptionCapability::NOT_PREEMPT;
    arp.preemptVuln.value = sbi_gen::PreemptionVulnerability::NOT_PREEMPTABLE;
    qos.arp = arp;
    return qos;
}

sbi_gen::Ambr default_session_ambr() {
    sbi_gen::Ambr ambr{};
    ambr.uplink = "1 Gbps";
    ambr.downlink = "1 Gbps";
    return ambr;
}

// Builds the SmPolicyDecision this PCF returns for both CreateSMPolicy and UpdateSMPolicy: one
// default SessionRule, using the request's own subsSessAmbr/subsDefQos when supplied so the
// decision at least reflects what the request actually said, falling back to the fixed defaults
// above otherwise. See file header for the disclosed simplification this represents.
sbi_gen::SmPolicyDecision build_default_decision(const sbi_gen::SmPolicyContextData& context) {
    sbi_gen::SessionRule rule{};
    rule.sessRuleId = "default";
    rule.authSessAmbr = context.subsSessAmbr.value_or(default_session_ambr());
    if (context.subsDefQos.has_value()) {
        sbi_gen::AuthorizedDefaultQos qos{};
        qos.n5qi = context.subsDefQos->n5qi;
        qos.arp = context.subsDefQos->arp;
        rule.authDefQos = qos;
    } else {
        rule.authDefQos = default_authorized_qos();
    }

    sbi_gen::SmPolicyDecision decision{};
    // sessRules is an opaque map (TS29512's additionalProperties-keyed-by-sessRuleId shape isn't
    // representable as a typed map by tools/sbi-codegen -- see nfs/pcf/src/stores.hpp), built by
    // hand as {sessRuleId: SessionRule}.
    decision.sessRules = json{{rule.sessRuleId, json(rule)}};
    decision.online = context.online;
    decision.offline = context.offline;
    decision.suppFeat = context.suppFeat.value_or("");
    return decision;
}

// ADR-0072 (gap-closure: real N28 end-to-end). Real 3GPP text only says "the key of the map is
// the S-NSSAI" for smPolicySnssaiData -- no wire encoding is spec-mandated (checked, not assumed).
// This project's own disclosed, deliberate choice: decimal sst + "-" + sd, used consistently by
// every writer/reader of this key in this project (this file, and the seed data the new N28
// integration test provisions via UDR's real PATCH). Not a claim of interop with any other real
// implementation's own choice, since none is spec-mandated to match against.
std::string snssai_map_key(const sbi_gen::Snssai& snssai) {
    return std::to_string(snssai.sst) + "-" + snssai.sd.value_or("");
}

// Real GET to UDR's `/policy-data/ues/{ueId}/sm-data` (TS29519_Policy_Data.yaml), navigating the
// real nested SmPolicyData -> SmPolicySnssaiData -> SmPolicyDnnData shape down to the one DNN this
// SM Policy request is actually for. Returns std::nullopt on any real failure (UDR unreachable,
// 404, malformed body, or the specific snssai/dnn combination genuinely not provisioned) -- the
// caller treats that as "no spending-limit enforcement for this session", not an error, matching
// this project's own established fail-open convention for this kind of best-effort policy lookup.
std::optional<json> fetch_sm_policy_dnn_data(sbi_core::http2::Client& udr_client,
                                             sbi_core::OAuth2Client& udr_oauth,
                                             const std::string& udr_base,
                                             const std::string& supi,
                                             const sbi_gen::Snssai& snssai,
                                             const std::string& dnn) {
    auto token = udr_oauth.get_bearer_token();
    if (!token.has_value()) {
        spdlog::warn("pcf: OAuth2 token fetch failed for UDR SmPolicyData lookup: {}",
                     token.error());
        return std::nullopt;
    }
    sbi_core::http2::ClientRequest req;
    req.method = "GET";
    req.url = udr_base + kUdrApiRoot + "/policy-data/ues/" + supi + "/sm-data";
    req.headers.emplace("authorization", "Bearer " + *token);
    auto resp = udr_client.send(req);
    if (!resp.has_value() || resp->status != 200) {
        return std::nullopt;
    }
    json doc;
    try {
        doc = json::parse(resp->body);
    } catch (const json::parse_error&) {
        return std::nullopt;
    }
    const auto snssai_key = snssai_map_key(snssai);
    auto snssai_it = doc.find("smPolicySnssaiData");
    if (snssai_it == doc.end() || !snssai_it->contains(snssai_key)) {
        return std::nullopt;
    }
    const auto& snssai_data = (*snssai_it)[snssai_key];
    auto dnn_it = snssai_data.find("smPolicyDnnData");
    if (dnn_it == snssai_data.end() || !dnn_it->contains(dnn)) {
        return std::nullopt;
    }
    return std::make_optional((*dnn_it)[dnn]);
}

// Real POST to CHF's `/nchf-spendinglimitcontrol/v1/subscriptions`
// (TS29594_Nchf_SpendingLimitControl.yaml). Returns the real subscriptionId (parsed from the real
// Location header CHF's own handler sets, matching that handler's own real, confirmed shape) plus
// the real SpendingLimitStatus body. std::nullopt on any failure -- caller treats that as
// "spending-limit tracking unavailable this call", not a reason to fail the whole SM Policy request
// (CHF being unreachable shouldn't block a PDU session from getting service, same fail-open
// reasoning as the UDR lookup above).
std::optional<std::pair<std::string, json>>
subscribe_spending_limit(sbi_core::http2::Client& chf_client,
                         sbi_core::OAuth2Client& chf_oauth,
                         const std::string& chf_base,
                         const std::string& supi,
                         const std::vector<std::string>& policy_counter_ids,
                         const std::string& notif_uri) {
    auto token = chf_oauth.get_bearer_token();
    if (!token.has_value()) {
        spdlog::warn("pcf: OAuth2 token fetch failed for CHF spending-limit subscribe: {}",
                     token.error());
        return std::nullopt;
    }
    sbi_gen::SpendingLimitContext ctx{};
    ctx.supi = supi;
    ctx.policyCounterIds = policy_counter_ids;
    ctx.notifUri = notif_uri;
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = chf_base + kChfSpendingLimitApiRoot + "/subscriptions";
    req.headers.emplace("content-type", "application/json");
    req.headers.emplace("authorization", "Bearer " + *token);
    json j = ctx;
    req.body = j.dump();
    auto resp = chf_client.send(req);
    if (!resp.has_value() || resp->status != 201) {
        spdlog::warn("pcf: CHF spending-limit subscribe failed (status {})",
                     resp.has_value() ? resp->status : -1);
        return std::nullopt;
    }
    const auto location_it = resp->headers.find("location");
    if (location_it == resp->headers.end()) {
        return std::nullopt;
    }
    const auto& location = location_it->second;
    const auto slash = location.find_last_of('/');
    const std::string subscription_id =
        slash == std::string::npos ? location : location.substr(slash + 1);
    json status_body;
    try {
        status_body = json::parse(resp->body);
    } catch (const json::parse_error&) {
        status_body = json::object();
    }
    return std::make_optional(std::make_pair(subscription_id, status_body));
}

// Real DELETE to CHF's `/nchf-spendinglimitcontrol/v1/subscriptions/{subscriptionId}`. Best-effort
// -- a failure here leaves a real, disclosed CHF-side subscription orphaned until its own real
// `expiry` lapses (no cleanup sweep exists in this build); logged, not retried.
void unsubscribe_spending_limit(sbi_core::http2::Client& chf_client,
                                sbi_core::OAuth2Client& chf_oauth,
                                const std::string& chf_base,
                                const std::string& subscription_id) {
    auto token = chf_oauth.get_bearer_token();
    if (!token.has_value()) {
        spdlog::warn("pcf: OAuth2 token fetch failed for CHF spending-limit unsubscribe: {}",
                     token.error());
        return;
    }
    sbi_core::http2::ClientRequest req;
    req.method = "DELETE";
    req.url = chf_base + kChfSpendingLimitApiRoot + "/subscriptions/" + subscription_id;
    req.headers.emplace("authorization", "Bearer " + *token);
    auto resp = chf_client.send(req);
    if (!resp.has_value() || resp->status != 204) {
        spdlog::warn("pcf: CHF spending-limit unsubscribe failed for {} (status {})",
                     subscription_id,
                     resp.has_value() ? resp->status : -1);
    }
}

// Runs on a dedicated thread, never on the server's io_context -- same reasoning as
// nfs/udr/src/main.cpp's run_nrf_lifecycle (docs/DECISIONS.md ADR-0006/ADR-0019).
void run_nrf_lifecycle(const std::string& pcf_instance_id, const std::string& nrf_base) {
    sbi_core::http2::TlsConfig client_tls{
        .cert_path = CERTS_DIR "/pcf/cert.pem",
        .key_path = CERTS_DIR "/pcf/key.pem",
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
        http_client, nrf_base + "/oauth2/token", pcf_instance_id, "nnrf-nfm", "NRF");

    constexpr int kHeartbeatSeconds = 30;
    json profile{
        {"nfInstanceId", pcf_instance_id},
        {"nfType", kNfType},
        {"nfStatus", "REGISTERED"},
        {"ipv4Addresses", json::array({"127.0.0.1"})},
        {"heartBeatTimer", kHeartbeatSeconds},
    };

    while (true) {
        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("pcf: OAuth2 token fetch failed: {}", token.error());
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        sbi_core::http2::ClientRequest put_req;
        put_req.method = "PUT";
        put_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + pcf_instance_id;
        put_req.headers.emplace("content-type", "application/json");
        put_req.headers.emplace("authorization", "Bearer " + *token);
        put_req.headers.emplace(
            sbi_core::headers::kSenderTimestamp,
            sbi_core::headers::format_sender_timestamp(std::chrono::system_clock::now()));
        put_req.body = profile.dump();

        auto put_resp = http_client.send(put_req);
        if (put_resp.has_value() && (put_resp->status == 200 || put_resp->status == 201)) {
            spdlog::info("pcf: registered with NRF (HTTP {})", put_resp->status);
            break;
        }
        spdlog::warn("pcf: NRF registration attempt failed, retrying in 5s");
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(kHeartbeatSeconds / 2));

        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("pcf: OAuth2 token fetch failed for heartbeat: {}", token.error());
            continue;
        }

        sbi_core::http2::ClientRequest patch_req;
        patch_req.method = "PATCH";
        patch_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + pcf_instance_id;
        patch_req.headers.emplace("content-type", "application/json-patch+json");
        patch_req.headers.emplace("authorization", "Bearer " + *token);
        patch_req.body =
            json::array({json{{"op", "replace"}, {"path", "/nfStatus"}, {"value", "REGISTERED"}}})
                .dump();

        auto patch_resp = http_client.send(patch_req);
        if (!patch_resp.has_value() || patch_resp->status != 200) {
            spdlog::warn("pcf: heartbeat failed");
        }
    }
}

// ADR-0286: push a policy change to the SMF that owns this SM policy association.
//
// The action table is operator data (config/pcf.json's `policy_counter_actions`), not code, for
// the reason this file's header already gives: TS 29.594 leaves `currentStatus` a free-form string
// and never enumerates it, so any status->policy rule written in C++ here would be invented. An
// operator (later, a GUI editing the same JSON) owns the mapping; PCF owns the mechanics.
//
// Silent no-op when nothing matches -- a status change with no configured action is a normal
// operational state, not an error.
void notify_smf_of_policy_change(sbi_core::http2::Client& client,
                                 pcf::SmPolicyStore& sm_policies,
                                 const nlohmann::json& policy_counter_actions,
                                 const std::string& sm_policy_id,
                                 const nlohmann::json& status,
                                 opentelemetry::metrics::Counter<std::uint64_t>* push_counter) {
    if (!policy_counter_actions.is_array() || policy_counter_actions.empty()) {
        return;
    }
    // The SmPolicyContextData SMF sent at creation carries the notificationUri SMF is listening on.
    const auto policy = sm_policies.get(sm_policy_id);
    if (!policy.has_value() || !policy->contains("context") ||
        !policy->at("context").contains("notificationUri")) {
        spdlog::warn("pcf: SM policy {} has no notificationUri -- its SMF cannot be told about a "
                     "spending-limit status change",
                     sm_policy_id);
        return;
    }
    const auto notification_uri = policy->at("context").at("notificationUri").get<std::string>();

    // TS 29.594's SpendingLimitStatus carries statusInfos as a map keyed by policyCounterId.
    if (!status.contains("statusInfos") || !status.at("statusInfos").is_object()) {
        return;
    }

    for (const auto& [counter_id, info] : status.at("statusInfos").items()) {
        if (!info.contains("currentStatus")) {
            continue;
        }
        const auto current = info.at("currentStatus").get<std::string>();
        for (const auto& action : policy_counter_actions) {
            if (!action.contains("policyCounterId") || !action.contains("currentStatus") ||
                !action.contains("smPolicyDecision")) {
                continue;
            }
            if (action.at("policyCounterId").get<std::string>() != counter_id ||
                action.at("currentStatus").get<std::string>() != current) {
                continue;
            }

            sbi_gen::SmPolicyNotification notification{};
            notification.resourceUri = "/npcf-smpolicycontrol/v1/sm-policies/" + sm_policy_id;
            try {
                notification.smPolicyDecision =
                    action.at("smPolicyDecision").get<sbi_gen::SmPolicyDecision>();
            } catch (const nlohmann::json::exception& e) {
                spdlog::error("pcf: policy_counter_actions entry for counter {} status {} is not a "
                              "valid SmPolicyDecision: {}",
                              counter_id,
                              current,
                              e.what());
                continue;
            }

            sbi_core::http2::ClientRequest req;
            req.method = "POST";
            req.url = notification_uri;
            req.headers.emplace("content-type", "application/json");
            req.body = nlohmann::json(notification).dump();
            auto resp = client.send(req);
            if (!resp.has_value() || (resp->status != 204 && resp->status != 200)) {
                spdlog::warn("pcf: pushing the policy change for counter {} ({}) to {} failed",
                             counter_id,
                             current,
                             notification_uri);
                continue;
            }
            if (push_counter != nullptr) {
                push_counter->Add(1);
            }
            spdlog::info("pcf: policy counter {} is now {} -- pushed the operator-configured "
                         "SmPolicyDecision to {}",
                         counter_id,
                         current,
                         notification_uri);
        }
    }
}

} // namespace

int main() {
    const auto config = nf_config::load("pcf", CONFIG_DIR);
    const auto port = nf_config::require<unsigned short>(config, "port");
    const auto metrics_bind_address =
        nf_config::require<std::string>(config, "metrics_bind_address");
    const auto sbi_bind_address = nf_config::require<std::string>(config, "sbi_bind_address");
    const auto nrf_base_url =
        nf_config::require<std::string>(config, "nrf_base_url", "PCF_NRF_BASE_URL");
    const auto udr_base_url =
        nf_config::require<std::string>(config, "udr_base_url", "PCF_UDR_BASE_URL");
    const auto chf_base_url =
        nf_config::require<std::string>(config, "chf_base_url", "PCF_CHF_BASE_URL");
    const auto self_base_url =
        nf_config::require<std::string>(config, "self_base_url", "PCF_SELF_BASE_URL");

    sbi_core::init_logging("pcf");
    sbi_core::init_tracing("pcf");
    sbi_core::init_metrics(metrics_bind_address);

    const std::string pcf_instance_id = sbi_core::generate_uuid_v4();
    spdlog::info("pcf: starting, nfInstanceId={}", pcf_instance_id);

    sbi_core::http2::TlsConfig server_tls{
        .cert_path = CERTS_DIR "/pcf/cert.pem",
        .key_path = CERTS_DIR "/pcf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };

    sbi_core::jwt::Verifier verifier(CERTS_DIR "/nrf-jwt/public.pem", kNrfInstanceId);

    pcf::AmPolicyStore am_policies;
    pcf::SmPolicyStore sm_policies;

    // ADR-0286: the operator-owned status->policy mapping, and PCF's own client for pushing it.
    const auto policy_counter_actions = config.contains("policy_counter_actions")
                                            ? config.at("policy_counter_actions")
                                            : nlohmann::json::array();
    if (!policy_counter_actions.empty()) {
        spdlog::info("pcf: {} policy-counter action(s) configured -- spending-limit status changes "
                     "will be pushed to the owning SMF",
                     policy_counter_actions.size());
    }
    sbi_core::http2::TlsConfig sm_notify_tls{
        .cert_path = CERTS_DIR "/pcf/cert.pem",
        .key_path = CERTS_DIR "/pcf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client sm_policy_notify_client(std::move(sm_notify_tls));
    pcf::SpendingLimitTrackingStore spending_limit_tracking;
    pcf::AppSessionStore app_sessions;
    pcf::UePolicyStore ue_policies;
    pcf::PcEventExposureStore pc_event_subs;
    pcf::AppAmContextStore app_am_contexts;
    pcf::MbsAppSessionStore mbs_app_sessions;
    pcf::MbsPolicyStore mbs_policies;
    pcf::PdtqPolicyStore pdtq_policies;
    pcf::BdtPolicyStore bdt_policies;

    // ADR-0072 (gap-closure: real N28 end-to-end) -- PCF's own client identity + token source for
    // calling UDR and CHF, same separate-http2::Client-per-target-NF pattern this project already
    // established (nfs/udm/src/main.cpp's own udr_client, nfs/ausf/src/main.cpp's own udm_client).
    sbi_core::http2::TlsConfig udr_client_tls{
        .cert_path = CERTS_DIR "/pcf/cert.pem",
        .key_path = CERTS_DIR "/pcf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client udr_client(std::move(udr_client_tls));
    sbi_core::OAuth2Client udr_oauth(
        udr_client, nrf_base_url + "/oauth2/token", pcf_instance_id, "nudr-dr", "UDR");

    sbi_core::http2::TlsConfig chf_client_tls{
        .cert_path = CERTS_DIR "/pcf/cert.pem",
        .key_path = CERTS_DIR "/pcf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client chf_client(std::move(chf_client_tls));
    sbi_core::OAuth2Client chf_oauth(chf_client,
                                     nrf_base_url + "/oauth2/token",
                                     pcf_instance_id,
                                     "nchf-spendinglimitcontrol",
                                     "CHF");

    auto meter = sbi_core::get_meter("pcf");
    auto am_create_counter = meter->CreateUInt64Counter(
        "pcf_am_policy_create_total", "Total CreateIndividualAMPolicyAssociation calls");
    auto am_update_counter = meter->CreateUInt64Counter(
        "pcf_am_policy_update_total",
        "Total ReportObservedEventTriggersForIndividualAMPolicyAssociation calls");
    auto am_delete_counter = meter->CreateUInt64Counter(
        "pcf_am_policy_delete_total", "Total DeleteIndividualAMPolicyAssociation calls");
    auto sm_create_counter =
        meter->CreateUInt64Counter("pcf_sm_policy_create_total", "Total CreateSMPolicy calls");
    auto sm_update_counter =
        meter->CreateUInt64Counter("pcf_sm_policy_update_total", "Total UpdateSMPolicy calls");
    auto sm_delete_counter =
        meter->CreateUInt64Counter("pcf_sm_policy_delete_total", "Total DeleteSMPolicy calls");
    auto spending_limit_subscribe_counter = meter->CreateUInt64Counter(
        "pcf_spending_limit_subscribe_total",
        "Total real Nchf_SpendingLimitControl subscriptions opened by this PCF");
    // ADR-0286: the N28 chain's observable proof -- a spending-limit change that reached an SMF.
    auto policy_update_push_counter = meter->CreateUInt64Counter(
        "pcf_sm_policy_updates_pushed_total",
        "SmPolicyDecisions pushed to an SMF after a spending-limit status change (N28/Sy)");
    auto spending_limit_notify_counter = meter->CreateUInt64Counter(
        "pcf_spending_limit_notify_total",
        "Total real Nchf_SpendingLimitControl statusNotification callbacks received");
    auto policy_auth_create_counter =
        meter->CreateUInt64Counter("pcf_policy_auth_create_total", "Total PostAppSessions calls");
    auto policy_auth_update_counter =
        meter->CreateUInt64Counter("pcf_policy_auth_update_total", "Total ModAppSession calls");
    auto policy_auth_delete_counter =
        meter->CreateUInt64Counter("pcf_policy_auth_delete_total", "Total DeleteAppSession calls");
    auto ue_policy_create_counter = meter->CreateUInt64Counter(
        "pcf_ue_policy_create_total", "Total CreateIndividualUEPolicyAssociation calls");
    auto ue_policy_update_counter = meter->CreateUInt64Counter(
        "pcf_ue_policy_update_total",
        "Total ReportObservedEventTriggersForIndividualUEPolicyAssociation calls");
    auto ue_policy_delete_counter = meter->CreateUInt64Counter(
        "pcf_ue_policy_delete_total", "Total DeleteIndividualUEPolicyAssociation calls");
    auto ee_create_counter = meter->CreateUInt64Counter("pcf_ee_create_subscription_total",
                                                        "Total PostPcEventExposureSubsc calls");
    auto ee_update_counter = meter->CreateUInt64Counter("pcf_ee_update_subscription_total",
                                                        "Total PutPcEventExposureSubsc calls");
    auto ee_delete_counter = meter->CreateUInt64Counter("pcf_ee_delete_subscription_total",
                                                        "Total DeletePcEventExposureSubsc calls");
    auto am_auth_create_counter =
        meter->CreateUInt64Counter("pcf_am_auth_create_total", "Total PostAppAmContexts calls");
    auto am_auth_update_counter =
        meter->CreateUInt64Counter("pcf_am_auth_update_total", "Total ModAppAmContext calls");
    auto am_auth_delete_counter =
        meter->CreateUInt64Counter("pcf_am_auth_delete_total", "Total DeleteAppAmContext calls");
    auto am_auth_evsubsc_put_counter = meter->CreateUInt64Counter(
        "pcf_am_auth_evsubsc_put_total", "Total updateAmEventsSubsc calls");
    auto am_auth_evsubsc_delete_counter = meter->CreateUInt64Counter(
        "pcf_am_auth_evsubsc_delete_total", "Total DeleteAmEventsSubsc calls");
    auto mbs_auth_create_counter = meter->CreateUInt64Counter(
        "pcf_mbs_auth_create_total", "Total CreateMBSAppSessionCtxt calls");
    auto mbs_auth_update_counter = meter->CreateUInt64Counter(
        "pcf_mbs_auth_update_total", "Total ModifyMBSAppSessionCtxt calls");
    auto mbs_auth_delete_counter = meter->CreateUInt64Counter(
        "pcf_mbs_auth_delete_total", "Total DeleteMBSAppSessionCtxt calls");
    auto mbs_policy_create_counter =
        meter->CreateUInt64Counter("pcf_mbs_policy_create_total", "Total CreateMBSPolicy calls");
    auto mbs_policy_update_counter =
        meter->CreateUInt64Counter("pcf_mbs_policy_update_total", "Total UpdateIndMBSPolicy calls");
    auto mbs_policy_delete_counter =
        meter->CreateUInt64Counter("pcf_mbs_policy_delete_total", "Total DeleteIndMBSPolicy calls");
    auto pdtq_create_counter =
        meter->CreateUInt64Counter("pcf_pdtq_create_total", "Total CreatePDTQPolicy calls");
    auto pdtq_update_counter =
        meter->CreateUInt64Counter("pcf_pdtq_update_total", "Total ModifyIndPDTQPolicy calls");
    auto pdtq_delete_counter =
        meter->CreateUInt64Counter("pcf_pdtq_delete_total", "Total DeleteIndPDTQPolicy calls");
    auto bdt_create_counter =
        meter->CreateUInt64Counter("pcf_bdt_create_total", "Total CreateBDTPolicy calls");
    auto bdt_update_counter =
        meter->CreateUInt64Counter("pcf_bdt_update_total", "Total UpdateBDTPolicy calls");
    auto bdt_delete_counter =
        meter->CreateUInt64Counter("pcf_bdt_delete_total", "Total DeleteBDTPolicy calls");

    boost::asio::io_context ioc;
    // 0.0.0.0: same Docker-reachability reasoning as NRF's bind -- see docs/DECISIONS.md ADR-0014.
    sbi_core::http2::Server server(ioc, sbi_bind_address, port, server_tls);

    // P15 / P4.12 (ADR-0280): optional TPS ceiling from this NF's own config (`max_tps`,
    // `tps_burst`), overridable per deployment via SBI_MAX_TPS. Absent means unlimited, so this
    // changes nothing until an operator opts in.
    if (const auto tps_limit = sbi_core::read_tps_limit(config); tps_limit.enabled()) {
        server.set_tps_limit(tps_limit.sustained_tps, tps_limit.burst);
        spdlog::info("TPS ceiling active: {} req/s sustained, burst {}",
                     tps_limit.sustained_tps,
                     tps_limit.burst > 0.0 ? tps_limit.burst : tps_limit.sustained_tps);
    }

    // --- Npcf_AMPolicyControl ---

    server.add_route(
        "POST",
        std::string(kAmApiRoot) + "/policies",
        [&verifier, &am_policies, &am_create_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<
                sbi_gen::PolicyAssociationRequest_Npcf_AMPolicyControl>(req, err);
            if (!body.has_value()) {
                return err;
            }

            sbi_gen::PolicyAssociation_Npcf_AMPolicyControl association{};
            association.request = *body;
            association.ueAmbr = body->ueAmbr.value_or(default_session_ambr());
            association.suppFeat = body->suppFeat;
            json j = association;
            const auto id = am_policies.create(j);
            am_create_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location", std::string(kAmApiRoot) + "/policies/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAmApiRoot) + "/policies/{polAssoId}",
        [&verifier, &am_policies](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto pol_asso_id = req.path_params.at("polAssoId");
            auto association = am_policies.get(pol_asso_id);
            if (!association.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No AM policy association " + pol_asso_id);
            }
            return sbi_core::http2::Response::json(200, association->dump());
        });

    server.add_route(
        "DELETE",
        std::string(kAmApiRoot) + "/policies/{polAssoId}",
        [&verifier, &am_policies, &am_delete_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto pol_asso_id = req.path_params.at("polAssoId");
            if (!am_policies.remove(pol_asso_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No AM policy association " + pol_asso_id);
            }
            am_delete_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "POST",
        std::string(kAmApiRoot) + "/policies/{polAssoId}/update",
        [&verifier, &am_policies, &am_update_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<
                sbi_gen::PolicyAssociationUpdateRequest_Npcf_AMPolicyControl>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto pol_asso_id = req.path_params.at("polAssoId");
            auto stored = am_policies.get(pol_asso_id);
            if (!stored.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No AM policy association " + pol_asso_id);
            }
            auto association = stored->get<sbi_gen::PolicyAssociation_Npcf_AMPolicyControl>();
            if (body->ueAmbr.has_value()) {
                association.ueAmbr = body->ueAmbr;
            }
            if (body->servAreaRes.has_value()) {
                association.servAreaRes = body->servAreaRes;
            }
            if (body->triggers.has_value()) {
                association.triggers = body->triggers;
            }
            am_policies.put(pol_asso_id, json(association));

            sbi_gen::PolicyUpdate_Npcf_AMPolicyControl update{};
            update.resourceUri = std::string(kAmApiRoot) + "/policies/" + pol_asso_id;
            update.triggers = body->triggers;
            update.servAreaRes = association.servAreaRes;
            update.ueAmbr = association.ueAmbr;
            am_update_counter->Add(1);
            json j = update;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    // --- Npcf_SMPolicyControl ---

    server.add_route(
        "POST",
        std::string(kSmApiRoot) + "/sm-policies",
        [&verifier,
         &sm_policies,
         &sm_create_counter,
         &udr_client,
         &udr_oauth,
         &udr_base_url,
         &chf_client,
         &chf_oauth,
         &chf_base_url,
         &self_base_url,
         &spending_limit_tracking,
         &spending_limit_subscribe_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::SmPolicyContextData>(req, err);
            if (!body.has_value()) {
                return err;
            }

            const auto decision = build_default_decision(*body);
            sbi_gen::SmPolicyControl control{};
            control.context = *body;
            control.policy = decision;
            const auto id = sm_policies.create(json(control));
            sm_create_counter->Add(1);

            // ADR-0072 (gap-closure: real N28 end-to-end). Real, fail-open best-effort: fetches
            // this subscriber's real SmPolicyDnnData from UDR for the request's own S-NSSAI+DNN,
            // and if `subscSpendingLimits` is real+true there, opens a real CHF
            // Nchf_SpendingLimitControl subscription for whichever policyCounterIds are already
            // named in `spendLimInfo`'s own keys (this project's own disclosed choice for "which
            // counters" -- see fetch_sm_policy_dnn_data's own comment; 3GPP leaves the initial
            // counter-selection mechanism unspecified). Neither UDR nor CHF being reachable, nor
            // spending limits simply not being configured for this subscriber, fails the SM Policy
            // request itself -- this is real, additional enforcement, not a mandatory dependency.
            auto dnn_data = fetch_sm_policy_dnn_data(
                udr_client, udr_oauth, udr_base_url, body->supi, body->sliceInfo, body->dnn);
            if (dnn_data.has_value() && dnn_data->value("subscSpendingLimits", false)) {
                std::vector<std::string> policy_counter_ids;
                if (auto spend_it = dnn_data->find("spendLimInfo"); spend_it != dnn_data->end()) {
                    for (const auto& [counter_id, _] : spend_it->items()) {
                        policy_counter_ids.push_back(counter_id);
                    }
                }
                if (!policy_counter_ids.empty()) {
                    const std::string notif_uri = self_base_url + kSmApiRoot + "/sm-policies/" +
                                                  id + "/spending-limit-notify";
                    auto subscribed = subscribe_spending_limit(chf_client,
                                                               chf_oauth,
                                                               chf_base_url,
                                                               body->supi,
                                                               policy_counter_ids,
                                                               notif_uri);
                    if (subscribed.has_value()) {
                        pcf::SpendingLimitTrackingStore::Entry entry{};
                        entry.chf_subscription_id = subscribed->first;
                        entry.supi = body->supi;
                        entry.last_status = subscribed->second;
                        spending_limit_tracking.put(id, entry);
                        spending_limit_subscribe_counter->Add(1);
                        spdlog::info("pcf: opened real CHF spending-limit subscription {} for SM "
                                     "policy {} ({} policy counters)",
                                     subscribed->first,
                                     id,
                                     policy_counter_ids.size());
                    }
                }
            }

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location", std::string(kSmApiRoot) + "/sm-policies/" + id);
            json j = decision;
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kSmApiRoot) + "/sm-policies/{smPolicyId}",
        [&verifier, &sm_policies](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto sm_policy_id = req.path_params.at("smPolicyId");
            auto control = sm_policies.get(sm_policy_id);
            if (!control.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No SM policy " + sm_policy_id);
            }
            return sbi_core::http2::Response::json(200, control->dump());
        });

    server.add_route(
        "POST",
        std::string(kSmApiRoot) + "/sm-policies/{smPolicyId}/update",
        [&verifier, &sm_policies, &sm_update_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            // SmPolicyUpdateContextData is an opaque fallback in the generated DTOs (an `allOf`
            // shape tools/sbi-codegen doesn't model -- see nfs/pcf/src/stores.hpp's header),
            // parsed here as raw JSON rather than a typed struct.
            json body;
            try {
                body = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto sm_policy_id = req.path_params.at("smPolicyId");
            auto stored = sm_policies.get(sm_policy_id);
            if (!stored.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No SM policy " + sm_policy_id);
            }
            auto control = stored->get<sbi_gen::SmPolicyControl>();
            // This build's decisioning is entirely a function of the ORIGINAL context (see
            // build_default_decision) -- there is no real trigger-driven re-evaluation, so an
            // UpdateSMPolicy call re-derives the same decision from the stored context rather than
            // reading the report itself. Disclosed, not silently assumed: matches the file
            // header's stated simplification.
            control.policy = build_default_decision(control.context);
            sm_policies.put(sm_policy_id, json(control));
            sm_update_counter->Add(1);
            json j = control.policy;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "POST",
        std::string(kSmApiRoot) + "/sm-policies/{smPolicyId}/delete",
        [&verifier,
         &sm_policies,
         &sm_delete_counter,
         &chf_client,
         &chf_oauth,
         &chf_base_url,
         &spending_limit_tracking](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            // SmPolicyDeleteData has no mandatory fields (TS29512) -- accept whatever body is
            // sent without requiring it to parse as a specific typed DTO.
            const auto sm_policy_id = req.path_params.at("smPolicyId");
            if (!sm_policies.remove(sm_policy_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No SM policy " + sm_policy_id);
            }
            sm_delete_counter->Add(1);
            // ADR-0072: real, best-effort CHF unsubscribe if this SM policy ever opened a real
            // spending-limit subscription -- no-op (not an error) if it never did.
            if (auto tracked = spending_limit_tracking.remove(sm_policy_id); tracked.has_value()) {
                unsubscribe_spending_limit(
                    chf_client, chf_oauth, chf_base_url, tracked->chf_subscription_id);
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // ADR-0072 (gap-closure: real N28 end-to-end). PCF's own real statusNotification callback
    // target -- the path this project chose for the notifUri it supplies at subscribe time (see
    // subscribe_spending_limit's own call site). CHF POSTs a real SpendingLimitStatus here
    // whenever a tracked policy counter's status changes. Real, disclosed scope: this build
    // stores the pushed status (so a later GetSMPolicy/inspection could see it) but does NOT
    // re-run any PCC-rule/session-rule decisioning in response -- 3GPP itself leaves the mapping
    // from a free-form policyCounterId status string to a concrete policy action operator-defined
    // (see docs/DECISIONS.md ADR-0072), so automating that mapping here would mean inventing
    // business logic no spec or user decision has actually specified.
    // Real spec: the callback's own URL is constructed as `{notifUri}/notify`
    // (TS29594_Nchf_SpendingLimitControl.yaml's own callback key literally is
    // `'{$request.body#/notifUri}/notify'`) -- CHF always appends "/notify" to whatever notifUri
    // was supplied, so this project's own chosen notifUri value (see subscribe_spending_limit's
    // own call site) must be registered here WITH that same suffix for CHF's real POST to land.
    server.add_route(
        "POST",
        std::string(kSmApiRoot) + "/sm-policies/{smPolicyId}/spending-limit-notify/notify",
        [&spending_limit_tracking,
         &spending_limit_notify_counter,
         &sm_policy_notify_client,
         &sm_policies,
         &policy_counter_actions,
         &policy_update_push_counter](const sbi_core::http2::Request& req) {
            // Real spec: this callback's own security scheme is the subscriber's choice (TS29594
            // `security: [{}, oAuth2ClientCredentials: [nchf-spendinglimitcontrol]]`) -- no bearer
            // check here, matching every other real callback endpoint already deferred/undefended
            // the same way elsewhere in this project (e.g. CHF's own not-yet-implemented
            // chargingNotification receiver).
            json status;
            try {
                status = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto sm_policy_id = req.path_params.at("smPolicyId");
            if (!spending_limit_tracking.update_status(sm_policy_id, status)) {
                return sbi_core::http2::problem_response(
                    404,
                    "Not Found",
                    "No tracked spending-limit subscription for SM policy " + sm_policy_id);
            }
            spending_limit_notify_counter->Add(1);
            spdlog::info("pcf: received real spending-limit statusNotification for SM policy {}",
                         sm_policy_id);

            // ADR-0286 (the user's standing N28/Sy directive): carry the status change through to
            // SMF, which is what makes this chain end-to-end rather than PCF<->CHF only.
            //
            // The mapping from a policy counter's status to a policy change is OPERATOR-DEFINED --
            // this file's own header already records why inventing one here would be fabrication
            // (TS 29.594 makes PolicyCounterInfo.currentStatus a free-form string the spec never
            // enumerates). So the mapping is DATA: `policy_counter_actions` in config/pcf.json,
            // read at startup, each entry naming a policyCounterId, a currentStatus, and the
            // SmPolicyDecision fragment to push when they match. That is this project's own P7
            // ("product/tariff/policy is data, never code") applied to the one place it was still
            // being deferred for lack of a rule to encode.
            notify_smf_of_policy_change(sm_policy_notify_client,
                                        sm_policies,
                                        policy_counter_actions,
                                        sm_policy_id,
                                        status,
                                        policy_update_push_counter.get());

            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Npcf_PolicyAuthorization (ADR-0080, gap-closure task #103) ---

    server.add_route(
        "POST",
        std::string(kPolicyAuthApiRoot) + "/app-sessions",
        [&verifier, &app_sessions, &policy_auth_create_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::AppSessionContext>(req, err);
            if (!body.has_value()) {
                return err;
            }
            if (!body->ascReqData.has_value()) {
                return sbi_core::http2::problem_response(
                    400, "Missing mandatory IE", "AppSessionContext requires ascReqData");
            }

            sbi_gen::AppSessionContext ctx{};
            ctx.ascReqData = body->ascReqData;
            // No ServAuthInfo failure code set = the real "authorized" outcome -- see this file's
            // own header comment for why that's not a fabricated approval, just correctly absent.
            ctx.ascRespData = sbi_gen::AppSessionContextRespData{};
            json j = ctx;
            const auto id = app_sessions.create(j);
            policy_auth_create_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kPolicyAuthApiRoot) + "/app-sessions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kPolicyAuthApiRoot) + "/app-sessions/{appSessionId}",
        [&verifier, &app_sessions](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("appSessionId");
            auto ctx = app_sessions.get(id);
            if (!ctx.has_value()) {
                return sbi_core::http2::problem_response(404, "Not Found", "No app session " + id);
            }
            return sbi_core::http2::Response::json(200, ctx->dump());
        });

    server.add_route(
        "PATCH",
        std::string(kPolicyAuthApiRoot) + "/app-sessions/{appSessionId}",
        [&verifier, &app_sessions, &policy_auth_update_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("appSessionId");
            auto ctx = app_sessions.get(id);
            if (!ctx.has_value()) {
                return sbi_core::http2::problem_response(404, "Not Found", "No app session " + id);
            }
            // application/merge-patch+json (RFC 7396) per the real spec -- NOT RFC 6902 JSON
            // Patch (which NRF's own UpdateNFInstance uses); confirmed by reading
            // TS29514_Npcf_PolicyAuthorization.yaml's own ModAppSession requestBody content-type
            // directly, not assumed to match NRF's own convention.
            json patch_doc;
            try {
                patch_doc = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            if (ctx->contains("ascReqData")) {
                (*ctx)["ascReqData"].merge_patch(patch_doc);
            } else {
                (*ctx)["ascReqData"] = patch_doc;
            }
            app_sessions.put(id, *ctx);
            policy_auth_update_counter->Add(1);
            return sbi_core::http2::Response::json(200, ctx->dump());
        });

    server.add_route(
        "POST",
        std::string(kPolicyAuthApiRoot) + "/app-sessions/{appSessionId}/delete",
        [&verifier, &app_sessions, &policy_auth_delete_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            // EventsSubscReqData (the optional real request body here) has no mandatory fields --
            // accepted without requiring a specific typed parse, same convention as DeleteSMPolicy
            // above.
            const auto id = req.path_params.at("appSessionId");
            if (!app_sessions.remove(id)) {
                return sbi_core::http2::problem_response(404, "Not Found", "No app session " + id);
            }
            policy_auth_delete_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "PUT",
        std::string(kPolicyAuthApiRoot) + "/app-sessions/{appSessionId}/events-subscription",
        [&verifier, &app_sessions](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("appSessionId");
            auto ctx = app_sessions.get(id);
            if (!ctx.has_value()) {
                return sbi_core::http2::problem_response(404, "Not Found", "No app session " + id);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::EventsSubscReqData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const bool is_new = !(*ctx)["ascReqData"].contains("evSubsc") ||
                                (*ctx)["ascReqData"]["evSubsc"].is_null();
            json j = *body;
            (*ctx)["ascReqData"]["evSubsc"] = j;
            app_sessions.put(id, *ctx);

            sbi_core::http2::Response resp;
            resp.status = is_new ? 201 : 200;
            resp.headers.emplace("content-type", "application/json");
            if (is_new) {
                resp.headers.emplace("location",
                                     std::string(kPolicyAuthApiRoot) + "/app-sessions/" + id +
                                         "/events-subscription");
            }
            // EventsSubscPutData is an opaque anyOf shape tools/sbi-codegen couldn't resolve into
            // a typed struct (see TS26510_CommonData_grp.hpp's own "OPAQUE FALLBACK" comment) --
            // returning the real stored EventsSubscReqData representation instead: real,
            // schema-compatible content, not a fabricated shape.
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "DELETE",
        std::string(kPolicyAuthApiRoot) + "/app-sessions/{appSessionId}/events-subscription",
        [&verifier, &app_sessions](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("appSessionId");
            auto ctx = app_sessions.get(id);
            if (!ctx.has_value()) {
                return sbi_core::http2::problem_response(404, "Not Found", "No app session " + id);
            }
            (*ctx)["ascReqData"].erase("evSubsc");
            app_sessions.put(id, *ctx);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "POST",
        std::string(kPolicyAuthApiRoot) + "/app-sessions/pcscf-restoration",
        [&verifier](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            // Real, disclosed simplification: this lab has no real per-UE App Session Context
            // inventory keyed by UE identity to actually terminate on P-CSCF restoration (TS
            // 29.514's own real trigger for this operation) -- acknowledges (204) without any App
            // Session Contexts actually existing to search/terminate, same class of
            // simplification as every other "no real trigger source in this lab yet" gap already
            // disclosed in this file's own header.
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Npcf_UEPolicyControl (ADR-0204, gap-closure task #163) ---

    server.add_route(
        "POST",
        std::string(kUePolicyApiRoot) + "/policies",
        [&verifier, &ue_policies, &ue_policy_create_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<
                sbi_gen::PolicyAssociationRequest_Npcf_UEPolicyControl>(req, err);
            if (!body.has_value()) {
                return err;
            }

            sbi_gen::PolicyAssociation_Npcf_UEPolicyControl association{};
            association.request = *body;
            association.suppFeat = body->suppFeat;
            json j = association;
            const auto id = ue_policies.create(j);
            ue_policy_create_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location", std::string(kUePolicyApiRoot) + "/policies/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kUePolicyApiRoot) + "/policies/{polAssoId}",
        [&verifier, &ue_policies](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto pol_asso_id = req.path_params.at("polAssoId");
            auto association = ue_policies.get(pol_asso_id);
            if (!association.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No UE policy association " + pol_asso_id);
            }
            return sbi_core::http2::Response::json(200, association->dump());
        });

    server.add_route(
        "DELETE",
        std::string(kUePolicyApiRoot) + "/policies/{polAssoId}",
        [&verifier, &ue_policies, &ue_policy_delete_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto pol_asso_id = req.path_params.at("polAssoId");
            if (!ue_policies.remove(pol_asso_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No UE policy association " + pol_asso_id);
            }
            ue_policy_delete_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "POST",
        std::string(kUePolicyApiRoot) + "/policies/{polAssoId}/update",
        [&verifier, &ue_policies, &ue_policy_update_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<
                sbi_gen::PolicyAssociationUpdateRequest_Npcf_UEPolicyControl>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto pol_asso_id = req.path_params.at("polAssoId");
            auto stored = ue_policies.get(pol_asso_id);
            if (!stored.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No UE policy association " + pol_asso_id);
            }
            auto association = stored->get<sbi_gen::PolicyAssociation_Npcf_UEPolicyControl>();
            if (body->triggers.has_value()) {
                association.triggers = body->triggers;
            }
            ue_policies.put(pol_asso_id, json(association));

            // Disclosed simplification (ADR-0204): no real UE Route Selection Policy (URSP) or
            // access-network-discovery-and-selection-policy (ANDSP) generation logic exists in
            // this build -- the updated policies reported back are always empty (`uePolicy`
            // absent), same "acknowledge the real report, no real policy content to hand back"
            // class of gap already established for Nudm_NIDDAU/Nudm_SSAU (ADR-0202).
            sbi_gen::PolicyUpdate_Npcf_UEPolicyControl update{};
            update.resourceUri = std::string(kUePolicyApiRoot) + "/policies/" + pol_asso_id;
            update.triggers = body->triggers;
            ue_policy_update_counter->Add(1);
            json j = update;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    // --- Npcf_EventExposure (ADR-0204, gap-closure task #163) ---

    server.add_route(
        "POST",
        std::string(kEventExposureApiRoot) + "/subscriptions",
        [&verifier, &pc_event_subs, &ee_create_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::PcEventExposureSubsc>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            const auto id = pc_event_subs.create(j);
            ee_create_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kEventExposureApiRoot) + "/subscriptions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kEventExposureApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &pc_event_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto sub_id = req.path_params.at("subscriptionId");
            auto subscription = pc_event_subs.get(sub_id);
            if (!subscription.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No policy control events subscription " + sub_id);
            }
            return sbi_core::http2::Response::json(200, subscription->dump());
        });

    server.add_route(
        "PUT",
        std::string(kEventExposureApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &pc_event_subs, &ee_update_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto sub_id = req.path_params.at("subscriptionId");
            if (!pc_event_subs.get(sub_id).has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No policy control events subscription " + sub_id);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::PcEventExposureSubsc>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            pc_event_subs.put(sub_id, j);
            ee_update_counter->Add(1);
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "DELETE",
        std::string(kEventExposureApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &pc_event_subs, &ee_delete_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto sub_id = req.path_params.at("subscriptionId");
            if (!pc_event_subs.remove(sub_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No policy control events subscription " + sub_id);
            }
            ee_delete_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Npcf_AMPolicyAuthorization (ADR-0205, gap-closure task #163, second PCF slice) ---

    server.add_route(
        "POST",
        std::string(kAmPolicyAuthApiRoot) + "/app-am-contexts",
        [&verifier, &app_am_contexts, &am_auth_create_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::AppAmContextData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            const auto id = app_am_contexts.create(j);
            am_auth_create_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kAmPolicyAuthApiRoot) + "/app-am-contexts/" + id);
            // AppAmContextRespData is an opaque anyOf shape tools/sbi-codegen couldn't resolve
            // into a typed struct (see TS26510_CommonData_grp.hpp's own "OPAQUE FALLBACK"
            // comment) -- returning the real stored AppAmContextData representation instead: real
            // schema-compatible content, not a fabricated shape. Disclosed: no real event-matching
            // logic exists in this build, so the response never embeds an AmEventsNotification.
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAmPolicyAuthApiRoot) + "/app-am-contexts/{appAmContextId}",
        [&verifier, &app_am_contexts](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("appAmContextId");
            auto ctx = app_am_contexts.get(id);
            if (!ctx.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No application AM context " + id);
            }
            return sbi_core::http2::Response::json(200, ctx->dump());
        });

    server.add_route(
        "PATCH",
        std::string(kAmPolicyAuthApiRoot) + "/app-am-contexts/{appAmContextId}",
        [&verifier, &app_am_contexts, &am_auth_update_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("appAmContextId");
            auto stored = app_am_contexts.get(id);
            if (!stored.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No application AM context " + id);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::AppAmContextUpdateData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            auto context = stored->get<sbi_gen::AppAmContextData>();
            if (body->termNotifUri.has_value()) {
                context.termNotifUri = *body->termNotifUri;
            }
            if (body->expiry.has_value()) {
                context.expiry = body->expiry;
            }
            if (body->highThruInd.has_value()) {
                context.highThruInd = body->highThruInd;
            }
            if (body->covReq.has_value()) {
                context.covReq = body->covReq;
            }
            if (body->asTimeDisParam.has_value()) {
                context.asTimeDisParam = body->asTimeDisParam;
            }
            if (body->sliceReplReq.has_value()) {
                context.sliceReplReq = body->sliceReplReq;
            }
            json j = context;
            app_am_contexts.put(id, j);
            am_auth_update_counter->Add(1);
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "DELETE",
        std::string(kAmPolicyAuthApiRoot) + "/app-am-contexts/{appAmContextId}",
        [&verifier, &app_am_contexts, &am_auth_delete_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("appAmContextId");
            if (!app_am_contexts.remove(id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No application AM context " + id);
            }
            am_auth_delete_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "PUT",
        std::string(kAmPolicyAuthApiRoot) + "/app-am-contexts/{appAmContextId}/events-subscription",
        [&verifier, &app_am_contexts, &am_auth_evsubsc_put_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("appAmContextId");
            auto ctx = app_am_contexts.get(id);
            if (!ctx.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No application AM context " + id);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::AmEventsSubscData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const bool is_new = !ctx->contains("evSubsc") || (*ctx)["evSubsc"].is_null();
            json j = *body;
            (*ctx)["evSubsc"] = j;
            app_am_contexts.put(id, *ctx);
            am_auth_evsubsc_put_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = is_new ? 201 : 200;
            resp.headers.emplace("content-type", "application/json");
            if (is_new) {
                resp.headers.emplace("location",
                                     std::string(kAmPolicyAuthApiRoot) + "/app-am-contexts/" + id +
                                         "/events-subscription");
            }
            // AmEventsSubscRespData is the same opaque anyOf shape as AppAmContextRespData above
            // -- returning the real stored AmEventsSubscData representation instead.
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "DELETE",
        std::string(kAmPolicyAuthApiRoot) + "/app-am-contexts/{appAmContextId}/events-subscription",
        [&verifier, &app_am_contexts, &am_auth_evsubsc_delete_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("appAmContextId");
            auto ctx = app_am_contexts.get(id);
            if (!ctx.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No application AM context " + id);
            }
            ctx->erase("evSubsc");
            app_am_contexts.put(id, *ctx);
            am_auth_evsubsc_delete_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Npcf_MBSPolicyAuthorization (ADR-0205, gap-closure task #163, second PCF slice) ---

    server.add_route(
        "POST",
        std::string(kMbsPolicyAuthApiRoot) + "/contexts",
        [&verifier, &mbs_app_sessions, &mbs_auth_create_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::MbsAppSessionCtxt>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            const auto id = mbs_app_sessions.create(j);
            mbs_auth_create_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kMbsPolicyAuthApiRoot) + "/contexts/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kMbsPolicyAuthApiRoot) + "/contexts/{contextId}",
        [&verifier, &mbs_app_sessions](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("contextId");
            auto ctx = mbs_app_sessions.get(id);
            if (!ctx.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No MBS application session context " + id);
            }
            return sbi_core::http2::Response::json(200, ctx->dump());
        });

    server.add_route(
        "PATCH",
        std::string(kMbsPolicyAuthApiRoot) + "/contexts/{contextId}",
        [&verifier, &mbs_app_sessions, &mbs_auth_update_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("contextId");
            auto stored = mbs_app_sessions.get(id);
            if (!stored.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No MBS application session context " + id);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::MbsAppSessionCtxtPatch>(req, err);
            if (!body.has_value()) {
                return err;
            }
            auto context = stored->get<sbi_gen::MbsAppSessionCtxt>();
            if (body->mbsServInfo.has_value()) {
                context.mbsServInfo = body->mbsServInfo;
            }
            json j = context;
            mbs_app_sessions.put(id, j);
            mbs_auth_update_counter->Add(1);
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "DELETE",
        std::string(kMbsPolicyAuthApiRoot) + "/contexts/{contextId}",
        [&verifier, &mbs_app_sessions, &mbs_auth_delete_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("contextId");
            if (!mbs_app_sessions.remove(id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No MBS application session context " + id);
            }
            mbs_auth_delete_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Npcf_MBSPolicyControl (ADR-0205, gap-closure task #163, second PCF slice) ---

    server.add_route(
        "POST",
        std::string(kMbsPolicyControlApiRoot) + "/mbs-policies",
        [&verifier, &mbs_policies, &mbs_policy_create_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::MbsPolicyCtxtData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            sbi_gen::MbsPolicyData policy{};
            policy.mbsPolicyCtxtData = *body;
            json j = policy;
            const auto id = mbs_policies.create(j);
            mbs_policy_create_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kMbsPolicyControlApiRoot) + "/mbs-policies/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kMbsPolicyControlApiRoot) + "/mbs-policies/{mbsPolicyId}",
        [&verifier, &mbs_policies](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("mbsPolicyId");
            auto policy = mbs_policies.get(id);
            if (!policy.has_value()) {
                return sbi_core::http2::problem_response(404, "Not Found", "No MBS policy " + id);
            }
            return sbi_core::http2::Response::json(200, policy->dump());
        });

    server.add_route(
        "DELETE",
        std::string(kMbsPolicyControlApiRoot) + "/mbs-policies/{mbsPolicyId}",
        [&verifier, &mbs_policies, &mbs_policy_delete_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("mbsPolicyId");
            if (!mbs_policies.remove(id)) {
                return sbi_core::http2::problem_response(404, "Not Found", "No MBS policy " + id);
            }
            mbs_policy_delete_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "POST",
        std::string(kMbsPolicyControlApiRoot) + "/mbs-policies/{mbsPolicyId}/update",
        [&verifier, &mbs_policies, &mbs_policy_update_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("mbsPolicyId");
            auto stored = mbs_policies.get(id);
            if (!stored.has_value()) {
                return sbi_core::http2::problem_response(404, "Not Found", "No MBS policy " + id);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::MbsPolicyCtxtDataUpdate>(req, err);
            if (!body.has_value()) {
                return err;
            }
            auto policy = stored->get<sbi_gen::MbsPolicyData>();
            if (body->mbsServInfo.has_value()) {
                policy.mbsPolicyCtxtData.mbsServInfo = body->mbsServInfo;
            }
            // Disclosed simplification (ADR-0205): no real MBS PCC-rule/QoS decision engine
            // exists in this build, so `mbsPcrts`/`mbsErrorReport` in the update request are
            // accepted (structurally validated) but not applied to any real `mbsPolicies`
            // decision data -- same class of gap as PCF's own pre-existing SM/AM policy defaults
            // (this file's own header, "fixed default policy... not real subscriber-specific
            // decisioning").
            json j = policy;
            mbs_policies.put(id, j);
            mbs_policy_update_counter->Add(1);
            return sbi_core::http2::Response::json(200, j.dump());
        });

    // --- Npcf_PDTQPolicyControl (ADR-0206, gap-closure task #163, third and final PCF slice) ---

    server.add_route(
        "POST",
        std::string(kPdtqPolicyControlApiRoot) + "/pdtq-policies",
        [&verifier, &pdtq_policies, &pdtq_create_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::PdtqPolicyData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            const auto id = pdtq_policies.create(j);
            pdtq_create_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kPdtqPolicyControlApiRoot) + "/pdtq-policies/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kPdtqPolicyControlApiRoot) + "/pdtq-policies/{pdtqPolicyId}",
        [&verifier, &pdtq_policies](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("pdtqPolicyId");
            auto policy = pdtq_policies.get(id);
            if (!policy.has_value()) {
                return sbi_core::http2::problem_response(404, "Not Found", "No PDTQ policy " + id);
            }
            return sbi_core::http2::Response::json(200, policy->dump());
        });

    server.add_route(
        "PATCH",
        std::string(kPdtqPolicyControlApiRoot) + "/pdtq-policies/{pdtqPolicyId}",
        [&verifier, &pdtq_policies, &pdtq_update_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("pdtqPolicyId");
            auto stored = pdtq_policies.get(id);
            if (!stored.has_value()) {
                return sbi_core::http2::problem_response(404, "Not Found", "No PDTQ policy " + id);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::PdtqPolicyPatchData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            auto policy = stored->get<sbi_gen::PdtqPolicyData>();
            if (body->notifUri.has_value()) {
                policy.notifUri = body->notifUri;
            }
            if (body->selPdtqPolicyId.has_value()) {
                policy.selPdtqPolicyId = body->selPdtqPolicyId;
            }
            if (body->warnNotifReq.has_value()) {
                policy.warnNotifReq = body->warnNotifReq;
            }
            json j = policy;
            pdtq_policies.put(id, j);
            pdtq_update_counter->Add(1);
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "DELETE",
        std::string(kPdtqPolicyControlApiRoot) + "/pdtq-policies/{pdtqPolicyId}",
        [&verifier, &pdtq_policies, &pdtq_delete_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("pdtqPolicyId");
            if (!pdtq_policies.remove(id)) {
                return sbi_core::http2::problem_response(404, "Not Found", "No PDTQ policy " + id);
            }
            pdtq_delete_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Npcf_BDTPolicyControl (ADR-0206, gap-closure task #163, third and final PCF slice) ---

    server.add_route(
        "POST",
        std::string(kBdtPolicyControlApiRoot) + "/bdtpolicies",
        [&verifier, &bdt_policies, &bdt_create_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::BdtReqData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            // Disclosed simplification (ADR-0206): no real BDT decision engine exists in this
            // build, so the created resource's own `bdtPolData` (the set of transfer policies
            // the PCF offers) is honestly absent rather than fabricated -- `BdtPolicyData` itself
            // requires a real `bdtRefId` and at least one real `TransferPolicy`, neither of which
            // this build can produce.
            sbi_gen::BdtPolicy policy{};
            policy.bdtReqData = *body;
            json j = policy;
            const auto id = bdt_policies.create(j);
            bdt_create_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kBdtPolicyControlApiRoot) + "/bdtpolicies/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kBdtPolicyControlApiRoot) + "/bdtpolicies/{bdtPolicyId}",
        [&verifier, &bdt_policies](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("bdtPolicyId");
            auto policy = bdt_policies.get(id);
            if (!policy.has_value()) {
                return sbi_core::http2::problem_response(404, "Not Found", "No BDT policy " + id);
            }
            return sbi_core::http2::Response::json(200, policy->dump());
        });

    server.add_route(
        "PATCH",
        std::string(kBdtPolicyControlApiRoot) + "/bdtpolicies/{bdtPolicyId}",
        [&verifier, &bdt_policies, &bdt_update_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("bdtPolicyId");
            auto stored = bdt_policies.get(id);
            if (!stored.has_value()) {
                return sbi_core::http2::problem_response(404, "Not Found", "No BDT policy " + id);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::PatchBdtPolicy>(req, err);
            if (!body.has_value()) {
                return err;
            }
            if (body->bdtPolData.has_value()) {
                // Real, honest constraint: selecting a transfer policy requires this PCF to have
                // real transfer policies on offer (see CreateBDTPolicy's own disclosed gap
                // above) -- none exist in this build, so there is nothing valid to select.
                return sbi_core::http2::problem_response(
                    400,
                    "Bad Request",
                    "No transfer policies are available to select (no real BDT decision engine "
                    "exists in this build)");
            }
            auto policy = stored->get<sbi_gen::BdtPolicy>();
            if (body->bdtReqData.has_value() && policy.bdtReqData.has_value()) {
                if (body->bdtReqData->warnNotifReq.has_value()) {
                    policy.bdtReqData->warnNotifReq = body->bdtReqData->warnNotifReq;
                }
                if (body->bdtReqData->energyInd.has_value()) {
                    policy.bdtReqData->energyInd = body->bdtReqData->energyInd;
                }
                if (body->bdtReqData->notifUri.has_value()) {
                    policy.bdtReqData->notifUri = body->bdtReqData->notifUri;
                }
            }
            json j = policy;
            bdt_policies.put(id, j);
            bdt_update_counter->Add(1);
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "DELETE",
        std::string(kBdtPolicyControlApiRoot) + "/bdtpolicies/{bdtPolicyId}",
        [&verifier, &bdt_policies, &bdt_delete_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("bdtPolicyId");
            if (!bdt_policies.remove(id)) {
                return sbi_core::http2::problem_response(404, "Not Found", "No BDT policy " + id);
            }
            bdt_delete_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    std::thread(run_nrf_lifecycle, pcf_instance_id, nrf_base_url).detach();

    server.start();
    spdlog::info("pcf: listening on https://{}:{} (TLS 1.3 + mTLS)", sbi_bind_address, port);
    spdlog::info("pcf: Prometheus metrics at http://{}/metrics", metrics_bind_address);
    sbi_core::run_multi_threaded(ioc);
    return 0;
}
