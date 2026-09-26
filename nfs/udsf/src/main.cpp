// UDSF -- Unstructured Data Storage Function (ADR-0400..ADR-0402).
//
// Stage 2: TS 23.501 V19.8.0 clause 6.2.12 (the UDSF) -- specs/3gpp/TS_23.501_j80.txt.
// Stage 3: TS 29.598 V19.5.0 (specs/3gpp/TS_29.598_j50.txt) and its two R19 YAML files in
// specs/5G_APIs-REL-19/ (forge.3gpp.org, REL-19, commit bca84b6), every DTO generated from them by
// tools/sbi-codegen, every API root the YAML's own servers[0].url (ADR-0325):
//
//   Nudsf_DataRepository  {apiRoot}/nudsf-dr/v1     21 operations -- dr_routes.cpp
//   Nudsf_Timer           {apiRoot}/nudsf-timer/v1  6 operations  -- timer_routes.cpp
//
// and the four notifications the UDSF originates (recordExpired, onDataChange,
// subscriptionExpiryNotification, timerExpiry) -- worker.cpp.
//
// State: Valkey only (store.hpp); no in-process state, so replicas scale horizontally and a
// restart loses nothing. The datastore is required at startup (fail closed, ADR-0366 rule).
//
// DISCLOSED here and in ADR-0400..0402: features advertised DR 0x6D (AdvancedQuery,
// CombinedSearchRetrieve, BulkOperations, PartialRecordUpdate, RecordDeletePartialSuccess) and
// Timer 0x5 (PeriodicTimer, TimerDeletePartialSuccess). NOT advertised: Meta Schema (both APIs --
// the Meta Schema resource is served, but the record<->schema link is missing from the R19 YAML and
// TagType presence/UNIQUE_KEY are not enforced) and AdvancedCounting (the YAML's tag-count-filter
// encoding contradicts the TS). Notifications are delivered at most once. No LI POI: TS 33.127 has
// no UDSF clause.
#include "sbi_core/http2_client.hpp"
#include "sbi_core/http2_server.hpp"
#include "sbi_core/io_context_pool.hpp"
#include "sbi_core/jwt.hpp"
#include "sbi_core/logging.hpp"
#include "sbi_core/metrics.hpp"
#include "sbi_core/oauth2_client.hpp"
#include "sbi_core/otel.hpp"
#include "sbi_core/rate_limit.hpp"
#include "sbi_core/uuid.hpp"

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>
#include <opentelemetry/trace/scope.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <map>
#include <string>
#include <thread>

#include "nf_config/nf_config.hpp"
#include "nf_config/redis.hpp"
#include "routes.hpp"
#include "worker.hpp"

using json = nlohmann::json;

namespace udsf {

sbi_core::http2::Handler instrument(const std::string& operation, sbi_core::http2::Handler h) {
    static auto counter = sbi_core::get_meter("udsf")->CreateUInt64Counter(
        "udsf_requests_total", "UDSF SBI requests by 3GPP operationId and HTTP status");
    return [operation, h = std::move(h)](const Request& req) {
        auto span = sbi_core::get_tracer()->StartSpan("udsf." + operation);
        auto scope = opentelemetry::trace::Scope(span);
        Response r;
        try {
            r = h(req);
        } catch (const std::exception& e) {
            // A datastore failure mid-request (fail closed: never answer as if it had worked).
            spdlog::error("udsf: {} failed: {}", operation, e.what());
            r = problem(500, "Internal Server Error", e.what(), std::string("SYSTEM_FAILURE"));
        }
        span->SetAttribute("http.response.status_code", r.status);
        counter->Add(1, {{"operation", operation}, {"status", std::to_string(r.status)}});
        return r;
    };
}

} // namespace udsf

