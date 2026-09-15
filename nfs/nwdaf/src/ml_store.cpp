#include "ml_store.hpp"

#include <iterator>

namespace nwdaf {

namespace {
constexpr const char* kCounter = "nwdaf:next_id";
constexpr const char* kProvSubPrefix = "nwdaf:mlprov:sub:";
constexpr const char* kProvSubIndex = "nwdaf:mlprov:subs";
constexpr const char* kProvLeasePrefix = "nwdaf:mlprov:lease:";
constexpr const char* kModelPrefix = "nwdaf:mlmodel:";
constexpr const char* kTrainLeasePrefix = "nwdaf:mltrain:";
constexpr const char* kRegPrefix = "nwdaf:mlmon:reg:";
constexpr const char* kRegIndex = "nwdaf:mlmon:regs";
constexpr const char* kDegradedPrefix = "nwdaf:mldegraded:";
constexpr const char* kStorageSubPrefix = "nwdaf:mlstoragesub:";
constexpr const char* kActiveModelPrefix = "nwdaf:anlf:model:";
} // namespace

std::int64_t MlStore::next_number() {
    return redis_->incr(kCounter);
}

std::string MlStore::next_id(const char* prefix) {
    return std::string(prefix) + std::to_string(next_number());
}

std::string MlStore::create_provision_subscription(const nlohmann::json& record) {
    const auto id = next_id("nwdaf-ml-");
    redis_->set(kProvSubPrefix + id, record.dump());
    redis_->sadd(kProvSubIndex, id);
    return id;
}

std::optional<nlohmann::json> MlStore::get_provision_subscription(const std::string& id) {
    const auto raw = redis_->get(kProvSubPrefix + id);
    if (!raw) {
        return std::nullopt;
    }
    return nlohmann::json::parse(*raw);
}

bool MlStore::replace_provision_subscription(const std::string& id, const nlohmann::json& record) {
    return redis_->set(kProvSubPrefix + id,
                       record.dump(),
                       std::chrono::milliseconds(0),
                       sw::redis::UpdateType::EXIST);
}

bool MlStore::remove_provision_subscription(const std::string& id) {
    redis_->srem(kProvSubIndex, id);
    return redis_->del(kProvSubPrefix + id) > 0;
}

std::vector<std::pair<std::string, nlohmann::json>> MlStore::all_provision_subscriptions() {
    std::vector<std::string> ids;
    redis_->smembers(kProvSubIndex, std::back_inserter(ids));
    std::vector<std::pair<std::string, nlohmann::json>> out;
    for (const auto& id : ids) {
        if (auto s = get_provision_subscription(id)) {
            out.emplace_back(id, std::move(*s));
        }
    }
    return out;
}

bool MlStore::claim_delivery(const std::string& id, std::chrono::milliseconds ttl) {
    return redis_->set(kProvLeasePrefix + id, "1", ttl, sw::redis::UpdateType::NOT_EXIST);
}

std::optional<nlohmann::json> MlStore::get_model(const std::string& event) {
    const auto raw = redis_->get(kModelPrefix + event);
    if (!raw) {
        return std::nullopt;
    }
    return nlohmann::json::parse(*raw);
}

void MlStore::put_model(const std::string& event, const nlohmann::json& record) {
    redis_->set(kModelPrefix + event, record.dump());
}

bool MlStore::acquire_training_lease(const std::string& event, std::chrono::milliseconds ttl) {
    return redis_->set(kTrainLeasePrefix + event, "1", ttl, sw::redis::UpdateType::NOT_EXIST);
}

void MlStore::release_training_lease(const std::string& event) {
    redis_->del(kTrainLeasePrefix + event);
}

std::optional<std::string> MlStore::get_storage_subscription(const std::string& event) {
    const auto raw = redis_->get(kStorageSubPrefix + event);
    if (!raw) {
        return std::nullopt;
    }
    return *raw;
}

void MlStore::put_storage_subscription(const std::string& event, const std::string& trans_ref_id) {
    redis_->set(kStorageSubPrefix + event, trans_ref_id);
}

std::string MlStore::create_registration(const nlohmann::json& record) {
    const auto id = next_id("nwdaf-mlreg-");
    redis_->set(kRegPrefix + id, record.dump());
    redis_->sadd(kRegIndex, id);
    return id;
}

std::optional<nlohmann::json> MlStore::get_registration(const std::string& id) {
    const auto raw = redis_->get(kRegPrefix + id);
    if (!raw) {
        return std::nullopt;
    }
    return nlohmann::json::parse(*raw);
}

void MlStore::put_registration(const std::string& id, const nlohmann::json& record) {
    redis_->set(kRegPrefix + id, record.dump());
}

bool MlStore::remove_registration(const std::string& id) {
    redis_->srem(kRegIndex, id);
    return redis_->del(kRegPrefix + id) > 0;
}

std::vector<std::pair<std::string, nlohmann::json>> MlStore::all_registrations() {
    std::vector<std::string> ids;
    redis_->smembers(kRegIndex, std::back_inserter(ids));
    std::vector<std::pair<std::string, nlohmann::json>> out;
    for (const auto& id : ids) {
        if (auto r = get_registration(id)) {
            out.emplace_back(id, std::move(*r));
        }
    }
    return out;
}

std::optional<nlohmann::json> MlStore::get_degraded(const std::string& event) {
    const auto raw = redis_->get(kDegradedPrefix + event);
    if (!raw) {
        return std::nullopt;
    }
    return nlohmann::json::parse(*raw);
}

void MlStore::put_degraded(const std::string& event, const nlohmann::json& notif) {
    redis_->set(kDegradedPrefix + event, notif.dump());
}

void MlStore::clear_degraded(const std::string& event) {
    redis_->del(kDegradedPrefix + event);
}

bool MlStore::open_holder(const std::string& key, const nlohmann::json& record) {
    return redis_->set(
        key, record.dump(), std::chrono::milliseconds(0), sw::redis::UpdateType::NOT_EXIST);
}

std::optional<nlohmann::json> MlStore::get_holder(const std::string& key) {
    const auto raw = redis_->get(key);
    if (!raw) {
        return std::nullopt;
    }
    return nlohmann::json::parse(*raw);
}

void MlStore::close_holder(const std::string& key) {
    redis_->del(key);
    redis_->del(key + ":alive");
}

void MlStore::touch_holder(const std::string& key, std::chrono::milliseconds ttl) {
    redis_->set(key + ":alive", "1", ttl);
}

bool MlStore::holder_alive(const std::string& key) {
    return redis_->exists(key + ":alive") > 0;
}

std::optional<nlohmann::json> MlStore::get_active_model(const std::string& event) {
    const auto raw = redis_->get(kActiveModelPrefix + event);
    if (!raw) {
        return std::nullopt;
    }
    return nlohmann::json::parse(*raw);
}

void MlStore::put_active_model(const std::string& event, const nlohmann::json& record) {
    redis_->set(kActiveModelPrefix + event, record.dump());
}

} // namespace nwdaf
