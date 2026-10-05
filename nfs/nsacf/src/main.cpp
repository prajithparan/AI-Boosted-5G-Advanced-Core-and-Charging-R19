// nfs/nsacf: NSACF (Network Slice Admission Control Function), TS 29.536.
// Sources: specs/5G_APIs-REL-19/TS29536_Nnsacf_NSAC.yaml and
// specs/5G_APIs-REL-19/TS29536_Nnsacf_SliceEventExposure.yaml, commit
// bca84b60a37773133bcae97e5c6c0d10a93b47b6. This project's fifth Tier 2 NF (after 5G-EIR ADR-0187,
// SMSF ADR-0188, GMLC ADR-0189, LMF ADR-0191), continuing the continuous move-to-next-NF process
// (ADR-0184).
//
// In scope: BOTH of this NF's real API files, all 8 real operations -- counted by direct read of
// the YAML, not estimated:
//
//   Nnsacf_NSAC (TS29536_Nnsacf_NSAC.yaml, /nnsacf-nsac/v1)
//     POST /slices/ues                    NumOfUEsUpdate     -- admission on the number of UEs
//     POST /slices/pdus                   NumOfPDUsUpdate    -- admission on the number of PDUs
//     POST /slices/local-configs/update   LocalNumberUpdate  -- set this NSACF's local maxima
//     POST /slices/roaming-quotas/query   QuotaUpdate        -- read the maxima back
//
//   Nnsacf_SliceEventExposure (TS29536_Nnsacf_SliceEventExposure.yaml, /nnsacf-slice-ee/v1)
//     POST  /subscriptions                     CreateSubscription
//     PATCH /subscriptions/{subscriptionId}    PartialModifySubscription
//     PUT   /subscriptions/{subscriptionId}    CompleteModifySubscription
//     DELETE /subscriptions/{subscriptionId}   DeleteSubscription
//
// Why NSACF was chosen next, rather than the next smallest YAML: it has REAL consumers already
// built in this project. TS 23.502 has AMF invoke the UE-number service during Registration and
// SMF invoke the PDU-number service during PDU Session Establishment, and both of those procedures
// now run end to end here (ADR-0267). Whether they call NSACF yet is a separate question, answered
// honestly under "disclosed" below.
//
// The admission logic is real, not a stub: quotas per S-NSSAI come from config, and the counts are
// maintained as SETS of identities (SUPI for UEs, (SUPI, pduSessionId) for PDU sessions), because
// TS 29.536's operations are idempotent -- a re-sent INCREASE for a UE already on the slice must
// not consume a second unit of quota. See nfs/nsacf/src/stores.hpp.
//
// Real, disclosed, and stated up front rather than found in review:
//
//  - NOT WIRED YET. Neither AMF nor SMF calls this NF. Its admission decisions are therefore
//    correct and reachable over real SBI, but nothing in this project's own registration or PDU
//    session path consults them yet. Same shape as 5G-EIR's own disclosed non-wiring (ADR-0187),
//    and the same standard applies: a server with no consumer is not "done" (this project's own
//    full-YAML-coverage rule), it is a complete NF awaiting its wiring increment.
//  - Subscriptions are in-memory and process-local, like every other event-exposure service here.
//  - PERIODIC reporting is driven by a 1-second scan thread, so a `notificationPeriod` is honoured
//    to within a second rather than exactly. Stated because it is a real property of this
//    implementation, not a spec allowance.
//  - The EAC ACTIVATION THRESHOLD is a local policy value (`eac_activation_percent` in
//    config/nsacf.json), not a field of any 3GPP schema. TS 23.501 §5.15.11.1 says NSACF switches
//    the serving AMF to Early Admission Control as a slice approaches its maximum, but it does not
//    standardise WHEN -- so this is configuration, and it is called out here rather than dressed
//    up as a spec value.
//  - `eacNotificationUri` is carried per-request in UeACRequestData, so NSACF can only notify a
//    consumer that has supplied one. Until some NF does, EAC state is tracked and logged but has
//    nowhere to be delivered -- a property of the spec's own design, not a shortcut.

#include "sbi_core/datetime.hpp"
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

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "TS29536_Nnsacf_NSAC.hpp"
#include "nf_config/nf_config.hpp"
#include "stores.hpp"

