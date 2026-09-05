// nfs/chf: CHF (Charging Function), Nchf_ConvergedCharging service (TS 32.291).
// Source: specs/5G_APIs-REL-19/TS32291_Nchf_ConvergedCharging.yaml
// (commit bca84b60a37773133bcae97e5c6c0d10a93b47b6). Phase 4's first NF (CLAUDE.md's charging
// domain).
//
// Scope, in three approved stages:
// - Stage 0/1 (ADR-0044): Nchf_ConvergedCharging_Create -- POST /chargingdata. Real request
//   parsing, real ChargingDataRef allocation, a schema-valid ChargingDataResponse. Also builds and
//   logs a TM Forum SID Individual for the subscriber (docs/CHARGING_MAPPING.md, ADR-0045).
// - ADR-0046: Nchf_ConvergedCharging_Release -- POST /chargingdata/{ChargingDataRef}/
//   release. Validates the ref is a real, still-active one (404 if not), returns 204 per spec.
// - ADR-0048: a real rating engine. CHF is now a real HTTP client of bss/product-catalog
//   (ADR-0047) -- when a request's multipleUnitUsage carries a ratingGroup, CHF looks up the
//   ProductOfferingPrice whose own real `ratingGroup` characteristic matches it (UPDATE, ADR-0072:
//   originally just picked the first Active/isSellable offering regardless of ratingGroup, a real
//   correctness gap fixed this turn -- see build_rating_grant's own comment), converts its
//   unitOfMeasure into a real GrantedUnit, and returns it in multipleUnitInformation along with
//   real quota-policy fields (validityTime/quotaHoldingTime/*QuotaThreshold) read from that same
//   price's own characteristics. See build_rating_grant's own comment for the real conversion
//   details.
//
// - ADR-0050 Stage 4: Nchf_ConvergedCharging_Update -- POST /chargingdata/{ChargingDataRef}/
//   update. Validates the ref is still active (404 if not, same convention as Release), logs the
//   real reported usage (multipleUnitUsage[].usedUnitContainer[]) SMF's Stage 3 code now genuinely
//   sends, and re-authorizes: issues a fresh GrantedUnit from the same catalog-lookup rating engine
//   Create already uses. Disclosed, real simplifications, not silently different from a real OCS:
//   no balance/wallet deduction against what was already consumed (no such store exists yet, see
//   docs/CHARGING_MAPPING.md's TMF654 gap note); does not differentiate a Volume-Threshold report
//   from a Volume-Quota-exhaustion report (SMF's own Stage 3 code doesn't yet forward that
//   distinction as a real Trigger in the request body either -- a real, disclosed gap on the SMF
//   side, not fixed by this stage).
//
// - P4.2 (CHARGING_PROMPT.md/ADR-0055), starting this turn: two real 3GPP-defined sibling
//   services, hosted under this same CHF binary --
//     * Nchf_OfflineOnlyCharging (TS 32.291): Create/Update/Release, mirroring
//       ConvergedCharging's own shape but with NO rating-engine call (its ChargingDataResponse
//       schema carries no multipleUnitInformation/grantedUnit field at all -- confirmed directly
//       against the vendored YAML, not assumed).
//     * Nchf_SpendingLimitControl (TS 29.594): Subscribe/Update/Unsubscribe
//       (POST/PUT/DELETE /subscriptions) -- CHF is the real SERVER here (PCF subscribes TO CHF,
//       confirmed directly against the YAML, correcting an initial assumption that this would be
//       a CHF-as-PCF-client integration). The real statusNotification/subscriptionTermination
//       callbacks (CHF as client, POSTing to the subscriber's notifUri) are NOT implemented --
//       no real policy-counter-breach-detection engine exists yet to trigger them from, same
//       deferred-not-dropped category as ConvergedCharging's own chargingNotification below.
//   Real, disclosed rename that came with adding OfflineOnlyCharging's YAML: it independently
//   defines its own `ChargingDataRequest`/`ChargingDataResponse`/`MultipleUnitUsage`/
//   `UsedUnitContainer`/`NFIdentification`/`NodeFunctionality` schema names, colliding with
//   ConvergedCharging's own (two real, independent 3GPP services that happen to reuse type names
//   for different shapes) -- sbi-codegen's existing collision-disambiguation (ADR-0010) suffixed
//   BOTH sides with their source service name, so every reference in this file and
//   nfs/smf/src/main.cpp changed from e.g. `sbi_gen::ChargingDataRequest` to
//   `sbi_gen::ChargingDataRequest_Nchf_ConvergedCharging`. Mechanical, verified (full rebuild +
//   146/146 tests, including the real SMF<->CHF integration test), not a functional change.
//
// - P4.5 (ADR-0059/ADR-0060), Diameter Gy: CHF is now also a real Diameter server (RFC 6733 +
//   RFC 4006 DCC), diameter_server.cpp, listening alongside the HTTP/2 SBI server on the same
//   process. CER/CEA capability exchange (Stage 1-2), then real CCR-Initial/Update/Termination
//   dispatched to the exact same rating/reservation/CDR/audit code
//   Nchf_ConvergedCharging_Create/Update/Release below call (charging_engine.hpp's
//   charge_one_usage/finalize_subscriber_balance) -- the single-code-path property
//   CHARGING_PROMPT.md's P4.5 explicitly requires, live-verified by charging an identical usage
//   event via both protocols and confirming an identical GrantedUnit/cost/RatingDecision result.
//
// Deliberately still deferred, not dropped: the Nchf_ConvergedCharging chargingNotification
// callback (real trigger condition -- e.g. server-initiated reauthorization -- doesn't exist yet),
// Nchf_SpendingLimitControl's own notify/terminate callbacks (same reason), and N41/N42 (AMF)
// wiring (AMF has no real UE Registration procedure in this codebase yet to trigger a charging
// call from -- no NGAP/NAS stack exists; a separate, much larger plan for that is drafted but not
// started). See docs/DECISIONS.md ADR-0044/ADR-0046/ADR-0048/ADR-0050/ADR-0055.
//
// Disclosed simplifications, stated up front:
// - No real subscriber-to-product assignment: the rating engine grants from whichever
//   ProductOffering happens to be first (Active+isSellable) in the catalog, not a real per-
//   subscriber rate plan lookup (no customer/subscription store exists) -- same category of
//   simplification as PCF's fixed-default policy, ADR-0028. If the catalog has no matching
//   offering (e.g. nothing seeded yet), Create still succeeds with an empty grant, same as before
//   this turn -- schema-valid, not a real charging decision, disclosed not hidden.
// - ChargingDataResponse's invocationSequenceNumber is set by echoing the request's own value.
//   TS32291_Nchf_ConvergedCharging.yaml carries no per-field description text distinguishing
//   "echo the request's sequence" from "CHF assigns its own independent sequence" for this field,
//   and no normative TS 32.291 prose is vendored in this repo to check -- echoing the request's
//   value is the least-invented choice (no independent CHF-side sequencing semantics assumed) and
//   disclosed here rather than picked silently.
// - Real Redis/Valkey persistence (CHARGING_PROMPT.md entity E3's "recoverable across restarts"
//   requirement, docs/DATA_MODEL.md's own persistence assignment). `OfflineChargingDataStore`
//   still only tracks active-ref existence (real, disclosed, unchanged gap: no genuine offline-
//   session content is stored). `ChargingDataStore` now DOES hold real content (P4.3/ADR-0057):
//   the session's SUPI and running reserved-balance total, both needed for the real ABMF
//   integration below -- see stores.hpp's own header comment.
// - P4.3 (ADR-0056/0057): CHF's rating engine now makes real Nchf-ConvergedCharging grants
//   contingent on a real balance reservation against bss/balance-management (ADR-0056) --
//   closing the "no balance/wallet deduction against what was already consumed" gap disclosed
//   since ADR-0048/0050. Create/Update reserve the granted price's real monetary cost against a
//   Bucket keyed by the request's SUPI (this project's own disclosed convention -- no real
//   customer-to-bucket provisioning system exists); a grant is only included in the response if
//   the reservation succeeds. Release finalizes the session's full reserved total as a real
//   permanent debit and unreserves the same amount. Disclosed, real simplification: finalization
//   is for the FULL session total, not proportional to SMF's actually-reported usage
//   (usedUnitContainer) -- a real per-usage proportional refund is deferred, not fabricated as
//   more sophisticated than it is. See build_rating_grant/reserve_subscriber_balance/
//   finalize_subscriber_balance's own comments for the full reasoning.

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

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <thread>

