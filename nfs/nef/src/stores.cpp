#include "stores.hpp"

namespace nef {

void PfdCatalogStore::seed(const std::string& application_id, nlohmann::json pfd_data_for_app) {
    std::lock_guard<std::mutex> lock(mutex_);
    catalog_[application_id] = std::move(pfd_data_for_app);
}

std::optional<nlohmann::json> PfdCatalogStore::get(const std::string& application_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = catalog_.find(application_id);
    if (it == catalog_.end()) {
        return std::nullopt;
    }
    return std::make_optional(it->second);
}

std::vector<nlohmann::json>
PfdCatalogStore::get_many(const std::vector<std::string>& application_ids) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<nlohmann::json> result;
    for (const auto& id : application_ids) {
        auto it = catalog_.find(id);
        if (it != catalog_.end()) {
            result.push_back(it->second);
        }
    }
    return result;
}

std::optional<nlohmann::json>
PfdCatalogStore::get_if_changed_since(const std::string& application_id,
                                      const std::optional<std::string>& since) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = catalog_.find(application_id);
    if (it == catalog_.end()) {
        return std::nullopt;
    }
    if (!since.has_value()) {
        return std::make_optional(it->second);
    }
    const std::string stored_timestamp = it->second.value("pfdTimestamp", "");
    if (stored_timestamp > *since) {
        return std::make_optional(it->second);
    }
    return std::nullopt;
}

std::string PfdSubscriptionStore::create(nlohmann::json subscription) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string id = "pfdsub-" + std::to_string(next_id_++);
    subscriptions_.emplace(id, std::move(subscription));
    return id;
}

std::optional<nlohmann::json> PfdSubscriptionStore::get(const std::string& sub_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(sub_id);
    if (it == subscriptions_.end()) {
        return std::nullopt;
    }
    return std::make_optional(it->second);
}

bool PfdSubscriptionStore::put(const std::string& sub_id, nlohmann::json subscription) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(sub_id);
    if (it == subscriptions_.end()) {
        return false;
    }
    it->second = std::move(subscription);
    return true;
}

bool PfdSubscriptionStore::remove(const std::string& sub_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return subscriptions_.erase(sub_id) > 0;
}

std::string DnaiMapSubStore::create(nlohmann::json subscription) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string id = "dnaimapsub-" + std::to_string(next_id_++);
    subscriptions_.emplace(id, std::move(subscription));
    return id;
}

std::optional<nlohmann::json> DnaiMapSubStore::get(const std::string& sub_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(sub_id);
    if (it == subscriptions_.end()) {
        return std::nullopt;
    }
    return std::make_optional(it->second);
}

bool DnaiMapSubStore::remove(const std::string& sub_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return subscriptions_.erase(sub_id) > 0;
}

std::string EasDeploySubStore::create(nlohmann::json subscription) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string id = "easdeploysub-" + std::to_string(next_id_++);
    subscriptions_.emplace(id, std::move(subscription));
    return id;
}

std::optional<nlohmann::json> EasDeploySubStore::get(const std::string& sub_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(sub_id);
    if (it == subscriptions_.end()) {
        return std::nullopt;
    }
    return std::make_optional(it->second);
}

bool EasDeploySubStore::remove(const std::string& sub_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return subscriptions_.erase(sub_id) > 0;
}

std::string SmContextStore::create(nlohmann::json created_data) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string id = "smctx-" + std::to_string(next_id_++);
    contexts_.emplace(id, std::move(created_data));
    return id;
}

std::optional<nlohmann::json> SmContextStore::get(const std::string& sm_context_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = contexts_.find(sm_context_id);
    if (it == contexts_.end()) {
        return std::nullopt;
    }
    return std::make_optional(it->second);
}

bool SmContextStore::update(const std::string& sm_context_id,
                            const nlohmann::json& partial_update) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = contexts_.find(sm_context_id);
    if (it == contexts_.end()) {
        return false;
    }
    for (const auto& [key, value] : partial_update.items()) {
        it->second[key] = value;
    }
    return true;
}

bool SmContextStore::remove(const std::string& sm_context_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return contexts_.erase(sm_context_id) > 0;
}