namespace {

using nlohmann::json;

#ifndef CERTS_DIR
#error "CERTS_DIR must be defined by CMake (see nfs/nsacf/CMakeLists.txt)"
#endif
#ifndef CONFIG_DIR
#error "CONFIG_DIR must be defined by CMake (see nfs/nsacf/CMakeLists.txt)"
#endif

// Real TS 29.510 NFType enum value, confirmed by direct read of
// specs/5G_APIs-REL-19/TS29510_Nnrf_NFManagement.yaml (line 2194) and against this project's own
// NRF known_nf_types() list, which already accepts it.
constexpr const char* kNfType = "NSACF";
constexpr const char* kNsacApiRoot = "/nnsacf-nsac/v1";
constexpr const char* kEeApiRoot = "/nnsacf-slice-ee/v1";

// Must match nfs/nrf/src/main.cpp's kNrfInstanceId exactly -- see docs/DECISIONS.md ADR-0018.
constexpr const char* kNrfInstanceId = "5ba9a927-1d31-4c8e-8a10-000000000001";

// Same pattern as every other NF's check_bearer -- see nfs/nrf/src/main.cpp's comment for why a
// missing Authorization header is not itself a 401 (the YAML's own
// `security: [{}, oAuth2ClientCredentials:[...]]` bootstrap alternative).
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

// The slice quotas this NSACF enforces, from config/nsacf.json. Real values, not seeded fiction:
// they are this lab's own configured slices, and LocalNumberUpdate can change them at runtime
// exactly as the spec intends.
void configure_slices_from_config(const json& config, nsacf::SliceAdmissionStore& store) {
    const auto slices = nf_config::require<json>(config, "slices");
    for (const auto& entry : slices) {
        const auto sst = entry.at("sst").get<std::int64_t>();
        std::optional<std::string> sd;
        if (entry.contains("sd") && !entry.at("sd").is_null()) {
            sd = entry.at("sd").get<std::string>();
        }
        const auto key = nsacf::slice_key(sst, sd);
        store.configure(
            key, entry.at("max_ues").get<std::int64_t>(), entry.at("max_pdus").get<std::int64_t>());
        spdlog::info("nsacf: slice {} admits at most {} UE(s) and {} PDU session(s)",
                     key,
                     entry.at("max_ues").get<std::int64_t>(),
                     entry.at("max_pdus").get<std::int64_t>());
    }
}

// --- Nnsacf_SliceEventExposure reporting (TS 29.536 §5.3, the `eventReport` callback) ---------
//
// The YAML defines a real callback on CreateSubscription: POST a SACEventReport to the
// subscription's own `eventNotifyUri`, answered 204. Everything below exists to send that.

// One report for one slice, shaped exactly as SACEventReportItem requires: eventType, eventState,
// timeStamp and eventFilter are all MANDATORY in the schema.
sbi_gen::SACEventReportItem make_report_item(const std::string& event_type,
                                             std::int64_t sst,
                                             const std::optional<std::string>& sd,
                                             std::int64_t current,
                                             std::optional<std::int64_t> remaining,
                                             bool still_active) {
    sbi_gen::SACEventReportItem item;
    item.eventType.value = event_type;
    item.eventState.active = still_active;
    if (remaining.has_value()) {
        item.eventState.remainReports = *remaining;
    }
    item.timeStamp = sbi_core::format_rfc3339(std::chrono::system_clock::now());
    item.eventFilter.sst = sst;
    item.eventFilter.sd = sd;

    // sliceStautsInfo carries what was actually reached -- the number that made this report worth
    // sending. The spec's own spelling of the field is kept (`sliceStautsInfo`), typo and all,
    // because it is what goes on the wire.
    sbi_gen::SACEventStatus status;
    sbi_gen::SACInfo reached;
    if (event_type == sbi_gen::SACEventType::NUM_OF_REGD_UES) {
        reached.numericValNumUes = current;
        status.reachedNumUes = reached;
    } else {
        reached.numericValNumPduSess = current;
        status.reachedNumPduSess = reached;
    }
    item.sliceStautsInfo = status;
    return item;
}

// True when this subscription's threshold is met by `current` against `maximum`. Both forms the
// SACInfo schema allows are honoured: an absolute count and a percentage of the maximum.
bool threshold_reached(const nsacf::SacSubscriptionRecord& sub,
                       std::int64_t current,
                       std::int64_t maximum) {
    if (sub.threshold_absolute.has_value() && current >= *sub.threshold_absolute) {
        return true;
    }
    if (sub.threshold_percent.has_value() && maximum > 0) {
        return (current * 100) / maximum >= *sub.threshold_percent;
    }
    return false;
}

// Delivers one report and counts it against maxReports. Failure is logged, never retried -- the
// same fire-and-forget contract nfs/udr's own onDataChange delivery uses (ADR-0179).
void deliver_report(sbi_core::http2::Client& notify_client,
                    nsacf::SacSubscriptionStore& subscriptions,
                    const nsacf::SacSubscriptionRecord& sub,
                    std::int64_t sst,
                    const std::optional<std::string>& sd,
                    std::int64_t current) {
    const auto remaining = subscriptions.record_report_sent(sub.id);
    const bool still_active = !remaining.has_value() || *remaining > 0;

    sbi_gen::SACEventReport report;
    report.report = make_report_item(sub.event_type, sst, sd, current, remaining, still_active);
    if (sub.correlation_id.has_value()) {
        report.notifyCorrelationId = *sub.correlation_id;
    }

    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = sub.notify_uri;
    req.headers.emplace("content-type", "application/json");
    req.body = json(report).dump();
    auto resp = notify_client.send(req);
    if (!resp.has_value() || resp->status != 204) {
        spdlog::warn("nsacf: SACEventReport delivery to {} failed or non-204 (subscription {})",
                     sub.notify_uri,
                     sub.id);
        return;
    }
    spdlog::info("nsacf: reported {}={} for slice {} to subscription {}{}",
                 sub.event_type,
                 current,
                 nsacf::slice_key(sst, sd),
                 sub.id,
                 still_active ? "" : " (maxReports reached, subscription now inactive)");
}

// Called after every admission change. Evaluates only THRESHOLD subscriptions; PERIODIC ones are
// driven by their own timer below, which is what the two eventTrigger values mean.
void report_threshold_events(sbi_core::http2::Client& notify_client,
                             nsacf::SacSubscriptionStore& subscriptions,
                             nsacf::SliceAdmissionStore& slices,
                             const std::string& event_type,
                             std::int64_t sst,
                             const std::optional<std::string>& sd) {
    const auto key = nsacf::slice_key(sst, sd);
    const auto quotas = slices.quotas(key);
    if (!quotas.has_value()) {
        return;
    }
    const bool is_ue = event_type == sbi_gen::SACEventType::NUM_OF_REGD_UES;
    const std::int64_t current = is_ue ? slices.ue_count(key) : slices.pdu_count(key);
    const std::int64_t maximum = is_ue ? quotas->first : quotas->second;

    for (const auto& sub : subscriptions.active_snapshot()) {
        if (sub.event_type != event_type ||
            sub.event_trigger != sbi_gen::SACEventTrigger::THRESHOLD) {
            continue;
        }
        bool watches_slice = false;
        for (const auto& [filter_sst, filter_sd] : sub.slices) {
            if (nsacf::slice_key(filter_sst, filter_sd) == key) {
                watches_slice = true;
                break;
            }
        }
        if (!watches_slice || !threshold_reached(sub, current, maximum)) {
            continue;
        }
        deliver_report(notify_client, subscriptions, sub, sst, sd, current);
    }
}

// PERIODIC subscriptions. One scan thread rather than a timer per subscription: the cadence this
// service reports at is seconds, and a thread per subscription would be the wrong shape for a
// list that is edited while it is read.
// ADR-0394 (extended): this thread used to be detached with no stop signal at all, holding
// references (subscriptions/slices) to objects main() owns and destructs on return -- reproduced
// directly as a real `Fatal glibc error: pthread_mutex_lock.c: ... assertion failed` crash during
// AMF integration tests that send this process SIGTERM while it is mid-sleep, one of several
// real instances of the exact same bug class the AMF NGAP thread had (see NgapShutdownCoordinator's
// own header comment in nfs/amf/src/ngap_task.cpp for the original root-cause writeup). `main()`
// no longer detaches this thread -- it joins it after its own run_multi_threaded(ioc) returns, so
// this loop only needs to notice `stop_requested` and return; it does not need to interrupt a
// blocking syscall the way the AMF/SCTP case did.
void run_periodic_reporting(nsacf::SacSubscriptionStore& subscriptions,
                            nsacf::SliceAdmissionStore& slices,
                            std::atomic<bool>& stop_requested) {
    sbi_core::http2::TlsConfig client_tls{
        .cert_path = CERTS_DIR "/nsacf/cert.pem",
        .key_path = CERTS_DIR "/nsacf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client notify_client(std::move(client_tls));

    std::unordered_map<std::string, std::chrono::steady_clock::time_point> next_due;
    while (!stop_requested.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (stop_requested.load()) {
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        for (const auto& sub : subscriptions.active_snapshot()) {
            if (sub.event_trigger != sbi_gen::SACEventTrigger::PERIODIC ||
                sub.period_seconds <= 0) {
                continue;
            }
            auto due_it = next_due.find(sub.id);
            if (due_it == next_due.end()) {
                next_due[sub.id] = now + std::chrono::seconds(sub.period_seconds);
                continue;
            }
            if (now < due_it->second) {
                continue;
            }
            due_it->second = now + std::chrono::seconds(sub.period_seconds);

            const bool is_ue = sub.event_type == sbi_gen::SACEventType::NUM_OF_REGD_UES;
            for (const auto& [sst, sd] : sub.slices) {
                const auto key = nsacf::slice_key(sst, sd);
                const std::int64_t current = is_ue ? slices.ue_count(key) : slices.pdu_count(key);
                deliver_report(notify_client, subscriptions, sub, sst, sd, current);
            }
        }
    }
}

// --- Early Admission Control (TS 23.501 §5.15.11.1, the NSAC YAML's `eacNotification` callback) -
//
// NSACF tells the serving AMF to switch a slice into Early Admission Control as it approaches its
// maximum, and back out when it recedes. The notification body is EacNotification: a map of
// S-NSSAI-as-string to EACMode (ACTIVE/DEACTIVE), posted to the `eacNotificationUri` the
// requesting NF supplied in its own UeACRequestData, answered 204.
void evaluate_eac_mode(sbi_core::http2::Client& notify_client,
                       nsacf::EacModeStore& eac,
                       nsacf::SliceAdmissionStore& slices,
                       std::int64_t sst,
                       const std::optional<std::string>& sd,
                       std::int64_t activation_percent) {
    const auto key = nsacf::slice_key(sst, sd);
    const auto quotas = slices.quotas(key);
    if (!quotas.has_value() || quotas->first <= 0) {
        return;
    }
    const std::int64_t current = slices.ue_count(key);
    const bool should_be_active = (current * 100) / quotas->first >= activation_percent;

    const auto changed = eac.set_active(key, should_be_active);
    if (!changed.has_value()) {
        return; // mode unchanged: notifying again would be noise, not information
    }

    const auto uri = eac.notification_uri();
    if (!uri.has_value()) {
        spdlog::info("nsacf: slice {} EAC mode is now {} ({} of {} UEs) -- no NF has supplied an "
                     "eacNotificationUri, so there is nobody to tell",
                     key,
                     *changed ? "ACTIVE" : "DEACTIVE",
                     current,
                     quotas->first);
        return;
    }

    sbi_gen::EacNotification notification;
    notification.eacModeList =
        json{{key, *changed ? sbi_gen::EACMode::ACTIVE : sbi_gen::EACMode::DEACTIVE}};

    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = *uri;
    req.headers.emplace("content-type", "application/json");
    req.body = json(notification).dump();
    auto resp = notify_client.send(req);
    if (!resp.has_value() || resp->status != 204) {
        spdlog::warn("nsacf: EacNotification delivery to {} failed or non-204", *uri);
        return;
    }
    spdlog::info("nsacf: slice {} switched to EAC {} ({} of {} UEs) -- told {}",
                 key,
                 *changed ? "ACTIVE" : "DEACTIVE",
                 current,
                 quotas->first,
                 *uri);
}

// Builds the internal record from the real SACEventSubscription the caller sent. Kept beside the
// route so the mapping from spec field to internal field is visible in one place.
nsacf::SacSubscriptionRecord to_record(const sbi_gen::SACEventSubscription& sub,
                                       const std::string& raw) {
    nsacf::SacSubscriptionRecord record;
    record.notify_uri = sub.eventNotifyUri;
    record.event_type = sub.event.eventType.value;
    record.event_trigger = sub.event.eventTrigger.has_value()
                               ? sub.event.eventTrigger->value
                               : std::string(sbi_gen::SACEventTrigger::THRESHOLD);
    for (const auto& snssai : sub.event.eventFilter) {
        record.slices.emplace_back(snssai.sst, snssai.sd);
    }
    if (sub.event.notifThreshold.has_value()) {
        const auto& t = *sub.event.notifThreshold;
        // The schema carries UE and PDU forms separately; take whichever matches this
        // subscription's own event type rather than the first one present.
        if (record.event_type == sbi_gen::SACEventType::NUM_OF_REGD_UES) {
            record.threshold_absolute = t.numericValNumUes;
            record.threshold_percent = t.percValueNumUes;
        } else {
            record.threshold_absolute = t.numericValNumPduSess;
            record.threshold_percent = t.percValueNumPduSess;
        }
    }
    record.remain_reports = sub.maxReports;
    if (sub.event.notificationPeriod.has_value()) {
        record.period_seconds = *sub.event.notificationPeriod;
    }
    record.correlation_id = sub.notifyCorrelationId;
    record.raw = raw;
    return record;
}

// Runs on a dedicated thread, never on the server's io_context -- same reasoning as
// nfs/ausf/src/main.cpp's run_nrf_lifecycle (docs/DECISIONS.md ADR-0006/ADR-0019).
void run_nrf_lifecycle(const std::string& nsacf_instance_id,
                       const std::string& nrf_base,
                       const std::string& advertised_ipv4) {
    sbi_core::http2::TlsConfig client_tls{
        .cert_path = CERTS_DIR "/nsacf/cert.pem",
        .key_path = CERTS_DIR "/nsacf/key.pem",
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
        http_client, nrf_base + "/oauth2/token", nsacf_instance_id, "nnrf-nfm", "NRF");

    constexpr int kHeartbeatSeconds = 30;
    json profile{
        {"nfInstanceId", nsacf_instance_id},
        {"nfType", kNfType},
        {"nfStatus", "REGISTERED"},
        // ADR-0453/follow-up: NRF's NFProfile validation requires a real dotted-quad IPv4
        // literal here, not a Docker Compose hostname -- see nfs/udsf/src/main.cpp's fix for the
        // same bug, root-caused live.
        {"ipv4Addresses", json::array({"127.0.0.1"})},
        {"heartBeatTimer", kHeartbeatSeconds},
    };

    while (true) {
        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("nsacf: OAuth2 token fetch failed: {}", token.error());
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        sbi_core::http2::ClientRequest put_req;
        put_req.method = "PUT";
        put_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + nsacf_instance_id;
        put_req.headers.emplace("content-type", "application/json");
        put_req.headers.emplace("authorization", "Bearer " + *token);
        put_req.headers.emplace(
            sbi_core::headers::kSenderTimestamp,
            sbi_core::headers::format_sender_timestamp(std::chrono::system_clock::now()));
        put_req.body = profile.dump();
        auto put_resp = http_client.send(put_req);
        if (put_resp.has_value() && (put_resp->status == 200 || put_resp->status == 201)) {
            spdlog::info("nsacf: registered with NRF (HTTP {})", put_resp->status);
            break;
        }
        spdlog::warn("nsacf: NRF registration attempt failed, retrying in 5s");
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(kHeartbeatSeconds / 2));

        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("nsacf: OAuth2 token fetch failed for heartbeat: {}", token.error());
            continue;
        }

        sbi_core::http2::ClientRequest patch_req;
        patch_req.method = "PATCH";
        patch_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + nsacf_instance_id;
        patch_req.headers.emplace("content-type", "application/json-patch+json");
        patch_req.headers.emplace("authorization", "Bearer " + *token);
        patch_req.body =
            json::array({json{{"op", "replace"}, {"path", "/nfStatus"}, {"value", "REGISTERED"}}})
                .dump();

        auto patch_resp = http_client.send(patch_req);
        if (!patch_resp.has_value() || patch_resp->status != 200) {
            spdlog::warn("nsacf: heartbeat failed");
        }
    }
}

} // namespace

