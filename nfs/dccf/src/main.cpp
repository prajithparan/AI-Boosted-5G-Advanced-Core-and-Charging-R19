// DCCF -- Data Collection Coordination Function (ADR-0366; architecture ADR-0359 step 2).
//
// Stage 2: TS 23.288 V19.7.0 clause 5A.2 (data collection coordination), 5A.3.2 (delivery via a
// Messaging Framework), procedure 6.2.6.3.4. Stage 3: TS 29.574 V19.7.0 and its two R19 YAML
// files (forge.3gpp.org, REL-19, commit bca84b6), every DTO generated, every API root the YAML's
// servers[0].url (ADR-0325):
//
//   Ndccf_DataManagement     {apiRoot}/ndccf-datamanagement/v1     Subscribe (POST/PUT) and
//                                                                  Unsubscribe (DELETE) for
//                                                                  /data-subscriptions and
//                                                                  /analytics-subscriptions
//   Ndccf_ContextManagement  {apiRoot}/ndccf-contextmanagement/v1  Register/Update/Deregister
//                                                                  of /data-collection-profiles
//
// What the DCCF does with a subscription (TS 23.288 6.2.6.3.4, steps 3-6 and 12-14):
//   1. fingerprints the source-facing part of it (nfs/dccf/src/subscription_store.hpp) and looks
//      for a collection already serving the same data -- 5A.2's "already being collected";
//   2. new data: configures the MFAF (Nmfaf_3daDataManagement_Configure) with the consumer's
//      notification target + correlation, receives the MFAF Notification Target Address, and
//      subscribes the Data Source (or the NWDAF for analytics) with THAT address as its notify
//      target, so every notification flows Source -> MFAF -> consumer(s);
//   3. known data: adds the consumer to the MFAF configuration (one more MessageConfiguration
//      against the same mfafCorreId) -- the source is not touched;
//   4. on unsubscribe, the reverse: remove the consumer; when it was the last, deconfigure the
//      MFAF and unsubscribe the source.
// Delivery itself is the MFAF's (Nmfaf_3caDataManagement_Notify, "the same content as those
// sent via a Ndccf_DataManagement service", 5A.3.2) -- this DCCF does not deliver directly.
//
// Data Sources this DCCF can subscribe: the ones this project has producers for -- AMF
// (Namf_EventExposure), NRF (Nnrf_NFManagement NFStatusSubscribe), NSACF
// (Nnsacf_SliceEventExposure) -- and the NWDAF (Nnwdaf_EventsSubscription) for analytics. A
// data subscription naming any other source (SMF, UDM, NEF, AF, UPF, GMLC, LMF, PCF) is
// answered with TS 29.574's own SUBSCRIPTION_CANNOT_BE_SERVED (table 5.1.7.3-1), never accepted
// and silently unserved. Peers are located by config base URLs, the pattern nfs/smf uses for
// AMF/PCF (config mandate: nothing hardcoded); targetNfId/targetNfSetId are accepted and logged,
// not used for selection.
//
// DISCLOSED, not hidden (features per TS 29.574 table 5.1.8-1):
//   * Supported features advertised: none. UserConsent handling is a config policy:
//     "consumer-checked" rejects a per-UE subscription (supi/gpsi in the source subscription)
//     that does not carry checkedConsentInd with 403 USER_CONSENT_NOT_GRANTED; the Nudm_SDM
//     user-consent lookup and the Nudm_SDM_Subscribe for consent changes (4.2.2.2, step 2 of the
//     procedure) are NOT built -- this project's UDM/UDR have no user-consent data yet.
//     "not-enforced" accepts and logs. DataAnaCollect (notifEndpoints, storeInd) and EnhDataMgmt
//     (muting, storage handling, deletion alerts, pending notifications) are not built.
//   * Formatting/processing instructions are passed through to the MFAF, which applies exactly
//     what ADR-0365 says it applies (consTrigNotif) and logs the rest.
//   * Ndccf_DataManagement_Transfer (/transfer-data-sub), Fetch (the MFAF's fetch serves the
//     consumer), and Notify (the MFAF's) have no DCCF-side implementation; /transfer-data-sub is
//     not registered (404), disclosed here rather than answered with an invented status.
//   * immReport, timePeriod, adrfId/ardfSetId: accepted, logged, not applied (ADRF is step 3).
//   * A PUT that changes the fingerprint is an unsubscribe of the old collection and a subscribe
//     to the new one, with the same subscriptionId.
//   * LI: TS 33.127 has no DCCF-specific POI clause; nothing built.
#include "sbi_core/http2_client.hpp"
#include "sbi_core/http2_server.hpp"
#include "sbi_core/io_context_pool.hpp"
#include "sbi_core/json_body.hpp"
#include "sbi_core/jwt.hpp"
#include "sbi_core/logging.hpp"
#include "sbi_core/metrics.hpp"
#include "sbi_core/oauth2_client.hpp"
#include "sbi_core/otel.hpp"
#include "sbi_core/problem_details.hpp"
#include "sbi_core/rate_limit.hpp"
#include "sbi_core/uuid.hpp"

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "TS26510_CommonData_grp.hpp"
#include "TS29574_Ndccf_ContextManagement.hpp"
#include "TS29576_Nmfaf_3daDataManagement.hpp"
#include "nf_config/nf_config.hpp"
#include "nf_config/redis.hpp"
#include "subscription_store.hpp"