std::string EcsAddrCfgInfoSubStore::create(nlohmann::json subscription) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string id = "ecsaddrsub-" + std::to_string(next_id_++);
    subscriptions_.emplace(id, std::move(subscription));
    return id;
}

std::optional<nlohmann::json> EcsAddrCfgInfoSubStore::get(const std::string& sub_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(sub_id);
    if (it == subscriptions_.end()) {
        return std::nullopt;
    }
    return std::make_optional(it->second);
}

bool EcsAddrCfgInfoSubStore::put(const std::string& sub_id, nlohmann::json subscription) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(sub_id);
    if (it == subscriptions_.end()) {
        return false;
    }
    it->second = std::move(subscription);
    return true;
}

bool EcsAddrCfgInfoSubStore::patch(const std::string& sub_id, const nlohmann::json& merge_patch) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(sub_id);
    if (it == subscriptions_.end()) {
        return false;
    }
    it->second.merge_patch(merge_patch);
    return true;
}

bool EcsAddrCfgInfoSubStore::remove(const std::string& sub_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return subscriptions_.erase(sub_id) > 0;
}

std::string NefEventExposureSubStore::create(nlohmann::json subscription) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string id = "evtexpsub-" + std::to_string(next_id_++);
    subscriptions_.emplace(id, std::move(subscription));
    return id;
}

std::optional<nlohmann::json> NefEventExposureSubStore::get(const std::string& sub_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(sub_id);
    if (it == subscriptions_.end()) {
        return std::nullopt;
    }
    return std::make_optional(it->second);
}

bool NefEventExposureSubStore::put(const std::string& sub_id, nlohmann::json subscription) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(sub_id);
    if (it == subscriptions_.end()) {
        return false;
    }
    it->second = std::move(subscription);
    return true;
}

bool NefEventExposureSubStore::remove(const std::string& sub_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return subscriptions_.erase(sub_id) > 0;
}

std::string TrafficInfluDataSubStore::create(nlohmann::json subscription) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string id = "trafficinfsub-" + std::to_string(next_id_++);
    subscriptions_.emplace(id, std::move(subscription));
    return id;
}

std::optional<nlohmann::json> TrafficInfluDataSubStore::get(const std::string& sub_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(sub_id);
    if (it == subscriptions_.end()) {
        return std::nullopt;
    }
    return std::make_optional(it->second);
}

bool TrafficInfluDataSubStore::put(const std::string& sub_id, nlohmann::json subscription) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(sub_id);
    if (it == subscriptions_.end()) {
        return false;
    }
    it->second = std::move(subscription);
    return true;
}

bool TrafficInfluDataSubStore::remove(const std::string& sub_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return subscriptions_.erase(sub_id) > 0;
}

std::string InferEventSubStore::create(nlohmann::json subscription) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string id = "infersub-" + std::to_string(next_id_++);
    subscriptions_.emplace(id, std::move(subscription));
    return id;
}

std::optional<nlohmann::json> InferEventSubStore::get(const std::string& sub_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(sub_id);
    if (it == subscriptions_.end()) {
        return std::nullopt;
    }
    return std::make_optional(it->second);
}

bool InferEventSubStore::put(const std::string& sub_id, nlohmann::json subscription) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(sub_id);
    if (it == subscriptions_.end()) {
        return false;
    }
    it->second = std::move(subscription);
    return true;
}

bool InferEventSubStore::patch(const std::string& sub_id, const nlohmann::json& merge_patch) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(sub_id);
    if (it == subscriptions_.end()) {
        return false;
    }
    it->second.merge_patch(merge_patch);
    return true;
}

bool InferEventSubStore::remove(const std::string& sub_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return subscriptions_.erase(sub_id) > 0;
}

std::string TrainEventsSubStore::create(nlohmann::json subscription) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string id = "trainsub-" + std::to_string(next_id_++);
    subscriptions_.emplace(id, std::move(subscription));
    return id;
}

std::optional<nlohmann::json> TrainEventsSubStore::get(const std::string& sub_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(sub_id);
    if (it == subscriptions_.end()) {
        return std::nullopt;
    }
    return std::make_optional(it->second);
}

bool TrainEventsSubStore::put(const std::string& sub_id, nlohmann::json subscription) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(sub_id);
    if (it == subscriptions_.end()) {
        return false;
    }
    it->second = std::move(subscription);
    return true;
}

