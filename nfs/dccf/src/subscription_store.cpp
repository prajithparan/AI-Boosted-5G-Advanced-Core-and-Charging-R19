#include "subscription_store.hpp"

#include <cstdint>

namespace dccf {

namespace {
constexpr const char* kSubPrefix = "dccf:sub:";
constexpr const char* kSrcPrefix = "dccf:src:";
constexpr const char* kProfilePrefix = "dccf:profile:";
constexpr const char* kCounter = "dccf:next_id";

template <typename T>
bool replace_if_present(sw::redis::Redis& redis, const std::string& key, const T& value) {
    return redis.set(key,
                     nlohmann::json(value).dump(),
                     std::chrono::milliseconds(0),
                     sw::redis::UpdateType::EXIST);
}
} // namespace

void to_json(nlohmann::json& j, const ConsumerSubscription& v) {
    j = nlohmann::json{{"kind", v.kind == Kind::Data ? "data" : "analytics"},
                       {"request", v.request},
                       {"fingerprint", v.fingerprint}};
}

void from_json(const nlohmann::json& j, ConsumerSubscription& v) {
    v.kind = j.at("kind").get<std::string>() == "data" ? Kind::Data : Kind::Analytics;
    v.request = j.at("request");
    j.at("fingerprint").get_to(v.fingerprint);
}

void to_json(nlohmann::json& j, const SourceCollection& v) {
    j = nlohmann::json{{"source", v.source},
                       {"sourceResourceUri", v.source_resource_uri},
                       {"mfafTransRefId", v.mfaf_trans_ref_id},
                       {"mfafNotifUri", v.mfaf_notif_uri},
                       {"mfafCorreId", v.mfaf_corre_id},
                       {"consumers", v.consumers}};
}

void from_json(const nlohmann::json& j, SourceCollection& v) {
    j.at("source").get_to(v.source);
    j.at("sourceResourceUri").get_to(v.source_resource_uri);
    j.at("mfafTransRefId").get_to(v.mfaf_trans_ref_id);
    j.at("mfafNotifUri").get_to(v.mfaf_notif_uri);
    j.at("mfafCorreId").get_to(v.mfaf_corre_id);
    j.at("consumers").get_to(v.consumers);
}

std::string SubscriptionStore::next_id(const char* prefix) {
    return std::string(prefix) + std::to_string(redis_->incr(kCounter));
}

std::string SubscriptionStore::create_subscription(const ConsumerSubscription& sub) {
    const auto id = next_id("dccf-sub-");
    redis_->set(kSubPrefix + id, nlohmann::json(sub).dump());
    return id;
}

std::optional<ConsumerSubscription> SubscriptionStore::get_subscription(const std::string& id) {
    const auto raw = redis_->get(kSubPrefix + id);
    if (!raw) {
        return std::nullopt;
    }
    return nlohmann::json::parse(*raw).get<ConsumerSubscription>();
}

bool SubscriptionStore::replace_subscription(const std::string& id,
                                             const ConsumerSubscription& sub) {
    return replace_if_present(*redis_, kSubPrefix + id, sub);
}

bool SubscriptionStore::remove_subscription(const std::string& id) {
    return redis_->del(kSubPrefix + id) > 0;
}

std::optional<SourceCollection> SubscriptionStore::get_source(const std::string& fp) {
    const auto raw = redis_->get(kSrcPrefix + fp);
    if (!raw) {
        return std::nullopt;
    }
    return nlohmann::json::parse(*raw).get<SourceCollection>();
}

void SubscriptionStore::put_source(const std::string& fp, const SourceCollection& src) {
    redis_->set(kSrcPrefix + fp, nlohmann::json(src).dump());
}

void SubscriptionStore::remove_source(const std::string& fp) {
    redis_->del(kSrcPrefix + fp);
}

std::string SubscriptionStore::create_profile(const sbi_gen::NdccfDataCollectionProfile& p) {
    const auto id = next_id("dccf-profile-");
    redis_->set(kProfilePrefix + id, nlohmann::json(p).dump());
    return id;
}

bool SubscriptionStore::replace_profile(const std::string& id,
                                        const sbi_gen::NdccfDataCollectionProfile& p) {
    return replace_if_present(*redis_, kProfilePrefix + id, p);
}

bool SubscriptionStore::remove_profile(const std::string& id) {
    return redis_->del(kProfilePrefix + id) > 0;
}

std::string fingerprint(const std::string& source,
                        nlohmann::json source_sub,
                        const std::vector<std::string>& notification_fields) {
    for (const auto& f : notification_fields) {
        source_sub.erase(f);
    }
    // nlohmann::json objects serialise with keys in sorted order, so dump() is canonical.
    const std::string canonical = source + "|" + source_sub.dump();
    std::uint64_t h = 1469598103934665603ULL;
    for (const unsigned char c : canonical) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

} // namespace dccf