// docs/DECISIONS.md ADR-0077 -- no hardcoded DB URL/deployment literal in source. This turn only
// retrofits port/metrics_bind_address/nrf_base_url/rating_database_url (the field confirmed
// actively broken this session, see ADR-0084's own disclosure) -- chf_redis_conninfo/
// chf_doris_options/CHF_AI_QUOTA_SIZING_ENABLED/CHF_QUOTA_MODEL_PATH are already
// getenv-overridable but still deferred to a later, CHF-focused sub-turn of task #109 (a real,
// disclosed, deliberately narrower scope than AMF/UDR's own full retrofit -- CHF's own config
// surface is large enough to be its own increment).
#include "nf_config/nf_config.hpp"

// TS29594_Nchf_SpendingLimitControl's own types now live in TS26510_CommonData_grp.hpp -- see
// stores.hpp's own comment (ADR-0072).
#include "TS26510_CommonData_grp.hpp"
#include "TS32291_Nchf_OfflineOnlyCharging.hpp"
#include "bss_sid/balance.hpp"
#include "bss_sid/party.hpp"
#include "bss_sid/product.hpp"
#include "bss_sid/rating.hpp"
#include "cap_server.hpp"
#include "cdr.hpp"
#include "charging_engine.hpp"
#include "diameter_core/header.hpp"
#include "diameter_server.hpp"
#include "rating_decision_store.hpp"
#include "ss7_core/m3ua_dictionary.hpp"
#include "stores.hpp"

namespace {

// ADR-0295: one optional TPS setting, config file first then environment -- the same precedence
// nf_config::require uses per key. std::nullopt means "not configured", which is what leaves a
// ceiling off; that distinction matters, because 0 means "configured to zero" and is rejected by
// the callers' own `> 0.0` check rather than silently treated as absent.
std::optional<double>
chf_tps_setting(const nlohmann::json& config, const char* key, const char* env_name) {
    if (const char* env = std::getenv(env_name); env != nullptr && *env != '\0') {
        return std::strtod(env, nullptr);
    }
    if (config.contains(key) && !config.at(key).is_null()) {
        return config.at(key).get<double>();
    }
    return std::nullopt;
}

using nlohmann::json;

#ifndef CERTS_DIR
#error "CERTS_DIR must be defined by CMake (see nfs/chf/CMakeLists.txt)"
#endif
#ifndef CONFIG_DIR
#error "CONFIG_DIR must be defined by CMake (see nfs/chf/CMakeLists.txt)"
#endif

// P4.5/ADR-0059 Stage 2: real Diameter (Gy) listener, real IANA-assigned port (RFC 6733 §2.1,
// diameter_core::kDiameterTcpPort). Lab-internal identity, disclosed -- no real registered DNS
// realm/enterprise number, matching the same per-NF naming convention already used for TLS cert
// CNs (scripts/gen-lab-pki.sh).
constexpr unsigned short kDiameterPort = diameter_core::kDiameterTcpPort;
// P4.5/ADR-0061: real CAP (gsmSCF) listener, real IANA-assigned M3UA port (RFC 4666 §1.4.8,
// ss7_core::dictionary::kSctpPort) -- CHF's second, non-SBI real network listener alongside
// Diameter's, same "second real protocol on the same NF" shape.
constexpr unsigned short kCapPort = ss7_core::dictionary::kSctpPort;
constexpr const char* kDiameterOriginHost = "chf.5gc-r19.local";
constexpr const char* kDiameterOriginRealm = "5gc-r19.local";
constexpr const char* kNfType = "CHF";
constexpr const char* kApiRoot = "/nchf-convergedcharging/v3";
// Real basePath confirmed directly from TS32291_Nchf_OfflineOnlyCharging.yaml's own `servers`
// block (ADR-0055) -- P4.2.
constexpr const char* kOfflineApiRoot = "/nchf-offlineonlycharging/v1";
// Real basePath confirmed directly from TS29594_Nchf_SpendingLimitControl.yaml's own `servers`
// block (ADR-0055) -- P4.2.
constexpr const char* kSpendingLimitApiRoot = "/nchf-spendinglimitcontrol/v1";
// P4.5/ADR-0060 (Stage 3): kProductCatalogBase/kProductCatalogApiRoot/kBalanceManagementBase/
// kBalanceManagementApiRoot now live in charging_engine.hpp (chf:: namespace) -- shared with
// diameter_server.cpp, not duplicated here.

// Must match nfs/nrf/src/main.cpp's kNrfInstanceId exactly -- see docs/DECISIONS.md ADR-0018.
constexpr const char* kNrfInstanceId = "5ba9a927-1d31-4c8e-8a10-000000000001";

// Redis/Valkey connection string for CHF's stores (E3 persistence, see stores.hpp).
// ADR-0245 (task #109): the default now lives in config/chf.json, not a string literal here --
// ADR-0077's project-wide rule. CHF_REDIS_URL still overrides at deployment time, exactly as
// before, so deploy/docker/docker-compose.yml and .github/workflows/ci.yml are unaffected.
std::string chf_redis_conninfo(const nlohmann::json& config) {
    return nf_config::require<std::string>(config, "redis_url", "CHF_REDIS_URL");
}

// P4.4/ADR-0058, migrated ADR-0192: real Doris connection options for CdrWriter. ADR-0245
// (task #109): every default moved to config/chf.json, same rule and same per-key
// CHF_DORIS_* env overrides as before. The config file's shipped values match apache/doris's own
// official all-in-one Docker image (ADR-0192): FE MySQL-protocol port 9030, root user with no
// password -- a real, disclosed lab-only credential shape, same class as this project's own CHF
// PostgreSQL trust-auth precedent (ADR-0060). A real deployment overrides them; there is
// deliberately no in-source fallback if the config file omits a key.
chf::DorisOptions chf_doris_options(const nlohmann::json& config) {
    chf::DorisOptions options;
    options.host = nf_config::require<std::string>(config, "doris_host", "CHF_DORIS_HOST");
    options.port = nf_config::require<std::uint16_t>(config, "doris_port", "CHF_DORIS_PORT");
    options.user = nf_config::require<std::string>(config, "doris_user", "CHF_DORIS_USER");
    options.password =
        nf_config::require<std::string>(config, "doris_password", "CHF_DORIS_PASSWORD");
    options.database =
        nf_config::require<std::string>(config, "doris_database", "CHF_DORIS_DATABASE");
    return options;
}

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

// Runs on a dedicated thread, never on the server's io_context -- same reasoning as every other
// NF's run_nrf_lifecycle (docs/DECISIONS.md ADR-0006/ADR-0019).
void run_nrf_lifecycle(const std::string& chf_instance_id, const std::string& nrf_base) {
    sbi_core::http2::TlsConfig client_tls{
        .cert_path = CERTS_DIR "/chf/cert.pem",
        .key_path = CERTS_DIR "/chf/key.pem",
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
        http_client, nrf_base + "/oauth2/token", chf_instance_id, "nnrf-nfm", "NRF");

    constexpr int kHeartbeatSeconds = 30;
    json profile{
        {"nfInstanceId", chf_instance_id},
        {"nfType", kNfType},
        {"nfStatus", "REGISTERED"},
        {"ipv4Addresses", json::array({"127.0.0.1"})},
        {"heartBeatTimer", kHeartbeatSeconds},
    };

    while (true) {
        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("chf: OAuth2 token fetch failed: {}", token.error());
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        sbi_core::http2::ClientRequest put_req;
        put_req.method = "PUT";
        put_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + chf_instance_id;
        put_req.headers.emplace("content-type", "application/json");
        put_req.headers.emplace("authorization", "Bearer " + *token);
        put_req.headers.emplace(
            sbi_core::headers::kSenderTimestamp,
            sbi_core::headers::format_sender_timestamp(std::chrono::system_clock::now()));
        put_req.body = profile.dump();

        auto put_resp = http_client.send(put_req);
        if (put_resp.has_value() && (put_resp->status == 200 || put_resp->status == 201)) {
            spdlog::info("chf: registered with NRF (HTTP {})", put_resp->status);
            break;
        }
        spdlog::warn("chf: NRF registration attempt failed, retrying in 5s");
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(kHeartbeatSeconds / 2));

        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("chf: OAuth2 token fetch failed for heartbeat: {}", token.error());
            continue;
        }