using json = nlohmann::json;

namespace {

constexpr const char* kNfType = "DCCF";
// Must match nfs/nrf/src/main.cpp's kNrfInstanceId exactly -- see docs/DECISIONS.md ADR-0018.
constexpr const char* kNrfInstanceId = "5ba9a927-1d31-4c8e-8a10-000000000001";
// servers[0].url of each YAML, verified by tests/conformance's api_root_conformance (ADR-0325).
constexpr const char* kDataManagementRoot = "/ndccf-datamanagement/v1";
constexpr const char* kContextManagementRoot = "/ndccf-contextmanagement/v1";
// The peers' roots -- each the servers[0].url of ITS YAML, used as a client here.
constexpr const char* kMfaf3daRoot = "/nmfaf-3dadatamanagement/v1";
constexpr const char* kAmfEventExposureRoot = "/namf-evts/v1";
constexpr const char* kNrfNfmRoot = "/nnrf-nfm/v1";
constexpr const char* kNsacfSliceEeRoot = "/nnsacf-slice-ee/v1";
constexpr const char* kNwdafEventsSubscriptionRoot = "/nnwdaf-eventssubscription/v1";

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

sbi_core::http2::Response
problem(int status, const std::string& title, const std::string& detail, const char* cause) {
    auto pd = sbi_core::make_problem_details(status, title, detail, cause);
    sbi_core::http2::Response r;
    r.status = status;
    r.headers.emplace("content-type", "application/problem+json");
    r.body = json(pd).dump();
    return r;
}

// One Data Source kind: where it is, which OAuth scope its API declares, how its subscription
// carries the notification target, and how the DCCF points that target at the MFAF.
struct SourceKind {
    const char* name;                      // for the log and the collection record
    const char* nf_type;                   // OAuth2 target NF type
    const char* scope;                     // the YAML's securitySchemes scope
    std::string base_url;                  // from config
    std::string subscriptions_path;        // {root}/subscriptions
    std::vector<std::string> notif_fields; // stripped from the fingerprint, overwritten below
    // Rewrites the source subscription's notify target/correlation to the MFAF's and wraps it in
    // the POST body the source's YAML expects. Returns the body.
    json (*aim_at_mfaf)(const json& source_sub,
                        const std::string& mfaf_notif_uri,
                        const std::string& mfaf_corre_id,
                        const std::string& dccf_instance_id);
};

json aim_amf(const json& sub,
             const std::string& uri,
             const std::string& corr,
             const std::string& me) {
    // TS 29.518 AmfCreateEventSubscription { subscription: AmfEventSubscription }; the
    // notify target is eventNotifyUri + notifyCorrelationId, nfId is the subscribing NF's.
    json s = sub;
    s["eventNotifyUri"] = uri;
    s["notifyCorrelationId"] = corr;
    s["nfId"] = me;
    return json{{"subscription", s}};
}

json aim_nrf(const json& sub, const std::string& uri, const std::string&, const std::string& me) {
    // TS 29.510 SubscriptionData: nfStatusNotificationUri; NotificationData carries no
    // correlation id -- the MFAF's inbound URI carries the mfafCorreId, which is enough.
    json s = sub;
    s["nfStatusNotificationUri"] = uri;
    s["reqNfInstanceId"] = me;
    return s;
}

json aim_nsacf(const json& sub,
               const std::string& uri,
               const std::string& corr,
               const std::string& me) {
    // TS 29.536 SACEventSubscription: eventNotifyUri, notifyCorrelationId, nfId.
    json s = sub;
    s["eventNotifyUri"] = uri;
    s["notifyCorrelationId"] = corr;
    s["nfId"] = me;
    return s;
}

json aim_nwdaf(const json& sub,
               const std::string& uri,
               const std::string& corr,
               const std::string&) {
    // TS 29.520 NnwdafEventsSubscription: notificationURI, notifCorrId.
    json s = sub;
    s["notificationURI"] = uri;
    s["notifCorrId"] = corr;
    return s;
}

// Which DataSubscription alternative the request carries, per TS 29.575's oneOf. Exactly one.
struct Alternative {
    std::string attribute; // "amfDataSub", ...
    json body;
};

std::optional<Alternative> single_alternative(const json& data_sub, std::string& err) {
    static const char* kAlternatives[] = {"amfDataSub",
                                          "smfDataSub",
                                          "udmDataSub",
                                          "nefDataSub",
                                          "afDataSub",
                                          "nrfDataSub",
                                          "nsacfDataSub",
                                          "upfDataSub",
                                          "gmlcDataSub",
                                          "lmfDataSub",
                                          "pcfDataSub"};
    std::optional<Alternative> found;
    for (const char* a : kAlternatives) {
        if (data_sub.contains(a)) {
            if (found) {
                err = "dataSub carries more than one source subscription (TS 29.575 "
                      "DataSubscription is a oneOf)";
                return std::nullopt;
            }
            found = Alternative{a, data_sub.at(a)};
        }
    }
    if (!found) {
        err = "dataSub carries no source subscription (TS 29.575 DataSubscription is a oneOf)";
    }
    return found;
}

bool targets_a_user(const json& source_sub) {
    return source_sub.contains("supi") || source_sub.contains("gpsi") ||
           source_sub.contains("supis") || source_sub.contains("gpsis");
}

// ---- NRF lifecycle (same shape as every other NF, ADR-0006/0019) --------------------------------

void run_nrf_lifecycle(const std::string& instance_id,
                       const std::string& nrf_base,
                       const std::string& advertised_ipv4,
                       int heartbeat_seconds) {
    sbi_core::http2::TlsConfig client_tls{
        .cert_path = CERTS_DIR "/dccf/cert.pem",
        .key_path = CERTS_DIR "/dccf/key.pem",
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
        http_client, nrf_base + "/oauth2/token", instance_id, "nnrf-nfm", "NRF");
    const auto service = [](const char* name) {
        return json{{"serviceInstanceId", name},
                    {"serviceName", name},
                    {"versions",
                     json::array({json{{"apiVersionInUri", "v1"}, {"apiFullVersion", "1.0.0"}}})},
                    {"scheme", "https"},
                    {"nfServiceStatus", "REGISTERED"}};
    };
    json profile{
        {"nfInstanceId", instance_id},
        {"nfType", kNfType},
        {"nfStatus", "REGISTERED"},
        {"ipv4Addresses", json::array({advertised_ipv4})},
        {"heartBeatTimer", heartbeat_seconds},
        {"nfServices",
         json::array({service("ndccf-datamanagement"), service("ndccf-contextmanagement")})},
    };
    while (true) {
        auto token = oauth.get_bearer_token();
        if (!token) {
            spdlog::warn("dccf: NRF token unavailable ({}), retrying", token.error());
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }
        sbi_core::http2::ClientRequest put_req;
        put_req.method = "PUT";
        put_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + instance_id;
        put_req.headers.emplace("content-type", "application/json");
        put_req.headers.emplace("authorization", "Bearer " + *token);
        put_req.body = profile.dump();
        auto put_resp = http_client.send(put_req);
        if (put_resp && (put_resp->status == 200 || put_resp->status == 201)) {
            spdlog::info("dccf: registered with NRF (HTTP {})", put_resp->status);
            break;
        }
        spdlog::warn("dccf: NRF registration failed ({}), retrying",
                     put_resp ? std::to_string(put_resp->status) : put_resp.error());
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(heartbeat_seconds / 2));
        auto token = oauth.get_bearer_token();
        if (!token) {
            continue;
        }
        sbi_core::http2::ClientRequest patch_req;
        patch_req.method = "PATCH";
        patch_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + instance_id;
        patch_req.headers.emplace("content-type", "application/json-patch+json");
        patch_req.headers.emplace("authorization", "Bearer " + *token);
        patch_req.body =
            json::array({json{{"op", "replace"}, {"path", "/nfStatus"}, {"value", "REGISTERED"}}})
                .dump();
        http_client.send(patch_req);
    }
}

} // namespace

