#include "config_store.hpp"

#include <nlohmann/json.hpp>

namespace mfaf {

namespace {
constexpr const char* kCfgPrefix = "mfaf:cfg:";
constexpr const char* kCfgIndex = "mfaf:cfgs";
constexpr const char* kCorrPrefix = "mfaf:corr:";
constexpr const char* kBufPrefix = "mfaf:buf:";
constexpr const char* kBufIndexPrefix = "mfaf:bufidx:";
constexpr const char* kCounter = "mfaf:next_id";
} // namespace

std::string ConfigStore::next_id(const char* prefix) {
    return std::string(prefix) + std::to_string(redis_->incr(kCounter));
}

void ConfigStore::index(const std::string& id, const sbi_gen::MfafConfiguration& cfg) {
    if (!cfg.messageConfigurations) {
        return;
    }
    for (const auto& mc : *cfg.messageConfigurations) {
        if (mc.mfafNotiInfo) {
            redis_->set(kCorrPrefix + mc.mfafNotiInfo->mfafCorreId, id);
        }
    }
}

void ConfigStore::unindex(const sbi_gen::MfafConfiguration& cfg) {
    if (!cfg.messageConfigurations) {
        return;
    }
    for (const auto& mc : *cfg.messageConfigurations) {
        if (mc.mfafNotiInfo) {
            redis_->del(kCorrPrefix + mc.mfafNotiInfo->mfafCorreId);
        }
    }
}

std::string ConfigStore::create(const sbi_gen::MfafConfiguration& cfg) {
    const auto id = next_id("mfaf-cfg-");
    redis_->set(kCfgPrefix + id, nlohmann::json(cfg).dump());
    redis_->sadd(kCfgIndex, id);
    index(id, cfg);
    return id;
}

std::optional<sbi_gen::MfafConfiguration> ConfigStore::get(const std::string& id) {
    const auto raw = redis_->get(kCfgPrefix + id);
    if (!raw) {
        return std::nullopt;
    }
    return nlohmann::json::parse(*raw).get<sbi_gen::MfafConfiguration>();
}

bool ConfigStore::replace(const std::string& id, const sbi_gen::MfafConfiguration& cfg) {
    const auto old = get(id);
    if (!old) {
        return false;
    }
    // SET XX: only an existing key. A DELETE on another replica between our GET and this SET
    // makes the update a 404 rather than resurrecting the resource.
    if (!redis_->set(kCfgPrefix + id,
                     nlohmann::json(cfg).dump(),
                     std::chrono::milliseconds(0),
                     sw::redis::UpdateType::EXIST)) {
        return false;
    }
    unindex(*old);
    index(id, cfg);
    return true;
}

bool ConfigStore::remove(const std::string& id) {
    const auto old = get(id);
    if (!old) {
        return false;
    }
    unindex(*old);
    redis_->srem(kCfgIndex, id);
    const auto fetch_ids = [&] {
        std::vector<std::string> ids;
        redis_->smembers(kBufIndexPrefix + id, std::back_inserter(ids));
        return ids;
    }();
    for (const auto& f : fetch_ids) {
        redis_->del(kBufPrefix + f);
    }
    redis_->del(kBufIndexPrefix + id);
    return redis_->del(kCfgPrefix + id) > 0;
}

std::optional<std::pair<std::string, sbi_gen::MfafConfiguration>>
ConfigStore::find_by_correlation(const std::string& mfaf_corre_id) {
    const auto id = redis_->get(kCorrPrefix + mfaf_corre_id);
    if (!id) {
        return std::nullopt;
    }
    auto cfg = get(*id);
    if (!cfg) {
        redis_->del(kCorrPrefix + mfaf_corre_id); // index outlived its configuration
        return std::nullopt;
    }
    return std::make_pair(*id, std::move(*cfg));
}

std::string ConfigStore::buffer_put(const std::string& trans_ref_id,
                                    const sbi_gen::NmfafDataAnaNotification& notification,
                                    std::chrono::seconds ttl) {
    const auto fetch_id = next_id("mfaf-fetch-");
    redis_->set(kBufPrefix + fetch_id,
                nlohmann::json(notification).dump(),
                std::chrono::duration_cast<std::chrono::milliseconds>(ttl));
    redis_->sadd(kBufIndexPrefix + trans_ref_id, fetch_id);
    return fetch_id;
}

std::optional<sbi_gen::NmfafDataAnaNotification>
ConfigStore::buffer_take(const std::string& fetch_corr_id) {
    // GETDEL (Valkey/Redis >= 6.2): read and remove atomically, so two replicas answering the
    // same fetch cannot both hand out the buffer.
    const auto raw =
        redis_->command<sw::redis::OptionalString>("GETDEL", kBufPrefix + fetch_corr_id);
    if (!raw) {
        return std::nullopt;
    }
    return nlohmann::json::parse(*raw).get<sbi_gen::NmfafDataAnaNotification>();
}

std::vector<std::pair<std::string, sbi_gen::NmfafDataAnaNotification>>
ConfigStore::buffers_for(const std::string& trans_ref_id) {
    std::vector<std::string> ids;
    redis_->smembers(kBufIndexPrefix + trans_ref_id, std::back_inserter(ids));
    std::vector<std::pair<std::string, sbi_gen::NmfafDataAnaNotification>> out;
    for (const auto& f : ids) {
        if (auto n = buffer_take(f)) {
            out.emplace_back(f, std::move(*n));
        }
    }
    redis_->del(kBufIndexPrefix + trans_ref_id);
    return out;
}

} // namespace mfaf
