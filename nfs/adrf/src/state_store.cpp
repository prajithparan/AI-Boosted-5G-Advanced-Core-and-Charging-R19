#include "state_store.hpp"

#include <iterator>

namespace adrf {

namespace {
constexpr const char* kCounter = "adrf:next_id";
constexpr const char* kStoreSubPrefix = "adrf:storesub:";
constexpr const char* kCollPrefix = "adrf:coll:";
constexpr const char* kDataSetPrefix = "adrf:dataset:";
constexpr const char* kRsubPrefix = "adrf:rsub:";
constexpr const char* kRsubIndex = "adrf:rsubs";
constexpr const char* kFetchPrefix = "adrf:fetch:";
constexpr const char* kLeasePrefix = "adrf:lease:";

template <typename T> void put_opt(nlohmann::json& j, const char* key, const std::optional<T>& v) {
    if (v) {
        j[key] = *v;
    }
}
template <typename T> void get_opt(const nlohmann::json& j, const char* key, std::optional<T>& v) {
    if (j.contains(key)) {
        v = j.at(key).get<T>();
    }
}
} // namespace

void to_json(nlohmann::json& j, const StorageSubscription& v) {
    j = {{"kind", v.kind}, {"fingerprint", v.fingerprint}, {"request", v.request}};
    put_opt(j, "dataSetId", v.data_set_id);
}
void from_json(const nlohmann::json& j, StorageSubscription& v) {
    v.kind = j.at("kind");
    v.fingerprint = j.at("fingerprint");
    v.request = j.at("request");
    get_opt(j, "dataSetId", v.data_set_id);
}

void to_json(nlohmann::json& j, const Collection& v) {
    j = {{"target", v.target},
         {"resourceUri", v.resource_uri},
         {"notifCorrId", v.notif_corr_id},
         {"kind", v.kind},
         {"spec", v.spec},
         {"transRefIds", v.trans_ref_ids}};
    put_opt(j, "storeHandl", v.store_handl);
    put_opt(j, "dataSetTag", v.data_set_tag);
}
void from_json(const nlohmann::json& j, Collection& v) {
    v.target = j.at("target");
    v.resource_uri = j.at("resourceUri");
    v.notif_corr_id = j.at("notifCorrId");
    v.kind = j.at("kind");
    v.spec = j.at("spec");
    v.trans_ref_ids = j.at("transRefIds").get<std::vector<std::string>>();
    get_opt(j, "storeHandl", v.store_handl);
    get_opt(j, "dataSetTag", v.data_set_tag);
}

void to_json(nlohmann::json& j, const RetrievalSubscription& v) {
    j = {{"request", v.request}};
    put_opt(j, "fingerprint", v.fingerprint);
    put_opt(j, "dataSetId", v.data_set_id);
}
void from_json(const nlohmann::json& j, RetrievalSubscription& v) {
    v.request = j.at("request");
    get_opt(j, "fingerprint", v.fingerprint);
    get_opt(j, "dataSetId", v.data_set_id);
}

std::string StateStore::next_id(const char* prefix) {
    return std::string(prefix) + std::to_string(redis_->incr(kCounter));
}

std::string StateStore::create_storage_subscription(const StorageSubscription& s) {
    const auto id = next_id("adrf-store-");
    redis_->set(kStoreSubPrefix + id, nlohmann::json(s).dump());
    if (s.data_set_id) {
        redis_->sadd(kDataSetPrefix + *s.data_set_id, id);
    }
    return id;
}

std::optional<StorageSubscription>
StateStore::get_storage_subscription(const std::string& trans_ref_id) {
    const auto raw = redis_->get(kStoreSubPrefix + trans_ref_id);
    if (!raw) {
        return std::nullopt;
    }
    return nlohmann::json::parse(*raw).get<StorageSubscription>();
}

void StateStore::remove_storage_subscription(const std::string& trans_ref_id) {
    if (const auto s = get_storage_subscription(trans_ref_id); s && s->data_set_id) {
        redis_->srem(kDataSetPrefix + *s->data_set_id, trans_ref_id);
    }
    redis_->del(kStoreSubPrefix + trans_ref_id);
}

std::vector<std::string> StateStore::data_set_members(const std::string& data_set_id) {
    std::vector<std::string> ids;
    redis_->smembers(kDataSetPrefix + data_set_id, std::back_inserter(ids));
    return ids;
}

std::optional<Collection> StateStore::get_collection(const std::string& fingerprint) {
    const auto raw = redis_->get(kCollPrefix + fingerprint);
    if (!raw) {
        return std::nullopt;
    }
    return nlohmann::json::parse(*raw).get<Collection>();
}

void StateStore::put_collection(const std::string& fingerprint, const Collection& c) {
    redis_->set(kCollPrefix + fingerprint, nlohmann::json(c).dump());
}

void StateStore::remove_collection(const std::string& fingerprint) {
    redis_->del(kCollPrefix + fingerprint);
}

std::string StateStore::create_retrieval_subscription(const RetrievalSubscription& s) {
    const auto id = next_id("adrf-rsub-");
    redis_->set(kRsubPrefix + id, nlohmann::json(s).dump());
    redis_->sadd(kRsubIndex, id);
    return id;
}

std::optional<RetrievalSubscription> StateStore::get_retrieval_subscription(const std::string& id) {
    const auto raw = redis_->get(kRsubPrefix + id);
    if (!raw) {
        return std::nullopt;
    }
    return nlohmann::json::parse(*raw).get<RetrievalSubscription>();
}

bool StateStore::remove_retrieval_subscription(const std::string& id) {
    redis_->srem(kRsubIndex, id);
    return redis_->del(kRsubPrefix + id) > 0;
}

std::vector<std::string> StateStore::retrieval_subscription_ids() {
    std::vector<std::string> ids;
    redis_->smembers(kRsubIndex, std::back_inserter(ids));
    return ids;
}

void StateStore::put_fetch(const std::string& fetch_corr_id,
                           const std::string& store_trans_id,
                           std::chrono::seconds ttl) {
    redis_->set(kFetchPrefix + fetch_corr_id, store_trans_id, ttl);
}

std::optional<std::string> StateStore::take_fetch(const std::string& fetch_corr_id) {
    // GETDEL: a fetch correlation id is consumed once, whichever replica answers it.
    auto v = redis_->command<sw::redis::OptionalString>("GETDEL", kFetchPrefix + fetch_corr_id);
    if (!v) {
        return std::nullopt;
    }
    return *v;
}

bool StateStore::acquire_lease(const std::string& name, std::chrono::milliseconds ttl) {
    return redis_->set(kLeasePrefix + name, "1", ttl, sw::redis::UpdateType::NOT_EXIST);
}

} // namespace adrf
