// MFAF -- Messaging Framework Adaptor Function (ADR-0365; architecture ADR-0359).
//
// Stage 2: TS 23.288 V19.7.0 clause 5A.3.2 ("Data Delivery via a Messaging Framework") and the
// procedure of clause 6.2.6.3.4 (specs/3gpp/TS_23.288_j70.txt). Stage 3: TS 29.576 V19.7.0
// (specs/3gpp/TS_29.576_j70.txt) and its three R19 YAML files in specs/5G_APIs-REL-19/
// (forge.3gpp.org, REL-19, commit bca84b6), every DTO generated from them, every API root the
// YAML's own servers[0].url (ADR-0325):
//
//   Nmfaf_3daDataManagement   {apiRoot}/nmfaf-3dadatamanagement/v1   Configure (POST, PUT),
//                                                                    Deconfigure (DELETE)
//   Nmfaf_3caDataManagement   callbacks only -- Notify is a POST the MFAF SENDS to the
//                             consumer's notificationURI; Fetch is a POST the consumer sends to
//                             the fetchUri the MFAF chose. The YAML's /mfaf-data-analytics is a
//                             pseudo-operation ("clients shall NOT invoke") and is not served.
//   Nmfaf_ContextManagement   {apiRoot}/nmfaf-contextmanagement/v1   Transfer (POST)
//
// The Messaging Framework itself is Apache Kafka (ADR-0359: the spec's "Messaging Framework" is
// a durable pub/sub bus and ADR-0355 already built one). This NF is the 3GPP-shaped adaptor over
// it, in both directions of TS 23.288 figure 5A.3.2-1:
//
//   Data Source ──Nnf_EventExposure_Notify──▶ MFAF replica X ──▶ Kafka topic (key = mfafCorreId)
//   Kafka ──consumer group──▶ MFAF replica Y ──Nmfaf_3caDataManagement_Notify──▶ consumer
//
// Scalable by construction, not by claim: configurations, correlation index and fetch buffers
// live in Valkey (config_store.hpp), so any replica serves any request; inbound notifications
// are produced to Kafka and delivered by whichever replica the consumer group hands the
// partition to, so N replicas split delivery and a dead replica's partitions move to the
// survivors -- one delivery per notification, never one per replica.
//
// What the MFAF chooses, because TS 29.576 leaves it to the MFAF (4.2.2.2.2: "determine the MFAF
// notification information"; 4.3.2.2.2: "the URI {fetchUri} which was previously provided by the
// MFAF"): the MFAF Notification Target Address is
//   https://{advertised}:{port}/mfaf-inbound/v1/notifications/{mfafCorreId}
// and the fetch URI is
//   https://{advertised}:{port}/mfaf-inbound/v1/fetch
// Neither is a 3GPP API root (no YAML declares them) -- they are this implementation's, under a
// prefix no 3GPP service uses, and a Data Source treats them as the opaque callback they are.
//
// DISCLOSED, not hidden (features per TS 29.576 tables 5.1.8-1, 5.2.8-1, 5.3.8-1):
//   * Supported features advertised: none. MultiProcessingInstruction, DataAnaCollect and
//     MfafTransfer (3DA) and DataProcess (3CA) are not implemented; a configuration carrying
//     mfafTransferInfo is answered 400, PUT on such a configuration is not reachable, and
//     notifEndpoints / procInstruct / multiProcInstructs are accepted and logged, not applied.
//   * Formatting: consumer-triggered notification (consTrigNotif -> FetchInstruction, buffer,
//     Fetch) is implemented. Clubbing, notification windows, periodic/increasing-period and
//     cross-event reporting (ReportingOptions) are not -- each inbound notification is delivered
//     as it arrives. Processing (NotifSummaryReport) is not built.
//   * The inbound body has no 3GPP schema (see classify.hpp). Without a 3gpp-Sbi-Callback header
//     an SMF/NEF/AF/PCF body cannot be placed in its DataNotification bucket and is dropped with
//     a counter. This project's own notifiers do not yet send that header -- a follow-up.
//   * Transfer (ContextManagement) hands over configurations and buffers and removes them here;
//     the receiving MFAF's re-creation with new ids is its own 3DA POST, which this MFAF does not
//     initiate (MfafTransfer not supported).
//   * LI: TS 33.127 has no MFAF-specific POI clause; nothing built.
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
#include "sbi_core/problem_details.hpp"
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
#include <vector>

