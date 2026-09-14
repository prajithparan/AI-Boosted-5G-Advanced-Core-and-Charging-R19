// NWDAF -- Network Data Analytics Function, Phase A (ADR-0358).
//
// Stage 2: TS 23.288 V19.7.0 (specs/3gpp/TS_23.288_j70.txt). Stage 3: the ten TS 29.520 R19
// YAML files in specs/5G_APIs-REL-19/ (forge.3gpp.org, REL-19, commit bca84b6). Phase A builds
// two of the ten services, both generated from their YAML and served at the API roots the YAML's
// own servers[0].url declares (ADR-0325 -- never derived from a filename):
//
//   Nnwdaf_AnalyticsInfo        {apiRoot}/nnwdaf-analyticsinfo/v1        GetNWDAFAnalytics,
//                                                                          GetNwdafContext
//   Nnwdaf_EventsSubscription   {apiRoot}/nnwdaf-eventssubscription/v1   the 7 operations on
//                                                                          /subscriptions and
//                                                                          /transfers
//
// Two analytics IDs are computed, each from the source TS 23.288 names for it -- see
// analytics.hpp. Every other NwdafEvent value the YAML defines is answered honestly: a request
// for an analytic this NWDAF does not compute gets the YAML's own 404 with
// ProblemDetailsAnalyticsInfoRequest semantics, not an empty 200 that looks like "no load".
//
// This is the AnLF. MTLF (MLModelProvision/Training/Monitor) is Phase B; DataManagement over the
// event bus is Phase C; RoamingAnalytics/RoamingData/VFL are Phase D. State (subscriptions,
// transfers) is in-process for Phase A, the same disclosed simplification NSACF's stores carry,
// and the same P8/P11 externalisation debt.
//
// DISCLOSED, not hidden:
//   * /transfers is accepted and stored. No second NWDAF exists to transfer TO, so nothing is
//     transferred. The resource semantics (201/204/404) are real; the transfer is not.
//   * Notifications are produced by a periodic thread on the interval in config; the YAML's
//     NotificationMethod PERIODIC is honoured, THRESHOLD/ON_EVENT_DETECTION are treated as
//     periodic in Phase A.
//   * LI: TS 33.127 clause 7.18 requires an IRI-POI in NWDAF. Not built (blocker #0).

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
#include <spdlog/spdlog.h>

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "TS26510_CommonData_grp.hpp"
#include "analytics.hpp"
#include "feature_store.hpp"
#include "nf_config/nf_config.hpp"
#include "subscription_store.hpp"

using json = nlohmann::json;

