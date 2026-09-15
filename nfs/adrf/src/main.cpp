// ADRF -- Analytics Data Repository Function (ADR-0367; architecture ADR-0359 step 3).
//
// Stage 2: TS 23.288 V19.7.0 clause 5B (ADRF), procedures 6.2B (storage, retrieval, deletion
// of data and analytics, ML model storage). Stage 3: TS 29.575 V19.7.0 and its two R19 YAML
// files (forge.3gpp.org, REL-19, commit bca84b6), every DTO generated, every API root the YAML's
// servers[0].url (ADR-0325):
//
//   Nadrf_DataManagement     {apiRoot}/nadrf-datamanagement/v1
//     StorageRequest (POST /data-store-records), RetrievalRequest (GET /data-store-records),
//     Delete (DELETE /data-store-records/{storeTransId}, POST /remove-stored-data-analytics),
//     StorageSubscriptionRequest (POST /request-storage-sub), StorageSubscriptionRemoval
//     (POST /request-storage-sub-removal), RetrievalSubscribe (POST /data-retrieval-
//     subscriptions), RetrievalUnsubscribe (DELETE .../{subscriptionId}), RetrievalNotify
//     (POST {notificationURI} / {delNotifUri}, this NF as the client)
//   Nadrf_MLModelManagement  {apiRoot}/nadrf-mlmodelmanagement/v1
//     StorageRequest (POST /mlmodel-store-records, PUT .../{storeTransId}), RetrievalRequest
//     (GET /mlmodel-store-records), Delete (DELETE .../{storeTransId}, POST
//     /remove-stored-mlmodel)
//
// Where things are kept (ADR-0359's table, no in-process state): the stored records in Apache
// Doris (record_store.hpp -- the analytics repository, so the MTLF can train on what the ADRF
// holds), ML models in PostgreSQL (model_store.hpp -- bytes included, so every replica serves
// every model), subscriptions / collections / fetch buffers / leases in Valkey (state_store.hpp).
//
// How a storage subscription is served (TS 23.288 6.2B.2 / TS 29.575 4.2.2.3.2): the ADRF
// subscribes ONCE per distinct spec at the DCCF (Ndccf_DataManagement, TS 29.574 -- data or
// analytics) or directly at the NWDAF (Nnwdaf_EventsSubscription -- analytics), with its own
// inbound URI  {self}/adrf-inbound/v1/notifications/{fingerprint}  as the notification target;
// every further transRefId asking for the same spec is mapped onto that collection (4.2.2.3.2
// NOTE 2), and the target is unsubscribed when the last one is removed. What arrives -- the
// MFAF's NmfafDataRetrievalNotification for DCCF collections, the NWDAF's
// NnwdafEventsSubscriptionNotification for direct ones -- is stored as one NadrfDataStoreRecord
// each, carrying the spec it came from as its anaSub / dataSub.
//
// "The same data": the record's spec fingerprint (record_store.hpp), the DCCF's construction
// (ADR-0366). A RetrievalSubscribe / remove-stored-data-analytics naming a spec finds the records
// a storage subscription with that spec produced; a record stored directly via StorageRequest is
// fingerprinted the same way from the anaSub / dataSub it carries.
//
// Features advertised (suppFeat, TS 29.500 6.6): EnhDataMgmt (Nadrf_DataManagement table 5.1.8-1
// no. 3 -- storeHandl, dataSetTag / dataSetId, deletion alerts) and EnModelMgmt
// (Nadrf_MLModelManagement table 5.2.8-1 no. 1 -- allowConsumerList, per-model results).
//
// DISCLOSED, not hidden:
//   * targetNfId is resolved to its nfType at the NRF (DCCF or NWDAF); the target's ADDRESS is
//     the configured dccf_base_url / nwdaf_base_url, as every peer in this project is located
//     (our NF profiles carry no ipEndPoints). targetNfSetId selects the DCCF. A dataSub with an
//     NWDAF target goes to Nnwdaf_DataManagement (ADR-0368); the NWDAF's own
//     SUBSCRIPTION_CANNOT_BE_SERVED is passed through when it cannot serve it.
//   * 4.2.2.3.2 says the storage approach the ADRF decides is returned in the response's
//     storeHandl; the YAML's NadrfDataStoreSubscriptionRef has no such attribute (YAML wins on
//     shape, ADR-0250), so it is applied and logged. The 201 of StorageRequest does carry it.
//   * A pure termination (terminationReq with no content) cannot be expressed --
//     NadrfDataRetrievalNotification's oneOf demands anaNotifications, dataNotif or
//     fetchInstruct -- so a retrieval subscription whose timePeriod has ended is simply removed.
//   * The single-record GET response cannot carry analytics AND data: a data set that mixes the
//     two is answered with the kind of its oldest record, the rest logged.
//   * MLModel.mlModel is OpenAPI `string, format: binary` inside application/json; this ADRF
//     decodes it as base64 (RFC 4648) -- the only way octets travel in a JSON string -- and
//     serves stored models as application/octet-stream at {self}/adrf-mlmodel-files/v1/{id},
//     the address returned in mlFileAddr.mLModelUrl. mlFileFqdn download is not built (a
//     model named by FQDN alone is answered ML_MODEL_FILE_ADDRESS_NOT_FOUND).
//   * The allowed-consumer check compares the token subject with nfInstanceId entries and the
//     owner; nfSetId entries cannot be checked (a token carries no NF set) and are logged.
//   * Not built: UpEvents / LocEvents / LmfEvents / PcfEvents (no producers in this project),
//     dsc, 307/308 redirects, storage subscriptions carrying multiProcInstructs beyond
//     pass-through to the DCCF, MLflow (that is the MTLF's training-side tracker, ADR-0359).
//   * LI: TS 33.127 7.18 (NWDAF) has no ADRF clause; nothing built.
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
#include <openssl/evp.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "TS26510_CommonData_grp.hpp"
#include "TS29575_Nadrf_MLModelManagement.hpp"
#include "model_store.hpp"
#include "nf_config/nf_config.hpp"
#include "record_store.hpp"
#include "state_store.hpp"

using json = nlohmann::json;

namespace {

constexpr const char* kNfType = "ADRF";
// Must match nfs/nrf/src/main.cpp's kNrfInstanceId exactly -- see docs/DECISIONS.md ADR-0018.
constexpr const char* kNrfInstanceId = "5ba9a927-1d31-4c8e-8a10-000000000001";
// servers[0].url of each YAML, verified by tests/conformance's api_root_conformance (ADR-0325).
constexpr const char* kDataManagementRoot = "/nadrf-datamanagement/v1";
constexpr const char* kMLModelManagementRoot = "/nadrf-mlmodelmanagement/v1";
// The ADRF's own, non-3GPP URIs: where its collections deliver, and where stored models are
// served from (the mLModelUrl it hands out). Same convention as the MFAF's /mfaf-inbound/v1.
constexpr const char* kInboundPrefix = "/adrf-inbound/v1";
constexpr const char* kModelFilesPrefix = "/adrf-mlmodel-files/v1";
// The peers' roots -- each the servers[0].url of ITS YAML, used as a client here.
constexpr const char* kDccfDataManagementRoot = "/ndccf-datamanagement/v1";
constexpr const char* kNwdafEventsSubscriptionRoot = "/nnwdaf-eventssubscription/v1";
constexpr const char* kNwdafDataManagementRoot = "/nnwdaf-datamanagement/v1";
constexpr const char* kNrfNfmRoot = "/nnrf-nfm/v1";
// 3gpp-Sbi-Callback values (TS 29.500 5.2.3.2.3, Annex B rule: <API>_<callback name> for names
// not in table B-1) -- the callback keys of TS29575_Nadrf_DataManagement.yaml.
constexpr const char* kRetrievalCallback = "Nadrf_DataManagement_adrfDataRetrievalNotification";
constexpr const char* kStorageAlertCallback = "Nadrf_DataManagement_storageAlertNotification";
constexpr const char* kStorageSubAlertCallback = "Nadrf_DataManagement_storageSubAlertNotification";
// Feature bits (TS 29.500 6.6.2: feature number n is bit n-1 of the hex string).
constexpr unsigned kEnhDataMgmt = 1u << 2; // Nadrf_DataManagement no. 3
constexpr unsigned kEnModelMgmt = 1u << 0; // Nadrf_MLModelManagement no. 1

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

sbi_core::http2::Response json_response(int status, const json& body) {
    sbi_core::http2::Response r;
    r.status = status;
    r.headers.emplace("content-type", "application/json");
    r.body = body.dump();
    return r;
}

sbi_core::http2::Response no_content() {
    sbi_core::http2::Response r;
    r.status = 204;
    return r;
}

// suppFeat negotiation: the consumer's requested features ANDed with ours, as a hex string.
std::string negotiate(const json& body, unsigned ours) {
    unsigned requested = ours;
    if (body.contains("suppFeat") && body.at("suppFeat").is_string()) {
        try {
            requested = static_cast<unsigned>(
                std::stoul(body.at("suppFeat").get<std::string>(), nullptr, 16));
        } catch (const std::exception&) {
            requested = ours;
        }
    }
    char buf[16];
    std::snprintf(buf, sizeof buf, "%x", requested & ours);
    return buf;
}

std::optional<std::string> base64_decode(const std::string& text) {
    if (text.empty()) {
        return std::string{};
    }
    std::string out(3 * ((text.size() + 3) / 4) + 1, '\0');
    const int len = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                    reinterpret_cast<const unsigned char*>(text.data()),
                                    static_cast<int>(text.size()));
    if (len < 0) {
        return std::nullopt;
    }
    std::size_t actual = static_cast<std::size_t>(len);
    if (text.size() > 0 && text[text.size() - 1] == '=') {
        --actual;
    }
    if (text.size() > 1 && text[text.size() - 2] == '=') {
        --actual;
    }
    out.resize(actual);
    return out;
}