#include "TS26510_CommonData_grp.hpp"
#include "TS29576_Nmfaf_3caDataManagement.hpp"
#include "TS29576_Nmfaf_3daDataManagement.hpp"
#include "TS29576_Nmfaf_ContextManagement.hpp"
#include "classify.hpp"
#include "config_store.hpp"
#include "event_bus/consumer.hpp"
#include "event_bus/producer.hpp"
#include "nf_config/nf_config.hpp"

using json = nlohmann::json;

namespace {

constexpr const char* kNfType = "MFAF";
// Must match nfs/nrf/src/main.cpp's kNrfInstanceId exactly -- see docs/DECISIONS.md ADR-0018.
constexpr const char* kNrfInstanceId = "5ba9a927-1d31-4c8e-8a10-000000000001";
// servers[0].url of each YAML, verified by tests/conformance's api_root_conformance (ADR-0325).
constexpr const char* k3daRoot = "/nmfaf-3dadatamanagement/v1";
constexpr const char* kContextRoot = "/nmfaf-contextmanagement/v1";
// This implementation's own callback targets (see the file header) -- deliberately NOT named
// *Root: they are not 3GPP API roots and must not be mistaken for one.
constexpr const char* kInboundPrefix = "/mfaf-inbound/v1";
// TS 29.500 Annex B rule, <API>_<callback name>; the 3CA YAML names its callback "Notification".
constexpr const char* kNotifyCallbackType = "Nmfaf_3caDataManagement_Notification";

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

std::optional<std::string> header(const sbi_core::http2::Request& req, const char* name) {
    // Header names arrive lowercased from the HTTP/2 layer.
    std::string key(name);
    for (auto& ch : key) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    const auto it = req.headers.find(key);
    if (it == req.headers.end()) {
        return std::nullopt;
    }
    return it->second;
}

// TS 29.576 4.2.2.2.2 / 5.1.6.2.2: MfafConfiguration is a oneOf of messageConfigurations and
// mfafTransferInfo; MessageConfiguration requires notificationURI and correId. The generated
// struct cannot express the oneOf, so it is checked here. Returns the 400 to send, or nullopt.
std::optional<sbi_core::http2::Response>
validate_configuration(const sbi_gen::MfafConfiguration& cfg, bool is_update) {
    const bool has_msgs = cfg.messageConfigurations && !cfg.messageConfigurations->empty();
    const bool has_transfer = cfg.mfafTransferInfo.has_value();
    if (has_msgs == has_transfer) {
        return problem(400,
                       "Bad Request",
                       "MfafConfiguration shall contain exactly one of messageConfigurations "
                       "and mfafTransferInfo (TS 29.576 5.1.6.2.2)",
                       has_msgs ? "INVALID_MSG_FORMAT" : "MANDATORY_IE_MISSING");
    }
    if (has_transfer) {
        // The MfafTransfer feature (table 5.1.8-1, number 3) is not supported; 4.2.2.2.3 also
        // forbids PUT on a transfer configuration outright.
        return problem(400,
                       "Bad Request",
                       is_update
                           ? "mfafTransferInfo is not applicable to an update (TS 29.576 4.2.2.2.3)"
                           : "mfafTransferInfo requires the MfafTransfer feature, which this MFAF "
                             "does not support",
                       "INVALID_MSG_FORMAT");
    }
    for (const auto& mc : *cfg.messageConfigurations) {
        if (mc.notificationURI.empty() || mc.correId.empty()) {
            return problem(
                400,
                "Bad Request",
                "MessageConfiguration requires notificationURI and correId (TS 29.576 5.1.6.2.3)",
                "MANDATORY_IE_MISSING");
        }
    }
    return std::nullopt;
}

// What a Data Source's notification looks like once it is on the bus: the correlation the MFAF
// assigned, the callback type header if the source sent one, and the body verbatim.
struct BusEnvelope {
    std::string mfaf_corre_id;
    std::optional<std::string> callback_type;
    json body;
    std::string received_at;
};

void to_json(json& j, const BusEnvelope& e) {
    j = json{{"mfafCorreId", e.mfaf_corre_id}, {"body", e.body}, {"receivedAt", e.received_at}};
    if (e.callback_type) {
        j["callbackType"] = *e.callback_type;
    }
}

void from_json(const json& j, BusEnvelope& e) {
    j.at("mfafCorreId").get_to(e.mfaf_corre_id);
    e.body = j.at("body");
    j.at("receivedAt").get_to(e.received_at);
    if (j.contains("callbackType")) {
        e.callback_type = j.at("callbackType").get<std::string>();
    }
}

// ---- NRF lifecycle (same shape as every other NF, ADR-0006/0019) --------------------------------

void run_nrf_lifecycle(const std::string& instance_id,
                       const std::string& nrf_base,
                       const std::string& advertised_ipv4,
                       int heartbeat_seconds) {
    sbi_core::http2::TlsConfig client_tls{
        .cert_path = CERTS_DIR "/mfaf/cert.pem",
        .key_path = CERTS_DIR "/mfaf/key.pem",
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
        {"nfServices",
         json::array(
             {json{{"serviceInstanceId", "nmfaf-3dadatamanagement"},
                   {"serviceName", "nmfaf-3dadatamanagement"},
                   {"versions",
                    json::array({json{{"apiVersionInUri", "v1"}, {"apiFullVersion", "1.0.0"}}})},
                   {"scheme", "https"},
                   {"nfServiceStatus", "REGISTERED"}},
              json{{"serviceInstanceId", "nmfaf-3cadatamanagement"},
                   {"serviceName", "nmfaf-3cadatamanagement"},
                   {"versions",
                    json::array({json{{"apiVersionInUri", "v1"}, {"apiFullVersion", "1.0.0"}}})},
                   {"scheme", "https"},
                   {"nfServiceStatus", "REGISTERED"}},
              json{{"serviceInstanceId", "nmfaf-contextmanagement"},
                   {"serviceName", "nmfaf-contextmanagement"},
                   {"versions",
                    json::array({json{{"apiVersionInUri", "v1"}, {"apiFullVersion", "1.0.0"}}})},
                   {"scheme", "https"},
                   {"nfServiceStatus", "REGISTERED"}}})},
    };
    while (true) {
        auto token = oauth.get_bearer_token();
        if (!token) {
            spdlog::warn("mfaf: NRF token unavailable ({}), retrying", token.error());
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
            spdlog::info("mfaf: registered with NRF (HTTP {})", put_resp->status);
            break;
        }
        spdlog::warn("mfaf: NRF registration failed ({}), retrying",
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
    sbi_core::init_logging("mfaf");
    sbi_core::init_tracing("mfaf");
    const auto config = nf_config::load("mfaf", CONFIG_DIR);
    // Every port and tunable from config, env-overridable (user mandate). Nothing below is a
    // literal.
    const auto port = nf_config::require<unsigned short>(config, "port", "MFAF_PORT");
    const auto metrics_bind_address = nf_config::require<std::string>(
        config, "metrics_bind_address", "MFAF_METRICS_BIND_ADDRESS");
    const auto nrf_base =
        nf_config::require<std::string>(config, "nrf_base_url", "MFAF_NRF_BASE_URL");
    const auto advertised_ipv4 =
        nf_config::require<std::string>(config, "advertised_ipv4", "MFAF_ADVERTISED_IPV4");
    const auto redis_url = nf_config::require<std::string>(config, "redis_url", "MFAF_REDIS_URL");
    const auto heartbeat_seconds =
        nf_config::require<int>(config, "nrf_heartbeat_seconds", "MFAF_NRF_HEARTBEAT_SECONDS");
    const auto brokers =
        nf_config::require<std::string>(config, "event_bus_brokers", "MFAF_EVENT_BUS_BROKERS");
    const auto topic =
        nf_config::require<std::string>(config, "event_bus_topic", "MFAF_EVENT_BUS_TOPIC");
    const auto group_id =
        nf_config::require<std::string>(config, "event_bus_group_id", "MFAF_EVENT_BUS_GROUP_ID");
    const auto flush_timeout_ms = nf_config::require<int>(
        config, "event_bus_flush_timeout_ms", "MFAF_EVENT_BUS_FLUSH_TIMEOUT_MS");
    const auto poll_timeout_ms = nf_config::require<int>(
        config, "event_bus_poll_timeout_ms", "MFAF_EVENT_BUS_POLL_TIMEOUT_MS");
    const auto buffer_ttl_seconds = nf_config::require<int>(
        config, "fetch_buffer_ttl_seconds", "MFAF_FETCH_BUFFER_TTL_SECONDS");
    if (brokers.empty()) {
        // The bus IS the Messaging Framework the MFAF adapts (TS 23.288 5A.3.2); without it there
        // is nothing to adapt. No silent in-process fallback (ADR-0359: one path).
        spdlog::critical("mfaf: event_bus_brokers is empty -- the MFAF cannot run without its "
                         "Messaging Framework");
        return 1;
    }

    sbi_core::init_metrics(metrics_bind_address);
    const std::string instance_id = sbi_core::generate_uuid_v4();
    spdlog::info("mfaf: starting, nfInstanceId={}", instance_id);

    sbi_core::http2::TlsConfig server_tls{
        .cert_path = CERTS_DIR "/mfaf/cert.pem",
        .key_path = CERTS_DIR "/mfaf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::jwt::Verifier verifier(CERTS_DIR "/nrf-jwt/public.pem", kNrfInstanceId);
    sbi_core::http2::TlsConfig client_tls{
        .cert_path = CERTS_DIR "/mfaf/cert.pem",
        .key_path = CERTS_DIR "/mfaf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client client(std::move(client_tls)); // 3CA Notify delivery
    std::mutex client_mutex;

    mfaf::ConfigStore store(std::make_shared<sw::redis::Redis>(redis_url));
    event_bus::Producer producer({.brokers = brokers,
                                  .client_id = "mfaf-" + instance_id,
                                  .flush_timeout_ms = flush_timeout_ms});
    event_bus::Consumer consumer({.brokers = brokers,
                                  .group_id = group_id,
                                  .client_id = "mfaf-" + instance_id,
                                  .topics = {topic},
                                  .poll_timeout_ms = poll_timeout_ms});

    auto meter = sbi_core::get_meter("mfaf");
    auto inbound_counter = meter->CreateUInt64Counter(
        "mfaf_inbound_notifications_total",
        "Data Source notifications received at the MFAF Notification Target Address");
    auto unmatched_counter = meter->CreateUInt64Counter(
        "mfaf_inbound_unmatched_total",
        "Inbound notifications with no configuration for their mfafCorreId");
    auto unclassified_counter = meter->CreateUInt64Counter(
        "mfaf_inbound_unclassified_total",
        "Inbound notifications whose source NF could not be determined (see classify.hpp)");
    auto delivered_counter = meter->CreateUInt64Counter(
        "mfaf_notifications_delivered_total",
        "Nmfaf_3caDataManagement_Notify deliveries acknowledged by the consumer");
    auto delivery_failed_counter =
        meter->CreateUInt64Counter("mfaf_notifications_failed_total",
                                   "Nmfaf_3caDataManagement_Notify deliveries not acknowledged");
    auto fetch_counter = meter->CreateUInt64Counter(
        "mfaf_fetches_total", "Nmfaf_3caDataManagement_Fetch requests served");

    const std::string self_base = "https://" + advertised_ipv4 + ":" + std::to_string(port);
    const std::string fetch_uri = self_base + kInboundPrefix + "/fetch";

    boost::asio::io_context ioc;
    sbi_core::http2::Server server(ioc, "0.0.0.0", port, server_tls);
    if (const auto tps_limit = sbi_core::read_tps_limit(config); tps_limit.enabled()) {
        server.set_tps_limit(tps_limit.sustained_tps, tps_limit.burst);
    }

    // Fills in the MFAF notification information the DCCF left to us (TS 29.576 4.2.2.2.2):
    // one mfafCorreId per MessageConfiguration that has none, each with this MFAF's inbound URI.
    const auto assign_noti_info = [&](sbi_gen::MfafConfiguration& cfg) {
        for (auto& mc : *cfg.messageConfigurations) {
            if (!mc.mfafNotiInfo) {
                sbi_gen::MfafNotiInfo info;
                info.mfafCorreId = store.next_id("mfaf-corr-");
                info.mfafNotifUri =
                    self_base + kInboundPrefix + "/notifications/" + info.mfafCorreId;
                mc.mfafNotiInfo = std::move(info);
            }
        }
    };
    const auto log_unsupported = [](const sbi_gen::MfafConfiguration& cfg, const std::string& id) {
        for (const auto& mc : *cfg.messageConfigurations) {
            if (mc.notifEndpoints) {
                spdlog::warn("mfaf: {} carries notifEndpoints (DataAnaCollect feature) -- not "
                             "applied, only notificationURI is delivered to",
                             id);
            }
            if (mc.procInstruct || mc.multiProcInstructs) {
                spdlog::warn("mfaf: {} carries processing instructions (DataProcess feature) -- "
                             "not applied, notifications are delivered unprocessed",
                             id);
            }
            if (mc.formatInstruct && mc.formatInstruct->reportingOptions) {
                spdlog::warn("mfaf: {} carries reportingOptions (clubbing/windows) -- not applied, "
                             "each notification is delivered as it arrives",
                             id);
            }
            if (mc.adrfId) {
                spdlog::warn("mfaf: {} names an ADRF ({}) -- storage in the ADRF is not built "
                             "(ADR-0359 step 3)",
                             id,
                             *mc.adrfId);
            }
        }
    };

    // ---- Nmfaf_3daDataManagement: Configure / Deconfigure ---------------------------------------
    server.add_route(
        "POST",
        std::string(k3daRoot) + "/configurations",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto cfg = sbi_core::http2::parse_json_body<sbi_gen::MfafConfiguration>(req, err);
            if (!cfg) {
                return err;
            }
            if (auto bad = validate_configuration(*cfg, false)) {
                return *bad;
            }
            assign_noti_info(*cfg);
            const auto id = store.create(*cfg);
            log_unsupported(*cfg, id);
            spdlog::info("mfaf: configuration {} created ({} message configuration(s))",
                         id,
                         cfg->messageConfigurations->size());
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location", std::string(k3daRoot) + "/configurations/" + id);
            resp.body = json(*cfg).dump();
            return resp;
        });
    server.add_route(
        "PUT",
        std::string(k3daRoot) + "/configurations/{transRefId}",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("transRefId");
            sbi_core::http2::Response err;
            auto cfg = sbi_core::http2::parse_json_body<sbi_gen::MfafConfiguration>(req, err);
            if (!cfg) {
                return err;
            }
            if (auto bad = validate_configuration(*cfg, true)) {
                return *bad;
            }
            assign_noti_info(*cfg);
            if (!store.replace(id, *cfg)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "no Individual MFAF Configuration " + id);
            }
            log_unsupported(*cfg, id);
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = json(*cfg).dump();
            return resp;
        });
    server.add_route("DELETE",
                     std::string(k3daRoot) + "/configurations/{transRefId}",
                     [&](const sbi_core::http2::Request& req) {
                         if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                             return sbi_core::http2::problem_response(
                                 401, "Unauthorized", auth->error);
                         }
                         const auto id = req.path_params.at("transRefId");
                         if (!store.remove(id)) {
                             return sbi_core::http2::problem_response(
                                 404, "Not Found", "no Individual MFAF Configuration " + id);
                         }
                         spdlog::info("mfaf: configuration {} removed", id);
                         sbi_core::http2::Response resp;
                         resp.status = 204;
                         return resp;
                     });

    // ---- Nmfaf_ContextManagement: Transfer -----------------------------------------------------
    server.add_route(
        "POST", std::string(kContextRoot) + "/transfer", [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto treq = sbi_core::http2::parse_json_body<sbi_gen::ContextTransferReq>(req, err);
            if (!treq) {
                return err;
            }
            if (treq->refIds.empty()) {
                return problem(400,
                               "Bad Request",
                               "refIds shall not be empty (TS 29.576 5.3.6.2.2)",
                               "MANDATORY_IE_MISSING");
            }
            sbi_gen::ContextTransferResp tresp;
            tresp.configs = json::object();
            json buffered = json::object();
            for (const auto& ref : treq->refIds) {
                // refIds are resource URIs; the transRefId is the last path segment.
                const auto slash = ref.find_last_of('/');
                const std::string id = slash == std::string::npos ? ref : ref.substr(slash + 1);
                auto cfg = store.get(id);
                if (!cfg) {
                    continue; // 5.3.6.2.3: only the transferred ones appear in configs
                }
                tresp.configs[ref] = json(*cfg);
                for (const auto& [fetch_id, note] : store.buffers_for(id)) {
                    buffered[ref] = json(note); // one buffered notification per ref in this map
                }
                store.remove(id);
                spdlog::info("mfaf: configuration {} transferred away", id);
            }
            if (tresp.configs.empty()) {
                return sbi_core::http2::problem_response(
                    404,
                    "Not Found",
                    "none of the refIds is an Individual MFAF Configuration here");
            }
            if (!buffered.empty()) {
                tresp.bufferedNotifs = buffered;
            }
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = json(tresp).dump();
            return resp;
        });

    // ---- Inbound: the MFAF Notification Target Address (Data Source -> MFAF -> bus) -------------
    server.add_route(
        "POST",
        std::string(kInboundPrefix) + "/notifications/{mfafCorreId}",
        [&](const sbi_core::http2::Request& req) {
            const auto corr = req.path_params.at("mfafCorreId");
            inbound_counter->Add(1);
            json body;
            try {
                body = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            if (!store.find_by_correlation(corr)) {
                // The DCCF deconfigured this mapping (TS 23.288 6.2.6.3.4 step 14) but
                // the source still notifies: TS 29.500 RESOURCE_CONTEXT_NOT_FOUND is
                // the cause for exactly this -- callback URI known, context gone.
                unmatched_counter->Add(1);
                return problem(400,
                               "Bad Request",
                               "no MFAF configuration maps mfafCorreId " + corr,
                               "RESOURCE_CONTEXT_NOT_FOUND");
            }
            BusEnvelope env;
            env.mfaf_corre_id = corr;
            env.callback_type = header(req, sbi_core::headers::kCallback);
            env.body = std::move(body);
            env.received_at = sbi_core::format_rfc3339(std::chrono::system_clock::now());
            if (!producer.publish(topic, corr, json(env).dump())) {
                return sbi_core::http2::problem_response(
                    503,
                    "Service Unavailable",
                    "the Messaging Framework did not accept the notification");
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // ---- Nmfaf_3caDataManagement_Fetch (the fetchUri this MFAF hands out) -----------------------
    server.add_route(
        "POST", std::string(kInboundPrefix) + "/fetch", [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto ids = sbi_core::http2::parse_json_body<std::vector<std::string>>(req, err);
            if (!ids) {
                return err;
            }
            if (ids->empty()) {
                return problem(400,
                               "Bad Request",
                               "fetch correlation identifiers shall not be empty",
                               "MANDATORY_IE_MISSING");
            }
            fetch_counter->Add(1);
            // Merge every buffered notification the ids name into one
            // NmfafDataAnaNotification (4.3.2.2.2: "find the data or analytics").
            sbi_gen::NmfafDataAnaNotification merged;
            bool any = false;
            for (const auto& id : *ids) {
                auto n = store.buffer_take(id);
                if (!n) {
                    continue;
                }
                any = true;
                if (n->anaNotifications) {
                    if (!merged.anaNotifications) {
                        merged.anaNotifications =
                            std::vector<sbi_gen::NnwdafEventsSubscriptionNotification>{};
                    }
                    merged.anaNotifications->insert(merged.anaNotifications->end(),
                                                    n->anaNotifications->begin(),
                                                    n->anaNotifications->end());
                }
                if (n->dataNotif) {
                    if (!merged.dataNotif) {
                        merged.dataNotif = std::move(*n->dataNotif);
                    } else {
                        // Same-source buckets concatenate; the JSON round trip keeps
                        // this generic across the eleven bucket types.
                        json a = *merged.dataNotif;
                        const json b = *n->dataNotif;
                        for (auto it = b.begin(); it != b.end(); ++it) {
                            if (it->is_array() && a.contains(it.key()) && a[it.key()].is_array()) {
                                a[it.key()].insert(a[it.key()].end(), it->begin(), it->end());
                            } else {
                                a[it.key()] = *it;
                            }
                        }
                        merged.dataNotif = a.get<sbi_gen::DataNotification>();
                    }
                }
            }
            if (!any) {
                return sbi_core::http2::problem_response(
                    404,
                    "Not Found",
                    "no buffered data or analytics for the given fetch correlation identifiers "
                    "(expired or already fetched)");
            }
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = json(merged).dump();
            return resp;
        });

    // ---- Delivery: bus -> Nmfaf_3caDataManagement_Notify ----------------------------------------
    // One consumer-group member per replica. The handler runs on the consumer thread; the HTTP/2
    // client is shared with nothing else here, but the mutex keeps that true if a second sender
    // ever appears.
    std::thread delivery([&] {
        consumer.run([&](const event_bus::Record& record) {
            BusEnvelope env = json::parse(record.value).get<BusEnvelope>();
            auto found = store.find_by_correlation(env.mfaf_corre_id);
            if (!found) {
                unmatched_counter->Add(1);
                spdlog::info("mfaf: {} was deconfigured before delivery; dropped",
                             env.mfaf_corre_id);
                return;
            }
            auto classified = mfaf::classify(
                env.callback_type ? std::optional<std::string_view>(*env.callback_type)
                                  : std::nullopt,
                env.body);
            if (!classified) {
                unclassified_counter->Add(1);
                spdlog::warn("mfaf: inbound for {} could not be placed in a DataNotification "
                             "bucket (3gpp-Sbi-Callback: {}); dropped",
                             env.mfaf_corre_id,
                             env.callback_type.value_or("absent"));
                return;
            }
            const auto& [trans_ref_id, cfg] = *found;
            for (const auto& mc : *cfg.messageConfigurations) {
                if (!mc.mfafNotiInfo || mc.mfafNotiInfo->mfafCorreId != env.mfaf_corre_id) {
                    continue;
                }
                sbi_gen::NmfafDataRetrievalNotification note;
                note.correId = mc.correId;
                const bool fetch =
                    mc.formatInstruct && mc.formatInstruct->consTrigNotif.value_or(false);
                if (fetch) {
                    const auto fetch_id =
                        store.buffer_put(trans_ref_id,
                                         classified->notification,
                                         std::chrono::seconds(buffer_ttl_seconds));
                    sbi_gen::FetchInstruction fi;
                    fi.fetchUri = fetch_uri;
                    fi.fetchCorrIds = {fetch_id};
                    fi.expiry = sbi_core::format_rfc3339(std::chrono::system_clock::now() +
                                                         std::chrono::seconds(buffer_ttl_seconds));
                    note.fetchInstruction = std::move(fi);
                } else {
                    note.dataAnaNotif = classified->notification;
                }
                sbi_core::http2::ClientRequest req;
                req.method = "POST";
                req.url = mc.notificationURI;
                req.headers.emplace("content-type", "application/json");
                req.headers.emplace(sbi_core::headers::kCallback, kNotifyCallbackType);
                req.body = json(note).dump();
                std::optional<int> status;
                {
                    const std::lock_guard<std::mutex> lock(client_mutex);
                    if (auto resp = client.send(req); resp) {
                        status = resp->status;
                    }
                }
                if (status && *status >= 200 && *status < 300) {
                    delivered_counter->Add(1);
                } else {
                    delivery_failed_counter->Add(1);
                    spdlog::warn(
                        "mfaf: Nmfaf_3caDataManagement_Notify to {} for correId {} failed ({})",
                        mc.notificationURI,
                        mc.correId,
                        status.value_or(-1));
                }
            }
        });
    });
    sbi_core::on_shutdown_signal([&] { consumer.stop(); });

    std::thread(run_nrf_lifecycle, instance_id, nrf_base, advertised_ipv4, heartbeat_seconds)
        .detach();
    server.start();
    spdlog::info("mfaf: listening on https://0.0.0.0:{} (TLS 1.3 + mTLS); inbound at "
                 "{}{}/notifications/{{mfafCorreId}}",
                 port,
                 self_base,
                 kInboundPrefix);
    spdlog::info("mfaf: Prometheus metrics at http://{}/metrics", metrics_bind_address);
    sbi_core::run_multi_threaded(ioc);
    delivery.join(); // consumer.stop() was called by the shutdown watcher; run() returns
    return 0;
}