int main() {
    sbi_core::init_logging("dccf");
    sbi_core::init_tracing("dccf");
    const auto config = nf_config::load("dccf", CONFIG_DIR);
    // Every port and tunable from config, env-overridable (user mandate). Nothing below is a
    // literal.
    const auto port = nf_config::require<unsigned short>(config, "port", "DCCF_PORT");
    const auto metrics_bind_address = nf_config::require<std::string>(
        config, "metrics_bind_address", "DCCF_METRICS_BIND_ADDRESS");
    const auto nrf_base =
        nf_config::require<std::string>(config, "nrf_base_url", "DCCF_NRF_BASE_URL");
    const auto advertised_ipv4 =
        nf_config::require<std::string>(config, "advertised_ipv4", "DCCF_ADVERTISED_IPV4");
    const auto redis_url = nf_config::require<std::string>(config, "redis_url", "DCCF_REDIS_URL");
    const auto heartbeat_seconds =
        nf_config::require<int>(config, "nrf_heartbeat_seconds", "DCCF_NRF_HEARTBEAT_SECONDS");
    const auto mfaf_base =
        nf_config::require<std::string>(config, "mfaf_base_url", "DCCF_MFAF_BASE_URL");
    const auto amf_base =
        nf_config::require<std::string>(config, "amf_base_url", "DCCF_AMF_BASE_URL");
    const auto nsacf_base =
        nf_config::require<std::string>(config, "nsacf_base_url", "DCCF_NSACF_BASE_URL");
    const auto nwdaf_base =
        nf_config::require<std::string>(config, "nwdaf_base_url", "DCCF_NWDAF_BASE_URL");
    const auto consent_policy =
        nf_config::require<std::string>(config, "user_consent_policy", "DCCF_USER_CONSENT_POLICY");
    if (consent_policy != "consumer-checked" && consent_policy != "not-enforced") {
        spdlog::critical("dccf: user_consent_policy must be \"consumer-checked\" or "
                         "\"not-enforced\", got \"{}\"",
                         consent_policy);
        return 1;
    }

    sbi_core::init_metrics(metrics_bind_address);
    const std::string instance_id = sbi_core::generate_uuid_v4();
    spdlog::info("dccf: starting, nfInstanceId={}", instance_id);

    sbi_core::http2::TlsConfig server_tls{
        .cert_path = CERTS_DIR "/dccf/cert.pem",
        .key_path = CERTS_DIR "/dccf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::jwt::Verifier verifier(CERTS_DIR "/nrf-jwt/public.pem", kNrfInstanceId);
    sbi_core::http2::TlsConfig client_tls{
        .cert_path = CERTS_DIR "/dccf/cert.pem",
        .key_path = CERTS_DIR "/dccf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client client(std::move(client_tls));
    std::mutex client_mutex; // one client, used from every request handler
    // One OAuth2 client per producer API the DCCF consumes (TS 29.510 access tokens are scoped).
    sbi_core::OAuth2Client oauth_mfaf(
        client, nrf_base + "/oauth2/token", instance_id, "nmfaf-3dadatamanagement", "MFAF");
    sbi_core::OAuth2Client oauth_amf(
        client, nrf_base + "/oauth2/token", instance_id, "namf-evts", "AMF");
    sbi_core::OAuth2Client oauth_nrf(
        client, nrf_base + "/oauth2/token", instance_id, "nnrf-nfm", "NRF");
    sbi_core::OAuth2Client oauth_nsacf(
        client, nrf_base + "/oauth2/token", instance_id, "nnsacf-slice-ee", "NSACF");
    sbi_core::OAuth2Client oauth_nwdaf(
        client, nrf_base + "/oauth2/token", instance_id, "nnwdaf-eventssubscription", "NWDAF");

    dccf::SubscriptionStore store(nf_config::connect_redis_or_die(redis_url, "dccf"));

    auto meter = sbi_core::get_meter("dccf");
    auto subscriptions_counter = meter->CreateUInt64Counter(
        "dccf_subscriptions_created_total", "Ndccf_DataManagement subscriptions created");
    auto shared_counter = meter->CreateUInt64Counter(
        "dccf_subscriptions_shared_total",
        "Subscriptions served by an existing source collection (no new source subscription)");
    auto source_subs_counter = meter->CreateUInt64Counter(
        "dccf_source_subscriptions_total", "Subscriptions created at Data Sources / NWDAF");
    auto rejected_counter = meter->CreateUInt64Counter(
        "dccf_subscriptions_rejected_total",
        "Subscriptions answered SUBSCRIPTION_CANNOT_BE_SERVED or USER_CONSENT_NOT_GRANTED");

    const SourceKind kAmf{"AMF",
                          "AMF",
                          "namf-evts",
                          amf_base,
                          std::string(kAmfEventExposureRoot) + "/subscriptions",
                          {"eventNotifyUri",
                           "notifyCorrelationId",
                           "nfId",
                           "subsChangeNotifyUri",
                           "subsChangeNotifyCorrelationId"},
                          aim_amf};
    const SourceKind kNrf{"NRF",
                          "NRF",
                          "nnrf-nfm",
                          nrf_base,
                          std::string(kNrfNfmRoot) + "/subscriptions",
                          {"nfStatusNotificationUri", "reqNfInstanceId", "subscriptionId"},
                          aim_nrf};
    const SourceKind kNsacf{"NSACF",
                            "NSACF",
                            "nnsacf-slice-ee",
                            nsacf_base,
                            std::string(kNsacfSliceEeRoot) + "/subscriptions",
                            {"eventNotifyUri", "notifyCorrelationId", "nfId"},
                            aim_nsacf};
    const SourceKind kNwdaf{"NWDAF",
                            "NWDAF",
                            "nnwdaf-eventssubscription",
                            nwdaf_base,
                            std::string(kNwdafEventsSubscriptionRoot) + "/subscriptions",
                            {"notificationURI", "notifCorrId"},
                            aim_nwdaf};
    const auto oauth_for = [&](const SourceKind& k) -> sbi_core::OAuth2Client& {
        if (&k == &kAmf)
            return oauth_amf;
        if (&k == &kNrf)
            return oauth_nrf;
        if (&k == &kNsacf)
            return oauth_nsacf;
        return oauth_nwdaf;
    };

    // ---- the three peer interactions, each a plain SBI request with the right token -----------
    struct PeerResult {
        int status = -1;
        std::string body;
        std::string location;
        std::string error;
    };
    const auto call = [&](sbi_core::OAuth2Client& oauth,
                          const std::string& method,
                          const std::string& url,
                          const std::optional<json>& body) {
        PeerResult out;
        sbi_core::http2::ClientRequest req;
        req.method = method;
        req.url = url;
        if (body) {
            req.headers.emplace("content-type", "application/json");
            req.body = body->dump();
        }
        const std::lock_guard<std::mutex> lock(client_mutex);
        if (auto token = oauth.get_bearer_token()) {
            req.headers.emplace("authorization", "Bearer " + *token);
        } else {
            out.error = "no access token: " + token.error();
            return out;
        }
        auto resp = client.send(req);
        if (!resp) {
            out.error = resp.error();
            return out;
        }
        out.status = resp->status;
        out.body = resp->body;
        if (const auto it = resp->headers.find("location"); it != resp->headers.end()) {
            out.location = it->second;
        }
        return out;
    };

    // The MFAF configuration for a collection: one MessageConfiguration per consumer, all
    // bound to the same mfafNotiInfo once the MFAF has assigned it (TS 29.576 4.2.2.2.2).
    const auto message_configurations = [&](const std::vector<std::string>& consumer_ids,
                                            const std::optional<sbi_gen::MfafNotiInfo>& noti) {
        json mcs = json::array();
        for (const auto& id : consumer_ids) {
            const auto sub = store.get_subscription(id);
            if (!sub) {
                continue;
            }
            const bool data = sub->kind == dccf::Kind::Data;
            json mc{{"correId", sub->request.at(data ? "dataNotifCorrId" : "anaNotifCorrId")},
                    {"notificationURI", sub->request.at(data ? "dataNotifUri" : "anaNotifUri")}};
            if (sub->request.contains("formatInstruct")) {
                mc["formatInstruct"] = sub->request.at("formatInstruct");
            }
            if (sub->request.contains("procInstructs")) {
                mc["multiProcInstructs"] = sub->request.at("procInstructs");
            }
            if (noti) {
                mc["mfafNotiInfo"] = json(*noti);
            }
            mcs.push_back(std::move(mc));
        }
        return json{{"messageConfigurations", mcs}};
    };

    // Subscribe: returns the 201 body to send, or the error response.
    const auto subscribe = [&](dccf::Kind kind,
                               const SourceKind& source,
                               const json& source_sub,
                               const json& request,
                               const std::string& existing_id,
                               std::string& out_id) -> std::optional<sbi_core::http2::Response> {
        dccf::ConsumerSubscription sub;
        sub.kind = kind;
        sub.request = request;
        sub.fingerprint = dccf::fingerprint(source.name, source_sub, source.notif_fields);
        if (existing_id.empty()) {
            out_id = store.create_subscription(sub);
        } else {
            out_id = existing_id;
            store.replace_subscription(out_id, sub);
        }

        auto collection = store.get_source(sub.fingerprint);
        if (collection) {
            // 5A.2: already being collected -- add this consumer to the MFAF fan-out only.
            if (std::find(collection->consumers.begin(), collection->consumers.end(), out_id) ==
                collection->consumers.end()) {
                collection->consumers.push_back(out_id);
            }
            sbi_gen::MfafNotiInfo noti;
            noti.mfafNotifUri = collection->mfaf_notif_uri;
            noti.mfafCorreId = collection->mfaf_corre_id;
            auto r =
                call(oauth_mfaf,
                     "PUT",
                     mfaf_base + kMfaf3daRoot + "/configurations/" + collection->mfaf_trans_ref_id,
                     message_configurations(collection->consumers, noti));
            if (r.status != 200 && r.status != 204) {
                store.remove_subscription(out_id);
                return problem(500,
                               "Internal Server Error",
                               "MFAF rejected the updated configuration (" +
                                   std::to_string(r.status) + "): " + r.body + r.error,
                               "SYSTEM_FAILURE");
            }
            store.put_source(sub.fingerprint, *collection);
            shared_counter->Add(1);
            spdlog::info("dccf: {} joins collection {} ({} consumer(s), source {} untouched)",
                         out_id,
                         sub.fingerprint,
                         collection->consumers.size(),
                         source.name);
            return std::nullopt;
        }

        // New data: configure the MFAF first (step 5), then subscribe the source with the
        // MFAF's target (step 6).
        auto cfg = call(oauth_mfaf,
                        "POST",
                        mfaf_base + kMfaf3daRoot + "/configurations",
                        message_configurations({out_id}, std::nullopt));
        if (cfg.status != 201) {
            store.remove_subscription(out_id);
            return problem(500,
                           "Internal Server Error",
                           "MFAF did not accept the configuration (" + std::to_string(cfg.status) +
                               "): " + cfg.body + cfg.error,
                           "SYSTEM_FAILURE");
        }
        dccf::SourceCollection col;
        col.source = source.name;
        try {
            const auto created = json::parse(cfg.body).get<sbi_gen::MfafConfiguration>();
            const auto& noti = created.messageConfigurations->at(0).mfafNotiInfo;
            col.mfaf_notif_uri = noti->mfafNotifUri;
            col.mfaf_corre_id = noti->mfafCorreId;
        } catch (const std::exception& e) {
            store.remove_subscription(out_id);
            return problem(500,
                           "Internal Server Error",
                           std::string("MFAF configuration response lacks mfafNotiInfo: ") +
                               e.what(),
                           "SYSTEM_FAILURE");
        }
        col.mfaf_trans_ref_id = cfg.location.substr(cfg.location.find_last_of('/') + 1);

        const json body =
            source.aim_at_mfaf(source_sub, col.mfaf_notif_uri, col.mfaf_corre_id, instance_id);
        auto s = call(oauth_for(source),
                      "POST",
                      source.base_url + source.subscriptions_path,
                      std::optional<json>(body));
        if (s.status != 201) {
            call(oauth_mfaf,
                 "DELETE",
                 mfaf_base + kMfaf3daRoot + "/configurations/" + col.mfaf_trans_ref_id,
                 std::nullopt);
            store.remove_subscription(out_id);
            rejected_counter->Add(1);
            return problem(400,
                           "Bad Request",
                           std::string(source.name) + " did not accept the subscription (" +
                               std::to_string(s.status) + "): " + s.body + s.error,
                           "SUBSCRIPTION_CANNOT_BE_SERVED");
        }
        col.source_resource_uri =
            s.location.empty() ? source.base_url + source.subscriptions_path : s.location;
        if (!col.source_resource_uri.starts_with("http")) {
            col.source_resource_uri = source.base_url + col.source_resource_uri;
        }
        col.consumers = {out_id};
        store.put_source(sub.fingerprint, col);
        source_subs_counter->Add(1);
        spdlog::info("dccf: {} opens collection {} -- {} subscribed at {} via MFAF {} ({})",
                     out_id,
                     sub.fingerprint,
                     source.name,
                     col.source_resource_uri,
                     col.mfaf_trans_ref_id,
                     col.mfaf_corre_id);
        return std::nullopt;
    };

    // Unsubscribe one consumer; tears the collection down when it was the last (steps 12-14).
    const auto leave_collection = [&](const std::string& id,
                                      const dccf::ConsumerSubscription& sub) {
        auto collection = store.get_source(sub.fingerprint);
        if (!collection) {
            return;
        }
        collection->consumers.erase(
            std::remove(collection->consumers.begin(), collection->consumers.end(), id),
            collection->consumers.end());
        if (!collection->consumers.empty()) {
            sbi_gen::MfafNotiInfo noti;
            noti.mfafNotifUri = collection->mfaf_notif_uri;
            noti.mfafCorreId = collection->mfaf_corre_id;
            auto r =
                call(oauth_mfaf,
                     "PUT",
                     mfaf_base + kMfaf3daRoot + "/configurations/" + collection->mfaf_trans_ref_id,
                     message_configurations(collection->consumers, noti));
            if (r.status != 200 && r.status != 204) {
                spdlog::warn("dccf: MFAF configuration {} update after {} left failed ({})",
                             collection->mfaf_trans_ref_id,
                             id,
                             r.status);
            }
            store.put_source(sub.fingerprint, *collection);
            return;
        }
        const SourceKind* source = collection->source == "AMF"     ? &kAmf
                                   : collection->source == "NRF"   ? &kNrf
                                   : collection->source == "NSACF" ? &kNsacf
                                                                   : &kNwdaf;
        auto s = call(oauth_for(*source), "DELETE", collection->source_resource_uri, std::nullopt);
        if (s.status != 204 && s.status != 200) {
            spdlog::warn("dccf: {} unsubscribe at {} answered {}",
                         source->name,
                         collection->source_resource_uri,
                         s.status);
        }
        auto m = call(oauth_mfaf,
                      "DELETE",
                      mfaf_base + kMfaf3daRoot + "/configurations/" + collection->mfaf_trans_ref_id,
                      std::nullopt);
        if (m.status != 204) {
            spdlog::warn("dccf: MFAF deconfigure of {} answered {}",
                         collection->mfaf_trans_ref_id,
                         m.status);
        }
        store.remove_source(sub.fingerprint);
        spdlog::info("dccf: collection {} closed -- last consumer {} left; {} unsubscribed, MFAF "
                     "{} deconfigured",
                     sub.fingerprint,
                     id,
                     source->name,
                     collection->mfaf_trans_ref_id);
    };

    // Common handling of a data-subscription body: which source, is it one we serve, consent.
    const auto resolve_data_source =
        [&](const json& request,
            std::optional<sbi_core::http2::Response>& err) -> std::pair<const SourceKind*, json> {
        std::string why;
        const auto alt = single_alternative(request.at("dataSub"), why);
        if (!alt) {
            err = problem(400, "Bad Request", why, "INVALID_MSG_FORMAT");
            return {nullptr, {}};
        }
        const SourceKind* source = alt->attribute == "amfDataSub"     ? &kAmf
                                   : alt->attribute == "nrfDataSub"   ? &kNrf
                                   : alt->attribute == "nsacfDataSub" ? &kNsacf
                                                                      : nullptr;
        if (!source) {
            rejected_counter->Add(1);
            err = problem(400,
                          "Bad Request",
                          alt->attribute + ": this DCCF has no producer for that Data Source (AMF, "
                                           "NRF, NSACF are served; TS 29.574 table 5.1.7.3-1)",
                          "SUBSCRIPTION_CANNOT_BE_SERVED");
            return {nullptr, {}};
        }
        if (consent_policy == "consumer-checked" && targets_a_user(alt->body) &&
            !request.value("checkedConsentInd", false)) {
            rejected_counter->Add(1);
            err = problem(
                403,
                "Forbidden",
                "per-UE data collection requires checkedConsentInd under this DCCF's "
                "user_consent_policy; the DCCF-side Nudm_SDM consent check is not built (ADR-0366)",
                "USER_CONSENT_NOT_GRANTED");
            return {nullptr, {}};
        }
        for (const char* ignored : {"targetNfId",
                                    "targetNfSetId",
                                    "adrfId",
                                    "ardfSetId",
                                    "storeInd",
                                    "storeHandl",
                                    "timePeriod",
                                    "immReport",
                                    "notifEndpoints"}) {
            if (request.contains(ignored)) {
                spdlog::warn("dccf: {} present in the request -- accepted, not applied (ADR-0366)",
                             ignored);
            }
        }
        return {source, alt->body};
    };

    boost::asio::io_context ioc;
    sbi_core::http2::Server server(ioc, "0.0.0.0", port, server_tls);
    if (const auto tps_limit = sbi_core::read_tps_limit(config); tps_limit.enabled()) {
        server.set_tps_limit(tps_limit.sustained_tps, tps_limit.burst);
    }

    // ---- Ndccf_DataManagement: data subscriptions ----------------------------------------------
    const auto created_response =
        [&](const char* collection, const std::string& id, const json& body) {
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kDataManagementRoot) + "/" + collection + "/" + id);
            resp.body = body.dump();
            return resp;
        };
    server.add_route(
        "POST",
        std::string(kDataManagementRoot) + "/data-subscriptions",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto dto = sbi_core::http2::parse_json_body<sbi_gen::NdccfDataSubscription>(req, err);
            if (!dto) {
                return err;
            }
            const json request = json::parse(req.body);
            std::optional<sbi_core::http2::Response> bad;
            auto [source, source_sub] = resolve_data_source(request, bad);
            if (bad) {
                return *bad;
            }
            std::string id;
            if (auto e = subscribe(dccf::Kind::Data, *source, source_sub, request, "", id)) {
                return *e;
            }
            subscriptions_counter->Add(1);
            return created_response("data-subscriptions", id, request);
        });
    server.add_route(
        "PUT",
        std::string(kDataManagementRoot) + "/data-subscriptions/{subscriptionId}",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            const auto old = store.get_subscription(id);
            if (!old || old->kind != dccf::Kind::Data) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "no Individual DCCF Data Subscription " + id);
            }
            sbi_core::http2::Response err;
            auto dto = sbi_core::http2::parse_json_body<sbi_gen::NdccfDataSubscription>(req, err);
            if (!dto) {
                return err;
            }
            const json request = json::parse(req.body);
            std::optional<sbi_core::http2::Response> bad;
            auto [source, source_sub] = resolve_data_source(request, bad);
            if (bad) {
                return *bad;
            }
            if (dccf::fingerprint(source->name, source_sub, source->notif_fields) !=
                old->fingerprint) {
                leave_collection(id, *old); // different data: leave the old collection
            }
            std::string same = id;
            if (auto e = subscribe(dccf::Kind::Data, *source, source_sub, request, id, same)) {
                return *e;
            }
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = request.dump();
            return resp;
        });
    const auto unsubscribe = [&](dccf::Kind kind) {
        return [&, kind](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            const auto sub = store.get_subscription(id);
            if (!sub || sub->kind != kind) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "no such Individual DCCF Subscription " + id);
            }
            leave_collection(id, *sub);
            store.remove_subscription(id);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        };
    };
    server.add_route("DELETE",
                     std::string(kDataManagementRoot) + "/data-subscriptions/{subscriptionId}",
                     unsubscribe(dccf::Kind::Data));

    // ---- Ndccf_DataManagement: analytics subscriptions (served by the NWDAF) -------------------
    server.add_route(
        "POST",
        std::string(kDataManagementRoot) + "/analytics-subscriptions",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto dto =
                sbi_core::http2::parse_json_body<sbi_gen::NdccfAnalyticsSubscription>(req, err);
            if (!dto) {
                return err;
            }
            const json request = json::parse(req.body);
            std::string id;
            if (auto e = subscribe(
                    dccf::Kind::Analytics, kNwdaf, request.at("anaSub"), request, "", id)) {
                return *e;
            }
            subscriptions_counter->Add(1);
            return created_response("analytics-subscriptions", id, request);
        });
    server.add_route(
        "PUT",
        std::string(kDataManagementRoot) + "/analytics-subscriptions/{subscriptionId}",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            const auto old = store.get_subscription(id);
            if (!old || old->kind != dccf::Kind::Analytics) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "no Individual DCCF Analytics Subscription " + id);
            }
            sbi_core::http2::Response err;
            auto dto =
                sbi_core::http2::parse_json_body<sbi_gen::NdccfAnalyticsSubscription>(req, err);
            if (!dto) {
                return err;
            }
            const json request = json::parse(req.body);
            if (dccf::fingerprint(kNwdaf.name, request.at("anaSub"), kNwdaf.notif_fields) !=
                old->fingerprint) {
                leave_collection(id, *old);
            }
            std::string same = id;
            if (auto e = subscribe(
                    dccf::Kind::Analytics, kNwdaf, request.at("anaSub"), request, id, same)) {
                return *e;
            }
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = request.dump();
            return resp;
        });
    server.add_route("DELETE",
                     std::string(kDataManagementRoot) + "/analytics-subscriptions/{subscriptionId}",
                     unsubscribe(dccf::Kind::Analytics));

    // ---- Ndccf_ContextManagement: data collection profiles -------------------------------------
    server.add_route(
        "POST",
        std::string(kContextManagementRoot) + "/data-collection-profiles",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto p =
                sbi_core::http2::parse_json_body<sbi_gen::NdccfDataCollectionProfile>(req, err);
            if (!p) {
                return err;
            }
            const auto id = store.create_profile(*p);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kContextManagementRoot) +
                                     "/data-collection-profiles/" + id);
            resp.body = json(*p).dump();
            return resp;
        });
    server.add_route(
        "PUT",
        std::string(kContextManagementRoot) + "/data-collection-profiles/{profileId}",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto p =
                sbi_core::http2::parse_json_body<sbi_gen::NdccfDataCollectionProfile>(req, err);
            if (!p) {
                return err;
            }
            if (!store.replace_profile(req.path_params.at("profileId"), *p)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "no such data collection profile");
            }
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = json(*p).dump();
            return resp;
        });
    server.add_route("DELETE",
                     std::string(kContextManagementRoot) + "/data-collection-profiles/{profileId}",
                     [&](const sbi_core::http2::Request& req) {
                         if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                             return sbi_core::http2::problem_response(
                                 401, "Unauthorized", auth->error);
                         }
                         if (!store.remove_profile(req.path_params.at("profileId"))) {
                             return sbi_core::http2::problem_response(
                                 404, "Not Found", "no such data collection profile");
                         }
                         sbi_core::http2::Response resp;
                         resp.status = 204;
                         return resp;
                     });

    std::thread(run_nrf_lifecycle, instance_id, nrf_base, advertised_ipv4, heartbeat_seconds)
        .detach();
    server.start();
    spdlog::info("dccf: listening on https://0.0.0.0:{} (TLS 1.3 + mTLS); MFAF at {}, AMF at {}, "
                 "NSACF at {}, NWDAF at {}",
                 port,
                 mfaf_base,
                 amf_base,
                 nsacf_base,
                 nwdaf_base);
    spdlog::info("dccf: Prometheus metrics at http://{}/metrics", metrics_bind_address);
    sbi_core::run_multi_threaded(ioc);
    return 0;
}
