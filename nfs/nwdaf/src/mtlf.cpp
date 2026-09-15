#include "mtlf.hpp"

#include "sbi_core/datetime.hpp"
#include "sbi_core/json_body.hpp"
#include "sbi_core/problem_details.hpp"
#include "sbi_core/sbi_headers.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <string_view>

#include "TS26510_CommonData_grp.hpp"

namespace nwdaf {

using json = nlohmann::json;

namespace {

// servers[0].url of TS29520_Nnwdaf_MLModelProvision.yaml (ADR-0325).
constexpr const char* kProvisionRoot = "/nnwdaf-mlmodelprovision/v1";
// TS 29.500 Annex B: <API>_<callback key>; the YAML keys its callback "myNotification".
constexpr const char* kProvisionCallback = "Nnwdaf_MLModelProvision_myNotification";
// The ADRF's roots -- servers[0].url of TS29575_Nadrf_DataManagement.yaml and
// TS29575_Nadrf_MLModelManagement.yaml -- used as a client.
constexpr const char* kAdrfDataManagementRoot = "/nadrf-datamanagement/v1";
constexpr const char* kAdrfMlModelManagementRoot = "/nadrf-mlmodelmanagement/v1";
constexpr const char* kNrfDiscRoot = "/nnrf-disc/v1";

std::string base64_encode(std::string_view in) {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 2 < in.size()) {
        const auto a = static_cast<unsigned char>(in[i]);
        const auto b = static_cast<unsigned char>(in[i + 1]);
        const auto c = static_cast<unsigned char>(in[i + 2]);
        out.push_back(kTable[a >> 2]);
        out.push_back(kTable[((a & 0x03) << 4) | (b >> 4)]);
        out.push_back(kTable[((b & 0x0f) << 2) | (c >> 6)]);
        out.push_back(kTable[c & 0x3f]);
        i += 3;
    }
    if (i < in.size()) {
        const auto a = static_cast<unsigned char>(in[i]);
        const auto b = i + 1 < in.size() ? static_cast<unsigned char>(in[i + 1]) : 0;
        out.push_back(kTable[a >> 2]);
        out.push_back(kTable[((a & 0x03) << 4) | (b >> 4)]);
        out.push_back(i + 1 < in.size() ? kTable[(b & 0x0f) << 2] : '=');
        out.push_back('=');
    }
    return out;
}

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

// The nfInstanceId an NRF NotificationData is about: nfProfile.nfInstanceId, else the tail of
// nfInstanceUri.
std::string instance_of(const json& n) {
    if (n.contains("nfProfile") && n.at("nfProfile").contains("nfInstanceId")) {
        return n.at("nfProfile").at("nfInstanceId").get<std::string>();
    }
    const auto uri = n.value("nfInstanceUri", "");
    const auto slash = uri.rfind('/');
    return slash == std::string::npos ? uri : uri.substr(slash + 1);
}

} // namespace

Mtlf::Mtlf(MtlfOptions options,
           sbi_core::http2::Client& client,
           std::mutex& client_mutex,
           sbi_core::OAuth2Client& oauth_disc,
           sbi_core::OAuth2Client& oauth_adrf_dm,
           sbi_core::OAuth2Client& oauth_adrf_ml,
           sbi_core::jwt::Verifier& verifier,
           MlStore& store,
           TrainingExecutor& executor)
    : options_(std::move(options)), client_(client), client_mutex_(client_mutex),
      oauth_disc_(oauth_disc), oauth_adrf_dm_(oauth_adrf_dm), oauth_adrf_ml_(oauth_adrf_ml),
      verifier_(verifier), store_(store), executor_(executor) {}

bool Mtlf::trains(const std::string& event) const {
    return std::find(options_.events.begin(), options_.events.end(), event) !=
           options_.events.end();
}

json Mtlf::nrf_profile_info() const {
    // TS 29.510 MlAnalyticsInfo: "ML Analytics Filter information supported by the
    // Nnwdaf_MLModelProvision service".
    return json{{"mlAnalyticsList", json::array({json{{"mlAnalyticsIds", options_.events}}})}};
}

