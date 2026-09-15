#include "collection_store.hpp"

#include <iterator>

namespace nwdaf {

namespace {
constexpr const char* kCounter = "nwdaf:next_id";
constexpr const char* kCollectedPrefix = "nwdaf:collected:";
constexpr const char* kCollPrefix = "nwdaf:coll:";
constexpr const char* kDmSubPrefix = "nwdaf:dm:sub:";
constexpr const char* kDmSubIndex = "nwdaf:dm:subs";
constexpr const char* kFetchPrefix = "nwdaf:dm:fetch:";

std::int64_t epoch_ms(std::chrono::system_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}
} // namespace

void to_json(nlohmann::json& j, const DataManagementSubscription& v) {
    j = {{"source", v.source}, {"request", v.request}};
}
void from_json(const nlohmann::json& j, DataManagementSubscription& v) {
    v.source = j.at("source");
    v.request = j.at("request");
}

std::string CollectionStore::next_id(const char* prefix) {
    return std::string(prefix) + std::to_string(redis_->incr(kCounter));
}

CollectedEvent CollectionStore::append(const std::string& source, const nlohmann::json& event) {
    CollectedEvent e;
    e.seq = redis_->incr(kCounter);
    e.received_at = std::chrono::system_clock::now();
    e.event = event;
    const std::string key = kCollectedPrefix + source;
    const auto score = static_cast<double>(epoch_ms(e.received_at));
    redis_->zadd(
        key,
        nlohmann::json{{"seq", e.seq}, {"t", epoch_ms(e.received_at)}, {"event", event}}.dump(),
        score);
    // The observation window and the hard cap, both from config: older than the window goes,
    // and beyond the cap the oldest go.
    const auto oldest_kept = epoch_ms(e.received_at - window_);
    redis_->zremrangebyscore(key,
                             sw::redis::RightBoundedInterval<double>(
                                 static_cast<double>(oldest_kept), sw::redis::BoundType::OPEN));
    if (max_events_ > 0) {
        redis_->zremrangebyrank(key, 0, -max_events_ - 1);
    }
    return e;
}

std::vector<CollectedEvent> CollectionStore::events(const std::string& source,
                                                    std::chrono::system_clock::time_point from,
                                                    std::chrono::system_clock::time_point to) {
    std::vector<std::string> members;
    redis_->zrangebyscore(kCollectedPrefix + source,
                          sw::redis::BoundedInterval<double>(static_cast<double>(epoch_ms(from)),
                                                             static_cast<double>(epoch_ms(to)),
                                                             sw::redis::BoundType::CLOSED),
                          std::back_inserter(members));
    std::vector<CollectedEvent> out;
    out.reserve(members.size());
    for (const auto& m : members) {
        try {
            const auto j = nlohmann::json::parse(m);
            CollectedEvent e;
            e.seq = j.at("seq").get<std::int64_t>();
            e.received_at = std::chrono::system_clock::time_point(
                std::chrono::milliseconds(j.at("t").get<std::int64_t>()));
            e.event = j.at("event");
            out.push_back(std::move(e));
        } catch (const nlohmann::json::exception&) {
            // A member this build cannot read is skipped, never fatal to the analytic.
        }
    }
    return out;
}

bool CollectionStore::open_collection(const std::string& source, const nlohmann::json& record) {
    return redis_->set(kCollPrefix + source,
                       record.dump(),
                       std::chrono::milliseconds(0),
                       sw::redis::UpdateType::NOT_EXIST);
}

std::optional<nlohmann::json> CollectionStore::get_collection(const std::string& source) {
    const auto raw = redis_->get(kCollPrefix + source);
    if (!raw) {
        return std::nullopt;
    }
    return nlohmann::json::parse(*raw);
}

void CollectionStore::close_collection(const std::string& source) {
    redis_->del(kCollPrefix + source);
    redis_->del(kCollPrefix + source + ":alive");
}

void CollectionStore::touch_holder(const std::string& source, std::chrono::milliseconds ttl) {
    redis_->set(kCollPrefix + source + ":alive", "1", ttl);
}

bool CollectionStore::holder_alive(const std::string& source) {
    return redis_->exists(kCollPrefix + source + ":alive") > 0;
}

std::string CollectionStore::create_dm_subscription(const DataManagementSubscription& s) {
    const auto id = next_id("nwdaf-dm-");
    redis_->set(kDmSubPrefix + id, nlohmann::json(s).dump());
    redis_->sadd(kDmSubIndex, id);
    return id;
}

std::optional<DataManagementSubscription>
CollectionStore::get_dm_subscription(const std::string& id) {
    const auto raw = redis_->get(kDmSubPrefix + id);
    if (!raw) {
        return std::nullopt;
    }
    return nlohmann::json::parse(*raw).get<DataManagementSubscription>();
}

bool CollectionStore::replace_dm_subscription(const std::string& id,
                                              const DataManagementSubscription& s) {
    return redis_->set(kDmSubPrefix + id,
                       nlohmann::json(s).dump(),
                       std::chrono::milliseconds(0),
                       sw::redis::UpdateType::EXIST);
}

bool CollectionStore::remove_dm_subscription(const std::string& id) {
    redis_->srem(kDmSubIndex, id);
    return redis_->del(kDmSubPrefix + id) > 0;
}

std::vector<std::pair<std::string, DataManagementSubscription>>
CollectionStore::all_dm_subscriptions() {
    std::vector<std::string> ids;
    redis_->smembers(kDmSubIndex, std::back_inserter(ids));
    std::vector<std::pair<std::string, DataManagementSubscription>> out;
    for (const auto& id : ids) {
        if (auto s = get_dm_subscription(id)) {
            out.emplace_back(id, std::move(*s));
        }
    }
    return out;
}

void CollectionStore::put_fetch(const std::string& fetch_corr_id,
                                const nlohmann::json& notif,
                                std::chrono::seconds ttl) {
    redis_->set(kFetchPrefix + fetch_corr_id, notif.dump(), ttl);
}

std::optional<nlohmann::json> CollectionStore::take_fetch(const std::string& fetch_corr_id) {
    auto v = redis_->command<sw::redis::OptionalString>("GETDEL", kFetchPrefix + fetch_corr_id);
    if (!v) {
        return std::nullopt;
    }
    return nlohmann::json::parse(*v);
}

} // namespace nwdaf