namespace {

constexpr const char* kNfType = "NWDAF";
// Must match nfs/nrf/src/main.cpp's kNrfInstanceId exactly -- see docs/DECISIONS.md ADR-0018.
constexpr const char* kNrfInstanceId = "5ba9a927-1d31-4c8e-8a10-000000000001";
// servers[0].url of each YAML, verified by tests/conformance's api_root_conformance (ADR-0325).
constexpr const char* kAnalyticsInfoRoot = "/nnwdaf-analyticsinfo/v1";
constexpr const char* kEventsSubscriptionRoot = "/nnwdaf-eventssubscription/v1";

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

// A query parameter carrying a JSON object (TS 29.500 style for structured params). Absent is
// nullopt; present-but-malformed is an error the caller turns into the YAML's 400.
template <typename T>
std::optional<T>
json_query(const sbi_core::http2::Request& req, const char* name, std::string& err) {
    const auto it = req.query_params.find(name);
    if (it == req.query_params.end()) {
        return std::nullopt;
    }
    try {
        return json::parse(it->second).get<T>();
    } catch (const std::exception& e) {
        err = std::string("query parameter '") + name +
              "' is not valid JSON for its schema: " + e.what();
        return std::nullopt;
    }
}

// ---- data collection (TS 23.288 6.2) -----------------------------------------------------------

// NF_LOAD's input, straight from the NRF (Table 6.5.2-1). One discovery per NF type requested, or
// every type this project registers when the filter names none.
std::vector<sbi_gen::NFProfile_Nnrf_NFManagement>
collect_nf_profiles(sbi_core::http2::Client& client,
                    sbi_core::OAuth2Client& oauth,
                    const std::string& nrf_base,
                    const std::vector<std::string>& nf_types) {
    std::vector<sbi_gen::NFProfile_Nnrf_NFManagement> out;
    auto token = oauth.get_bearer_token();
    for (const auto& t : nf_types) {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = nrf_base + "/nnrf-disc/v1/nf-instances?target-nf-type=" + t +
                  "&requester-nf-type=NWDAF";
        if (token) {
            req.headers.emplace("authorization", "Bearer " + *token);
        }
        auto resp = client.send(req);
        if (!resp || resp->status != 200) {
            spdlog::warn("nwdaf: NRF discovery for {} failed ({})", t, resp ? resp->status : -1);
            continue;
        }
        try {
            const auto body = json::parse(resp->body);
            for (const auto& p : body.at("nfInstances")) {
                out.push_back(p.get<sbi_gen::NFProfile_Nnrf_NFManagement>());
            }
        } catch (const std::exception& e) {
            spdlog::warn("nwdaf: NRF discovery for {} returned an unparseable SearchResult: {}",
                         t,
                         e.what());
        }
    }
    return out;
}

struct Analytics {
    // The set of NF types NF_LOAD covers when a request does not filter -- from config, so an
    // operator adds a type without a rebuild.
    std::vector<std::string> default_nf_types;
    nwdaf::AbnormalBehaviourThresholds thresholds;
};

// The one function both Nnwdaf_AnalyticsInfo and the subscription notifier call, so a request
// and a notification for the same analytic cannot disagree.
std::optional<sbi_gen::AnalyticsData_Nnwdaf_AnalyticsInfo>
compute(const std::string& event_id,
        const std::optional<sbi_gen::EventFilter_Nnwdaf_AnalyticsInfo>& filter,
        const std::optional<sbi_gen::TargetUeInformation>& target,
        const Analytics& cfg,
        sbi_core::http2::Client& client,
        sbi_core::OAuth2Client& oauth,
        const std::string& nrf_base,
        nwdaf::FeatureStore& features,
        std::mutex& features_mutex) {
    sbi_gen::AnalyticsData_Nnwdaf_AnalyticsInfo data;
    data.timeStampGen = sbi_core::format_rfc3339(std::chrono::system_clock::now());

    if (event_id == sbi_gen::NwdafEvent::NF_LOAD) {
        std::vector<std::string> types = cfg.default_nf_types;
        if (filter && filter->nfTypes && !filter->nfTypes->empty()) {
            types.clear();
            for (const auto& t : *filter->nfTypes) {
                types.push_back(t.value);
            }
        }
        auto profiles = collect_nf_profiles(client, oauth, nrf_base, types);
        if (filter && filter->nfInstanceIds && !filter->nfInstanceIds->empty()) {
            std::erase_if(profiles, [&](const auto& p) {
                return std::find(filter->nfInstanceIds->begin(),
                                 filter->nfInstanceIds->end(),
                                 p.nfInstanceId) == filter->nfInstanceIds->end();
            });
        }
        data.nfLoadLevelInfos = nwdaf::nf_load_from_profiles(profiles);
        return data;
    }

    if (event_id == sbi_gen::NwdafEvent::ABNORMAL_BEHAVIOUR) {
        if (!features.connected()) {
            return std::nullopt; // the caller answers with the YAML's 404 + the real reason
        }
        std::vector<nwdaf::FeatureRow> today, yesterday;
        {
            const std::lock_guard<std::mutex> lock(features_mutex);
            const auto latest = features.latest_date();
            if (latest.empty()) {
                data.abnorBehavrs = std::vector<sbi_gen::AbnormalBehaviour>();
                return data; // a real, empty answer: no data yet
            }
            today = features.read_day(latest);
            // The day before, for the trend -- derived from the latest date, not from the clock.
            std::tm tm{};
            if (strptime(latest.c_str(), "%Y-%m-%d", &tm) != nullptr) {
                tm.tm_mday -= 1;
                const auto t = timegm(&tm);
                char buf[16];
                std::strftime(buf, sizeof(buf), "%Y-%m-%d", gmtime(&t));
                yesterday = features.read_day(buf);
            }
        }
        if (target && target->supis && !target->supis->empty()) {
            const auto& keep = *target->supis;
            const auto not_targeted = [&](const nwdaf::FeatureRow& r) {
                return std::find(keep.begin(), keep.end(), r.subscriber_identifier) == keep.end();
            };
            std::erase_if(today, not_targeted);
            std::erase_if(yesterday, not_targeted);
        }
        data.abnorBehavrs = nwdaf::detect_abnormal_behaviour(today, yesterday, cfg.thresholds);
        return data;
    }

    return std::nullopt; // an analytic this NWDAF does not compute
}

// ---- NRF lifecycle (same shape as every other NF, ADR-0006/0019) --------------------------------

void run_nrf_lifecycle(const std::string& instance_id,
                       const std::string& nrf_base,
                       const std::string& advertised_ipv4,
                       int heartbeat_seconds) {
    sbi_core::http2::TlsConfig client_tls{
        .cert_path = CERTS_DIR "/nwdaf/cert.pem",
        .key_path = CERTS_DIR "/nwdaf/key.pem",
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
    json profile{
        {"nfInstanceId", instance_id},
        {"nfType", kNfType},
        {"nfStatus", "REGISTERED"},
        {"ipv4Addresses", json::array({advertised_ipv4})},
        {"heartBeatTimer", heartbeat_seconds},
        // TS 29.510 NwdafInfo would go here (supported analytics IDs). Phase A registers the two
        // it computes so a consumer discovering by analytics can find it honestly.
        {"nwdafInfo",
         json{{"eventIds",
               json::array(
                   {sbi_gen::NwdafEvent::NF_LOAD, sbi_gen::NwdafEvent::ABNORMAL_BEHAVIOUR})}}},
    };
    while (true) {
        auto token = oauth.get_bearer_token();
        if (!token) {
            spdlog::error("nwdaf: OAuth2 token fetch failed: {}", token.error());
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }
        sbi_core::http2::ClientRequest put_req;
        put_req.method = "PUT";
        put_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + instance_id;
        put_req.headers.emplace("content-type", "application/json");
        put_req.headers.emplace("authorization", "Bearer " + *token);
        put_req.headers.emplace(
            sbi_core::headers::kSenderTimestamp,
            sbi_core::headers::format_sender_timestamp(std::chrono::system_clock::now()));
        put_req.body = profile.dump();
        auto put_resp = http_client.send(put_req);
        if (put_resp && (put_resp->status == 200 || put_resp->status == 201)) {
            spdlog::info("nwdaf: registered with NRF (HTTP {})", put_resp->status);
            break;
        }
        spdlog::warn("nwdaf: NRF registration attempt failed, retrying in 5s");
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
    sbi_core::init_logging("nwdaf");
    sbi_core::init_tracing("nwdaf");
    const auto config = nf_config::load("nwdaf", CONFIG_DIR);
    // Every port and tunable from config, env-overridable (user mandate). Nothing below is a
    // literal.
    const auto port = nf_config::require<unsigned short>(config, "port", "NWDAF_PORT");
    const auto metrics_bind_address = nf_config::require<std::string>(
        config, "metrics_bind_address", "NWDAF_METRICS_BIND_ADDRESS");
    const auto nrf_base =
        nf_config::require<std::string>(config, "nrf_base_url", "NWDAF_NRF_BASE_URL");
    const auto advertised_ipv4 =
        nf_config::require<std::string>(config, "advertised_ipv4", "NWDAF_ADVERTISED_IPV4");
    const auto redis_url = nf_config::require<std::string>(config, "redis_url", "NWDAF_REDIS_URL");
    const auto heartbeat_seconds =
        nf_config::require<int>(config, "nrf_heartbeat_seconds", "NWDAF_NRF_HEARTBEAT_SECONDS");
    const auto notify_interval_seconds = nf_config::require<int>(
        config, "notification_interval_seconds", "NWDAF_NOTIFICATION_INTERVAL_SECONDS");

    Analytics analytics;
    analytics.default_nf_types =
        nf_config::require<std::vector<std::string>>(config, "nf_load_default_nf_types");
    analytics.thresholds.sigma_threshold = nf_config::require<double>(
        config, "abnormal_sigma_threshold", "NWDAF_ABNORMAL_SIGMA_THRESHOLD");
    analytics.thresholds.large_rate_floor_octets = nf_config::require<double>(
        config, "abnormal_large_rate_floor_octets", "NWDAF_ABNORMAL_LARGE_RATE_FLOOR_OCTETS");
    analytics.thresholds.session_count_threshold = nf_config::require<std::int64_t>(
        config, "abnormal_session_count_threshold", "NWDAF_ABNORMAL_SESSION_COUNT_THRESHOLD");
    analytics.thresholds.max_exception_level =
        nf_config::require<std::int64_t>(config, "abnormal_max_exception_level");
    analytics.thresholds.confidence_full_at_sessions =
        nf_config::require<std::int64_t>(config, "abnormal_confidence_full_at_sessions");

    nwdaf::FeatureStoreOptions fs;
    fs.host =
        nf_config::require<std::string>(config, "feature_store_host", "NWDAF_FEATURE_STORE_HOST");
    fs.port =
        nf_config::require<std::uint16_t>(config, "feature_store_port", "NWDAF_FEATURE_STORE_PORT");
    fs.user =
        nf_config::require<std::string>(config, "feature_store_user", "NWDAF_FEATURE_STORE_USER");
    fs.password = nf_config::require<std::string>(
        config, "feature_store_password", "NWDAF_FEATURE_STORE_PASSWORD");
    fs.database = nf_config::require<std::string>(
        config, "feature_store_database", "NWDAF_FEATURE_STORE_DATABASE");
    fs.max_rows = nf_config::require<std::int64_t>(
        config, "feature_store_max_rows", "NWDAF_FEATURE_STORE_MAX_ROWS");

    sbi_core::init_metrics(metrics_bind_address);
    const std::string instance_id = sbi_core::generate_uuid_v4();
    spdlog::info("nwdaf: starting, nfInstanceId={}", instance_id);

    sbi_core::http2::TlsConfig server_tls{
        .cert_path = CERTS_DIR "/nwdaf/cert.pem",
        .key_path = CERTS_DIR "/nwdaf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::jwt::Verifier verifier(CERTS_DIR "/nrf-jwt/public.pem", kNrfInstanceId);

    // Outbound: NRF discovery for NF_LOAD, and notification delivery.
    sbi_core::http2::TlsConfig client_tls{
        .cert_path = CERTS_DIR "/nwdaf/cert.pem",
        .key_path = CERTS_DIR "/nwdaf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client client(std::move(client_tls));
    sbi_core::OAuth2Client oauth(
        client, nrf_base + "/oauth2/token", instance_id, "nnrf-disc", "NRF");
    std::mutex client_mutex; // the client is used from the request path and the notifier thread

    nwdaf::FeatureStore features(fs);
    std::mutex features_mutex;
    // ADR-0360: subscription state in Valkey, shared by every NWDAF replica.
    nwdaf::SubscriptionStore store(std::make_shared<sw::redis::Redis>(redis_url));

    auto meter = sbi_core::get_meter("nwdaf");
    auto analytics_counter = meter->CreateUInt64Counter("nwdaf_analytics_requests_total",
                                                        "Nnwdaf_AnalyticsInfo requests served");
    auto unsupported_counter =
        meter->CreateUInt64Counter("nwdaf_analytics_unsupported_total",
                                   "Requests for an analytics ID this NWDAF does not compute");
    auto notify_counter = meter->CreateUInt64Counter(
        "nwdaf_notifications_sent_total", "Nnwdaf_EventsSubscription notifications delivered");

    boost::asio::io_context ioc;
    sbi_core::http2::Server server(ioc, "0.0.0.0", port, server_tls);
    if (const auto tps_limit = sbi_core::read_tps_limit(config); tps_limit.enabled()) {
        server.set_tps_limit(tps_limit.sustained_tps, tps_limit.burst);
    }

    // ---- Nnwdaf_AnalyticsInfo ----------------------------------------------------------------
    server.add_route(
        "GET",
        std::string(kAnalyticsInfoRoot) + "/analytics",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ev = req.query_params.find("event-id");
            if (ev == req.query_params.end()) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", "query parameter 'event-id' is required");
            }
            std::string err;
            auto filter =
                json_query<sbi_gen::EventFilter_Nnwdaf_AnalyticsInfo>(req, "event-filter", err);
            if (!err.empty()) {
                return sbi_core::http2::problem_response(400, "Bad Request", err);
            }
            auto target = json_query<sbi_gen::TargetUeInformation>(req, "tgt-ue", err);
            if (!err.empty()) {
                return sbi_core::http2::problem_response(400, "Bad Request", err);
            }
            std::optional<sbi_gen::AnalyticsData_Nnwdaf_AnalyticsInfo> data;
            {
                const std::lock_guard<std::mutex> lock(client_mutex);
                data = compute(ev->second,
                               filter,
                               target,
                               analytics,
                               client,
                               oauth,
                               nrf_base,
                               features,
                               features_mutex);
            }
            if (!data) {
                unsupported_counter->Add(1);
                // The YAML's 404 for this operation. The detail says WHICH of the two reasons.
                const std::string why =
                    ev->second == sbi_gen::NwdafEvent::ABNORMAL_BEHAVIOUR
                        ? "ABNORMAL_BEHAVIOUR needs the CHF feature store, which is unreachable"
                        : "analytics ID '" + ev->second +
                              "' is not computed by this NWDAF (Phase A: NF_LOAD, "
                              "ABNORMAL_BEHAVIOUR)";
                return sbi_core::http2::problem_response(404, "Not Found", why);
            }
            analytics_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = json(*data).dump();
            return resp;
        });

    server.add_route("GET",
                     std::string(kAnalyticsInfoRoot) + "/context",
                     [&](const sbi_core::http2::Request& req) {
                         if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                             return sbi_core::http2::problem_response(
                                 401, "Unauthorized", auth->error);
                         }
                         // GetNwdafContext returns ContextData for the requested context-ids. Phase
                         // A holds no per-UE context (no DataManagement yet), so the YAML's 204 "no
                         // content" is the true answer rather than an empty object pretending to be
                         // context.
                         sbi_core::http2::Response resp;
                         resp.status = 204;
                         return resp;
                     });

    // ---- Nnwdaf_EventsSubscription -----------------------------------------------------------
    server.add_route(
        "POST",
        std::string(kEventsSubscriptionRoot) + "/subscriptions",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::NnwdafEventsSubscription>(req, err);
            if (!body) {
                return err;
            }
            if (body->eventSubscriptions.empty()) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", "eventSubscriptions must not be empty");
            }
            const auto id = store.create_subscription(*body);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kEventsSubscriptionRoot) + "/subscriptions/" + id);
            resp.body = json(*body).dump();
            return resp;
        });

    server.add_route(
        "PUT",
        std::string(kEventsSubscriptionRoot) + "/subscriptions/{subscriptionId}",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::NnwdafEventsSubscription>(req, err);
            if (!body) {
                return err;
            }
            const auto id = req.path_params.at("subscriptionId");
            if (!store.replace_subscription(id, *body)) {
                return sbi_core::http2::problem_response(404, "Not Found", "no subscription " + id);
            }
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = json(*body).dump();
            return resp;
        });

    server.add_route(
        "DELETE",
        std::string(kEventsSubscriptionRoot) + "/subscriptions/{subscriptionId}",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            if (!store.remove_subscription(id)) {
                return sbi_core::http2::problem_response(404, "Not Found", "no subscription " + id);
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // /transfers -- stored, not transferred (see the header). Real resource semantics.
    server.add_route(
        "POST",
        std::string(kEventsSubscriptionRoot) + "/transfers",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::AnalyticsSubscriptionsTransfer>(req, err);
            if (!body) {
                return err;
            }
            const auto id = store.create_transfer(*body);
            spdlog::warn("nwdaf: analytics subscription transfer {} accepted and stored; no peer "
                         "NWDAF exists to transfer to (Phase A disclosure)",
                         id);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("location",
                                 std::string(kEventsSubscriptionRoot) + "/transfers/" + id);
            return resp;
        });
    server.add_route(
        "PUT",
        std::string(kEventsSubscriptionRoot) + "/transfers/{transferId}",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::AnalyticsSubscriptionsTransfer>(req, err);
            if (!body) {
                return err;
            }
            const auto id = req.path_params.at("transferId");
            if (!store.replace_transfer(id, *body)) {
                return sbi_core::http2::problem_response(404, "Not Found", "no transfer " + id);
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });
    server.add_route(
        "DELETE",
        std::string(kEventsSubscriptionRoot) + "/transfers/{transferId}",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("transferId");
            if (!store.remove_transfer(id)) {
                return sbi_core::http2::problem_response(404, "Not Found", "no transfer " + id);
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // ---- notifier: every subscription with a notificationURI gets its events on the interval --
    std::thread([&] {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(notify_interval_seconds));
            std::vector<std::pair<std::string, sbi_gen::NnwdafEventsSubscription>> snapshot;
            try {
                for (auto& [id, sub] : store.all_subscriptions()) {
                    if (sub.notificationURI) {
                        snapshot.emplace_back(id, std::move(sub));
                    }
                }
            } catch (const std::exception& e) {
                spdlog::error("nwdaf: notifier could not read subscriptions from Valkey: {}",
                              e.what());
                continue;
            }
            for (const auto& [id, sub] : snapshot) {
                sbi_gen::NnwdafEventsSubscriptionNotification note;
                note.subscriptionId = id;
                note.notifCorrId = sub.notifCorrId;
                note.eventNotifications =
                    std::vector<sbi_gen::EventNotification_Nnwdaf_EventsSubscription>();
                for (const auto& es : sub.eventSubscriptions) {
                    std::optional<sbi_gen::EventFilter_Nnwdaf_AnalyticsInfo> filter;
                    if (es.nfTypes || es.nfInstanceIds) {
                        filter = sbi_gen::EventFilter_Nnwdaf_AnalyticsInfo{};
                        filter->nfTypes = es.nfTypes;
                        filter->nfInstanceIds = es.nfInstanceIds;
                    }
                    std::optional<sbi_gen::AnalyticsData_Nnwdaf_AnalyticsInfo> data;
                    {
                        const std::lock_guard<std::mutex> lock(client_mutex);
                        data = compute(es.event.value,
                                       filter,
                                       es.tgtUe,
                                       analytics,
                                       client,
                                       oauth,
                                       nrf_base,
                                       features,
                                       features_mutex);
                    }
                    sbi_gen::EventNotification_Nnwdaf_EventsSubscription en;
                    en.event = es.event;
                    en.timeStampGen = sbi_core::format_rfc3339(std::chrono::system_clock::now());
                    if (data) {
                        en.nfLoadLevelInfos = data->nfLoadLevelInfos;
                        en.abnorBehavrs = data->abnorBehavrs;
                    } else {
                        // The YAML's own way to say "could not produce this one": NwdafFailureCode.
                        sbi_gen::NwdafFailureCode fc;
                        fc.value = sbi_gen::NwdafFailureCode::UNAVAILABLE_DATA;
                        en.failNotifyCode = fc;
                    }
                    note.eventNotifications->push_back(std::move(en));
                }
                sbi_core::http2::ClientRequest req;
                req.method = "POST";
                req.url = *sub.notificationURI;
                req.headers.emplace("content-type", "application/json");
                req.body = json(note).dump();
                std::optional<int> status;
                {
                    const std::lock_guard<std::mutex> lock(client_mutex);
                    if (auto resp = client.send(req); resp) {
                        status = resp->status;
                    }
                }
                if (status && *status >= 200 && *status < 300) {
                    notify_counter->Add(1);
                } else {
                    spdlog::warn("nwdaf: notification for {} to {} failed ({})",
                                 id,
                                 *sub.notificationURI,
                                 status.value_or(-1));
                }
            }
        }
    }).detach();

    std::thread(run_nrf_lifecycle, instance_id, nrf_base, advertised_ipv4, heartbeat_seconds)
        .detach();

    server.start();
    spdlog::info("nwdaf: listening on https://0.0.0.0:{} (TLS 1.3 + mTLS)", port);
    spdlog::info("nwdaf: Prometheus metrics at http://{}/metrics", metrics_bind_address);
    sbi_core::run_multi_threaded(ioc);
    return 0;
}