        sbi_core::http2::ClientRequest patch_req;
        patch_req.method = "PATCH";
        patch_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + chf_instance_id;
        patch_req.headers.emplace("content-type", "application/json-patch+json");
        patch_req.headers.emplace("authorization", "Bearer " + *token);
        patch_req.body =
            json::array({json{{"op", "replace"}, {"path", "/nfStatus"}, {"value", "REGISTERED"}}})
                .dump();

        auto patch_resp = http_client.send(patch_req);
        if (!patch_resp.has_value() || patch_resp->status != 200) {
            spdlog::warn("chf: heartbeat failed");
        }
    }
}

} // namespace

int main() {
    const auto config = nf_config::load("chf", CONFIG_DIR);
    const auto port = nf_config::require<unsigned short>(config, "port");
    const auto metrics_bind_address =
        nf_config::require<std::string>(config, "metrics_bind_address");
    const auto nrf_base_url =
        nf_config::require<std::string>(config, "nrf_base_url", "CHF_NRF_BASE_URL");
    const auto rating_database_url =
        nf_config::require<std::string>(config, "rating_database_url", "CHF_RATING_DATABASE_URL");

    sbi_core::init_logging("chf");
    sbi_core::init_tracing("chf");
    sbi_core::init_metrics(metrics_bind_address);

    const std::string chf_instance_id = sbi_core::generate_uuid_v4();
    spdlog::info("chf: starting, nfInstanceId={}", chf_instance_id);

    // Task #109 (ADR-0274): touch both peer base URLs now so a missing/misspelled key in
    // config/chf.json fails fast at boot, with the resolved path in the message, rather than
    // throwing out of a charging request later. charging_engine caches them in a function-local
    // static, so this is the read that populates it.
    spdlog::info("chf: product catalog at {}, balance management at {}",
                 chf::product_catalog_base(),
                 chf::balance_management_base());

    sbi_core::http2::TlsConfig server_tls{
        .cert_path = CERTS_DIR "/chf/cert.pem",
        .key_path = CERTS_DIR "/chf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };

    sbi_core::jwt::Verifier verifier(CERTS_DIR "/nrf-jwt/public.pem", kNrfInstanceId);

    // Real Redis/Valkey persistence for CHF's stores (E3's "recoverable across restarts" -- see
    // stores.hpp's own header comment). One shared client: sw::redis::Redis pools connections
    // internally and is genuinely thread-safe, confirmed by reading its own header, not the
    // per-store single-connection-behind-a-mutex pattern bss/product-catalog uses for libpqxx
    // (ADR-0054), since libpqxx::connection has no such built-in pooling.
    auto redis = std::make_shared<sw::redis::Redis>(chf_redis_conninfo(config));
    // sw::redis::Redis's connection pool connects lazily on first command (confirmed: pool size
    // defaults to 1, no eager-connect option used here) -- a real PING here, not assumed
    // connectivity, gives the same fail-fast-at-startup behavior every other NF's real dependency
    // check already has (e.g. bss/product-catalog's libpqxx::connection, which throws immediately
    // in its own constructor if unreachable).
    redis->ping();
    spdlog::info("chf: connected to Redis/Valkey");
    chf::ChargingDataStore charging_data_store(redis);
    chf::OfflineChargingDataStore offline_charging_data_store(redis);
    chf::SpendingLimitSubscriptionStore spending_limit_store(redis);
    chf::PolicyCounterConfigStore policy_counter_config_store(redis);

    // P4.4/ADR-0058: real CDF (CDR generation, TS 32.240/32.296) -- see cdr.hpp's own header for
    // the full disclosure of what this real CDR record is (and is not: not a conformant TS 32.298
    // CDR, that spec isn't vendored -- schema.doris.sql explains why). ADR-0192: CDR storage is
    // Apache Doris.
    chf::CdrWriter cdr_writer(chf_doris_options(config));

    // P14 (ADR-0283): retention-driven archival, OFF by default.
    //
    // Default off deliberately, and not out of caution alone: these are billing records, and a
    // retention window is an operator policy decision (regulatory retention periods differ by
    // jurisdiction). A default that silently deleted CDRs after N days would be this project
    // choosing someone's compliance posture for them.
    const auto cdr_retention_days =
        config.contains("cdr_retention_days") ? config.at("cdr_retention_days").get<int>() : 0;
    const auto cdr_archive_dir = config.contains("cdr_archive_dir")
                                     ? config.at("cdr_archive_dir").get<std::string>()
                                     : std::string("/var/lib/chf/cdr-archive");
    if (cdr_retention_days > 0) {
        spdlog::info("chf: CDR retention active -- archiving rows older than {} day(s) to {}",
                     cdr_retention_days,
                     cdr_archive_dir);
        std::thread([&cdr_writer, cdr_retention_days, cdr_archive_dir] {
            // Hourly. A retention sweep is not time-critical -- a row living an extra hour past
            // its window is a non-event, while a tight loop against the CDR store is not.
            while (true) {
                std::this_thread::sleep_for(std::chrono::hours(1));
                cdr_writer.apply_retention(cdr_retention_days, cdr_archive_dir);
            }
        }).detach();
    }
    if (cdr_writer.is_connected()) {
        spdlog::info("chf: connected to Doris (CDF)");
    } else {
        spdlog::warn("chf: Doris unavailable, CDF/CDR generation disabled for this process");
    }

    // P4.5/ADR-0060 (E5): real RatingDecision audit table -- see rating_decision_store.hpp's own
    // header for the same graceful-degradation design principle CdrWriter already established
    // (ADR-0058): a PostgreSQL outage must never crash or block real-time charging.
    chf::RatingDecisionStore rating_decision_store(rating_database_url);
    if (rating_decision_store.is_connected()) {
        spdlog::info("chf: connected to PostgreSQL (RatingDecision audit, E5)");
    } else {
        spdlog::warn("chf: PostgreSQL unavailable, RatingDecision audit disabled for this process");
    }

    // P4.8 (CHARGING_PROMPT.md Angle 1a, ADR-0074): real ONNX Runtime in-process inference for
    // predictive quota sizing. Real kill switch (CHF_AI_QUOTA_SIZING_ENABLED, default OFF until
    // proven -- see this project's own procedure-list approval) and real model path
    // (CHF_QUOTA_MODEL_PATH, produced by nfs/chf/training/train_quota_sizing.py) -- both getenv-
    // based, same never-hardcode-config precedent as chf_redis_conninfo/chf_doris_options
    // above. Only main.cpp's real HTTP Nchf_ConvergedCharging Create/Update handlers below use
    // this -- Diameter Gy and CAP gsmSCF stay deterministic-only (charging_engine.hpp's own
    // header comment explains why).
    // ADR-0245 (task #109): defaults now in config/chf.json; the same CHF_* env vars still
    // override. Real, disclosed behaviour change: the enable flag is parsed as JSON, so "true"/
    // "false" work as before but a typo now fails fast at startup instead of silently meaning
    // "disabled" -- deliberate, matching nf_config's own stated no-silent-fallback discipline.
    const bool ai_quota_sizing_enabled =
        nf_config::require<bool>(config, "ai_quota_sizing_enabled", "CHF_AI_QUOTA_SIZING_ENABLED");
    const std::string quota_model_path =
        nf_config::require<std::string>(config, "quota_model_path", "CHF_QUOTA_MODEL_PATH");
    // Real, disclosed (ADR-0150): the AI inference latency budget (50000us, raised from the
    // original 5000us after a real observed CI failure on a contended runner -- see
    // ai_inference.hpp's own header comment). ADR-0245 (task #109): the value now lives in
    // config/chf.json rather than being read from an env var against an in-source constant
    // default; CHF_AI_QUOTA_LATENCY_BUDGET_US still overrides. chf::kDefaultAiQuotaLatencyBudget
    // remains AiQuotaSizer's own library-level default for callers that construct one directly
    // (the conformance tests do) -- it is no longer this binary's deployment default.
    const std::chrono::microseconds ai_quota_latency_budget{nf_config::require<std::int64_t>(
        config, "ai_quota_latency_budget_us", "CHF_AI_QUOTA_LATENCY_BUDGET_US")};
    chf::AiQuotaSizer ai_quota_sizer(
        quota_model_path, ai_quota_sizing_enabled, ai_quota_latency_budget);
    chf::QuotaFeatureStore quota_feature_store(redis);

    // CHF's own client to bss/product-catalog (ADR-0048) -- mTLS only, no OAuth2 (product-catalog
    // has no NRF-issued token source, see ADR-0047). Only ever touched from route handlers, which
    // all run on ioc's single thread -- same "second client safe on the shared ioc thread" pattern
    // ADR-0027 established.
    sbi_core::http2::TlsConfig catalog_client_tls{
        .cert_path = CERTS_DIR "/chf/cert.pem",
        .key_path = CERTS_DIR "/chf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client catalog_client(std::move(catalog_client_tls));

    // P4.3 (ADR-0056/0057): CHF's own client to bss/balance-management -- same mTLS-only, no-OAuth2
    // reasoning as catalog_client above (balance-management, like product-catalog, has no
    // NRF-issued token source).
    sbi_core::http2::TlsConfig balance_client_tls{
        .cert_path = CERTS_DIR "/chf/cert.pem",
        .key_path = CERTS_DIR "/chf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client balance_client(std::move(balance_client_tls));

    // ADR-0072 (gap-closure: real N28 end-to-end). CHF's own real client for pushing
    // statusNotification callbacks to subscribers' arbitrary notifUri targets -- same mTLS-only,
    // no-OAuth2 reasoning as catalog_client/balance_client above (the callback target is whichever
    // NF supplied notifUri, not a fixed NRF-discoverable service this project could token-source
    // for generically).
    sbi_core::http2::TlsConfig notify_client_tls{
        .cert_path = CERTS_DIR "/chf/cert.pem",
        .key_path = CERTS_DIR "/chf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client notify_client(std::move(notify_client_tls));

    auto meter = sbi_core::get_meter("chf");
    auto create_counter = meter->CreateUInt64Counter("chf_charging_data_create_total",
                                                     "Total Nchf_ConvergedCharging_Create calls");
    // P12 (ADR-0282): a business-level alarm signal, not an operational one -- each increment is a
    // charging record that never reached the CDR store for a session that has now ended.
    auto cdr_sequence_gap_counter = meter->CreateUInt64Counter(
        "chf_cdr_sequence_gap_total",
        "Missing CDR invocationSequenceNumbers detected at session release (P12 business alarm)");
    auto release_counter = meter->CreateUInt64Counter("chf_charging_data_release_total",
                                                      "Total Nchf_ConvergedCharging_Release calls");
    auto update_counter = meter->CreateUInt64Counter("chf_charging_data_update_total",
                                                     "Total Nchf_ConvergedCharging_Update calls");
    auto grant_counter = meter->CreateUInt64Counter(
        "chf_rating_grant_total", "Total real GrantedUnit rating decisions issued");
    auto reserve_rejected_counter = meter->CreateUInt64Counter(
        "chf_balance_reserve_rejected_total",
        "Total grants withheld because the real balance reservation was rejected (P4.3/ADR-0057)");
    auto offline_create_counter = meter->CreateUInt64Counter(
        "chf_offline_charging_data_create_total", "Total Nchf_OfflineOnlyCharging_Create calls");
    auto offline_update_counter = meter->CreateUInt64Counter(
        "chf_offline_charging_data_update_total", "Total Nchf_OfflineOnlyCharging_Update calls");
    auto offline_release_counter = meter->CreateUInt64Counter(
        "chf_offline_charging_data_release_total", "Total Nchf_OfflineOnlyCharging_Release calls");
    auto spending_limit_subscribe_counter = meter->CreateUInt64Counter(
        "chf_spending_limit_subscribe_total", "Total Nchf_SpendingLimitControl Subscribe calls");
    auto spending_limit_update_counter = meter->CreateUInt64Counter(
        "chf_spending_limit_update_total", "Total Nchf_SpendingLimitControl subscription updates");
    auto spending_limit_unsubscribe_counter =
        meter->CreateUInt64Counter("chf_spending_limit_unsubscribe_total",
                                   "Total Nchf_SpendingLimitControl Unsubscribe calls");
    // P4.5/ADR-0060 Stage 3: real Diameter Gy CCR-I/U/T counters, mirroring
    // create_counter/update_counter/release_counter's own per-operation shape above -- the same
    // real charging decisions, arriving over a different real 3GPP protocol.
    auto ccr_initial_counter = meter->CreateUInt64Counter(
        "chf_diameter_ccr_initial_total", "Total real Diameter Gy CCR-Initial requests handled");
    auto ccr_update_counter = meter->CreateUInt64Counter(
        "chf_diameter_ccr_update_total", "Total real Diameter Gy CCR-Update requests handled");
    auto ccr_termination_counter =
        meter->CreateUInt64Counter("chf_diameter_ccr_termination_total",
                                   "Total real Diameter Gy CCR-Termination requests handled");
    // P4.5/ADR-0059 Stage 4 (Rf half): real Diameter Base Accounting ACR counters, mirroring
    // offline_create_counter/offline_update_counter/offline_release_counter's own per-operation
    // shape above -- the same real Nchf_OfflineOnlyCharging decisions, arriving over Rf instead of
    // the HTTP SBI path. EVENT_RECORD gets its own counter (a real, distinct Accounting-Record-Type
    // from START_RECORD, not folded together).
    auto acr_event_counter =
        meter->CreateUInt64Counter("chf_diameter_acr_event_total",
                                   "Total real Diameter Rf ACR (EVENT_RECORD) requests handled");
    auto acr_start_counter =
        meter->CreateUInt64Counter("chf_diameter_acr_start_total",
                                   "Total real Diameter Rf ACR (START_RECORD) requests handled");
    auto acr_interim_counter =
        meter->CreateUInt64Counter("chf_diameter_acr_interim_total",
                                   "Total real Diameter Rf ACR (INTERIM_RECORD) requests handled");
    auto acr_stop_counter = meter->CreateUInt64Counter(
        "chf_diameter_acr_stop_total", "Total real Diameter Rf ACR (STOP_RECORD) requests handled");
    // P4.5/ADR-0059 Stage 4 (Sy half): real TS 29.219 SLR/STR counters, mirroring
    // spending_limit_subscribe_counter/spending_limit_update_counter/
    // spending_limit_unsubscribe_counter's own per-operation shape above -- the same real
    // Nchf_SpendingLimitControl decisions, arriving over Sy instead of the HTTP SBI path.
    auto slr_initial_counter =
        meter->CreateUInt64Counter("chf_diameter_slr_initial_total",
                                   "Total real Diameter Sy SLR (INITIAL_REQUEST) requests handled");
    auto slr_intermediate_counter = meter->CreateUInt64Counter(
        "chf_diameter_slr_intermediate_total",
        "Total real Diameter Sy SLR (INTERMEDIATE_REQUEST) requests handled");
    auto str_counter =
        meter->CreateUInt64Counter("chf_diameter_str_total",
                                   "Total real Diameter Sy STR (Final Spending Limit Report) "
                                   "requests handled");
    // P4.5/ADR-0061: real CAP (TS 29.078) counters, mirroring the Diameter counters' own
    // per-operation shape above -- the same real charging decisions, arriving over CAP/SS7
    // instead of Diameter or the HTTP SBI path.
    auto cap_initial_dp_counter = meter->CreateUInt64Counter(
        "chf_cap_initial_dp_total", "Total real CAP InitialDP requests handled");
    auto cap_apply_charging_counter = meter->CreateUInt64Counter(
        "chf_cap_apply_charging_total", "Total real CAP ApplyCharging responses sent");

    // P4.5/ADR-0060 Stage 3 + ADR-0059 Stage 4: real Diameter server -- CER/CEA capability exchange
    // (Stage 2) plus real CCR-I/U/T (Stage 3, Gy) dispatched through the exact same
    // charging_engine.hpp functions Nchf_ConvergedCharging's HTTP Create/Update/Release handlers
    // below call, real ACR (Stage 4, Rf) dispatched onto the same offline_charging_data_store
    // Nchf_OfflineOnlyCharging's own HTTP handlers use, and real SLR/STR (Stage 4, Sy) dispatched
    // onto the same spending_limit_store Nchf_SpendingLimitControl's own HTTP handlers use -- the
    // single-code-path property CHARGING_PROMPT.md's P4.5 explicitly requires, for all three
    // protocols. Constructed here, after catalog_client/balance_client/the counters above all
    // exist, since its own per-connection threads need real, already-built dependencies to share
    // (see diameter_server.hpp's own header for why it builds its OWN dedicated catalog/balance
    // http2::Client pair per connection rather than reusing catalog_client/balance_client, which
    // are confined to ioc's thread).
    sbi_core::http2::TlsConfig diameter_client_tls{
        .cert_path = CERTS_DIR "/chf/cert.pem",
        .key_path = CERTS_DIR "/chf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    chf::DiameterServer diameter_server(kDiameterPort,
                                        kDiameterOriginHost,
                                        kDiameterOriginRealm,
                                        std::move(diameter_client_tls),
                                        charging_data_store,
                                        cdr_writer,
                                        rating_decision_store,
                                        offline_charging_data_store,
                                        spending_limit_store,
                                        policy_counter_config_store,
                                        grant_counter.get(),
                                        reserve_rejected_counter.get(),
                                        ccr_initial_counter.get(),
                                        ccr_update_counter.get(),
                                        ccr_termination_counter.get(),
                                        acr_event_counter.get(),
                                        acr_start_counter.get(),
                                        acr_interim_counter.get(),
                                        acr_stop_counter.get(),
                                        slr_initial_counter.get(),
                                        slr_intermediate_counter.get(),
                                        str_counter.get());
    // P15 (ADR-0285): the Diameter front door's own TPS ceiling, separate from the SBI one --
    // "per-protocol" is what P15 asks for, and a shared budget would let an SBI storm starve Gy.
    // OFF unless `diameter_max_tps` is configured; see diameter_server.cpp's own Result-Code
    // caveat for the second reason it is off by default.
    // ADR-0295: env override, same config-file-first-then-env convention nf_config::require and
    // sbi_core::read_tps_limit's own SBI_MAX_TPS already use. It exists so the ceiling can be
    // driven from a test without a test value living in checked-in config.
    if (chf_tps_setting(config, "diameter_max_tps", "CHF_DIAMETER_MAX_TPS").has_value()) {
        const auto diameter_tps =
            *chf_tps_setting(config, "diameter_max_tps", "CHF_DIAMETER_MAX_TPS");
        if (diameter_tps > 0.0) {
            const auto diameter_burst =
                chf_tps_setting(config, "diameter_tps_burst", "CHF_DIAMETER_TPS_BURST")
                    .value_or(diameter_tps);
            diameter_server.set_tps_limit(diameter_tps, diameter_burst);
            spdlog::info("chf: Diameter TPS ceiling active: {} req/s sustained, burst {}",
                         diameter_tps,
                         diameter_burst);
        }
    }

    spdlog::info("chf: Diameter (Gy+Rf+Sy) listening on tcp://0.0.0.0:{}", kDiameterPort);

    // P4.5/ADR-0061: real CAP server -- CHF plays the real gsmSCF role, receiving InitialDP from
    // a real gsmSSF peer and dispatching into the exact same charge_one_usage shared code path
    // above, extending CHARGING_PROMPT.md's single-code-path property to a fourth real protocol.
    // Real, disclosed scope: only the InitialDP -> RequestReportBCSMEvent+ApplyCharging half of
    // the real call flow is implemented -- see cap_server.hpp's own header for the full real,
    // disclosed gap list (EventReportBCSM/ApplyChargingReport not yet handled).
    sbi_core::http2::TlsConfig cap_client_tls{
        .cert_path = CERTS_DIR "/chf/cert.pem",
        .key_path = CERTS_DIR "/chf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    chf::CapServer cap_server(kCapPort,
                              std::move(cap_client_tls),
                              charging_data_store,
                              cdr_writer,
                              rating_decision_store,
                              grant_counter.get(),
                              reserve_rejected_counter.get(),
                              cap_initial_dp_counter.get(),
                              cap_apply_charging_counter.get());
    // P15 (ADR-0288): the SS7/M3UA front door's own ceiling -- the third protocol, completing
    // "per-protocol" TPS governance. Its own bucket, like Diameter's: a CAP storm and an SBI storm
    // are different failures and must not share a budget.
    if (chf_tps_setting(config, "cap_max_tps", "CHF_CAP_MAX_TPS").has_value()) {
        const auto cap_tps = *chf_tps_setting(config, "cap_max_tps", "CHF_CAP_MAX_TPS");
        if (cap_tps > 0.0) {
            const auto cap_burst =
                chf_tps_setting(config, "cap_tps_burst", "CHF_CAP_TPS_BURST").value_or(cap_tps);
            cap_server.set_tps_limit(cap_tps, cap_burst);
            spdlog::info("chf: CAP/SS7 TPS ceiling active: {} msg/s sustained, burst {}",
                         cap_tps,
                         cap_burst);
        }
    }

    spdlog::info("chf: CAP (gsmSCF) listening on sctp://0.0.0.0:{}", kCapPort);

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

    // --- Nchf_ConvergedCharging ---

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/chargingdata",
        [&verifier,
         &charging_data_store,
         &create_counter,
         &catalog_client,
         &balance_client,
         &grant_counter,
         &reserve_rejected_counter,
         &cdr_writer,
         &rating_decision_store,
         &ai_quota_sizer,
         &quota_feature_store,
         &chf_instance_id](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<
                sbi_gen::ChargingDataRequest_Nchf_ConvergedCharging>(req, err);
            if (!body.has_value()) {
                return err;
            }

            const auto supi = body->subscriberIdentifier.value_or("");
            const auto ref = charging_data_store.create(supi);

            // docs/CHARGING_MAPPING.md's resolved mapping: build the TM Forum SID record for the
            // subscriber this charging event is for. Logged, not yet persisted or exposed via a
            // real TMF632 REST surface (no Party-management store exists in this codebase yet) --
            // this demonstrates CHF's internal charging record is genuinely SID-shaped, which is
            // as much of the mapping as has a real, unambiguous 3GPP field to build it from today
            // (see the mapping doc's own scope section for why every other field is deferred).
            if (body->subscriberIdentifier.has_value()) {
                const auto individual =
                    bss_sid::map_supi_to_individual(*body->subscriberIdentifier);
                spdlog::info("chf: mapped subscriberIdentifier to TM Forum SID Individual: {}",
                             nlohmann::json(individual).dump());
            }

            sbi_gen::ChargingDataResponse_Nchf_ConvergedCharging response{};
            response.invocationTimeStamp =
                sbi_core::format_rfc3339(std::chrono::system_clock::now());
            // See file header for why this echoes the request's value rather than assigning an
            // independent CHF-side sequence.
            response.invocationSequenceNumber = body->invocationSequenceNumber;

            // ADR-0048/ADR-0057: the real rating engine. Only runs if the request actually asked
            // for units (a real MultipleUnitUsage entry, mandatory ratingGroup) -- SMF's own call
            // always sends exactly one (see nfs/smf/src/main.cpp), but this handler doesn't
            // assume that, it reads what's actually there. ADR-0057: a grant is only actually
            // included in the response if its real monetary cost was successfully reserved
            // against the subscriber's real balance (bss/balance-management) -- real prepaid
            // enforcement, closing this project's long-disclosed "no balance/wallet deduction"
            // gap.
            if (body->multipleUnitUsage.has_value()) {
                std::vector<sbi_gen::MultipleUnitInformation> granted;
                for (const auto& usage : *body->multipleUnitUsage) {
                    sbi_gen::MultipleUnitInformation info{};
                    info.ratingGroup = usage.ratingGroup;
                    const auto charged = chf::charge_one_usage(
                        catalog_client,
                        balance_client,
                        cdr_writer,
                        rating_decision_store,
                        charging_data_store,
                        grant_counter.get(),
                        reserve_rejected_counter.get(),
                        ref,
                        "Create",
                        supi,
                        body->nfConsumerIdentification.nodeFunctionality.value,
                        chf_instance_id,
                        body->invocationSequenceNumber,
                        usage,
                        &ai_quota_sizer,
                        &quota_feature_store,
                        sbi_core::parse_rfc3339_to_time_t(body->invocationTimeStamp),
                        // ADR-0303: the request's own attributes, so an offering scoped to a
                        // slice, a UPF, a DNN or a visited PLMN can be matched or skipped.
                        chf::collect_charging_attributes(*body, usage));
                    if (charged.reserved && charged.rating.grant.has_value()) {
                        info.grantedUnit = charged.rating.grant;
                        // ADR-0072 (gap-closure: real N40 product-configurability): real
                        // quota-policy fields, populated from the matched ProductOfferingPrice's
                        // own real characteristics -- see build_rating_grant's own comment.
                        info.validityTime = charged.rating.validityTimeSec;
                        info.quotaHoldingTime = charged.rating.quotaHoldingTimeSec;
                        info.volumeQuotaThreshold = charged.rating.volumeQuotaThreshold;
                        info.timeQuotaThreshold = charged.rating.timeQuotaThreshold;
                        info.unitQuotaThreshold = charged.rating.unitQuotaThreshold;
                    }
                    granted.push_back(info);
                }
                response.multipleUnitInformation = std::move(granted);
            }

            create_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location", std::string(kApiRoot) + "/chargingdata/" + ref);
            json j = response;
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/chargingdata/{ChargingDataRef}/update",
        [&verifier,
         &charging_data_store,
         &update_counter,
         &catalog_client,
         &balance_client,
         &grant_counter,
         &reserve_rejected_counter,
         &cdr_writer,
         &rating_decision_store,
         &ai_quota_sizer,
         &quota_feature_store,
         &chf_instance_id](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<
                sbi_gen::ChargingDataRequest_Nchf_ConvergedCharging>(req, err);
            if (!body.has_value()) {
                return err;
            }

            const auto ref = req.path_params.at("ChargingDataRef");
            if (!charging_data_store.is_active(ref)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No active charging data resource " + ref);
            }
            const auto supi = charging_data_store.get_supi(ref).value_or("");

            // ADR-0050 Stage 4: log the real reported usage this call carries -- SMF's Stage 3
            // Nchf_ConvergedCharging_Update, itself built from UPF's real Stage 2 Usage Report.
            // This is CHF's real evidence that consumption tracking closed the loop, not a
            // fabricated placeholder.
            if (body->multipleUnitUsage.has_value()) {
                for (const auto& usage : *body->multipleUnitUsage) {
                    if (usage.usedUnitContainer.has_value()) {
                        for (const auto& used : *usage.usedUnitContainer) {
                            spdlog::info(
                                "chf: Update for ChargingDataRef={} reports ratingGroup={} "
                                "used {} octets (localSequenceNumber={})",
                                ref,
                                usage.ratingGroup,
                                used.totalVolume.value_or(0),
                                used.localSequenceNumber);
                        }
                    }
                }
            }

            sbi_gen::ChargingDataResponse_Nchf_ConvergedCharging response{};
            response.invocationTimeStamp =
                sbi_core::format_rfc3339(std::chrono::system_clock::now());
            response.invocationSequenceNumber = body->invocationSequenceNumber;

            // Real re-authorization: a fresh grant for continued usage, from the same rating
            // engine Create already uses -- ADR-0057: a real reservation against the subscriber's
            // balance is attempted for each fresh grant too, same prepaid-enforcement point as
            // Create. Still disclosed, real simplification carried forward: no differentiation
            // between a Volume-Threshold report and a Volume-Quota-exhaustion one (see this
            // file's own header comment for why), and no proportional finalize against what was
            // actually reported used this call (see finalize_subscriber_balance's own comment --
            // Release finalizes the full session total, not incrementally per Update).
            if (body->multipleUnitUsage.has_value()) {
                std::vector<sbi_gen::MultipleUnitInformation> granted;
                for (const auto& usage : *body->multipleUnitUsage) {
                    sbi_gen::MultipleUnitInformation info{};
                    info.ratingGroup = usage.ratingGroup;
                    const auto charged = chf::charge_one_usage(
                        catalog_client,
                        balance_client,
                        cdr_writer,
                        rating_decision_store,
                        charging_data_store,
                        grant_counter.get(),
                        reserve_rejected_counter.get(),
                        ref,
                        "Update",
                        supi,
                        body->nfConsumerIdentification.nodeFunctionality.value,
                        chf_instance_id,
                        body->invocationSequenceNumber,
                        usage,
                        &ai_quota_sizer,
                        &quota_feature_store,
                        sbi_core::parse_rfc3339_to_time_t(body->invocationTimeStamp),
                        // ADR-0303: the request's own attributes, so an offering scoped to a
                        // slice, a UPF, a DNN or a visited PLMN can be matched or skipped.
                        chf::collect_charging_attributes(*body, usage));
                    if (charged.reserved && charged.rating.grant.has_value()) {
                        info.grantedUnit = charged.rating.grant;
                        // ADR-0072 (gap-closure: real N40 product-configurability): real
                        // quota-policy fields, populated from the matched ProductOfferingPrice's
                        // own real characteristics -- see build_rating_grant's own comment.
                        info.validityTime = charged.rating.validityTimeSec;
                        info.quotaHoldingTime = charged.rating.quotaHoldingTimeSec;
                        info.volumeQuotaThreshold = charged.rating.volumeQuotaThreshold;
                        info.timeQuotaThreshold = charged.rating.timeQuotaThreshold;
                        info.unitQuotaThreshold = charged.rating.unitQuotaThreshold;
                    }
                    granted.push_back(info);
                }
                response.multipleUnitInformation = std::move(granted);
            }

            // CHARGING_PROMPT.md's own explicit P4.4 requirement: real gap detection. Checked on
            // every Update (not just Release) so a missing invocationSequenceNumber is surfaced
            // as close to real time as this build's synchronous request handling allows, not only
            // discovered at session end.
            try {
                const auto gaps = cdr_writer.detect_gaps(ref);
                if (!gaps.empty()) {
                    std::string gap_list;
                    for (const auto& gap : gaps) {
                        if (!gap_list.empty()) {
                            gap_list += ", ";
                        }
                        gap_list += std::to_string(gap);
                    }
                    spdlog::warn("chf: CDR sequence gap detected for ChargingDataRef={} -- missing "
                                 "invocationSequenceNumber(s): {}",
                                 ref,
                                 gap_list);
                }
            } catch (const std::exception& e) {
                spdlog::warn("chf: CDR gap-detection query failed for ChargingDataRef={}: {}",
                             ref,
                             e.what());
            }

            update_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            json j = response;
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/chargingdata/{ChargingDataRef}/release",
        [&verifier,
         &charging_data_store,
         &release_counter,
         &balance_client,
         &cdr_writer,
         &cdr_sequence_gap_counter,
         &chf_instance_id](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            // Real spec shape (requestBody required: true, schema ChargingDataRequest) -- parsed
            // for validation/mandatory-field-checking parity with Create.
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<
                sbi_gen::ChargingDataRequest_Nchf_ConvergedCharging>(req, err);
            if (!body.has_value()) {
                return err;
            }

            const auto ref = req.path_params.at("ChargingDataRef");
            // ADR-0057: finalize (real permanent debit + unreserve) whatever this session
            // reserved, BEFORE releasing the ref -- get_supi/get_reserved_total read
            // chf:cdr:content:{ref}, which release() deliberately does not erase (see
            // ChargingDataStore::release's own comment), but reading before releasing keeps the
            // real order-of-operations obviously correct rather than relying on that.
            const auto supi = charging_data_store.get_supi(ref);
            const auto reserved_total = charging_data_store.get_reserved_total(ref);
            // ADR-0297: what was granted across this session, to proportion the debit against.
            const auto granted_volume = charging_data_store.get_granted_volume(ref);
            const auto granted_service_units = charging_data_store.get_granted_service_units(ref);
            const auto granted_time = charging_data_store.get_granted_time(ref); // ADR-0304
            if (!charging_data_store.release(ref)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No active charging data resource " + ref);
            }
            // ADR-0297: what the consumer actually reported used, summed across every
            // usedUnitContainer of every rating group in this Release -- the real TS 32.291 figure
            // that was previously decoded, written to the CDR, and then ignored when charging.
            double used_volume = 0.0;
            double used_service_units = 0.0;
            double used_time = 0.0; // ADR-0304: usedUnitContainer's own `time`, in seconds
            if (body->multipleUnitUsage.has_value()) {
                for (const auto& usage : *body->multipleUnitUsage) {
                    if (!usage.usedUnitContainer.has_value()) {
                        continue;
                    }
                    for (const auto& container : *usage.usedUnitContainer) {
                        used_volume += static_cast<double>(container.totalVolume.value_or(0));
                        used_service_units +=
                            static_cast<double>(container.serviceSpecificUnits.value_or(0));
                        used_time += static_cast<double>(container.time.value_or(0));
                    }
                }
            }
            const auto amount_to_debit = chf::proportional_debit(reserved_total,
                                                                 granted_volume,
                                                                 used_volume,
                                                                 granted_service_units,
                                                                 used_service_units,
                                                                 granted_time,
                                                                 used_time);
            if (supi.has_value() && !supi->empty()) {
                spdlog::info("chf: Release finalize for {} -- reserved {}, debiting {} "
                             "(used {} of {} octets, {} of {} service units)",
                             ref,
                             reserved_total,
                             amount_to_debit,
                             used_volume,
                             granted_volume,
                             used_service_units,
                             granted_service_units);
                chf::finalize_subscriber_balance(balance_client,
                                                 *supi,
                                                 reserved_total,
                                                 amount_to_debit,
                                                 "Nchf_ConvergedCharging_Release " + ref);
            }

            // P4.4/ADR-0058: a real, final CDR row for this session -- reserved_cost here is the
            // session's TOTAL committed cost (finalize_subscriber_balance's own real amount), not
            // a per-rating-group figure the way Create/Update's rows are.
            try {
                chf::CdrRecord cdr{};
                cdr.charging_data_ref = ref;
                cdr.invocation_sequence_number = body->invocationSequenceNumber;
                cdr.service_type = "ConvergedCharging";
                cdr.operation = "Release";
                cdr.subscriber_identifier = supi.value_or("");
                cdr.nf_consumer_node_functionality =
                    body->nfConsumerIdentification.nodeFunctionality.value;
                cdr.recording_network_function_id = chf_instance_id;
                if (reserved_total > 0.0) {
                    cdr.reserved_cost = reserved_total;
                }
                // Real TS 32.291 `invocationTimeStamp` from the Release request itself, falling
                // back to write time only if the peer sent something unparseable (see
                // write_converged_charging_cdr's own header for why this is not now()).
                cdr.invocation_time_stamp =
                    sbi_core::parse_rfc3339_to_time_t(body->invocationTimeStamp)
                        .value_or(
                            std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
                cdr_writer.write(cdr);

                // P12 (ADR-0282): CDR sequence-gap detection, raised as a business alarm.
                //
                // docs/DATA_MODEL.md's E4 says a gap "is itself a P12 business-level alarm
                // condition, not just a log line". CdrWriter::detect_gaps has existed since
                // ADR-0058 and was never called by anything -- the detection was built, the alarm
                // was not. Release is the right moment: the session is over, so its sequence
                // numbers should be contiguous, and a hole means a charging record for this
                // session never reached the CDR store. That is lost revenue, not a diagnostic.
                const auto gaps = cdr_writer.detect_gaps(ref);
                if (!gaps.empty()) {
                    cdr_sequence_gap_counter->Add(static_cast<std::uint64_t>(gaps.size()));
                    std::string missing;
                    for (const auto gap : gaps) {
                        missing += (missing.empty() ? "" : ",") + std::to_string(gap);
                    }
                    spdlog::error("chf: CDR SEQUENCE GAP for ChargingDataRef={} -- missing "
                                  "invocationSequenceNumber(s) {}: a charging record for this "
                                  "session never reached the CDR store",
                                  ref,
                                  missing);
                }
            } catch (const std::exception& e) {
                spdlog::warn(
                    "chf: CDR write to Doris failed for ChargingDataRef={}: {}", ref, e.what());
            }

            release_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nchf_OfflineOnlyCharging (TS 32.291, P4.2/ADR-0055) ---
    //
    // Real spec shape confirmed directly against TS32291_Nchf_OfflineOnlyCharging.yaml: unlike
    // ConvergedCharging, ChargingDataResponse_Nchf_OfflineOnlyCharging carries no
    // multipleUnitInformation/grantedUnit field at all -- offline charging (e.g. bulk SMS,
    // delayed-CDR events) records usage for later billing, it does not grant real-time online
    // quota. So this handler set does NOT call the rating engine (build_rating_grant) at all,
    // unlike Create/Update above -- a real, spec-driven difference, not an oversight.

    server.add_route(
        "POST",
        std::string(kOfflineApiRoot) + "/offlinechargingdata",
        [&verifier, &offline_charging_data_store, &offline_create_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<
                sbi_gen::ChargingDataRequest_Nchf_OfflineOnlyCharging>(req, err);
            if (!body.has_value()) {
                return err;
            }

            const auto ref = offline_charging_data_store.create();

            sbi_gen::ChargingDataResponse_Nchf_OfflineOnlyCharging response{};
            response.invocationTimeStamp =
                sbi_core::format_rfc3339(std::chrono::system_clock::now());
            response.invocationSequenceNumber = body->invocationSequenceNumber;

            offline_create_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kOfflineApiRoot) + "/offlinechargingdata/" + ref);
            json j = response;
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "POST",
        std::string(kOfflineApiRoot) + "/offlinechargingdata/{OfflineChargingDataRef}/update",
        [&verifier, &offline_charging_data_store, &offline_update_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<
                sbi_gen::ChargingDataRequest_Nchf_OfflineOnlyCharging>(req, err);
            if (!body.has_value()) {
                return err;
            }

            const auto ref = req.path_params.at("OfflineChargingDataRef");
            if (!offline_charging_data_store.is_active(ref)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No active offline charging data resource " + ref);
            }

            sbi_gen::ChargingDataResponse_Nchf_OfflineOnlyCharging response{};
            response.invocationTimeStamp =
                sbi_core::format_rfc3339(std::chrono::system_clock::now());
            response.invocationSequenceNumber = body->invocationSequenceNumber;

            offline_update_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            json j = response;
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "POST",
        std::string(kOfflineApiRoot) + "/offlinechargingdata/{OfflineChargingDataRef}/release",
        [&verifier, &offline_charging_data_store, &offline_release_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<
                sbi_gen::ChargingDataRequest_Nchf_OfflineOnlyCharging>(req, err);
            if (!body.has_value()) {
                return err;
            }

            const auto ref = req.path_params.at("OfflineChargingDataRef");
            if (!offline_charging_data_store.release(ref)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No active offline charging data resource " + ref);
            }
            offline_release_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nchf_SpendingLimitControl (TS 29.594, P4.2/ADR-0055) ---
    //
    // Real spec shape confirmed directly against TS29594_Nchf_SpendingLimitControl.yaml: CHF is
    // the SERVER for this service (PCF subscribes TO CHF), unlike the "N28 wiring" phrase in
    // CHARGING_PROMPT.md's P4.2 prompt might suggest at a glance -- see ADR-0055 for the full
    // finding. Subscribe/Update/Unsubscribe below are real, live resource CRUD.
    //
    // UPDATE (ADR-0072, gap-closure: real N28 end-to-end): `currentStatus` is no longer a
    // hardcoded "unknown" -- it's a real, configurable value (PolicyCounterConfigStore, Redis-
    // backed) set via this project's own `/chf-admin/v1/policy-counters/{policyCounterId}` PUT
    // endpoint (NOT a 3GPP-defined resource -- real, disclosed, this project's own operator/GUI
    // config surface, since the spec itself leaves the actual status values operator-defined). The
    // real statusNotification callback (CHF as client, POSTing `{notifUri}/notify`) is now
    // implemented too, triggered by that same admin endpoint -- every active subscription naming
    // the changed policyCounterId gets a real push. Real, disclosed non-scope: no automated
    // balance/usage-threshold-crossing engine triggers this on its own; the trigger is
    // operator/GUI-driven (a real, disclosed choice, not a gap being hidden).

    server.add_route(
        "POST",
        std::string(kSpendingLimitApiRoot) + "/subscriptions",
        [&verifier,
         &spending_limit_store,
         &policy_counter_config_store,
         &spending_limit_subscribe_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::SpendingLimitContext>(req, err);
            if (!body.has_value()) {
                return err;
            }

            const auto status =
                chf::build_spending_limit_status(*body, policy_counter_config_store);
            const auto id = spending_limit_store.create(*body);
            spending_limit_subscribe_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kSpendingLimitApiRoot) + "/subscriptions/" + id);
            json j = status;
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "PUT",
        std::string(kSpendingLimitApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier,
         &spending_limit_store,
         &policy_counter_config_store,
         &spending_limit_update_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::SpendingLimitContext>(req, err);
            if (!body.has_value()) {
                return err;
            }

            const auto id = req.path_params.at("subscriptionId");
            if (!spending_limit_store.update(id, *body)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No active spending limit subscription " + id);
            }
            spending_limit_update_counter->Add(1);

            const auto status =
                chf::build_spending_limit_status(*body, policy_counter_config_store);
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            json j = status;
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "DELETE",
        std::string(kSpendingLimitApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &spending_limit_store, &spending_limit_unsubscribe_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            if (!spending_limit_store.remove(id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No active spending limit subscription " + id);
            }
            spending_limit_unsubscribe_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // ADR-0072 (gap-closure: real N28 end-to-end). Real, THIS-PROJECT-OWNED operator/GUI config
    // endpoint -- NOT a 3GPP-defined resource (no such path exists in TS29594; PolicyCounterInfo.
    // currentStatus is explicitly operator-defined per that spec's own text, see stores.hpp's own
    // PolicyCounterConfigStore comment). Setting a status here is also the real trigger this
    // project's own statusNotification push now has: every currently-active subscription that
    // named this policyCounterId gets a real POST to `{notifUri}/notify` with the freshly-rebuilt
    // SpendingLimitStatus, closing the loop the file's own original header comment disclosed as
    // not-yet-implemented ("no real policy-counter-breach-detection engine exists yet to trigger
    // them from" -- this admin endpoint IS that trigger, operator/GUI-driven rather than
    // usage-driven, a real and disclosed choice given no real balance-threshold-crossing engine
    // exists in this codebase to trigger it automatically instead).
    server.add_route(
        "PUT",
        "/chf-admin/v1/policy-counters/{policyCounterId}",
        [&verifier, &policy_counter_config_store, &spending_limit_store, &notify_client](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json body;
            try {
                body = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            if (!body.contains("currentStatus") || !body["currentStatus"].is_string()) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", "body must be {\"currentStatus\": \"<string>\"}");
            }
            const auto policy_counter_id = req.path_params.at("policyCounterId");
            const std::string new_status = body["currentStatus"].get<std::string>();
            policy_counter_config_store.set_status(policy_counter_id, new_status);

            std::size_t notified = 0;
            for (auto& [subscription_id, context] : spending_limit_store.list_all()) {
                if (!context.notifUri.has_value() || !context.policyCounterIds.has_value() ||
                    std::find(context.policyCounterIds->begin(),
                              context.policyCounterIds->end(),
                              policy_counter_id) == context.policyCounterIds->end()) {
                    continue;
                }
                const auto status =
                    chf::build_spending_limit_status(context, policy_counter_config_store);
                sbi_core::http2::ClientRequest notify_req;
                notify_req.method = "POST";
                notify_req.url = *context.notifUri + "/notify";
                notify_req.headers.emplace("content-type", "application/json");
                json j = status;
                notify_req.body = j.dump();
                auto notify_resp = notify_client.send(notify_req);
                if (!notify_resp.has_value() || notify_resp->status != 204) {
                    spdlog::warn("chf: statusNotification push to {} failed for subscription {}",
                                 *context.notifUri,
                                 subscription_id);
                } else {
                    ++notified;
                }
            }
            spdlog::info("chf: policy counter {} set to '{}', pushed to {} subscription(s)",
                         policy_counter_id,
                         new_status,
                         notified);

            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    std::thread(run_nrf_lifecycle, chf_instance_id, nrf_base_url).detach();

    server.start();
    spdlog::info("chf: listening on https://0.0.0.0:{} (TLS 1.3 + mTLS)", port);
    spdlog::info("chf: Prometheus metrics at http://{}/metrics", metrics_bind_address);
    sbi_core::run_multi_threaded(ioc);
    return 0;
}