bool TrainEventsSubStore::patch(const std::string& sub_id, const nlohmann::json& merge_patch) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(sub_id);
    if (it == subscriptions_.end()) {
        return false;
    }
    it->second.merge_patch(merge_patch);
    return true;
}

bool TrainEventsSubStore::remove(const std::string& sub_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return subscriptions_.erase(sub_id) > 0;
}

std::string VflInferSubStore::create(nlohmann::json subscription) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string id = "vflinfersub-" + std::to_string(next_id_++);
    subscriptions_.emplace(id, std::move(subscription));
    return id;
}

std::optional<nlohmann::json> VflInferSubStore::get(const std::string& sub_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(sub_id);
    if (it == subscriptions_.end()) {
        return std::nullopt;
    }
    return std::make_optional(it->second);
}

bool VflInferSubStore::put(const std::string& sub_id, nlohmann::json subscription) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(sub_id);
    if (it == subscriptions_.end()) {
        return false;
    }
    it->second = std::move(subscription);
    return true;
}

bool VflInferSubStore::patch(const std::string& sub_id, const nlohmann::json& merge_patch) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(sub_id);
    if (it == subscriptions_.end()) {
        return false;
    }
    it->second.merge_patch(merge_patch);
    return true;
}

bool VflInferSubStore::remove(const std::string& sub_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return subscriptions_.erase(sub_id) > 0;
}

std::string NefVflTrainSubStore::create(nlohmann::json subscription) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string id = "vfltrainsub-" + std::to_string(next_id_++);
    subscriptions_.emplace(id, std::move(subscription));
    return id;
}

std::optional<nlohmann::json> NefVflTrainSubStore::get(const std::string& sub_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(sub_id);
    if (it == subscriptions_.end()) {
        return std::nullopt;
    }
    return std::make_optional(it->second);
}

bool NefVflTrainSubStore::put(const std::string& sub_id, nlohmann::json subscription) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(sub_id);
    if (it == subscriptions_.end()) {
        return false;
    }
    it->second = std::move(subscription);
    return true;
}

bool NefVflTrainSubStore::patch(const std::string& sub_id, const nlohmann::json& merge_patch) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(sub_id);
    if (it == subscriptions_.end()) {
        return false;
    }
    it->second.merge_patch(merge_patch);
    return true;
}

bool NefVflTrainSubStore::remove(const std::string& sub_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return subscriptions_.erase(sub_id) > 0;
}

// --- ADR-0302: AF-facing TrafficInfluence subscriptions ---

std::string AfTrafficInfluenceSubStore::create(const std::string& af_id,
                                               nlohmann::json subscription) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto sub_id = std::to_string(next_id_++);
    subscriptions_[key(af_id, sub_id)] = std::move(subscription);
    return sub_id;
}

std::optional<nlohmann::json> AfTrafficInfluenceSubStore::get(const std::string& af_id,
                                                              const std::string& sub_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(key(af_id, sub_id));
    if (it == subscriptions_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<nlohmann::json> AfTrafficInfluenceSubStore::list(const std::string& af_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto prefix = af_id + "/";
    std::vector<nlohmann::json> out;
    for (const auto& [k, v] : subscriptions_) {
        if (k.rfind(prefix, 0) == 0) {
            out.push_back(v);
        }
    }
    return out;
}

bool AfTrafficInfluenceSubStore::put(const std::string& af_id,
                                     const std::string& sub_id,
                                     nlohmann::json subscription) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(key(af_id, sub_id));
    if (it == subscriptions_.end()) {
        return false;
    }
    it->second = std::move(subscription);
    return true;
}

std::optional<nlohmann::json> AfTrafficInfluenceSubStore::merge_patch(const std::string& af_id,
                                                                      const std::string& sub_id,
                                                                      const nlohmann::json& patch) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(key(af_id, sub_id));
    if (it == subscriptions_.end()) {
        return std::nullopt;
    }
    it->second.merge_patch(patch);
    return it->second;
}

bool AfTrafficInfluenceSubStore::remove(const std::string& af_id, const std::string& sub_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return subscriptions_.erase(key(af_id, sub_id)) > 0;
}

} // namespace nef
