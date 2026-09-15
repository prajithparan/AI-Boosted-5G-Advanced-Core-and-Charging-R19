#include "ml_consumer.hpp"

#include "sbi_core/datetime.hpp"
#include "sbi_core/sbi_headers.hpp"

#include <spdlog/spdlog.h>

#include <chrono>

namespace nwdaf {

using json = nlohmann::json;

namespace {
constexpr const char* kProvisionRoot = "/nnwdaf-mlmodelprovision/v1";
constexpr const char* kHolderPrefix = "nwdaf:anlf:mlsub:";
} // namespace

MlConsumer::MlConsumer(MlConsumerOptions options,
                       sbi_core::http2::Client& client,
                       std::mutex& client_mutex,
                       sbi_core::OAuth2Client& oauth_mtlf,
                       sbi_core::OAuth2Client& oauth_adrf_ml,
                       MlStore& store)
    : options_(std::move(options)), client_(client), client_mutex_(client_mutex),
      oauth_mtlf_(oauth_mtlf), oauth_adrf_ml_(oauth_adrf_ml), store_(store) {}

// Nnwdaf_MLModelProvision_Subscribe (TS 29.520 4.5.2.2.2): the event with its filter, the
// reporting condition naming the accuracy metric and threshold (6.2A.2 "ML Model Monitoring
// Information"), and where to notify.
bool MlConsumer::subscribe(const std::string& event, std::string& resource_uri) {
    json es{{"mLEvent", event}, {"mLEventFilter", json::object()}};
    if (!options_.nf_types.empty()) {
        es["mLEventFilter"]["nfTypes"] = options_.nf_types;
    }
    if (options_.accuracy_threshold > 0) {
        es["mlEvRepCon"] =
            json{{"modelMetric", "ACCURACY"}, {"mlAccuracyThreshold", options_.accuracy_threshold}};
    }
    const json body{{"mLEventSubscs", json::array({es})},
                    {"notifUri", options_.notif_uri},
                    {"notifCorreId", "anlf-" + event},
                    {"eventReq", json{{"immRep", true}}}};
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = options_.mtlf_base_url + kProvisionRoot + "/subscriptions";
    req.headers.emplace("content-type", "application/json");
    req.body = body.dump();
    const std::lock_guard<std::mutex> lock(client_mutex_);
    auto token = oauth_mtlf_.get_bearer_token();
    if (!token) {
        spdlog::warn("nwdaf: no token for the MTLF: {}", token.error());
        return false;
    }
    req.headers.emplace("authorization", "Bearer " + *token);
    auto resp = client_.send(req);
    if (!resp || resp->status != 201) {
        // An absent MTLF is a legitimate lab state (statistics only); say so once, then
        // every twelfth retry.
        if (subscribe_failures_++ % 12 == 0) {
            spdlog::warn("nwdaf: Nnwdaf_MLModelProvision_Subscribe for {} at {} failed ({}) -- "
                         "NF_LOAD stays statistical until an MTLF answers",
                         event,
                         options_.mtlf_base_url,
                         resp ? std::to_string(resp->status) : resp.error());
        }
        return false;
    }
    subscribe_failures_ = 0;
    const auto it = resp->headers.find("location");
    resource_uri = it != resp->headers.end() ? it->second : "";
    if (!resource_uri.empty() && resource_uri.front() == '/') {
        resource_uri = options_.mtlf_base_url + resource_uri;
    }
    // immRep: a model may already be in the response.
    try {
        const auto rep = json::parse(resp->body);
        if (rep.contains("mLEventNotifs")) {
            on_notification(json::array({json{{"eventNotifs", rep.at("mLEventNotifs")}}}));
        }
    } catch (const std::exception&) {
    }
    spdlog::info("nwdaf: subscribed to ML models for {} at the MTLF ({})", event, resource_uri);
    return true;
}

void MlConsumer::unsubscribe(const std::string& resource_uri) {
    if (resource_uri.empty()) {
        return;
    }
    sbi_core::http2::ClientRequest req;
    req.method = "DELETE";
    req.url = resource_uri;
    const std::lock_guard<std::mutex> lock(client_mutex_);
    if (auto token = oauth_mtlf_.get_bearer_token()) {
        req.headers.emplace("authorization", "Bearer " + *token);
        static_cast<void>(client_.send(req));
    }
}

void MlConsumer::run(std::atomic<bool>& running, const std::function<bool(std::int64_t)>& pause) {
    if (!enabled()) {
        return;
    }
    const auto ttl = std::chrono::milliseconds(options_.holder_heartbeat_seconds * 3000);
    std::map<std::string, std::string> held; // event -> resource URI at the MTLF
    while (running) {
        for (const auto& event : options_.events) {
            const std::string key = kHolderPrefix + event;
            try {
                if (held.contains(event)) {
                    store_.touch_holder(key, ttl);
                    continue;
                }
                const auto holder = store_.get_holder(key);
                if (holder && store_.holder_alive(key)) {
                    continue; // another replica holds the subscription
                }
                if (holder) {
                    spdlog::warn("nwdaf: ML-model subscription holder for {} lapsed; taking it "
                                 "over",
                                 event);
                    unsubscribe(holder->value("resourceUri", ""));
                    store_.close_holder(key);
                }
                std::string resource_uri;
                if (!store_.open_holder(key, json{{"holder", options_.instance_id}})) {
                    continue; // lost the race
                }
                if (!subscribe(event, resource_uri)) {
                    store_.close_holder(key);
                    continue;
                }
                store_.close_holder(key);
                store_.open_holder(
                    key, json{{"holder", options_.instance_id}, {"resourceUri", resource_uri}});
                store_.touch_holder(key, ttl);
                held[event] = resource_uri;
            } catch (const std::exception& e) {
                spdlog::warn("nwdaf: ML-model subscription loop for {}: {}", event, e.what());
            }
        }
        const auto wait = held.size() == options_.events.size() ? options_.holder_heartbeat_seconds
                                                                : options_.retry_seconds;
        if (!pause(wait)) {
            break;
        }
    }
    for (const auto& [event, uri] : held) {
        try {
            unsubscribe(uri);
            store_.close_holder(kHolderPrefix + event);
        } catch (const std::exception&) {
        }
    }
}

int MlConsumer::on_notification(const json& body) {
    int taken = 0;
    for (const auto& n : body.is_array() ? body : json::array({body})) {
        for (const auto& en : n.value("eventNotifs", json::array())) {
            const auto event = en.value("event", "");
            if (event.empty() || !en.contains("modelUniqueId")) {
                continue;
            }
            json model{{"event", event},
                       {"modelUniqueId", en.at("modelUniqueId")},
                       {"modelProviderId", en.value("modelProviderId", "")},
                       {"modelUpdateInd", en.value("modelUpdateInd", false)},
                       {"receivedAt", sbi_core::format_rfc3339(std::chrono::system_clock::now())}};
            if (en.contains("mLFileAddr")) {
                model["fileUrl"] = en.at("mLFileAddr").value("mLModelUrl", "");
            }
            if (en.contains("mLModelAdrf")) {
                model["adrfId"] = en.at("mLModelAdrf").value("adrfId", "");
                model["storeTransId"] = en.at("mLModelAdrf").value("storTransId", "");
            }
            for (const auto& add : en.value("addModelInfo", json::array())) {
                if (add.value("modelUniqueId", std::int64_t(-1)) == en.at("modelUniqueId")) {
                    if (add.contains("accMLModel")) {
                        model["accuracyPct"] = add.at("accMLModel");
                    }
                    if (add.contains("modelMetric")) {
                        model["modelMetric"] = add.at("modelMetric");
                    }
                    if (add.contains("trainInpInfos")) {
                        model["trainInpInfos"] = add.at("trainInpInfos");
                    }
                }
            }
            if (model.value("fileUrl", "").empty()) {
                spdlog::warn("nwdaf: ML model {} for {} notified without a file address; not "
                             "taken into use (retrieval by storTransId alone is not built)",
                             en.at("modelUniqueId").dump(),
                             event);
                continue;
            }
            store_.put_active_model(event, model);
            spdlog::info("nwdaf: ML model {} for {} {} (accuracy {}%)",
                         en.at("modelUniqueId").dump(),
                         event,
                         model.value("modelUpdateInd", false) ? "updated" : "provisioned",
                         model.value("accuracyPct", 0));
            ++taken;
        }
    }
    return taken;
}

std::optional<json> MlConsumer::active_model(const std::string& event) {
    try {
        return store_.get_active_model(event);
    } catch (const std::exception& e) {
        spdlog::warn("nwdaf: active model for {} unreadable: {}", event, e.what());
        return std::nullopt;
    }
}

// Nadrf_MLModelManagement_RetrievalRequest (TS 29.575 4.3.2.3): the file at mLModelUrl, with
// this AnLF's token -- the MTLF listed it in allowConsumerList.
bool MlConsumer::ensure_loaded(const std::string& event, const json& model) {
    const auto id = model.value("modelUniqueId", std::int64_t(0));
    auto& rt = runtimes_[event];
    if (!rt) {
        rt = std::make_unique<ModelRuntime>();
    }
    if (rt->loaded() && rt->model_unique_id() == id) {
        return true;
    }
    sbi_core::http2::ClientRequest req;
    req.method = "GET";
    req.url = model.value("fileUrl", "");
    std::string bytes;
    {
        const std::lock_guard<std::mutex> lock(client_mutex_);
        auto token = oauth_adrf_ml_.get_bearer_token();
        if (!token) {
            spdlog::warn("nwdaf: no token for the ADRF: {}", token.error());
            return false;
        }
        req.headers.emplace("authorization", "Bearer " + *token);
        auto resp = client_.send(req);
        if (!resp || resp->status != 200) {
            spdlog::warn("nwdaf: retrieving ML model {} from {} failed ({})",
                         id,
                         req.url,
                         resp ? std::to_string(resp->status) : resp.error());
            return false;
        }
        bytes = std::move(resp->body);
    }
    return rt->load(bytes, id);
}

std::optional<double> MlConsumer::predict(const std::string& event,
                                          const NfLoadFeatures& features) {
    const auto model = active_model(event);
    if (!model) {
        return std::nullopt;
    }
    const std::lock_guard<std::mutex> lock(runtimes_mutex_);
    if (!ensure_loaded(event, *model)) {
        return std::nullopt;
    }
    return runtimes_[event]->predict(features);
}

} // namespace nwdaf