std::optional<adrf::TimeWindow> parse_window(const json& time_period) {
    if (!time_period.is_object() || !time_period.contains("startTime") ||
        !time_period.contains("stopTime")) {
        return std::nullopt;
    }
    const auto start = sbi_core::parse_rfc3339(time_period.at("startTime").get<std::string>());
    const auto stop = sbi_core::parse_rfc3339(time_period.at("stopTime").get<std::string>());
    if (!start || !stop) {
        return std::nullopt;
    }
    return adrf::TimeWindow{*start, *stop};
}

// Which kind a NadrfDataStoreRecord is, per its oneOf (anaSub+anaNotifications |
// dataSub+dataNotif) -- both members of a pair, exactly one pair.
std::optional<std::string> record_kind(const json& r, std::string& why) {
    const bool ana = r.contains("anaSub") || r.contains("anaNotifications");
    const bool data = r.contains("dataSub") || r.contains("dataNotif");
    if (ana && data) {
        why = "a record is analytics (anaSub + anaNotifications) or data (dataSub + dataNotif), "
              "not both (TS 29.575 NadrfDataStoreRecord oneOf)";
        return std::nullopt;
    }
    if (ana) {
        if (!r.contains("anaSub") || !r.contains("anaNotifications")) {
            why = "anaSub and anaNotifications are required together";
            return std::nullopt;
        }
        return "analytics";
    }
    if (data) {
        if (!r.contains("dataSub") || !r.contains("dataNotif")) {
            why = "dataSub and dataNotif are required together";
            return std::nullopt;
        }
        return "data";
    }
    why = "a record carries anaSub + anaNotifications or dataSub + dataNotif";
    return std::nullopt;
}

// The spec fingerprint of a record: its single anaSub / dataSub element when there is exactly
// one (what storage subscriptions produce), else the whole array.
std::string record_fingerprint(const std::string& kind, const json& r) {
    const json& subs = r.at(kind == "analytics" ? "anaSub" : "dataSub");
    if (subs.is_array() && subs.size() == 1) {
        return adrf::fingerprint(kind, subs.front());
    }
    return adrf::fingerprint(kind, subs);
}

// Merge several stored records of one kind into the single NadrfDataStoreRecord a GET returns
// (4.2.2.5.2): analytics -- anaNotifications and anaSub concatenated; data -- every
// DataNotification bucket concatenated, dataSub concatenated.
json merge_records(const std::vector<adrf::StoredRecord>& records, std::string& dropped_kind) {
    if (records.empty()) {
        return nullptr;
    }
    const std::string kind = records.front().kind;
    json out;
    if (kind == "analytics") {
        out["anaSub"] = json::array();
        out["anaNotifications"] = json::array();
    } else {
        out["dataSub"] = json::array();
        out["dataNotif"] = json::object();
    }
    for (const auto& r : records) {
        if (r.kind != kind) {
            dropped_kind = r.kind;
            continue;
        }
        if (kind == "analytics") {
            for (const auto& s : r.record.at("anaSub")) {
                out["anaSub"].push_back(s);
            }
            for (const auto& n : r.record.at("anaNotifications")) {
                out["anaNotifications"].push_back(n);
            }
        } else {
            for (const auto& s : r.record.at("dataSub")) {
                out["dataSub"].push_back(s);
            }
            for (const auto& [bucket, v] : r.record.at("dataNotif").items()) {
                if (v.is_array()) {
                    for (const auto& n : v) {
                        out["dataNotif"][bucket].push_back(n);
                    }
                } else if (!out["dataNotif"].contains(bucket)) {
                    out["dataNotif"][bucket] = v; // timeStamp: the oldest record's
                }
            }
        }
        if (r.record.contains("dataSetTag") && !out.contains("dataSetTag")) {
            out["dataSetTag"] = r.record.at("dataSetTag");
        }
    }
    return out;
}

// ---- NRF lifecycle (same shape as every other NF, ADR-0006/0019) --------------------------------

void run_nrf_lifecycle(const std::string& instance_id,
                       const std::string& nrf_base,
                       const std::string& advertised_ipv4,
                       int heartbeat_seconds) {
    sbi_core::http2::TlsConfig client_tls{
        .cert_path = CERTS_DIR "/adrf/cert.pem",
        .key_path = CERTS_DIR "/adrf/key.pem",
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
         json::array({service("nadrf-datamanagement"), service("nadrf-mlmodelmanagement")})},
    };
    while (true) {
        auto token = oauth.get_bearer_token();
        if (!token) {
            spdlog::warn("adrf: NRF token unavailable ({}), retrying", token.error());
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
            spdlog::info("adrf: registered with NRF (HTTP {})", put_resp->status);
            break;
        }
        spdlog::warn("adrf: NRF registration failed ({}), retrying",
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
        static_cast<void>(http_client.send(patch_req));
    }
}

} // namespace

