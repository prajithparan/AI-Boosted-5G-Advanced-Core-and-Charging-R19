#include "subscription_store.hpp"

#include <nlohmann/json.hpp>

namespace nwdaf {
namespace {

constexpr const char* kSubPrefix = "nwdaf:sub:";
constexpr const char* kSubIndex = "nwdaf:subs";
constexpr const char* kTransferPrefix = "nwdaf:transfer:";
constexpr const char* kTransferIndex = "nwdaf:transfers";
constexpr const char* kCounter = "nwdaf:next_id";

// A SET with UpdateType::EXIST is the atomic "replace only if present" the PUT routes need: two
// replicas racing a PUT against a DELETE cannot resurrect a deleted subscription.
template <typename T>
bool replace_if_present(sw::redis::Redis& redis, const std::string& key, const T& value) {
    return redis.set(key,
                     nlohmann::json(value).dump(),
                     std::chrono::milliseconds(0),
                     sw::redis::UpdateType::EXIST);
}

} // namespace

std::string SubscriptionStore::next_id(const char* prefix) {
    return std::string(prefix) + std::to_string(redis_->incr(kCounter));
}

std::string SubscriptionStore::create_subscription(const sbi_gen::NnwdafEventsSubscription& sub) {
    const auto id = next_id("nwdaf-sub-");
    redis_->set(kSubPrefix + id, nlohmann::json(sub).dump());
    redis_->sadd(kSubIndex, id);
    return id;
}

std::optional<sbi_gen::NnwdafEventsSubscription>
SubscriptionStore::get_subscription(const std::string& id) {
    const auto raw = redis_->get(kSubPrefix + id);
    if (!raw) {
        return std::nullopt;
    }
    return nlohmann::json::parse(*raw).get<sbi_gen::NnwdafEventsSubscription>();
}

bool SubscriptionStore::replace_subscription(const std::string& id,
                                             const sbi_gen::NnwdafEventsSubscription& sub) {
    return replace_if_present(*redis_, kSubPrefix + id, sub);
}

bool SubscriptionStore::remove_subscription(const std::string& id) {
    redis_->srem(kSubIndex, id);
    return redis_->del(kSubPrefix + id) > 0;
}

std::vector<std::pair<std::string, sbi_gen::NnwdafEventsSubscription>>
SubscriptionStore::all_subscriptions() {
    std::vector<std::string> ids;
    redis_->smembers(kSubIndex, std::back_inserter(ids));
    std::vector<std::pair<std::string, sbi_gen::NnwdafEventsSubscription>> out;
    out.reserve(ids.size());
    for (const auto& id : ids) {
        if (auto sub = get_subscription(id)) {
            out.emplace_back(id, std::move(*sub));
        } else {
            redis_->srem(kSubIndex, id); // index entry outlived its value: a DELETE on another
            // replica landed between our SMEMBERS and this GET
        }
    }
    return out;
}

std::string
SubscriptionStore::create_transfer(const sbi_gen::AnalyticsSubscriptionsTransfer& transfer) {
    const auto id = next_id("nwdaf-transfer-");
    redis_->set(kTransferPrefix + id, nlohmann::json(transfer).dump());
    redis_->sadd(kTransferIndex, id);
    return id;
}

bool SubscriptionStore::replace_transfer(const std::string& id,
                                         const sbi_gen::AnalyticsSubscriptionsTransfer& transfer) {
    return replace_if_present(*redis_, kTransferPrefix + id, transfer);
}

bool SubscriptionStore::remove_transfer(const std::string& id) {
    redis_->srem(kTransferIndex, id);
    return redis_->del(kTransferPrefix + id) > 0;
}

} // namespace nwdaf