namespace {

constexpr const char* kNfType = "UDSF";
// Must match nfs/nrf/src/main.cpp's kNrfInstanceId exactly -- see docs/DECISIONS.md ADR-0018.
constexpr const char* kNrfInstanceId = "5ba9a927-1d31-4c8e-8a10-000000000001";

std::string regex_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (std::string_view(R"(\^$.|?*+()[]{})").find(c) != std::string_view::npos) {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

// UdsfInfo.storageIdRanges (TS29510 NFManagement): realmId -> IdentityRange[] (pattern form).
json udsf_info(const udsf::Settings& settings) {
    std::map<std::string, std::vector<std::string>> by_realm;
    for (const auto& [realm, storage] : settings.storages) {
        by_realm[realm].push_back(regex_escape(storage));
    }
    json ranges = json::object();
    for (const auto& [realm, list] : by_realm) {
        std::string alt;
        for (const auto& s : list) {
            alt += (alt.empty() ? "" : "|") + s;
        }
        ranges[realm] = json::array({json{{"pattern", "^(" + alt + ")$"}}});
    }
    return json{{"storageIdRanges", ranges}};
}

void run_nrf_lifecycle(const std::string& instance_id,
                       const std::string& nrf_base,
                       const std::string& advertised_ipv4,
                       unsigned short port,
                       int heartbeat_seconds,
                       const json& info) {
    sbi_core::http2::TlsConfig client_tls{
        .cert_path = CERTS_DIR "/udsf/cert.pem",
        .key_path = CERTS_DIR "/udsf/key.pem",
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
    auto service = [&](const char* name, const char* version, const char* features) {
        return json{
            {"serviceInstanceId", name},
            {"serviceName", name},
            {"versions",
             json::array({json{{"apiVersionInUri", "v1"}, {"apiFullVersion", version}}})},
            {"scheme", "https"},
            {"nfServiceStatus", "REGISTERED"},
            {"ipEndPoints", json::array({json{{"ipv4Address", advertised_ipv4}, {"port", port}}})},
            {"supportedFeatures", features}};
    };
    const json profile{
        {"nfInstanceId", instance_id},
        {"nfType", kNfType},
        {"nfStatus", "REGISTERED"},
        {"ipv4Addresses", json::array({advertised_ipv4})},
        {"heartBeatTimer", heartbeat_seconds},
        {"udsfInfo", info},
        // info.version of each YAML: DataRepository 1.3.0; Timer 1.3.0 (see the YAML files).
        {"nfServices",
         json::array({service("nudsf-dr", "1.3.0", udsf::kDrFeatures),
                      service("nudsf-timer", "1.3.0", udsf::kTimerFeatures)})},
    };
    while (true) {
        auto token = oauth.get_bearer_token();
        if (!token) {
            spdlog::warn("udsf: NRF token unavailable ({}), retrying", token.error());
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
            spdlog::info("udsf: registered with NRF (HTTP {})", put_resp->status);
            break;
        }
        spdlog::warn("udsf: NRF registration failed ({}), retrying",
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
    sbi_core::init_logging("udsf");
    sbi_core::init_tracing("udsf");
    const auto config = nf_config::load("udsf", CONFIG_DIR);
    // Every port and tunable from config, env-overridable. Nothing below is a literal.
    const auto port = nf_config::require<unsigned short>(config, "port", "UDSF_PORT");
    const auto metrics_bind_address = nf_config::require<std::string>(
        config, "metrics_bind_address", "UDSF_METRICS_BIND_ADDRESS");
    const auto nrf_base =
        nf_config::require<std::string>(config, "nrf_base_url", "UDSF_NRF_BASE_URL");
    const auto advertised_ipv4 =
        nf_config::require<std::string>(config, "advertised_ipv4", "UDSF_ADVERTISED_IPV4");
    auto redis_url = nf_config::require<std::string>(config, "redis_url", "UDSF_REDIS_URL");
    const auto redis_pool_size =
        nf_config::require<int>(config, "redis_pool_size", "UDSF_REDIS_POOL_SIZE");
    const auto heartbeat_seconds =
        nf_config::require<int>(config, "nrf_heartbeat_seconds", "UDSF_NRF_HEARTBEAT_SECONDS");
    const auto storages = nf_config::require<json>(config, "storages", "UDSF_STORAGES");
    const auto sweep_ms = nf_config::require<int>(
        config, "expiry_sweep_interval_ms", "UDSF_EXPIRY_SWEEP_INTERVAL_MS");

    udsf::Settings settings;
    settings.max_record_ttl_seconds = nf_config::require<std::int64_t>(
        config, "max_record_ttl_seconds", "UDSF_MAX_RECORD_TTL_SECONDS");
    settings.max_subscription_seconds = nf_config::require<std::int64_t>(
        config, "max_subscription_seconds", "UDSF_MAX_SUBSCRIPTION_SECONDS");
    settings.cache_max_age_seconds = nf_config::require<std::int64_t>(
        config, "cache_max_age_seconds", "UDSF_CACHE_MAX_AGE_SECONDS");
    settings.oauth2_required =
        nf_config::require<bool>(config, "oauth2_required", "UDSF_OAUTH2_REQUIRED");
    // "storages": {"<realmId>": ["<storageId>", ...]} -- the realms and storages this UDSF serves
    // (TS 29.598 REALM_NOT_FOUND / STORAGE_NOT_FOUND; advertised as UdsfInfo.storageIdRanges).
    for (auto it = storages.begin(); it != storages.end(); ++it) {
        for (const auto& st : *it) {
            settings.storages.emplace(it.key(), st.get<std::string>());
        }
    }
    if (settings.storages.empty()) {
        spdlog::critical("udsf: no storages configured -- nothing to serve");
        return 1;
    }
    settings.self_base = "https://" + advertised_ipv4 + ":" + std::to_string(port);

    sbi_core::init_metrics(metrics_bind_address);
    const std::string instance_id = sbi_core::generate_uuid_v4();
    spdlog::info("udsf: starting, nfInstanceId={}", instance_id);

    // redis++ reads the pool size from the URI; the handlers, the worker and the transactions
    // (each holding a connection for WATCH..EXEC) share the pool.
    redis_url += (redis_url.find('?') == std::string::npos ? "?" : "&") +
                 std::string("pool_size=") + std::to_string(redis_pool_size);
    udsf::Store store(nf_config::connect_redis_or_die(redis_url, "udsf"),
                      settings.self_base + udsf::kDrRoot);

    sbi_core::http2::TlsConfig server_tls{
        .cert_path = CERTS_DIR "/udsf/cert.pem",
        .key_path = CERTS_DIR "/udsf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::jwt::Verifier verifier(CERTS_DIR "/nrf-jwt/public.pem", kNrfInstanceId);
    sbi_core::http2::Client notify_client(sbi_core::http2::TlsConfig{
        .cert_path = CERTS_DIR "/udsf/cert.pem",
        .key_path = CERTS_DIR "/udsf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    });

    udsf::Ctx ctx{store, verifier, settings};

    boost::asio::io_context ioc;
    sbi_core::http2::Server server(ioc, "0.0.0.0", port, server_tls);
    if (const auto tps_limit = sbi_core::read_tps_limit(config); tps_limit.enabled()) {
        server.set_tps_limit(tps_limit.sustained_tps, tps_limit.burst);
    }
    udsf::add_dr_routes(server, ctx);
    udsf::add_timer_routes(server, ctx);

    udsf::Worker worker(ctx, notify_client, std::chrono::milliseconds(sweep_ms));
    std::thread worker_thread([&] { worker.run(); });
    sbi_core::on_shutdown_signal([&] { worker.stop(); });

    std::thread(run_nrf_lifecycle,
                instance_id,
                nrf_base,
                advertised_ipv4,
                port,
                heartbeat_seconds,
                udsf_info(settings))
        .detach();
    server.start();
    spdlog::info("udsf: listening on https://0.0.0.0:{} (TLS 1.3 + mTLS), {} storage(s)",
                 port,
                 settings.storages.size());
    spdlog::info("udsf: Prometheus metrics at http://{}/metrics", metrics_bind_address);
    sbi_core::run_multi_threaded(ioc);
    worker_thread.join();
    return 0;
}
