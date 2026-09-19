#include "event_subscription_store.hpp"

namespace smf {

std::string EventSubscriptionStore::create(nlohmann::json subscription) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string sub_id = "evtsub-" + std::to_string(next_id_++);
    subscriptions_.emplace(sub_id, std::move(subscription));
    return sub_id;
}

std::optional<nlohmann::json> EventSubscriptionStore::get(const std::string& sub_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(sub_id);
    if (it == subscriptions_.end()) {
        return std::nullopt;
    }
    return std::make_optional(it->second);
}

bool EventSubscriptionStore::update(const std::string& sub_id, nlohmann::json subscription) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(sub_id);
    if (it == subscriptions_.end()) {
        return false;
    }
    it->second = std::move(subscription);
    return true;
}

bool EventSubscriptionStore::remove(const std::string& sub_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return subscriptions_.erase(sub_id) > 0;
}

std::vector<nlohmann::json> EventSubscriptionStore::snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<nlohmann::json> out;
    out.reserve(subscriptions_.size());
    for (const auto& [id, sub] : subscriptions_) {
        out.push_back(sub);
    }
    return out;
}

} // namespace smf