int main() {
    sbi_core::init_logging("adrf");
    sbi_core::init_tracing("adrf");
    const auto config = nf_config::load("adrf", CONFIG_DIR);
    // Every port and tunable from config, env-overridable (user mandate). Nothing below is a
    // literal.
    const auto port = nf_config::require<unsigned short>(config, "port", "ADRF_PORT");
    const auto metrics_bind_address = nf_config::require<std::string>(
        config, "metrics_bind_address", "ADRF_METRICS_BIND_ADDRESS");
    const auto nrf_base =
        nf_config::require<std::string>(config, "nrf_base_url", "ADRF_NRF_BASE_URL");
    const auto advertised_ipv4 =
        nf_config::require<std::string>(config, "advertised_ipv4", "ADRF_ADVERTISED_IPV4");
    const auto self_base =
        nf_config::require<std::string>(config, "self_base_url", "ADRF_SELF_BASE_URL");
    const auto redis_url = nf_config::require<std::string>(config, "redis_url", "ADRF_REDIS_URL");
    const auto heartbeat_seconds =
        nf_config::require<int>(config, "nrf_heartbeat_seconds", "ADRF_NRF_HEARTBEAT_SECONDS");
    const auto dccf_base =
        nf_config::require<std::string>(config, "dccf_base_url", "ADRF_DCCF_BASE_URL");
    const auto nwdaf_base =
        nf_config::require<std::string>(config, "nwdaf_base_url", "ADRF_NWDAF_BASE_URL");
    const auto data_store_cfg = config.at("data_store");
    adrf::RecordStoreOptions ds;
    ds.host = nf_config::require<std::string>(data_store_cfg, "host", "ADRF_DATA_STORE_HOST");
    ds.port = nf_config::require<std::uint16_t>(data_store_cfg, "port", "ADRF_DATA_STORE_PORT");
    ds.user = nf_config::require<std::string>(data_store_cfg, "user", "ADRF_DATA_STORE_USER");
    ds.password =
        nf_config::require<std::string>(data_store_cfg, "password", "ADRF_DATA_STORE_PASSWORD");
    ds.database =
        nf_config::require<std::string>(data_store_cfg, "database", "ADRF_DATA_STORE_DATABASE");
    ds.max_rows =
        nf_config::require<std::int64_t>(data_store_cfg, "max_rows", "ADRF_DATA_STORE_MAX_ROWS");
    const auto model_store_url = nf_config::require<std::string>(
        config, "ml_model_store_database_url", "ADRF_ML_MODEL_STORE_DATABASE_URL");
    const auto policy = config.at("storage_policy");
    const auto default_lifetime = nf_config::require<std::int64_t>(
        policy, "default_lifetime_seconds", "ADRF_DEFAULT_LIFETIME_SECONDS");
    const auto max_lifetime = nf_config::require<std::int64_t>(
        policy, "max_lifetime_seconds", "ADRF_MAX_LIFETIME_SECONDS");
    const auto alert_lead =
        nf_config::require<std::int64_t>(policy, "alert_lead_seconds", "ADRF_ALERT_LEAD_SECONDS");
    const auto alert_grace =
        nf_config::require<std::int64_t>(policy, "alert_grace_seconds", "ADRF_ALERT_GRACE_SECONDS");
    const auto reaper_interval = nf_config::require<std::int64_t>(
        policy, "reaper_interval_seconds", "ADRF_REAPER_INTERVAL_SECONDS");
    const auto retrieval_cfg = config.at("retrieval");
    const auto fetch_ttl = nf_config::require<std::int64_t>(
        retrieval_cfg, "fetch_buffer_ttl_seconds", "ADRF_FETCH_BUFFER_TTL_SECONDS");
    const auto sweep_interval = nf_config::require<std::int64_t>(
        retrieval_cfg, "sweep_interval_seconds", "ADRF_RETRIEVAL_SWEEP_INTERVAL_SECONDS");
    const auto model_access_policy = nf_config::require<std::string>(
        config, "ml_model_access_policy", "ADRF_ML_MODEL_ACCESS_POLICY");
    if (model_access_policy != "enforced" && model_access_policy != "not-enforced") {
        spdlog::critical("adrf: ml_model_access_policy must be \"enforced\" or \"not-enforced\", "
                         "got \"{}\"",
                         model_access_policy);
        return 1;
    }
    const auto model_max_bytes = nf_config::require<std::int64_t>(
        config, "ml_model_download_max_bytes", "ADRF_ML_MODEL_DOWNLOAD_MAX_BYTES");

    sbi_core::init_metrics(metrics_bind_address);
    const std::string instance_id = sbi_core::generate_uuid_v4();
    spdlog::info("adrf: starting, nfInstanceId={}", instance_id);

    sbi_core::http2::TlsConfig server_tls{
        .cert_path = CERTS_DIR "/adrf/cert.pem",
        .key_path = CERTS_DIR "/adrf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::jwt::Verifier verifier(CERTS_DIR "/nrf-jwt/public.pem", kNrfInstanceId);
    sbi_core::http2::TlsConfig client_tls{
        .cert_path = CERTS_DIR "/adrf/cert.pem",
        .key_path = CERTS_DIR "/adrf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client client(std::move(client_tls));
    std::mutex client_mutex; // one client, used from every request handler and the reapers
    sbi_core::OAuth2Client oauth_dccf(
        client, nrf_base + "/oauth2/token", instance_id, "ndccf-datamanagement", "DCCF");
    sbi_core::OAuth2Client oauth_nwdaf(
        client, nrf_base + "/oauth2/token", instance_id, "nnwdaf-eventssubscription", "NWDAF");
    sbi_core::OAuth2Client oauth_nwdaf_dm(
        client, nrf_base + "/oauth2/token", instance_id, "nnwdaf-datamanagement", "NWDAF");
    sbi_core::OAuth2Client oauth_nrf(
        client, nrf_base + "/oauth2/token", instance_id, "nnrf-nfm", "NRF");

    adrf::StateStore state(std::make_shared<sw::redis::Redis>(redis_url));
    adrf::RecordStore records(ds);
    adrf::ModelStore models(model_store_url);

    auto meter = sbi_core::get_meter("adrf");
    auto stored_counter =
        meter->CreateUInt64Counter("adrf_records_stored_total", "Data store records stored");
    auto deleted_counter =
        meter->CreateUInt64Counter("adrf_records_deleted_total", "Data store records deleted");
    auto retrieval_notif_counter = meter->CreateUInt64Counter(
        "adrf_retrieval_notifications_total", "Nadrf_DataManagement_RetrievalNotify sent");
    auto alert_counter = meter->CreateUInt64Counter("adrf_deletion_alerts_total",
                                                    "Deletion alerts sent to delNotifUri");
    auto storage_subs_counter =
        meter->CreateUInt64Counter("adrf_storage_subscriptions_total",
                                   "Nadrf_DataManagement_StorageSubscriptionRequest served");
    auto models_stored_counter =
        meter->CreateUInt64Counter("adrf_ml_models_stored_total", "ML models stored");
    auto models_served_counter =
        meter->CreateUInt64Counter("adrf_ml_models_served_total", "ML model files served");

    // ---- peer calls: one SBI request with the right token ---------------------------------------
    struct PeerResult {
        int status = -1;
        std::string body;
        std::string location;
        std::string error;
    };
    const auto call = [&](sbi_core::OAuth2Client* oauth,
                          const std::string& method,
                          const std::string& url,
                          const std::optional<json>& body,
                          const char* callback_type = nullptr) {
        PeerResult out;
        sbi_core::http2::ClientRequest req;
        req.method = method;
        req.url = url;
        if (body) {
            req.headers.emplace("content-type", "application/json");
            req.body = body->dump();
        }
        if (callback_type != nullptr) {
            req.headers.emplace(sbi_core::headers::kCallback, callback_type);
        }
        const std::lock_guard<std::mutex> lock(client_mutex);
        if (oauth != nullptr) {
            if (auto token = oauth->get_bearer_token()) {
                req.headers.emplace("authorization", "Bearer " + *token);
            } else {
                out.error = "no access token: " + token.error();
                return out;
            }
        }
        auto resp = client.send(req);
        if (!resp) {
            out.error = resp.error();
            return out;
        }
        out.status = static_cast<int>(resp->status);
        out.body = resp->body;
        if (const auto it = resp->headers.find("location"); it != resp->headers.end()) {
            out.location = it->second;
        }
        return out;
    };

    // ---- storage handling policy (4.2.2.2.2 / 4.2.2.3.2: operator policy bounds the lifetime) --
    struct AppliedHandling {
        std::chrono::seconds lifetime;
        std::optional<std::string> del_notif_uri;
        std::optional<std::string> del_notif_corr_id;
        json as_json; // the storeHandl to echo back
    };
    const auto apply_handling = [&](const std::optional<json>& requested) {
        AppliedHandling h{std::chrono::seconds(default_lifetime), std::nullopt, std::nullopt, {}};
        if (requested && requested->is_object()) {
            if (requested->contains("lifetime")) {
                const auto wanted = requested->at("lifetime").get<std::int64_t>();
                h.lifetime = std::chrono::seconds(std::min<std::int64_t>(wanted, max_lifetime));
                if (wanted > max_lifetime) {
                    spdlog::info("adrf: requested lifetime {}s exceeds policy maximum {}s -- "
                                 "applying the maximum",
                                 wanted,
                                 max_lifetime);
                }
            }
            if (requested->contains("delNotifUri")) {
                h.del_notif_uri = requested->at("delNotifUri").get<std::string>();
                if (requested->contains("delNotifCorrId")) {
                    h.del_notif_corr_id = requested->at("delNotifCorrId").get<std::string>();
                }
            }
        }
        h.as_json = json{{"lifetime", h.lifetime.count()}};
        if (h.del_notif_uri) {
            h.as_json["delNotifUri"] = *h.del_notif_uri;
        }
        if (h.del_notif_corr_id) {
            h.as_json["delNotifCorrId"] = *h.del_notif_corr_id;
        }
        return h;
    };

    // ---- RetrievalNotify: one NadrfDataRetrievalNotification per (subscription, kind) ----------
    const auto notify_retrieval = [&](const std::string& sub_id,
                                      const adrf::RetrievalSubscription& sub,
                                      const std::vector<adrf::StoredRecord>& matched) {
        for (const char* kind : {"analytics", "data"}) {
            std::vector<adrf::StoredRecord> of_kind;
            std::copy_if(matched.begin(),
                         matched.end(),
                         std::back_inserter(of_kind),
                         [&](const adrf::StoredRecord& r) { return r.kind == kind; });
            if (of_kind.empty()) {
                continue;
            }
            json note{{"notifCorrId", sub.request.at("notifCorrId")},
                      {"timeStamp", sbi_core::format_rfc3339(std::chrono::system_clock::now())}};
            if (sub.request.value("consTrigNotif", false)) {
                // Buffer: hand out fetch correlation ids the consumer redeems with
                // RetrievalRequest (4.2.2.8.2 NOTE), each one a stored record.
                json ids = json::array();
                for (const auto& r : of_kind) {
                    const auto fetch_id = state.next_id("adrf-fetch-");
                    state.put_fetch(fetch_id, r.store_trans_id, std::chrono::seconds(fetch_ttl));
                    ids.push_back(fetch_id);
                }
                note["fetchInstruct"] =
                    json{{"fetchUri", self_base + kDataManagementRoot + "/data-store-records"},
                         {"fetchCorrIds", ids},
                         {"expiry",
                          sbi_core::format_rfc3339(std::chrono::system_clock::now() +
                                                   std::chrono::seconds(fetch_ttl))}};
            } else {
                std::string dropped;
                const json merged = merge_records(of_kind, dropped);
                if (std::string(kind) == "analytics") {
                    note["anaNotifications"] = merged.at("anaNotifications");
                } else {
                    note["dataNotif"] = merged.at("dataNotif");
                }
            }
            const auto r = call(nullptr,
                                "POST",
                                sub.request.at("notificationURI").get<std::string>(),
                                std::optional<json>(note),
                                kRetrievalCallback);
            if (r.status >= 200 && r.status < 300) {
                retrieval_notif_counter->Add(1);
            } else {
                spdlog::warn("adrf: RetrievalNotify for {} to {} failed ({} {})",
                             sub_id,
                             sub.request.at("notificationURI").get<std::string>(),
                             r.status,
                             r.error);
            }
        }
    };

    // ---- storing a record: Doris row + every live retrieval subscription it matches -----------
    const auto store_record = [&](const std::string& kind,
                                  json record,
                                  const AppliedHandling& handling,
                                  const std::optional<json>& data_set_tag,
                                  const char* origin) {
        adrf::StoredRecord row;
        row.store_trans_id = state.next_id("adrf-rec-");
        row.kind = kind;
        row.origin = origin;
        row.spec_fp = record_fingerprint(kind, record);
        if (data_set_tag) {
            record["dataSetTag"] = *data_set_tag;
        }
        if (record.contains("dataSetTag")) {
            row.data_set_id = record.at("dataSetTag").at("dataSetId").get<std::string>();
        }
        const auto now = std::chrono::system_clock::now();
        row.collected_at = now;
        if (kind == "data" && record.at("dataNotif").contains("timeStamp")) {
            if (const auto t = sbi_core::parse_rfc3339(
                    record.at("dataNotif").at("timeStamp").get<std::string>())) {
                row.collected_at = *t;
            }
        }
        row.stored_at = now;
        row.expires_at = now + handling.lifetime;
        row.del_notif_uri = handling.del_notif_uri;
        row.del_notif_corr_id = handling.del_notif_corr_id;
        record["storeHandl"] = handling.as_json;
        record["suppFeat"] = negotiate(record, kEnhDataMgmt);
        row.record = record;
        records.insert(row);
        stored_counter->Add(1);
        spdlog::info("adrf: stored {} record {} (spec {}, data set {}, expires {})",
                     kind,
                     row.store_trans_id,
                     row.spec_fp,
                     row.data_set_id.value_or("-"),
                     sbi_core::format_rfc3339(row.expires_at));
        // Live retrieval subscriptions (4.2.2.6: "future notifications ... when they are received
        // by the ADRF"): matched by spec fingerprint or data set, inside their timePeriod.
        for (const auto& id : state.retrieval_subscription_ids()) {
            const auto sub = state.get_retrieval_subscription(id);
            if (!sub) {
                continue;
            }
            const auto window = parse_window(sub->request.at("timePeriod"));
            if (!window || row.collected_at < window->start || row.collected_at > window->stop) {
                continue;
            }
            const bool matches =
                (sub->fingerprint && *sub->fingerprint == row.spec_fp) ||
                (sub->data_set_id && row.data_set_id && *sub->data_set_id == *row.data_set_id);
            if (matches) {
                notify_retrieval(id, *sub, {row});
            }
        }
        return row;
    };

    // ---- the collection behind a storage subscription: subscribe at the DCCF / NWDAF ------------
    // Returns the collection, or an error response.
    const auto open_collection =
        [&](const std::string& kind,
            const json& spec,
            const std::string& target,
            const json& request,
            const std::string& fp,
            std::optional<sbi_core::http2::Response>& err) -> std::optional<adrf::Collection> {
        adrf::Collection col;
        col.target = target;
        col.kind = kind;
        col.spec = spec;
        col.notif_corr_id = fp;
        const std::string notif_uri = self_base + kInboundPrefix + "/notifications/" + fp;
        PeerResult r;
        if (target == "DCCF") {
            json body;
            if (kind == "analytics") {
                body = {{"anaSub", spec}, {"anaNotifUri", notif_uri}, {"anaNotifCorrId", fp}};
            } else {
                body = {{"dataSub", spec}, {"dataNotifUri", notif_uri}, {"dataNotifCorrId", fp}};
            }
            if (request.contains("formatInstruct")) {
                body["formatInstruct"] = request.at("formatInstruct");
            }
            if (request.contains("multiProcInstructs")) {
                body["procInstructs"] = request.at("multiProcInstructs");
            } else if (request.contains("procInstruct")) {
                body["procInstructs"] = json::array({request.at("procInstruct")});
            }
            if (request.contains("targetNfId")) {
                body["targetNfId"] = request.at("targetNfId");
            }
            if (request.contains("targetNfSetId")) {
                body["targetNfSetId"] = request.at("targetNfSetId");
            }
            r = call(&oauth_dccf,
                     "POST",
                     dccf_base + kDccfDataManagementRoot +
                         (kind == "analytics" ? "/analytics-subscriptions" : "/data-subscriptions"),
                     std::optional<json>(body));
            if (r.status != 201) {
                err = sbi_core::http2::problem_response(
                    502,
                    "Bad Gateway",
                    "the DCCF did not accept the storage subscription (" +
                        (r.status > 0 ? std::to_string(r.status) + " " + r.body : r.error) + ")");
                return std::nullopt;
            }
            col.resource_uri = r.location;
        } else if (kind == "analytics") {
            json body = spec;
            body["notificationURI"] = notif_uri;
            body["notifCorrId"] = fp;
            r = call(&oauth_nwdaf,
                     "POST",
                     nwdaf_base + std::string(kNwdafEventsSubscriptionRoot) + "/subscriptions",
                     std::optional<json>(body));
            if (r.status != 201) {
                err = sbi_core::http2::problem_response(
                    502,
                    "Bad Gateway",
                    "the NWDAF did not accept the analytics subscription (" +
                        (r.status > 0 ? std::to_string(r.status) + " " + r.body : r.error) + ")");
                return std::nullopt;
            }
            col.resource_uri = r.location;
        } else {
            // Data from an NWDAF: Nnwdaf_DataManagement_Subscribe (TS 29.520 4.4.2.2.2,
            // ADR-0368) -- the NWDAF serves what it collects, or answers
            // SUBSCRIPTION_CANNOT_BE_SERVED, which is passed through as the reason.
            json body{{"dataSub", spec}, {"notificURI", notif_uri}, {"notifCorrId", fp}};
            if (request.contains("formatInstruct")) {
                body["formatInstruct"] = request.at("formatInstruct");
            }
            r = call(&oauth_nwdaf_dm,
                     "POST",
                     nwdaf_base + std::string(kNwdafDataManagementRoot) + "/subscriptions",
                     std::optional<json>(body));
            if (r.status != 201) {
                err = sbi_core::http2::problem_response(
                    r.status == 400 ? 400 : 502,
                    r.status == 400 ? "Bad Request" : "Bad Gateway",
                    "the NWDAF did not accept the data subscription (" +
                        (r.status > 0 ? std::to_string(r.status) + " " + r.body : r.error) + ")");
                return std::nullopt;
            }
            col.resource_uri = r.location;
        }
        if (!col.resource_uri.starts_with("http")) {
            col.resource_uri = (target == "DCCF" ? dccf_base : nwdaf_base) + col.resource_uri;
        }
        return col;
    };

    const auto close_collection = [&](const std::string& fp, const adrf::Collection& col) {
        const auto r = call(col.target == "DCCF"      ? &oauth_dccf
                            : col.kind == "analytics" ? &oauth_nwdaf
                                                      : &oauth_nwdaf_dm,
                            "DELETE",
                            col.resource_uri,
                            std::nullopt);
        if (r.status != 204 && r.status != 200) {
            spdlog::warn("adrf: {} unsubscribe at {} answered {} {}",
                         col.target,
                         col.resource_uri,
                         r.status,
                         r.error);
        }
        state.remove_collection(fp);
        spdlog::info("adrf: collection {} closed -- {} unsubscribed at {}",
                     fp,
                     col.target,
                     col.resource_uri);
    };

    // Which NF a targetNfId names -- its nfType from the NRF's profile (TS 29.510 GET
    // /nf-instances/{nfInstanceId}); the address is the configured base URL of that kind.
    const auto resolve_target = [&](const json& request,
                                    std::optional<sbi_core::http2::Response>& err) -> std::string {
        if (request.contains("targetNfSetId")) {
            spdlog::info("adrf: targetNfSetId {} -- routed to the configured DCCF (ADR-0367)",
                         request.at("targetNfSetId").get<std::string>());
            return "DCCF";
        }
        const auto id = request.at("targetNfId").get<std::string>();
        const auto r = call(&oauth_nrf, "GET", nrf_base + kNrfNfmRoot + "/nf-instances/" + id, {});
        if (r.status != 200) {
            err = sbi_core::http2::problem_response(
                404, "Not Found", "targetNfId " + id + " is not registered at the NRF");
            return "";
        }
        std::string nf_type;
        try {
            nf_type = json::parse(r.body).value("nfType", "");
        } catch (const json::exception&) {
        }
        if (nf_type != "DCCF" && nf_type != "NWDAF") {
            err = sbi_core::http2::problem_response(400,
                                                    "Bad Request",
                                                    "targetNfId " + id + " is a " + nf_type +
                                                        "; TS 29.575 4.2.2.3.2 names a DCCF or "
                                                        "NWDAF");
            return "";
        }
        return nf_type;
    };

    boost::asio::io_context ioc;
    sbi_core::http2::Server server(ioc, "0.0.0.0", port, server_tls);
    if (const auto tps_limit = sbi_core::read_tps_limit(config); tps_limit.enabled()) {
        server.set_tps_limit(tps_limit.sustained_tps, tps_limit.burst);
    }

    // ============ Nadrf_DataManagement ==========================================================

    // ---- StorageRequest (4.2.2.2) ---------------------------------------------------------------
    server.add_route(
        "POST",
        std::string(kDataManagementRoot) + "/data-store-records",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto dto = sbi_core::http2::parse_json_body<sbi_gen::NadrfDataStoreRecord>(req, err);
            if (!dto) {
                return err;
            }
            json record = json::parse(req.body);
            std::string why;
            const auto kind = record_kind(record, why);
            if (!kind) {
                return problem(400, "Bad Request", why, "INVALID_MSG_FORMAT");
            }
            std::optional<json> requested;
            if (record.contains("storeHandl")) {
                requested = record.at("storeHandl");
            }
            try {
                const auto row =
                    store_record(*kind, record, apply_handling(requested), std::nullopt, "request");
                auto resp = json_response(201, row.record);
                resp.headers.emplace("location",
                                     std::string(kDataManagementRoot) + "/data-store-records/" +
                                         row.store_trans_id);
                return resp;
            } catch (const std::exception& e) {
                return sbi_core::http2::problem_response(500, "Internal Server Error", e.what());
            }
        });

    // ---- RetrievalRequest (4.2.2.5) -------------------------------------------------------------
    server.add_route(
        "GET",
        std::string(kDataManagementRoot) + "/data-store-records",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto q = [&](const char* name) -> std::optional<std::string> {
                const auto it = req.query_params.find(name);
                if (it == req.query_params.end()) {
                    return std::nullopt;
                }
                return it->second;
            };
            const auto by_id = q("store-trans-id");
            const auto by_fetch = q("fetch-correlation-ids");
            const auto by_set = q("data-set-id");
            if ((by_id ? 1 : 0) + (by_fetch ? 1 : 0) + (by_set ? 1 : 0) != 1) {
                return problem(400,
                               "Bad Request",
                               "exactly one of store-trans-id, fetch-correlation-ids, data-set-id "
                               "selects the records (TS 29.575 4.2.2.5.2)",
                               "MANDATORY_IE_MISSING");
            }
            try {
                std::vector<adrf::StoredRecord> found;
                if (by_id) {
                    if (auto r = records.get(*by_id)) {
                        found.push_back(std::move(*r));
                    }
                } else if (by_fetch) {
                    std::vector<std::string> ids;
                    for (const auto& fetch_id : sbi_core::http2::split_form_array(*by_fetch)) {
                        if (auto rec = state.take_fetch(fetch_id)) {
                            ids.push_back(*rec);
                        }
                    }
                    found = records.get_many(ids);
                } else {
                    found = records.by_data_set(*by_set, std::nullopt);
                }
                if (found.empty()) {
                    return no_content();
                }
                std::string dropped;
                json merged = merge_records(found, dropped);
                if (!dropped.empty()) {
                    spdlog::warn("adrf: data set {} mixes analytics and data records; answering "
                                 "with the {} ones only (single-record response, ADR-0367)",
                                 by_set.value_or("-"),
                                 found.front().kind);
                }
                merged["suppFeat"] = negotiate(json::object(), kEnhDataMgmt);
                return json_response(200, merged);
            } catch (const std::exception& e) {
                return sbi_core::http2::problem_response(500, "Internal Server Error", e.what());
            }
        });

    // ---- Delete by storeTransId (4.2.2.9.2) -----------------------------------------------------
    server.add_route(
        "DELETE",
        std::string(kDataManagementRoot) + "/data-store-records/{storeTransId}",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            try {
                if (!records.remove(req.path_params.at("storeTransId"))) {
                    return sbi_core::http2::problem_response(
                        404, "Not Found", "no such data store record");
                }
            } catch (const std::exception& e) {
                return sbi_core::http2::problem_response(500, "Internal Server Error", e.what());
            }
            deleted_counter->Add(1);
            return no_content();
        });

    // ---- Delete by specification (4.2.2.9.3) ----------------------------------------------------
    server.add_route(
        "POST",
        std::string(kDataManagementRoot) + "/remove-stored-data-analytics",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto dto = sbi_core::http2::parse_json_body<sbi_gen::NadrfStoredDataSpec>(req, err);
            if (!dto) {
                return err;
            }
            const json spec = json::parse(req.body);
            const int alternatives = (spec.contains("dataSpec") ? 1 : 0) +
                                     (spec.contains("anaSpec") ? 1 : 0) +
                                     (spec.contains("dataSetId") ? 1 : 0);
            if (alternatives != 1) {
                return problem(400,
                               "Bad Request",
                               "exactly one of dataSpec, anaSpec, dataSetId (TS 29.575 "
                               "NadrfStoredDataSpec oneOf)",
                               "INVALID_MSG_FORMAT");
            }
            const auto window = parse_window(spec.at("timePeriod"));
            if (!window) {
                return problem(
                    400, "Bad Request", "timePeriod is not a TimeWindow", "INVALID_MSG_FORMAT");
            }
            std::optional<std::string> fp;
            std::optional<std::string> data_set_id;
            if (spec.contains("dataSpec")) {
                fp = adrf::fingerprint("data", spec.at("dataSpec"));
            } else if (spec.contains("anaSpec")) {
                fp = adrf::fingerprint("analytics", spec.at("anaSpec"));
            } else {
                data_set_id = spec.at("dataSetId").get<std::string>();
            }
            try {
                const auto n = records.remove_matching(fp, data_set_id, *window);
                deleted_counter->Add(static_cast<std::uint64_t>(n));
                spdlog::info("adrf: remove-stored-data-analytics deleted {} record(s)", n);
            } catch (const std::exception& e) {
                return sbi_core::http2::problem_response(500, "Internal Server Error", e.what());
            }
            return no_content();
        });

    // ---- StorageSubscriptionRequest (4.2.2.3) ---------------------------------------------------
    server.add_route(
        "POST",
        std::string(kDataManagementRoot) + "/request-storage-sub",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto dto =
                sbi_core::http2::parse_json_body<sbi_gen::NadrfDataStoreSubscription>(req, err);
            if (!dto) {
                return err;
            }
            const json request = json::parse(req.body);
            if (request.contains("anaSub") == request.contains("dataSub")) {
                return problem(400,
                               "Bad Request",
                               "exactly one of anaSub, dataSub (TS 29.575 "
                               "NadrfDataStoreSubscription oneOf)",
                               request.contains("anaSub") ? "INVALID_MSG_FORMAT"
                                                          : "MANDATORY_IE_MISSING");
            }
            if (request.contains("targetNfId") == request.contains("targetNfSetId")) {
                return problem(400,
                               "Bad Request",
                               "exactly one of targetNfId, targetNfSetId",
                               request.contains("targetNfId") ? "INVALID_MSG_FORMAT"
                                                              : "MANDATORY_IE_MISSING");
            }
            const std::string kind = request.contains("anaSub") ? "analytics" : "data";
            const json& spec = request.at(kind == "analytics" ? "anaSub" : "dataSub");
            std::optional<sbi_core::http2::Response> bad;
            const auto target = resolve_target(request, bad);
            if (bad) {
                return *bad;
            }
            const auto fp = adrf::fingerprint(kind, spec);
            adrf::StorageSubscription sub;
            sub.kind = kind;
            sub.fingerprint = fp;
            sub.request = request;
            if (request.contains("dataSetTag")) {
                sub.data_set_id = request.at("dataSetTag").at("dataSetId").get<std::string>();
            }
            const auto handling = apply_handling(request.contains("storeHandl")
                                                     ? std::optional<json>(request.at("storeHandl"))
                                                     : std::nullopt);
            auto collection = state.get_collection(fp);
            std::string trans_ref_id;
            if (collection) {
                // 4.2.2.3.2 NOTE 2: the same data is already being stored -- a new transRefId,
                // no new subscription at the target.
                trans_ref_id = state.create_storage_subscription(sub);
                collection->trans_ref_ids.push_back(trans_ref_id);
                // "When more than one consumer has requested storage lifetime for the same data
                // ... the longest requested storage lifetime."
                if (!collection->store_handl ||
                    collection->store_handl->value("lifetime", std::int64_t{0}) <
                        handling.lifetime.count()) {
                    collection->store_handl = handling.as_json;
                }
                state.put_collection(fp, *collection);
                spdlog::info("adrf: {} joins collection {} ({} at {})",
                             trans_ref_id,
                             fp,
                             collection->target,
                             collection->resource_uri);
            } else {
                auto opened = open_collection(kind, spec, target, request, fp, bad);
                if (bad) {
                    return *bad;
                }
                trans_ref_id = state.create_storage_subscription(sub);
                opened->trans_ref_ids = {trans_ref_id};
                opened->store_handl = handling.as_json;
                if (request.contains("dataSetTag")) {
                    opened->data_set_tag = request.at("dataSetTag");
                }
                state.put_collection(fp, *opened);
                spdlog::info("adrf: {} opens collection {} -- {} subscribed at {}",
                             trans_ref_id,
                             fp,
                             opened->target,
                             opened->resource_uri);
            }
            storage_subs_counter->Add(1);
            spdlog::info("adrf: storage approach for {}: lifetime {}s, deletion alerts {}",
                         trans_ref_id,
                         handling.lifetime.count(),
                         handling.del_notif_uri ? "to " + *handling.del_notif_uri : "none");
            return json_response(200, json{{"transRefId", trans_ref_id}});
        });

    // ---- StorageSubscriptionRemoval (4.2.2.4) ---------------------------------------------------
    const auto remove_storage_subscription = [&](const std::string& trans_ref_id) {
        const auto sub = state.get_storage_subscription(trans_ref_id);
        if (!sub) {
            return false;
        }
        if (auto col = state.get_collection(sub->fingerprint)) {
            col->trans_ref_ids.erase(
                std::remove(col->trans_ref_ids.begin(), col->trans_ref_ids.end(), trans_ref_id),
                col->trans_ref_ids.end());
            if (col->trans_ref_ids.empty()) {
                close_collection(sub->fingerprint, *col);
            } else {
                state.put_collection(sub->fingerprint, *col);
            }
        }
        state.remove_storage_subscription(trans_ref_id);
        return true;
    };
    server.add_route(
        "POST",
        std::string(kDataManagementRoot) + "/request-storage-sub-removal",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto dto =
                sbi_core::http2::parse_json_body<sbi_gen::NadrfDataStoreSubscriptionRef>(req, err);
            if (!dto) {
                return err;
            }
            if (dto->transRefId.has_value() == dto->dataSetId.has_value()) {
                return problem(400,
                               "Bad Request",
                               "exactly one of transRefId, dataSetId (TS 29.575 "
                               "NadrfDataStoreSubscriptionRef oneOf)",
                               dto->transRefId ? "INVALID_MSG_FORMAT" : "MANDATORY_IE_MISSING");
            }
            std::vector<std::string> ids;
            if (dto->transRefId) {
                ids.push_back(*dto->transRefId);
            } else {
                ids = state.data_set_members(*dto->dataSetId);
            }
            int removed = 0;
            for (const auto& id : ids) {
                removed += remove_storage_subscription(id) ? 1 : 0;
            }
            if (removed == 0) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "no storage subscription for that transRefId / dataSetId");
            }
            return no_content();
        });

    // ---- inbound: what the DCCF (via the MFAF) or the NWDAF delivers to a collection ------------
    server.add_route(
        "POST",
        std::string(kInboundPrefix) + "/notifications/{fingerprint}",
        [&](const sbi_core::http2::Request& req) {
            const auto fp = req.path_params.at("fingerprint");
            const auto col = state.get_collection(fp);
            if (!col) {
                return problem(
                    400, "Bad Request", "no ADRF collection " + fp, "RESOURCE_CONTEXT_NOT_FOUND");
            }
            json body;
            try {
                body = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto handling = apply_handling(col->store_handl);
            std::vector<std::pair<std::string, json>> to_store; // kind, record
            if (col->target == "NWDAF" && col->kind == "data") {
                // NnwdafDataManagementNotif (TS 29.520 4.4.2.4.2): dataNotification is the
                // DataNotification buckets; a fetchInstruct would mean consTrigNotif, which this
                // ADRF never asks for.
                if (body.contains("dataNotification")) {
                    to_store.emplace_back("data",
                                          json{{"dataSub", json::array({col->spec})},
                                               {"dataNotif", body.at("dataNotification")}});
                } else if (body.contains("fetchInstruct")) {
                    spdlog::warn("adrf: collection {} delivered a fetch instruction; the ADRF "
                                 "subscribes without consTrigNotif -- ignored",
                                 fp);
                }
            } else if (col->target == "NWDAF") {
                // NnwdafEventsSubscriptionNotification, straight from the NWDAF.
                to_store.emplace_back("analytics",
                                      json{{"anaSub", json::array({col->spec})},
                                           {"anaNotifications", json::array({body})}});
            } else {
                // NmfafDataRetrievalNotification (TS 29.576 3ca): dataAnaNotif carries the
                // buckets; a fetchInstruction would mean the DCCF asked the MFAF to buffer,
                // which this ADRF never requests.
                if (body.contains("fetchInstruction")) {
                    spdlog::warn("adrf: collection {} delivered a fetch instruction; the ADRF "
                                 "subscribes without consTrigNotif -- ignored",
                                 fp);
                }
                const json dan = body.value("dataAnaNotif", json::object());
                if (dan.contains("anaNotifications")) {
                    to_store.emplace_back("analytics",
                                          json{{"anaSub", json::array({col->spec})},
                                               {"anaNotifications", dan.at("anaNotifications")}});
                }
                if (dan.contains("dataNotif")) {
                    to_store.emplace_back("data",
                                          json{{"dataSub", json::array({col->spec})},
                                               {"dataNotif", dan.at("dataNotif")}});
                }
            }
            if (to_store.empty()) {
                spdlog::warn("adrf: collection {} delivered nothing storable", fp);
                return no_content();
            }
            try {
                for (auto& [kind, record] : to_store) {
                    store_record(
                        kind, std::move(record), handling, col->data_set_tag, "subscription");
                }
            } catch (const std::exception& e) {
                return sbi_core::http2::problem_response(500, "Internal Server Error", e.what());
            }
            return no_content();
        });

    // ---- RetrievalSubscribe (4.2.2.6) -----------------------------------------------------------
    server.add_route(
        "POST",
        std::string(kDataManagementRoot) + "/data-retrieval-subscriptions",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto dto =
                sbi_core::http2::parse_json_body<sbi_gen::NadrfDataRetrievalSubscription>(req, err);
            if (!dto) {
                return err;
            }
            json request = json::parse(req.body);
            const int alternatives = (request.contains("anaSub") ? 1 : 0) +
                                     (request.contains("dataSub") ? 1 : 0) +
                                     (request.contains("dataSetId") ? 1 : 0);
            if (alternatives != 1) {
                return problem(400,
                               "Bad Request",
                               "exactly one of anaSub, dataSub, dataSetId (TS 29.575 "
                               "NadrfDataRetrievalSubscription oneOf)",
                               alternatives == 0 ? "MANDATORY_IE_MISSING" : "INVALID_MSG_FORMAT");
            }
            const auto window = parse_window(request.at("timePeriod"));
            if (!window) {
                return problem(
                    400, "Bad Request", "timePeriod is not a TimeWindow", "INVALID_MSG_FORMAT");
            }
            adrf::RetrievalSubscription sub;
            if (request.contains("anaSub")) {
                sub.fingerprint = adrf::fingerprint("analytics", request.at("anaSub"));
            } else if (request.contains("dataSub")) {
                sub.fingerprint = adrf::fingerprint("data", request.at("dataSub"));
            } else {
                sub.data_set_id = request.at("dataSetId").get<std::string>();
            }
            request["suppFeat"] = negotiate(request, kEnhDataMgmt);
            sub.request = request;
            const auto id = state.create_retrieval_subscription(sub);
            // What is already stored inside the window goes out now, after the 201 -- the
            // consumer's endpoint may be the thread waiting for this response.
            std::thread([&, id, sub, window]() {
                try {
                    const auto matched = sub.fingerprint
                                             ? records.by_fingerprint(*sub.fingerprint, window)
                                             : records.by_data_set(*sub.data_set_id, window);
                    if (!matched.empty()) {
                        notify_retrieval(id, sub, matched);
                    }
                } catch (const std::exception& e) {
                    spdlog::warn("adrf: initial retrieval for {} failed: {}", id, e.what());
                }
            }).detach();
            auto resp = json_response(201, request);
            resp.headers.emplace("location",
                                 std::string(kDataManagementRoot) +
                                     "/data-retrieval-subscriptions/" + id);
            return resp;
        });

    // ---- RetrievalUnsubscribe (4.2.2.7) ---------------------------------------------------------
    server.add_route(
        "DELETE",
        std::string(kDataManagementRoot) + "/data-retrieval-subscriptions/{subscriptionId}",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            if (!state.remove_retrieval_subscription(req.path_params.at("subscriptionId"))) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "no such data retrieval subscription");
            }
            return no_content();
        });

    // ============ Nadrf_MLModelManagement =======================================================

    // Store the models a NadrfMLModelStoreRecord names under `store_trans_id`; returns the
    // response body (mlModelInfo with the ADRF's own addresses, per-model results) and whether
    // every model failed for one reason (-> the 404 / 500 causes of 4.3.2.2.2).
    struct StoreOutcome {
        json body;
        int stored = 0;
        int failed = 0;
        std::string common_failure; // set when failed > 0 and every failure had this cause
    };
    const auto store_models = [&](const std::string& store_trans_id, const json& request) {
        StoreOutcome out;
        out.body = json::object();
        if (request.contains("nfInstanceId")) {
            out.body["nfInstanceId"] = request.at("nfInstanceId");
        }
        if (request.contains("nfSetId")) {
            out.body["nfSetId"] = request.at("nfSetId");
        }
        json infos = json::array();
        json results = json::array();
        const auto record_result = [&](std::int64_t id, const char* result) {
            results.push_back(json{{"modelUniqueId", id}, {"storeResult", result}});
            if (std::string(result) == "ML_MODEL_FILE_STORED_IN_ADRF") {
                ++out.stored;
                return;
            }
            ++out.failed;
            if (out.failed == 1) {
                out.common_failure = result;
            } else if (out.common_failure != result) {
                out.common_failure.clear();
            }
        };
        const auto keep = [&](std::int64_t id,
                              const std::string& bytes,
                              const std::optional<json>& source,
                              const std::optional<json>& allow) {
            adrf::StoredModel m;
            m.model_unique_id = id;
            m.store_trans_id = store_trans_id;
            m.source_addr = source;
            m.allow_consumers = allow;
            models.upsert_model(m, bytes);
            models_stored_counter->Add(1);
            json info{
                {"modelUniqueId", id},
                {"mlFileAddr",
                 json{{"mLModelUrl", self_base + kModelFilesPrefix + "/" + std::to_string(id)}}},
                {"mlStorageSize", static_cast<std::int64_t>(bytes.size())}};
            if (allow) {
                info["allowConsumerList"] = *allow;
            }
            infos.push_back(std::move(info));
            record_result(id, "ML_MODEL_FILE_STORED_IN_ADRF");
        };
        for (const auto& info : request.value("mlModelInfo", json::array())) {
            const auto id = info.at("modelUniqueId").get<std::int64_t>();
            const json addr = info.at("mlFileAddr");
            const std::optional<json> allow =
                info.contains("allowConsumerList")
                    ? std::optional<json>(info.at("allowConsumerList"))
                    : std::nullopt;
            if (!addr.contains("mLModelUrl")) {
                spdlog::warn("adrf: model {} names mlFileFqdn only; FQDN download is not built "
                             "(ADR-0367)",
                             id);
                record_result(id, "ML_MODEL_FILE_ADDRESS_NOT_FOUND");
                continue;
            }
            const auto r = call(nullptr, "GET", addr.at("mLModelUrl").get<std::string>(), {});
            if (r.status == 404) {
                record_result(id, "ML_MODEL_FILE_ADDRESS_NOT_FOUND");
                continue;
            }
            if (r.status != 200 || static_cast<std::int64_t>(r.body.size()) > model_max_bytes) {
                spdlog::warn("adrf: model {} download from {} failed ({} {}, {} octets)",
                             id,
                             addr.at("mLModelUrl").get<std::string>(),
                             r.status,
                             r.error,
                             r.body.size());
                record_result(id, "ML_MODEL_FILE_DOWNLOAD_FAILED");
                continue;
            }
            keep(id, r.body, std::optional<json>(addr), allow);
        }
        for (const auto& m : request.value("mlModels", json::array())) {
            const auto id = m.at("modelUniqueId").get<std::int64_t>();
            const std::optional<json> allow = m.contains("allowConsumerList")
                                                  ? std::optional<json>(m.at("allowConsumerList"))
                                                  : std::nullopt;
            const auto bytes = base64_decode(m.at("mlModel").get<std::string>());
            if (!bytes || static_cast<std::int64_t>(bytes->size()) > model_max_bytes) {
                // An inline model that is not base64 (or too large) never reached storage; the
                // nearest StoreResult is the download one.
                record_result(id, "ML_MODEL_FILE_DOWNLOAD_FAILED");
                continue;
            }
            keep(id, *bytes, std::nullopt, allow);
        }
        if (!infos.empty()) {
            out.body["mlModelInfo"] = infos;
        }
        if (out.failed > 0) {
            out.body["modelStoreResults"] = results;
        }
        out.body["suppFeat"] = negotiate(request, kEnModelMgmt);
        return out;
    };
    const auto all_failed_response = [&](const StoreOutcome& o) -> sbi_core::http2::Response {
        if (o.common_failure == "ML_MODEL_FILE_ADDRESS_NOT_FOUND") {
            return problem(404,
                           "Not Found",
                           "no ML model file address could be resolved",
                           "ML_MODEL_FILE_ADDRESS_NOT_FOUND");
        }
        return problem(500,
                       "Internal Server Error",
                       "no ML model could be downloaded or decoded",
                       "ML_MODEL_FILE_DOWNLOAD_FAILED");
    };
    const auto validate_ml_record =
        [&](const json& request) -> std::optional<sbi_core::http2::Response> {
        if (request.contains("nfInstanceId") == request.contains("nfSetId")) {
            return problem(400,
                           "Bad Request",
                           "exactly one of nfInstanceId, nfSetId (TS 29.575 "
                           "NadrfMLModelStoreRecord oneOf)",
                           request.contains("nfInstanceId") ? "INVALID_MSG_FORMAT"
                                                            : "MANDATORY_IE_MISSING");
        }
        if (!request.contains("mlModelInfo") && !request.contains("mlModels")) {
            return problem(
                400, "Bad Request", "mlModelInfo or mlModels is required", "MANDATORY_IE_MISSING");
        }
        for (const auto& m : request.value("mlModels", json::array())) {
            if (!m.contains("modelUniqueId") || !m.contains("mlModel")) {
                return problem(400,
                               "Bad Request",
                               "MLModel requires modelUniqueId and mlModel",
                               "MANDATORY_IE_MISSING");
            }
        }
        for (const auto& i : request.value("mlModelInfo", json::array())) {
            if (!i.contains("modelUniqueId") || !i.contains("mlFileAddr")) {
                return problem(400,
                               "Bad Request",
                               "MLModelInfo requires modelUniqueId and mlFileAddr",
                               "MANDATORY_IE_MISSING");
            }
        }
        return std::nullopt;
    };

    // ---- MLModelManagement_StorageRequest: create (4.3.2.2.2) -----------------------------------
    server.add_route(
        "POST",
        std::string(kMLModelManagementRoot) + "/mlmodel-store-records",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto dto = sbi_core::http2::parse_json_body<sbi_gen::NadrfMLModelStoreRecord>(req, err);
            if (!dto) {
                return err;
            }
            const json request = json::parse(req.body);
            if (auto bad = validate_ml_record(request)) {
                return *bad;
            }
            const auto id = state.next_id("adrf-ml-");
            try {
                models.create_record(id, dto->nfInstanceId, dto->nfSetId);
                const auto outcome = store_models(id, request);
                if (outcome.stored == 0) {
                    models.remove_record(id);
                    return all_failed_response(outcome);
                }
                auto resp = json_response(201, outcome.body);
                resp.headers.emplace("location",
                                     std::string(kMLModelManagementRoot) +
                                         "/mlmodel-store-records/" + id);
                spdlog::info("adrf: ML model store record {} created ({} stored, {} failed)",
                             id,
                             outcome.stored,
                             outcome.failed);
                return resp;
            } catch (const std::exception& e) {
                return sbi_core::http2::problem_response(500, "Internal Server Error", e.what());
            }
        });

    // ---- MLModelManagement_StorageRequest: update (4.3.2.2.3) -----------------------------------
    server.add_route(
        "PUT",
        std::string(kMLModelManagementRoot) + "/mlmodel-store-records/{storeTransId}",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto dto = sbi_core::http2::parse_json_body<sbi_gen::NadrfMLModelStoreRecord>(req, err);
            if (!dto) {
                return err;
            }
            const json request = json::parse(req.body);
            if (auto bad = validate_ml_record(request)) {
                return *bad;
            }
            const auto id = req.path_params.at("storeTransId");
            try {
                if (!models.record_exists(id)) {
                    return problem(
                        404, "Not Found", "no such ML model store record", "ML_MODEL_NOT_FOUND");
                }
                models.create_record(id, dto->nfInstanceId, dto->nfSetId);
                const auto outcome = store_models(id, request);
                if (outcome.stored == 0) {
                    return all_failed_response(outcome);
                }
                return json_response(200, outcome.body);
            } catch (const std::exception& e) {
                return sbi_core::http2::problem_response(500, "Internal Server Error", e.what());
            }
        });

    // May `consumer` (the token subject, empty when the request carried no token) retrieve `m`?
    // 4.3.2.3.2: the storing MTLF, or a member of the model's allowed consumer list.
    const auto may_retrieve = [&](const std::string& consumer, const adrf::StoredModel& m) {
        if (model_access_policy == "not-enforced") {
            return true;
        }
        if (!consumer.empty() && m.owner_nf_instance_id && *m.owner_nf_instance_id == consumer) {
            return true;
        }
        if (m.allow_consumers) {
            for (const auto& a : *m.allow_consumers) {
                if (a.contains("nfInstanceId") && !consumer.empty() &&
                    a.at("nfInstanceId").get<std::string>() == consumer) {
                    return true;
                }
                if (a.contains("nfSetId")) {
                    spdlog::debug("adrf: model {} allows NF set {}; a token names no NF set, "
                                  "not matched (ADR-0367)",
                                  m.model_unique_id,
                                  a.at("nfSetId").get<std::string>());
                }
            }
        }
        return false;
    };
    const auto consumer_of = [&](const sbi_core::http2::Request& req) {
        const auto auth = check_bearer(req, verifier);
        return (auth && auth->valid) ? auth->subject : std::string();
    };

    // ---- MLModelManagement_RetrievalRequest (4.3.2.3) -------------------------------------------
    server.add_route(
        "GET",
        std::string(kMLModelManagementRoot) + "/mlmodel-store-records",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto by_record = req.query_params.find("store-trans-id");
            const auto by_ids = req.query_params.find("model-unique-ids");
            if ((by_record != req.query_params.end()) == (by_ids != req.query_params.end())) {
                return problem(400,
                               "Bad Request",
                               "exactly one of store-trans-id, model-unique-ids (TS 29.575 "
                               "4.3.2.3.2)",
                               "MANDATORY_IE_MISSING");
            }
            try {
                std::vector<adrf::StoredModel> found;
                if (by_record != req.query_params.end()) {
                    found = models.models_in_record(by_record->second);
                } else {
                    std::vector<std::int64_t> ids;
                    for (const auto& s : sbi_core::http2::split_form_array(by_ids->second)) {
                        try {
                            ids.push_back(std::stoll(s));
                        } catch (const std::exception&) {
                            return problem(400,
                                           "Bad Request",
                                           "model-unique-ids must be integers",
                                           "INVALID_QUERY_PARAM");
                        }
                    }
                    found = models.models_by_ids(ids);
                }
                if (found.empty()) {
                    return no_content();
                }
                const auto consumer = consumer_of(req);
                json body = json::object();
                json infos = json::array();
                json results = json::array();
                for (const auto& m : found) {
                    if (!body.contains("nfInstanceId") && m.owner_nf_instance_id) {
                        body["nfInstanceId"] = *m.owner_nf_instance_id;
                    }
                    if (!body.contains("nfSetId") && m.owner_nf_set_id) {
                        body["nfSetId"] = *m.owner_nf_set_id;
                    }
                    if (!may_retrieve(consumer, m)) {
                        results.push_back(json{{"modelUniqueId", m.model_unique_id},
                                               {"storeResult", "RETRIEVAL_ML_MODEL_NOT_ALLOWED"}});
                        continue;
                    }
                    json info{{"modelUniqueId", m.model_unique_id},
                              {"mlFileAddr",
                               json{{"mLModelUrl",
                                     self_base + kModelFilesPrefix + "/" +
                                         std::to_string(m.model_unique_id)}}},
                              {"mlStorageSize", m.storage_size}};
                    if (m.allow_consumers) {
                        info["allowConsumerList"] = *m.allow_consumers;
                    }
                    infos.push_back(std::move(info));
                }
                if (infos.empty()) {
                    return problem(403,
                                   "Forbidden",
                                   "consumer '" + consumer +
                                       "' is neither the storing MTLF nor in the allowed consumer "
                                       "list of the requested model(s)",
                                   "RETRIEVAL_ML_MODEL_NOT_ALLOWED");
                }
                body["mlModelInfo"] = infos;
                if (!results.empty()) {
                    body["modelStoreResults"] = results;
                }
                body["suppFeat"] = negotiate(json::object(), kEnModelMgmt);
                return json_response(200, body);
            } catch (const std::exception& e) {
                return sbi_core::http2::problem_response(500, "Internal Server Error", e.what());
            }
        });

    // ---- the model file itself (the mLModelUrl this ADRF hands out) -----------------------------
    server.add_route(
        "GET",
        std::string(kModelFilesPrefix) + "/{modelUniqueId}",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            std::int64_t id = 0;
            try {
                id = std::stoll(req.path_params.at("modelUniqueId"));
            } catch (const std::exception&) {
                return sbi_core::http2::problem_response(404, "Not Found", "no such ML model");
            }
            try {
                const auto m = models.get_bytes(id);
                if (!m) {
                    return problem(404, "Not Found", "no such ML model", "ML_MODEL_NOT_FOUND");
                }
                if (!may_retrieve(consumer_of(req), *m)) {
                    return problem(403,
                                   "Forbidden",
                                   "not an allowed consumer of this ML model",
                                   "RETRIEVAL_ML_MODEL_NOT_ALLOWED");
                }
                models_served_counter->Add(1);
                sbi_core::http2::Response resp;
                resp.status = 200;
                resp.headers.emplace("content-type", "application/octet-stream");
                resp.body = m->bytes;
                return resp;
            } catch (const std::exception& e) {
                return sbi_core::http2::problem_response(500, "Internal Server Error", e.what());
            }
        });

    // ---- MLModelManagement_Delete: by record (4.3.2.4.2) ----------------------------------------
    server.add_route(
        "DELETE",
        std::string(kMLModelManagementRoot) + "/mlmodel-store-records/{storeTransId}",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("storeTransId");
            try {
                const auto in_record = models.models_in_record(id);
                if (!models.remove_record(id)) {
                    return problem(
                        404, "Not Found", "no ML model stored under " + id, "ML_MODEL_NOT_FOUND");
                }
                json results = json::array();
                for (const auto& m : in_record) {
                    results.push_back(json{{"modelUniqueId", m.model_unique_id},
                                           {"deleteResult", "ML_MODEL_DELETED"}});
                }
                spdlog::info(
                    "adrf: ML model store record {} deleted ({} model(s))", id, in_record.size());
                return json_response(200, results);
            } catch (const std::exception& e) {
                return sbi_core::http2::problem_response(500, "Internal Server Error", e.what());
            }
        });

    // ---- MLModelManagement_Delete: by unique id (4.3.2.4.3) -------------------------------------
    server.add_route(
        "POST",
        std::string(kMLModelManagementRoot) + "/remove-stored-mlmodel",
        [&](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto ids = sbi_core::http2::parse_json_body<std::vector<std::int64_t>>(req, err);
            if (!ids) {
                return err;
            }
            if (ids->empty()) {
                return problem(400,
                               "Bad Request",
                               "the list of ML model identifiers shall not be empty",
                               "MANDATORY_IE_MISSING");
            }
            try {
                json results = json::array();
                int deleted = 0;
                for (const auto id : *ids) {
                    const bool ok = models.remove_model(id);
                    deleted += ok ? 1 : 0;
                    results.push_back(
                        json{{"modelUniqueId", id},
                             {"deleteResult", ok ? "ML_MODEL_DELETED" : "ML_MODEL_NOT_FOUND"}});
                }
                if (deleted == 0) {
                    return problem(404,
                                   "Not Found",
                                   "none of the ML models is stored here",
                                   "ML_MODEL_NOT_FOUND");
                }
                if (deleted == static_cast<int>(ids->size())) {
                    return no_content();
                }
                return json_response(200, results);
            } catch (const std::exception& e) {
                return sbi_core::http2::problem_response(500, "Internal Server Error", e.what());
            }
        });

    // ============ background: lifetime reaper and retrieval-window sweeper =====================
    // Both lease-held (Valkey SET NX PX), so with N replicas exactly one runs each sweep.
    std::atomic<bool> running{true};
    // Sleep in slices so shutdown (running = false, then join) never waits a whole interval.
    const auto pause = [&](std::int64_t seconds) {
        for (std::int64_t i = 0; i < seconds * 5 && running; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        return running.load();
    };
    std::thread reaper([&]() {
        while (pause(reaper_interval)) {
            if (!state.acquire_lease("reaper", std::chrono::milliseconds(reaper_interval * 900))) {
                continue;
            }
            try {
                const auto now = std::chrono::system_clock::now();
                // 4.2.2.8.3: alert delNotifUri before the deletion; a consumer answering 200
                // {retrievalInd: true} gets a grace period to fetch.
                for (const auto& r :
                     records.awaiting_alert(now + std::chrono::seconds(alert_lead))) {
                    json alert{{"alertStorTransId", r.store_trans_id},
                               {"delNotifCorrId", r.del_notif_corr_id.value_or("")}};
                    // The callback name follows the operation that asked for the alert:
                    // storageAlertNotification for a StorageRequest record,
                    // storageSubAlertNotification for one a storage subscription produced.
                    const auto resp = call(nullptr,
                                           "POST",
                                           *r.del_notif_uri,
                                           std::optional<json>(alert),
                                           r.origin == "subscription" ? kStorageSubAlertCallback
                                                                      : kStorageAlertCallback);
                    alert_counter->Add(1);
                    records.mark_alert_sent(r.store_trans_id);
                    bool wants_it = false;
                    if (resp.status == 200) {
                        try {
                            wants_it = json::parse(resp.body).value("retrievalInd", false);
                        } catch (const json::exception&) {
                        }
                    }
                    if (wants_it) {
                        records.defer_expiry(r.store_trans_id,
                                             now + std::chrono::seconds(alert_grace));
                        spdlog::info("adrf: {} will be retrieved before deletion -- expiry "
                                     "deferred {}s",
                                     r.store_trans_id,
                                     alert_grace);
                    }
                }
                if (const auto n = records.remove_expired(now); n > 0) {
                    deleted_counter->Add(static_cast<std::uint64_t>(n));
                    spdlog::info("adrf: lifetime reaper deleted {} expired record(s)", n);
                }
            } catch (const std::exception& e) {
                spdlog::warn("adrf: reaper sweep failed: {}", e.what());
            }
        }
    });
    std::thread sweeper([&]() {
        while (pause(sweep_interval)) {
            if (!state.acquire_lease("sweeper", std::chrono::milliseconds(sweep_interval * 900))) {
                continue;
            }
            const auto now = std::chrono::system_clock::now();
            for (const auto& id : state.retrieval_subscription_ids()) {
                const auto sub = state.get_retrieval_subscription(id);
                if (!sub) {
                    continue;
                }
                const auto window = parse_window(sub->request.at("timePeriod"));
                if (window && window->stop < now) {
                    state.remove_retrieval_subscription(id);
                    spdlog::info("adrf: retrieval subscription {} ended with its timePeriod", id);
                }
            }
        }
    });
    std::thread(run_nrf_lifecycle, instance_id, nrf_base, advertised_ipv4, heartbeat_seconds)
        .detach();
    server.start();
    spdlog::info("adrf: listening on https://0.0.0.0:{} (TLS 1.3 + mTLS); DCCF at {}, NWDAF at "
                 "{}; data store {}:{}/{} ({}), ML model store ({})",
                 port,
                 dccf_base,
                 nwdaf_base,
                 ds.host,
                 ds.port,
                 ds.database,
                 records.connected() ? "connected" : "UNREACHABLE",
                 models.connected() ? "connected" : "UNREACHABLE");
    spdlog::info("adrf: Prometheus metrics at http://{}/metrics", metrics_bind_address);
    sbi_core::run_multi_threaded(ioc);
    // Joined, not detached: the stores they use are destroyed when main returns.
    running = false;
    reaper.join();
    sweeper.join();
    return 0;
}