Mtlf::Call Mtlf::call(sbi_core::OAuth2Client& oauth,
                      const std::string& method,
                      const std::string& url,
                      const json* body,
                      const std::optional<std::string>& callback) {
    Call out;
    sbi_core::http2::ClientRequest req;
    req.method = method;
    req.url = url;
    if (body != nullptr) {
        req.headers.emplace("content-type", "application/json");
        req.body = body->dump();
    }
    if (callback) {
        req.headers.emplace(sbi_core::headers::kCallback, *callback);
    }
    const std::lock_guard<std::mutex> lock(client_mutex_);
    auto token = oauth.get_bearer_token();
    if (!token) {
        out.error = token.error();
        return out;
    }
    req.headers.emplace("authorization", "Bearer " + *token);
    if (auto resp = client_.send(req); resp) {
        out.status = resp->status;
        out.body = resp->body;
        if (const auto it = resp->headers.find("location"); it != resp->headers.end()) {
            out.location = it->second;
        }
    } else {
        out.error = resp.error();
    }
    return out;
}

std::optional<std::string> Mtlf::adrf_base() {
    if (options_.adrf_base_url.empty()) {
        return std::nullopt;
    }
    return options_.adrf_base_url;
}

// The ADRF's nfInstanceId, from NRF discovery (TS 29.510) -- what mLModelAdrf.adrfId carries.
std::optional<std::string> Mtlf::adrf_instance_id() {
    if (!adrf_id_cached_.empty()) {
        return adrf_id_cached_;
    }
    const auto r = call(oauth_disc_,
                        "GET",
                        options_.nrf_base + kNrfDiscRoot +
                            "/nf-instances?target-nf-type=ADRF&requester-nf-type=NWDAF",
                        nullptr);
    if (r.status != 200) {
        return std::nullopt;
    }
    try {
        const auto found = json::parse(r.body).value("nfInstances", json::array());
        if (found.empty()) {
            return std::nullopt;
        }
        adrf_id_cached_ = found.front().at("nfInstanceId").get<std::string>();
        return adrf_id_cached_;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// An NWDAF containing AnLF that computes `event` (nwdafInfo.eventIds), other than this instance:
// the target of the ADRF's storage subscription.
std::optional<std::string> Mtlf::anlf_instance_id(const std::string& event) {
    const auto r = call(oauth_disc_,
                        "GET",
                        options_.nrf_base + kNrfDiscRoot +
                            "/nf-instances?target-nf-type=NWDAF&requester-nf-type=NWDAF",
                        nullptr);
    if (r.status != 200) {
        return std::nullopt;
    }
    try {
        for (const auto& p : json::parse(r.body).value("nfInstances", json::array())) {
            const auto id = p.value("nfInstanceId", "");
            if (id == options_.instance_id) {
                continue;
            }
            const auto ids = p.value("nwdafInfo", json::object()).value("eventIds", json::array());
            if (std::find(ids.begin(), ids.end(), json(event)) != ids.end()) {
                return id;
            }
        }
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

// TS 23.288 6.2E.2 step 8 / 6.2B.3: the MTLF asks the ADRF to collect and store the data the
// model is trained on -- here the AnLF's collected NRF NF-status data, through an
// Nnwdaf_DataManagement subscription the ADRF opens at the AnLF -- under this MTLF's data set.
bool Mtlf::ensure_storage_subscription(const std::string& event) {
    if (store_.get_storage_subscription(event)) {
        return true;
    }
    const auto base = adrf_base();
    if (!base) {
        return false;
    }
    const auto anlf = anlf_instance_id(event);
    if (!anlf) {
        spdlog::info("nwdaf-mtlf: no NWDAF containing AnLF for {} at the NRF yet; the ADRF "
                     "storage subscription waits",
                     event);
        return false;
    }
    const json request{
        {"dataSub",
         json{{"nrfDataSub", json{{"nfStatusNotificationUri", "https://placeholder.invalid/"}}}}},
        {"targetNfId", *anlf},
        {"dataSetTag", json{{"dataSetId", options_.data_set_id}}}};
    const auto r = call(
        oauth_adrf_dm_, "POST", *base + kAdrfDataManagementRoot + "/request-storage-sub", &request);
    if (r.status != 200) {
        spdlog::warn("nwdaf-mtlf: Nadrf_DataManagement_StorageSubscriptionRequest for {} failed "
                     "({} {})",
                     event,
                     r.status,
                     r.error.empty() ? r.body : r.error);
        return false;
    }
    try {
        const auto trans_ref = json::parse(r.body).at("transRefId").get<std::string>();
        store_.put_storage_subscription(event, trans_ref);
        spdlog::info("nwdaf-mtlf: the ADRF stores the AnLF's NRF data for {} under data set {} "
                     "(transRefId {})",
                     event,
                     options_.data_set_id,
                     trans_ref);
        return true;
    } catch (const std::exception& e) {
        spdlog::warn("nwdaf-mtlf: storage subscription response unreadable: {}", e.what());
        return false;
    }
}

// Nadrf_DataManagement_RetrievalRequest by data set (TS 29.575 4.2.2.5.2), turned into the
// sidecar's dataset document: per NF instance, its observations in stored order. `windows` is
// the number of usable training windows that data yields (the sidecar's rule, counted here so
// the retrain decision needs no Python).
json Mtlf::training_dataset(const std::string& event, std::int64_t& windows) {
    windows = 0;
    json series = json::object();
    std::int64_t records = 0;
    const auto base = adrf_base();
    if (base) {
        const auto r = call(oauth_adrf_dm_,
                            "GET",
                            *base + kAdrfDataManagementRoot +
                                "/data-store-records?data-set-id=" + options_.data_set_id,
                            nullptr);
        if (r.status == 200) {
            try {
                const auto body = json::parse(r.body);
                for (const auto& n : body.value("dataNotif", json::object())
                                         .value("nrfEventNotifs", json::array())) {
                    const auto id = instance_of(n);
                    if (id.empty()) {
                        continue;
                    }
                    json obs{{"status", "REGISTERED"}};
                    if (n.value("event", "") == "NF_DEREGISTERED") {
                        obs["status"] = "DEREGISTERED";
                    } else if (n.contains("nfProfile")) {
                        const auto& p = n.at("nfProfile");
                        obs["status"] = p.value("nfStatus", "REGISTERED");
                        if (p.contains("load") && p.at("load").is_number()) {
                            obs["load"] = p.at("load").get<std::int64_t>();
                        }
                    }
                    series[id].push_back(obs);
                    ++records;
                }
            } catch (const std::exception& e) {
                spdlog::warn(
                    "nwdaf-mtlf: ADRF data set {} unreadable: {}", options_.data_set_id, e.what());
            }
        } else if (r.status != 204) {
            spdlog::warn("nwdaf-mtlf: Nadrf_DataManagement_RetrievalRequest for data set {} "
                         "failed ({} {})",
                         options_.data_set_id,
                         r.status,
                         r.error.empty() ? r.body : r.error);
        }
    }
    // Usable windows: kNfLoadLags + 1 consecutive observations that all carry a load.
    constexpr std::size_t kLags = 4;
    for (const auto& [id, obs] : series.items()) {
        for (std::size_t i = kLags; i < obs.size(); ++i) {
            bool ok = true;
            for (std::size_t k = i - kLags; k <= i && ok; ++k) {
                ok = obs[k].contains("load");
            }
            windows += ok ? 1 : 0;
        }
    }
    return json{{"event", event},
                {"series", series},
                {"source",
                 json{{"kind", "adrf"},
                      {"adrf_id", adrf_instance_id().value_or("")},
                      {"data_set_id", options_.data_set_id},
                      {"records", records}}}};
}

// One training run: dataset -> executor -> Nadrf_MLModelManagement -> the shared model record.
bool Mtlf::train(const std::string& event, const char* why) {
    if (!store_.acquire_training_lease(event,
                                       std::chrono::seconds(options_.training_lease_seconds))) {
        return false; // another replica is training this event
    }
    struct Release {
        MlStore& s;
        const std::string& e;
        ~Release() { s.release_training_lease(e); }
    } release{store_, event};

    std::int64_t windows = 0;
    TrainingJob job;
    job.event = event;
    job.dataset = training_dataset(event, windows);
    job.min_samples = options_.min_samples;
    job.accuracy_tolerance = options_.accuracy_tolerance;
    spdlog::info("nwdaf-mtlf: training {} ({}; {} usable windows in the ADRF data set)",
                 event,
                 why,
                 windows);
    auto result = executor_.train(job);
    if (!result) {
        spdlog::error("nwdaf-mtlf: training {} failed: {}", event, result.error());
        return false;
    }

    // Consumers allowed to retrieve the model: every current subscriber to this event.
    json allow = json::array();
    std::vector<std::string> consumers;
    for (const auto& [id, sub] : store_.all_provision_subscriptions()) {
        const auto c = sub.value("consumer", "");
        if (!c.empty() && std::find(consumers.begin(), consumers.end(), c) == consumers.end()) {
            consumers.push_back(c);
            allow.push_back(json{{"nfInstanceId", c}});
        }
    }
    const auto model_unique_id = store_.next_number();
    json model_entry{{"modelUniqueId", model_unique_id},
                     {"mlModel", base64_encode(result->onnx_bytes)}};
    if (!allow.empty()) {
        model_entry["allowConsumerList"] = allow;
    }
    const auto base = adrf_base();
    if (!base) {
        spdlog::error("nwdaf-mtlf: no ADRF configured to store the trained model (adrf_base_url)");
        return false;
    }
    const json store_request{{"nfInstanceId", options_.instance_id},
                             {"mlModels", json::array({model_entry})}};
    const auto stored = call(oauth_adrf_ml_,
                             "POST",
                             *base + kAdrfMlModelManagementRoot + "/mlmodel-store-records",
                             &store_request);
    if (stored.status != 201) {
        spdlog::error("nwdaf-mtlf: Nadrf_MLModelManagement storage of model {} failed ({} {})",
                      model_unique_id,
                      stored.status,
                      stored.error.empty() ? stored.body : stored.error);
        return false;
    }
    std::string file_url;
    std::string store_trans_id;
    try {
        const auto body = json::parse(stored.body);
        file_url = body.at("mlModelInfo").front().at("mlFileAddr").at("mLModelUrl");
        const auto slash = stored.location.rfind('/');
        store_trans_id =
            slash == std::string::npos ? stored.location : stored.location.substr(slash + 1);
    } catch (const std::exception& e) {
        spdlog::error("nwdaf-mtlf: ADRF storage response unreadable: {}", e.what());
        return false;
    }
    const auto& rep = result->report;
    json record{{"event", event},
                {"modelUniqueId", model_unique_id},
                {"version", store_.next_number()},
                {"storeTransId", store_trans_id},
                {"adrfId", adrf_instance_id().value_or("")},
                {"fileUrl", file_url},
                {"ownerInstanceId", options_.instance_id},
                {"allowedConsumers", consumers},
                {"accuracyPct", rep.value("accuracy_pct", 0)},
                {"accuracyTolerance", rep.value("accuracy_tolerance", 0.0)},
                {"nExamples", rep.value("n_examples", 0)},
                {"nRealWindows", rep.value("n_real_windows", 0)},
                {"nInstances", rep.value("n_instances", 0)},
                {"dataSource", rep.value("data_source", "")},
                {"dataSetId", options_.data_set_id},
                {"mlflowRunId", rep.value("mlflow_run_id", "")},
                {"modelType", rep.value("model_type", "")},
                {"testMaeLoad", rep.value("test_mae_load", 0.0)},
                {"trainedAt", rep.value("trained_at", "")},
                {"retrained", store_.get_model(event).has_value()}};
    store_.put_model(event, record);
    spdlog::info("nwdaf-mtlf: model {} for {} stored at the ADRF ({}; accuracy {}%, {} examples, "
                 "data_source={}, MLflow run {})",
                 model_unique_id,
                 event,
                 store_trans_id,
                 rep.value("accuracy_pct", 0),
                 rep.value("n_examples", 0),
                 rep.value("data_source", ""),
                 rep.value("mlflow_run_id", ""));
    return true;
}

// A consumer that subscribed after the model was stored must be allowed to retrieve it: the
// owning replica re-stores the record with the wider allowConsumerList (Nadrf_MLModelManagement
// update, TS 29.575 4.3.2.2); a non-owning replica (a restarted MTLF has a new nfInstanceId)
// re-trains instead, which yields a record it owns.
bool Mtlf::reconcile_consumers(const std::string& event, const json& model) {
    std::vector<std::string> allowed = model.value("allowedConsumers", std::vector<std::string>());
    std::vector<std::string> missing;
    for (const auto& [id, sub] : store_.all_provision_subscriptions()) {
        const auto c = sub.value("consumer", "");
        if (c.empty()) {
            continue;
        }
        bool wants = false;
        for (const auto& es : sub.at("request").value("mLEventSubscs", json::array())) {
            wants |= es.value("mLEvent", "") == event;
        }
        if (wants && std::find(allowed.begin(), allowed.end(), c) == allowed.end() &&
            std::find(missing.begin(), missing.end(), c) == missing.end()) {
            missing.push_back(c);
        }
    }
    if (missing.empty()) {
        return true;
    }
    if (model.value("ownerInstanceId", "") != options_.instance_id) {
        return train(event, "a new consumer subscribed and this replica does not own the record");
    }
    const auto base = adrf_base();
    if (!base) {
        return false;
    }
    const auto bytes = call(oauth_adrf_ml_, "GET", model.value("fileUrl", ""), nullptr);
    if (bytes.status != 200) {
        spdlog::warn("nwdaf-mtlf: cannot re-read model {} from the ADRF ({})",
                     model.value("modelUniqueId", 0),
                     bytes.status);
        return false;
    }
    for (const auto& c : missing) {
        allowed.push_back(c);
    }
    json allow = json::array();
    for (const auto& c : allowed) {
        allow.push_back(json{{"nfInstanceId", c}});
    }
    const json update{{"nfInstanceId", options_.instance_id},
                      {"mlModels",
                       json::array({json{{"modelUniqueId", model.value("modelUniqueId", 0)},
                                         {"mlModel", base64_encode(bytes.body)},
                                         {"allowConsumerList", allow}}})}};
    const auto r = call(oauth_adrf_ml_,
                        "PUT",
                        *base + kAdrfMlModelManagementRoot + "/mlmodel-store-records/" +
                            model.value("storeTransId", ""),
                        &update);
    if (r.status != 200 && r.status != 204) {
        spdlog::warn("nwdaf-mtlf: widening the allow list of model {} failed ({})",
                     model.value("modelUniqueId", 0),
                     r.status);
        return false;
    }
    json updated = model;
    updated["allowedConsumers"] = allowed;
    store_.put_model(event, updated);
    return true;
}

// MLEventNotif (TS 29.520 4.5.2.4.2): the ADRF (Set) information and the file address, the
// unique id, the provider, and the lineage as AdditionalMLModelInformation.
json Mtlf::event_notif(const std::string& event,
                       const json& model,
                       const json& sub,
                       bool update) const {
    json addr{{"mLModelUrl", model.value("fileUrl", "")}};
    json adrf{{"adrfId", model.value("adrfId", "")},
              {"storTransId", model.value("storeTransId", "")}};
    json stats{{"data_source", model.value("dataSource", "")},
               {"n_examples", model.value("nExamples", 0)},
               {"n_real_windows", model.value("nRealWindows", 0)},
               {"n_instances", model.value("nInstances", 0)},
               {"data_set_id", model.value("dataSetId", "")},
               {"mlflow_run_id", model.value("mlflowRunId", "")},
               {"model_type", model.value("modelType", "")},
               {"accuracy_tolerance", model.value("accuracyTolerance", 0.0)},
               {"test_mae_load", model.value("testMaeLoad", 0.0)},
               {"trained_at", model.value("trainedAt", "")}};
    json add{{"modelUniqueId", model.value("modelUniqueId", 0)},
             {"mLFileAddr", addr},
             {"mLModelAdrf", adrf},
             {"modelMetric", sbi_gen::MLModelMetric::ACCURACY},
             {"accMLModel", model.value("accuracyPct", 0)},
             {"modelProviderId", options_.instance_id},
             {"modelUpdateInd", update},
             // TrainInputDataInfo: the input this model was trained on -- the NRF's
             // NotificationEventType stream (DccfEvent.nrfEvent, TS 29.574) -- one entry per
             // NRF event kind the data set carries, and the sidecar's statistics as the
             // free-form dataStatisticsInfos string.
             {"trainInpInfos",
              json::array(
                  {json{{"dataInfo", json{{"inpEvent", json{{"nrfEvent", "NF_REGISTERED"}}}}}},
                   json{{"dataInfo", json{{"inpEvent", json{{"nrfEvent", "NF_PROFILE_CHANGED"}}}}}},
                   json{{"dataInfo", json{{"inpEvent", json{{"nrfEvent", "NF_DEREGISTERED"}}}}},
                        {"dataStatisticsInfos", stats.dump()}}})}};
    json n{{"event", event},
           {"mLFileAddr", addr},
           {"mLModelAdrf", adrf},
           {"modelUniqueId", model.value("modelUniqueId", 0)},
           {"modelProviderId", options_.instance_id},
           {"modelUpdateInd", update},
           {"addModelInfo", json::array({add})}};
    if (sub.at("request").contains("notifCorreId")) {
        n["notifCorreId"] = sub.at("request").at("notifCorreId");
    }
    return n;
}

// Nnwdaf_MLModelProvision_Notify: everything this subscription has not yet been told.
void Mtlf::deliver(const std::string& sub_id, json sub) {
    json notifs = json::array();
    json delivered = sub.value("delivered", json::object());
    for (const auto& es : sub.at("request").value("mLEventSubscs", json::array())) {
        const auto event = es.value("mLEvent", "");
        const auto model = store_.get_model(event);
        if (!model) {
            continue;
        }
        const auto version = model->value("version", std::int64_t(0));
        const auto seen = delivered.value(event, std::int64_t(0));
        if (seen >= version) {
            continue;
        }
        notifs.push_back(event_notif(event, *model, sub, seen != 0));
        delivered[event] = version;
    }
    if (notifs.empty()) {
        return;
    }
    // The callback body is array(NwdafMLModelProvNotif) per the YAML.
    const json body = json::array({json{{"subscriptionId", sub_id}, {"eventNotifs", notifs}}});
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = sub.at("request").at("notifUri").get<std::string>();
    req.headers.emplace("content-type", "application/json");
    req.headers.emplace(sbi_core::headers::kCallback, kProvisionCallback);
    req.body = body.dump();
    long status = -1;
    {
        const std::lock_guard<std::mutex> lock(client_mutex_);
        if (auto resp = client_.send(req); resp) {
            status = resp->status;
        }
    }
    if (status >= 200 && status < 300) {
        sub["delivered"] = delivered;
        store_.replace_provision_subscription(sub_id, sub); // XX: gone means unsubscribed meanwhile
    } else {
        spdlog::warn("nwdaf-mtlf: Nnwdaf_MLModelProvision_Notify for {} to {} failed ({})",
                     sub_id,
                     req.url,
                     status);
    }
}

void Mtlf::run(std::atomic<bool>& running, const std::function<bool(std::int64_t)>& pause) {
    while (running) {
        try {
            for (const auto& event : options_.events) {
                ensure_storage_subscription(event);
                auto model = store_.get_model(event);
                bool subscribed = false;
                for (const auto& [id, sub] : store_.all_provision_subscriptions()) {
                    for (const auto& es : sub.at("request").value("mLEventSubscs", json::array())) {
                        subscribed |= es.value("mLEvent", "") == event;
                    }
                }
                if (!model) {
                    if (subscribed) {
                        train(event, "first subscription, no model yet");
                    }
                } else {
                    // Re-train when the data set grew enough, or on the time interval.
                    std::int64_t windows = 0;
                    const auto now = std::chrono::system_clock::now();
                    const auto trained_at = sbi_core::parse_rfc3339(model->value("trainedAt", ""));
                    const bool due_by_time =
                        options_.retrain_interval_seconds > 0 && trained_at &&
                        now - *trained_at > std::chrono::seconds(options_.retrain_interval_seconds);
                    bool due_by_data = false;
                    if (options_.retrain_min_new_windows > 0) {
                        training_dataset(event, windows); // counts, no Python
                        due_by_data = windows - model->value("nRealWindows", std::int64_t(0)) >=
                                      options_.retrain_min_new_windows;
                    }
                    if (due_by_data) {
                        train(event, "the ADRF data set grew");
                    } else if (due_by_time) {
                        train(event, "retrain interval elapsed");
                    }
                }
                // Before anyone is told about a model, every current subscriber must be able
                // to retrieve it.
                if (const auto current = store_.get_model(event)) {
                    reconcile_consumers(event, *current);
                }
            }
            // Lease slightly shorter than the tick so the next tick can claim afresh; the
            // `delivered` version keeps a re-delivery idempotent, the lease keeps N replicas
            // from all POSTing the same model in the same tick.
            const auto lease =
                std::chrono::milliseconds(options_.check_interval_seconds * 1000 * 9 / 10);
            for (auto& [id, sub] : store_.all_provision_subscriptions()) {
                if (store_.claim_delivery(id, lease)) {
                    deliver(id, sub);
                }
            }
        } catch (const std::exception& e) {
            spdlog::warn("nwdaf-mtlf: loop iteration failed: {}", e.what());
        }
        if (!pause(options_.check_interval_seconds)) {
            break;
        }
    }
}

void Mtlf::install_routes(sbi_core::http2::Server& server) {
    // Validation shared by create and update: the YAML's shape, then the events this MTLF can
    // provide models for. Events it cannot go to failEventReports (4.5.2.2.2); none accepted is
    // the spec's 500 UNAVAILABLE_ML_MODEL_FOR_ALLEVENTS.
    const auto validate = [this](const sbi_core::http2::Request& req,
                                 json& record,
                                 std::optional<sbi_core::http2::Response>& err) {
        auto auth = check_bearer(req, verifier_);
        if (auth && !auth->valid) {
            err = sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            return;
        }
        sbi_core::http2::Response bad;
        auto dto = sbi_core::http2::parse_json_body<sbi_gen::NwdafMLModelProvSubsc>(req, bad);
        if (!dto) {
            err = bad;
            return;
        }
        json request = json::parse(req.body);
        json failures = json::array();
        int accepted = 0;
        for (const auto& es : dto->mLEventSubscs) {
            if (es.posModelReqInd.value_or(false)) {
                failures.push_back(
                    json{{"event", es.mLEvent ? json(es.mLEvent->value) : json()},
                         {"failureCode",
                          sbi_gen::FailureCode_Nnwdaf_MLModelProvision::UNAVAILABLE_ML_MODEL}});
                continue; // Ue_Positioning models: not trained here
            }
            if (!es.mLEvent) {
                err = problem(400,
                              "Bad Request",
                              "MLEventSubscription requires mLEvent and mLEventFilter, or "
                              "posModelReqInd",
                              "MANDATORY_IE_MISSING");
                return;
            }
            if (trains(es.mLEvent->value)) {
                ++accepted;
            } else {
                failures.push_back(
                    json{{"event", es.mLEvent->value},
                         {"failureCode",
                          sbi_gen::FailureCode_Nnwdaf_MLModelProvision::UNAVAILABLE_ML_MODEL}});
            }
        }
        if (accepted == 0) {
            err = problem(500,
                          "Internal Server Error",
                          "no ML model is available for any of the requested events; this MTLF "
                          "trains " +
                              json(options_.events).dump(),
                          "UNAVAILABLE_ML_MODEL_FOR_ALLEVENTS");
            return;
        }
        request.erase("failEventReports");
        request.erase("mLEventNotifs");
        if (!failures.empty()) {
            request["failEventReports"] = failures;
        }
        record = json{{"request", request},
                      {"consumer", (auth && auth->valid) ? auth->subject : std::string()},
                      {"delivered", json::object()}};
    };

    // The representation returned: the request, with immediate reports when asked and
    // available (immRep, 4.5.2.2.2).
    const auto representation = [this](const std::string& id, json& record) {
        json body = record.at("request");
        const bool imm = body.value("eventReq", json::object()).value("immRep", false);
        if (imm) {
            json notifs = json::array();
            json delivered = record.value("delivered", json::object());
            for (const auto& es : body.value("mLEventSubscs", json::array())) {
                const auto event = es.value("mLEvent", "");
                if (const auto model = store_.get_model(event)) {
                    notifs.push_back(event_notif(event, *model, record, false));
                    delivered[event] = model->value("version", std::int64_t(0));
                }
            }
            if (!notifs.empty()) {
                body["mLEventNotifs"] = notifs;
                record["delivered"] = delivered;
                store_.replace_provision_subscription(id, record);
            }
        }
        return body;
    };

    server.add_route("POST",
                     std::string(kProvisionRoot) + "/subscriptions",
                     [=, this](const sbi_core::http2::Request& req) {
                         json record;
                         std::optional<sbi_core::http2::Response> err;
                         validate(req, record, err);
                         if (err) {
                             return *err;
                         }
                         const auto id = store_.create_provision_subscription(record);
                         auto resp = json_response(201, representation(id, record));
                         resp.headers.emplace("location",
                                              std::string(kProvisionRoot) + "/subscriptions/" + id);
                         return resp;
                     });

    server.add_route("PUT",
                     std::string(kProvisionRoot) + "/subscriptions/{subscriptionId}",
                     [=, this](const sbi_core::http2::Request& req) {
                         const auto id = req.path_params.at("subscriptionId");
                         auto existing = store_.get_provision_subscription(id);
                         if (!existing) {
                             return sbi_core::http2::problem_response(
                                 404, "Not Found", "no ML model provision subscription " + id);
                         }
                         json record;
                         std::optional<sbi_core::http2::Response> err;
                         validate(req, record, err);
                         if (err) {
                             return *err;
                         }
                         // A modification keeps what was already delivered: a re-trained model is
                         // notified as an update, not as a first provision.
                         record["delivered"] = existing->value("delivered", json::object());
                         if (!store_.replace_provision_subscription(id, record)) {
                             return sbi_core::http2::problem_response(
                                 404, "Not Found", "no ML model provision subscription " + id);
                         }
                         return json_response(200, representation(id, record));
                     });

    server.add_route("DELETE",
                     std::string(kProvisionRoot) + "/subscriptions/{subscriptionId}",
                     [this](const sbi_core::http2::Request& req) {
                         if (auto auth = check_bearer(req, verifier_); auth && !auth->valid) {
                             return sbi_core::http2::problem_response(
                                 401, "Unauthorized", auth->error);
                         }
                         const auto id = req.path_params.at("subscriptionId");
                         if (!store_.remove_provision_subscription(id)) {
                             return sbi_core::http2::problem_response(
                                 404, "Not Found", "no ML model provision subscription " + id);
                         }
                         sbi_core::http2::Response resp;
                         resp.status = 204;
                         return resp;
                     });
}

} // namespace nwdaf