int main() {
    sbi_core::init_logging("nsacf");
    sbi_core::init_tracing("nsacf");

    // ADR-0077/ADR-0273: no deployment parameter is a literal in source.
    const auto config = nf_config::load("nsacf", CONFIG_DIR);
    const auto port = nf_config::require<unsigned short>(config, "port");
    const auto metrics_bind_address =
        nf_config::require<std::string>(config, "metrics_bind_address");
    const auto nrf_base =
        nf_config::require<std::string>(config, "nrf_base_url", "NSACF_NRF_BASE_URL");
    const auto advertised_ipv4 =
        nf_config::require<std::string>(config, "advertised_ipv4", "NSACF_ADVERTISED_IPV4");

    sbi_core::init_metrics(metrics_bind_address);

    const std::string nsacf_instance_id = sbi_core::generate_uuid_v4();
    spdlog::info("nsacf: starting, nfInstanceId={}", nsacf_instance_id);

    sbi_core::http2::TlsConfig server_tls{
        .cert_path = CERTS_DIR "/nsacf/cert.pem",
        .key_path = CERTS_DIR "/nsacf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };

    sbi_core::jwt::Verifier verifier(CERTS_DIR "/nrf-jwt/public.pem", kNrfInstanceId);

    nsacf::SliceAdmissionStore slices;
    configure_slices_from_config(config, slices);
    nsacf::SacSubscriptionStore subscriptions;
    nsacf::EacModeStore eac;

    // Local policy, not a 3GPP field -- see this file's own header disclosure.
    const auto eac_activation_percent =
        nf_config::require<std::int64_t>(config, "eac_activation_percent");

    // Notifications are sent on the server's own threads (a route handler delivers the reports its
    // own admission decision triggered), so this client is shared and must outlive them.
    sbi_core::http2::TlsConfig notify_tls{
        .cert_path = CERTS_DIR "/nsacf/cert.pem",
        .key_path = CERTS_DIR "/nsacf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client notify_client(std::move(notify_tls));

    auto meter = sbi_core::get_meter("nsacf");
    auto ue_admit_counter =
        meter->CreateUInt64Counter("nsacf_ue_admissions_total", "UEs admitted onto a slice");
    auto ue_reject_counter = meter->CreateUInt64Counter(
        "nsacf_ue_rejections_total", "UE admission requests rejected at the slice maximum");
    auto pdu_admit_counter = meter->CreateUInt64Counter("nsacf_pdu_admissions_total",
                                                        "PDU sessions admitted onto a slice");
    auto pdu_reject_counter =
        meter->CreateUInt64Counter("nsacf_pdu_rejections_total",
                                   "PDU session admission requests rejected at the slice maximum");

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

    // --- Nnsacf_NSAC: NumOfUEsUpdate ---
    //
    // Returns 204 when every requested operation succeeded and 200 with the failure list when some
    // did not -- the YAML's own two success codes ("Successful ACU operation" / "Partial
    // successful ACU operation"), not an invention. A slice this NSACF has no quota for is
    // SLICE_NOT_FOUND, which is deliberately distinct from a slice whose quota is full.
    server.add_route(
        "POST",
        std::string(kNsacApiRoot) + "/slices/ues",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::UeACRequestData>(req, err);
            if (!body.has_value()) {
                return err;
            }

            std::vector<sbi_gen::AcuFailureItem> failures;
            for (const auto& info : body->ueACRequestInfo) {
                for (const auto& op : info.acuOperationList) {
                    const auto key = nsacf::slice_key(op.snssai.sst, op.snssai.sd);
                    if (op.updateFlag.value == sbi_gen::AcuFlag::DECREASE) {
                        slices.release_ue(key, info.supi);
                        continue;
                    }
                    // INCREASE and UPDATE both assert "this UE is on this slice now". UPDATE is
                    // the spec's re-assertion after a change of serving PLMN/access, which is
                    // exactly the idempotent case the identity set handles.
                    const auto result = slices.admit_ue(key, info.supi);
                    if (!result.slice_known) {
                        sbi_gen::AcuFailureItem item;
                        item.snssai = op.snssai;
                        item.reason.value = sbi_gen::AcuFailureReason::SLICE_NOT_FOUND;
                        failures.push_back(item);
                        continue;
                    }
                    if (!result.admitted) {
                        ue_reject_counter->Add(1);
                        sbi_gen::AcuFailureItem item;
                        item.snssai = op.snssai;
                        item.reason.value = sbi_gen::AcuFailureReason::EXCEED_MAX_UE_NUM;
                        failures.push_back(item);
                        spdlog::info("nsacf: slice {} at its maximum of {} UE(s) -- {} rejected",
                                     key,
                                     result.maximum,
                                     info.supi);
                        continue;
                    }
                    ue_admit_counter->Add(1);
                }
            }

            // Whatever the outcome, the counts moved: report the slices this request touched and
            // re-evaluate their EAC mode. Done after the whole request rather than per operation,
            // so one request produces at most one report per slice.
            if (body->eacNotificationUri.has_value()) {
                eac.remember_notification_uri(*body->eacNotificationUri);
            }
            for (const auto& info : body->ueACRequestInfo) {
                for (const auto& op : info.acuOperationList) {
                    report_threshold_events(notify_client,
                                            subscriptions,
                                            slices,
                                            sbi_gen::SACEventType::NUM_OF_REGD_UES,
                                            op.snssai.sst,
                                            op.snssai.sd);
                    evaluate_eac_mode(notify_client,
                                      eac,
                                      slices,
                                      op.snssai.sst,
                                      op.snssai.sd,
                                      eac_activation_percent);
                }
            }

            if (failures.empty()) {
                sbi_core::http2::Response resp;
                resp.status = 204;
                return resp;
            }
            sbi_gen::UeACResponseData resp_body;
            resp_body.acuFailureList = json(failures);
            return sbi_core::http2::Response::json(200, json(resp_body).dump());
        });

    // --- Nnsacf_NSAC: NumOfPDUsUpdate ---
    server.add_route(
        "POST",
        std::string(kNsacApiRoot) + "/slices/pdus",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::PduACRequestData>(req, err);
            if (!body.has_value()) {
                return err;
            }

            std::vector<sbi_gen::AcuFailureItem> failures;
            for (const auto& info : body->pduACRequestInfo) {
                for (const auto& op : info.acuOperationList) {
                    const auto key = nsacf::slice_key(op.snssai.sst, op.snssai.sd);
                    if (op.updateFlag.value == sbi_gen::AcuFlag::DECREASE) {
                        slices.release_pdu(key, info.supi, info.pduSessionId);
                        continue;
                    }
                    const auto result = slices.admit_pdu(key, info.supi, info.pduSessionId);
                    if (!result.slice_known) {
                        sbi_gen::AcuFailureItem item;
                        item.snssai = op.snssai;
                        item.reason.value = sbi_gen::AcuFailureReason::SLICE_NOT_FOUND;
                        item.pduSessionId = info.pduSessionId;
                        failures.push_back(item);
                        continue;
                    }
                    if (!result.admitted) {
                        pdu_reject_counter->Add(1);
                        sbi_gen::AcuFailureItem item;
                        item.snssai = op.snssai;
                        item.reason.value = sbi_gen::AcuFailureReason::EXCEED_MAX_PDU_NUM;
                        item.pduSessionId = info.pduSessionId;
                        failures.push_back(item);
                        spdlog::info(
                            "nsacf: slice {} at its maximum of {} PDU session(s) -- {}/{} rejected",
                            key,
                            result.maximum,
                            info.supi,
                            info.pduSessionId);
                        continue;
                    }
                    pdu_admit_counter->Add(1);
                }
            }

            for (const auto& info : body->pduACRequestInfo) {
                for (const auto& op : info.acuOperationList) {
                    report_threshold_events(notify_client,
                                            subscriptions,
                                            slices,
                                            sbi_gen::SACEventType::NUM_OF_ESTD_PDU_SESSIONS,
                                            op.snssai.sst,
                                            op.snssai.sd);
                }
            }

            if (failures.empty()) {
                sbi_core::http2::Response resp;
                resp.status = 204;
                return resp;
            }
            sbi_gen::PduACResponseData resp_body;
            resp_body.acuFailureList = json(failures);
            return sbi_core::http2::Response::json(200, json(resp_body).dump());
        });

    // --- Nnsacf_NSAC: LocalNumberUpdate ---
    //
    // The spec's own way for an operator/OAM path to change this NSACF's local maxima at runtime.
    // 204 is this operation's only success code in the YAML.
    server.add_route(
        "POST",
        std::string(kNsacApiRoot) + "/slices/local-configs/update",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::ACUpdateData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto key = nsacf::slice_key(body->snssai.sst, body->snssai.sd);
            const auto existing = slices.quotas(key);
            // Both maxima are OPTIONAL in the real schema: a request carrying only one changes
            // only that one, rather than silently zeroing the other.
            const std::int64_t max_ues = body->maxUesNumber.has_value()
                                             ? *body->maxUesNumber
                                             : (existing.has_value() ? existing->first : 0);
            const std::int64_t max_pdus = body->maxPdusNumber.has_value()
                                              ? *body->maxPdusNumber
                                              : (existing.has_value() ? existing->second : 0);
            slices.configure(key, max_ues, max_pdus);
            spdlog::info("nsacf: LocalNumberUpdate -- slice {} now admits {} UE(s), {} PDU(s)",
                         key,
                         max_ues,
                         max_pdus);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nnsacf_NSAC: QuotaUpdate ---
    //
    // Reads the maxima back. `quotaType` selects which of the two the answer carries; a slice this
    // NSACF does not know is a real 404, not an answer of zero.
    server.add_route(
        "POST",
        std::string(kNsacApiRoot) + "/slices/roaming-quotas/query",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::QuotaUpdateRequestData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto key = nsacf::slice_key(body->snssai.sst, body->snssai.sd);
            const auto quotas = slices.quotas(key);
            if (!quotas.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Slice Not Found", "No admission configuration for S-NSSAI " + key);
            }
            sbi_gen::QuotaUpdateResponseData resp_body;
            resp_body.snssai = body->snssai;
            if (body->quotaType.value == sbi_gen::SliceQuotaType::MAX_UE_NUM ||
                body->quotaType.value == sbi_gen::SliceQuotaType::BOTH) {
                resp_body.maxUesNumber = quotas->first;
            }
            if (body->quotaType.value == sbi_gen::SliceQuotaType::MAX_PDU_NUM ||
                body->quotaType.value == sbi_gen::SliceQuotaType::BOTH) {
                resp_body.maxPdusNumber = quotas->second;
            }
            return sbi_core::http2::Response::json(200, json(resp_body).dump());
        });

    // --- Nnsacf_SliceEventExposure: subscription CRUD ---
    //
    // Stored and returned faithfully. Nothing publishes to them yet -- see this file's own header
    // disclosure; that is a missing reporting path, not a missing CRUD surface.
    server.add_route(
        "POST",
        std::string(kEeApiRoot) + "/subscriptions",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::SACEventSubscription>(req, err);
            if (!body.has_value()) {
                return err;
            }
            if (body->event.eventFilter.empty()) {
                return sbi_core::http2::problem_response(
                    400,
                    "Missing or invalid mandatory IE",
                    "SACEvent.eventFilter must name at least one S-NSSAI (minItems: 1)");
            }

            const auto id = subscriptions.create(to_record(*body, req.body));

            // The 201 body is CreatedSACEventSubscription, NOT the request echoed back:
            // {subscription, subscriptionId, report?}. An earlier draft of this NF returned the
            // raw request, which the schema does not allow -- a real conformance bug, fixed here
            // rather than left because the routes "worked".
            sbi_gen::CreatedSACEventSubscription created;
            created.subscription = *body;
            created.subscriptionId = id;

            // immediateFlag: the spec's own way to ask for one report at subscription time,
            // carried in the creation response rather than as a separate POST.
            if (body->event.immediateFlag.value_or(false) && !body->event.eventFilter.empty()) {
                const auto& first = body->event.eventFilter.front();
                const auto key = nsacf::slice_key(first.sst, first.sd);
                const bool is_ue =
                    body->event.eventType.value == sbi_gen::SACEventType::NUM_OF_REGD_UES;
                created.report =
                    make_report_item(body->event.eventType.value,
                                     first.sst,
                                     first.sd,
                                     is_ue ? slices.ue_count(key) : slices.pdu_count(key),
                                     body->maxReports,
                                     true);
            }

            sbi_core::http2::Response resp =
                sbi_core::http2::Response::json(201, json(created).dump());
            resp.headers.emplace("location", std::string(kEeApiRoot) + "/subscriptions/" + id);
            spdlog::info("nsacf: created slice event subscription {} ({}, {}) watching {} slice(s)",
                         id,
                         body->event.eventType.value,
                         body->event.eventTrigger.has_value() ? body->event.eventTrigger->value
                                                              : "THRESHOLD (default)",
                         body->event.eventFilter.size());
            return resp;
        });

    server.add_route(
        "PATCH",
        std::string(kEeApiRoot) + "/subscriptions/{subscriptionId}",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            auto existing = subscriptions.get_raw(id);
            if (!existing.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Subscription Not Found", "No subscription with id " + id);
            }
            json current;
            json patch;
            try {
                current = json::parse(*existing);
                patch = json::parse(req.body);
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(
                    400, "Missing or invalid mandatory IE", e.what());
            }
            // A merge, which is what SACSubscriptionPatch is: named fields replace, absent fields
            // are left alone. Not an RFC 6902 patch document -- the YAML's PATCH body is a partial
            // representation, so applying it as one would be wrong.
            current.merge_patch(patch);
            // Re-parse the merged result: a PATCH can change the threshold, the period or the
            // watched slices, and the reporting loop reads the RECORD, not the raw body. Storing
            // the merged JSON without rebuilding the record would leave reports running against
            // the pre-patch subscription -- a silent divergence.
            sbi_gen::SACEventSubscription merged;
            try {
                merged = current.get<sbi_gen::SACEventSubscription>();
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(
                    400, "Missing or invalid mandatory IE", e.what());
            }
            subscriptions.replace(id, to_record(merged, current.dump()));
            return sbi_core::http2::Response::json(200, current.dump());
        });

    server.add_route(
        "PUT",
        std::string(kEeApiRoot) + "/subscriptions/{subscriptionId}",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::SACEventSubscription>(req, err);
            if (!body.has_value()) {
                return err;
            }
            if (!subscriptions.replace(id, to_record(*body, req.body))) {
                return sbi_core::http2::problem_response(
                    404, "Subscription Not Found", "No subscription with id " + id);
            }
            return sbi_core::http2::Response::json(200, req.body);
        });

    server.add_route(
        "DELETE",
        std::string(kEeApiRoot) + "/subscriptions/{subscriptionId}",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            if (!subscriptions.remove(id)) {
                return sbi_core::http2::problem_response(
                    404, "Subscription Not Found", "No subscription with id " + id);
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    std::thread(run_nrf_lifecycle, nsacf_instance_id, nrf_base, advertised_ipv4).detach();
    // ADR-0394 (extended): NOT detached -- see run_periodic_reporting's own header comment. Joined
    // below, after run_multi_threaded(ioc) returns, so subscriptions/slices are not destructed
    // while this thread might still be using them.
    std::atomic<bool> periodic_reporting_stop{false};
    sbi_core::on_shutdown_signal(
        [&periodic_reporting_stop] { periodic_reporting_stop.store(true); });
    std::thread periodic_reporting_thread(run_periodic_reporting,
                                          std::ref(subscriptions),
                                          std::ref(slices),
                                          std::ref(periodic_reporting_stop));

    server.start();
    spdlog::info("nsacf: listening on https://0.0.0.0:{} (TLS 1.3 + mTLS)", port);
    spdlog::info("nsacf: Prometheus metrics at http://{}/metrics", metrics_bind_address);
    sbi_core::run_multi_threaded(ioc);
    periodic_reporting_thread.join();
    return 0;
}
