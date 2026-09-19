#include "vfl_subscription_store.hpp"

namespace nwdaf {

std::string VflSubscriptionStore::create(nlohmann::json subscription) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string sub_id = prefix_ + std::to_string(next_id_++);
    subscriptions_.emplace(sub_id, std::move(subscription));
    return sub_id;
}

std::optional<nlohmann::json> VflSubscriptionStore::get(const std::string& sub_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(sub_id);
    if (it == subscriptions_.end()) {
        return std::nullopt;
    }
    return std::make_optional(it->second);
}

bool VflSubscriptionStore::replace(const std::string& sub_id, nlohmann::json subscription) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(sub_id);
    if (it == subscriptions_.end()) {
        return false;
    }
    it->second = std::move(subscription);
    return true;
}

std::optional<nlohmann::json> VflSubscriptionStore::merge_patch(const std::string& sub_id,
                                                               const nlohmann::json& patch) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(sub_id);
    if (it == subscriptions_.end()) {
        return std::nullopt;
    }
    it->second.merge_patch(patch);
    return std::make_optional(it->second);
}

bool VflSubscriptionStore::remove(const std::string& sub_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return subscriptions_.erase(sub_id) > 0;
}

} // namespace nwdaf
