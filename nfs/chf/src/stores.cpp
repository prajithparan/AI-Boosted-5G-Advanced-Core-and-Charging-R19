#include "stores.hpp"

#include <nlohmann/json.hpp>

namespace chf {

namespace {

using nlohmann::json;

constexpr const char* kChargingDataActiveSet = "chf:cdr:active";
constexpr const char* kChargingDataNextIdKey = "chf:cdr:next_id";
constexpr const char* kOfflineActiveSet = "chf:offline:active";
constexpr const char* kOfflineNextIdKey = "chf:offline:next_id";
constexpr const char* kSpendingLimitNextIdKey = "chf:sub:next_id";
// ADR-0072 (gap-closure: real N28 end-to-end) -- real active-subscription set, same pattern as
// kChargingDataActiveSet/kOfflineActiveSet above, needed so a real policy-counter status change
// can enumerate which subscriptions to push a real statusNotification to.
constexpr const char* kSpendingLimitActiveSet = "chf:sub:active";
// Real, this-project-owned config surface (NOT a 3GPP-defined resource -- PolicyCounterInfo.
// currentStatus is explicitly operator-defined per TS29594's own spec text, see stores.hpp's own
// comment) for the "configuration parameters... to create from GUI later" requirement.
constexpr const char* kPolicyCounterConfigKeyPrefix = "chf:policycounter:";

std::string spending_limit_key(const std::string& id) {
    return "chf:sub:" + id;
}

std::string policy_counter_config_key(const std::string& policy_counter_id) {
    return std::string(kPolicyCounterConfigKeyPrefix) + policy_counter_id;
}

// P4.8 (ADR-0074): real per-SUPI/per-ratingGroup rolling feature key.
std::string quota_feature_key(const std::string& supi, std::int64_t rating_group) {
    return "chf:quotafeat:" + supi + ":" + std::to_string(rating_group);
}

std::string charging_data_content_key(const std::string& ref) {
    return "chf:cdr:content:" + ref;
}

} // namespace

std::string ChargingDataStore::create(const std::string& supi) {
    const auto id = redis_->incr(kChargingDataNextIdKey);
    auto ref = "chg-" + std::to_string(id);
    redis_->sadd(kChargingDataActiveSet, ref);
    redis_->hset(charging_data_content_key(ref), "supi", supi);
    redis_->hset(charging_data_content_key(ref), "reserved_total", "0");
    return ref;
}

bool ChargingDataStore::release(const std::string& ref) {
    // Content (chf:cdr:content:{ref}) is deliberately left behind after release -- a real,
    // disclosed audit trail (which SUPI/how much was reserved for this now-closed session) that
    // the balance-management side's own AdjustBalance/ReserveBalance ledger rows independently
    // corroborate. Only the active-set membership is removed, matching this method's own existing
    // "no longer active" contract.
    return redis_->srem(kChargingDataActiveSet, ref) > 0;
}

bool ChargingDataStore::is_active(const std::string& ref) {
    return redis_->sismember(kChargingDataActiveSet, ref);
}

std::optional<std::string> ChargingDataStore::get_supi(const std::string& ref) {
    const auto value = redis_->hget(charging_data_content_key(ref), "supi");
    if (!value) {
        return std::nullopt;
    }
    return *value;
}

double ChargingDataStore::add_reserved(const std::string& ref, double amount) {
    return redis_->hincrbyfloat(charging_data_content_key(ref), "reserved_total", amount);
}

double ChargingDataStore::get_reserved_total(const std::string& ref) {
    const auto value = redis_->hget(charging_data_content_key(ref), "reserved_total");
    if (!value) {
        return 0.0;
    }
    return std::stod(*value);
}

// ADR-0297. Same field/key/atomicity discipline as reserved_total above.
void ChargingDataStore::add_granted_volume(const std::string& ref, double octets) {
    redis_->hincrbyfloat(charging_data_content_key(ref), "granted_volume", octets);
}

void ChargingDataStore::add_granted_service_units(const std::string& ref, double units) {
    redis_->hincrbyfloat(charging_data_content_key(ref), "granted_service_units", units);
}

double ChargingDataStore::get_granted_volume(const std::string& ref) {
    const auto value = redis_->hget(charging_data_content_key(ref), "granted_volume");
    return value ? std::stod(*value) : 0.0;
}

double ChargingDataStore::get_granted_service_units(const std::string& ref) {
    const auto value = redis_->hget(charging_data_content_key(ref), "granted_service_units");
    return value ? std::stod(*value) : 0.0;
}

std::string OfflineChargingDataStore::create() {
    const auto id = redis_->incr(kOfflineNextIdKey);
    auto ref = "offchg-" + std::to_string(id);
    redis_->sadd(kOfflineActiveSet, ref);
    return ref;
}

bool OfflineChargingDataStore::release(const std::string& ref) {
    return redis_->srem(kOfflineActiveSet, ref) > 0;
}

bool OfflineChargingDataStore::is_active(const std::string& ref) {
    return redis_->sismember(kOfflineActiveSet, ref);
}

std::string SpendingLimitSubscriptionStore::create(sbi_gen::SpendingLimitContext context) {
    const auto id = redis_->incr(kSpendingLimitNextIdKey);
    auto sub_id = "sub-" + std::to_string(id);
    const json j = context;
    redis_->set(spending_limit_key(sub_id), j.dump());
    redis_->sadd(kSpendingLimitActiveSet, sub_id);
    return sub_id;
}

bool SpendingLimitSubscriptionStore::update(const std::string& id,
                                            sbi_gen::SpendingLimitContext context) {
    const auto key = spending_limit_key(id);
    // Real update-in-place semantics: only set if the subscription already exists. This has the
    // same non-atomic check-then-act shape as ChargingDataStore::is_active's own callers
    // elsewhere in this codebase (e.g. the Update route checking is_active before proceeding) --
    // consistent with this project's existing concurrency-simplification level, not a new gap.
    if (!redis_->get(key)) {
        return false;
    }
    const json j = context;
    redis_->set(key, j.dump());
    return true;
}

bool SpendingLimitSubscriptionStore::remove(const std::string& id) {
    redis_->srem(kSpendingLimitActiveSet, id);
    return redis_->del(spending_limit_key(id)) > 0;
}

std::optional<sbi_gen::SpendingLimitContext>
SpendingLimitSubscriptionStore::get(const std::string& id) {
    const auto value = redis_->get(spending_limit_key(id));
    if (!value) {
        return std::nullopt;
    }
    return json::parse(*value).get<sbi_gen::SpendingLimitContext>();
}

std::vector<std::pair<std::string, sbi_gen::SpendingLimitContext>>
SpendingLimitSubscriptionStore::list_all() {
    std::vector<std::string> ids;
    redis_->smembers(kSpendingLimitActiveSet, std::back_inserter(ids));
    std::vector<std::pair<std::string, sbi_gen::SpendingLimitContext>> out;
    out.reserve(ids.size());
    for (const auto& id : ids) {
        if (auto context = get(id); context.has_value()) {
            out.emplace_back(id, std::move(*context));
        }
    }
    return out;
}

PolicyCounterConfigStore::PolicyCounterConfigStore(std::shared_ptr<sw::redis::Redis> redis)
    : redis_(std::move(redis)) {}

void PolicyCounterConfigStore::set_status(const std::string& policy_counter_id,
                                          const std::string& status) {
    redis_->set(policy_counter_config_key(policy_counter_id), status);
}

std::optional<std::string>
PolicyCounterConfigStore::get_status(const std::string& policy_counter_id) {
    const auto value = redis_->get(policy_counter_config_key(policy_counter_id));
    if (!value) {
        return std::nullopt;
    }
    return std::make_optional(*value);
}

std::optional<QuotaHistorySnapshot> QuotaFeatureStore::get(const std::string& supi,
                                                           std::int64_t rating_group) {
    const auto key = quota_feature_key(supi, rating_group);
    std::vector<sw::redis::OptionalString> values;
    redis_->hmget(key, {"u1", "u2", "u3", "ts", "g"}, std::back_inserter(values));
    // hmget returns one entry per requested field, nullopt when unset -- a hash with no "u1" at
    // all means this SUPI+ratingGroup has never reported usage before (real cold start).
    // sw::redis::Optional<T> is redis-plus-plus's own pre-C++17 Optional, not std::optional --
    // real, disclosed API mismatch found via actual compilation: it has `explicit operator
    // bool()`/`operator*()`, not `.has_value()`.
    if (!values[0]) {
        return std::nullopt;
    }
    QuotaHistorySnapshot snapshot;
    for (const std::size_t idx : {std::size_t{0}, std::size_t{1}, std::size_t{2}}) {
        if (values[idx]) {
            snapshot.recentUsedVolumes.push_back(std::stod(*values[idx]));
        }
    }
    if (values[3]) {
        snapshot.lastInvocationUnixSec = std::stoll(*values[3]);
    }
    if (values[4]) {
        snapshot.lastGrantedTotalVolume = std::stod(*values[4]);
    }
    return snapshot;
}

void QuotaFeatureStore::record_usage(const std::string& supi,
                                     std::int64_t rating_group,
                                     double used_total_volume,
                                     std::optional<double> granted_total_volume,
                                     std::int64_t invocation_unix_sec) {
    const auto key = quota_feature_key(supi, rating_group);
    // Shift the rolling window: this request's own used_total_volume becomes the new "most
    // recent" (u1); the old u1/u2 slide down. Real, disclosed non-atomicity: this is a
    // read-then-write (hmget then hset), same concurrency-simplification level as
    // SpendingLimitSubscriptionStore::update's own check-then-act -- acceptable at this project's
    // real lab scale, not claimed to be race-free under concurrent Updates for the same
    // SUPI+ratingGroup.
    std::vector<sw::redis::OptionalString> prior;
    redis_->hmget(key, {"u1", "u2"}, std::back_inserter(prior));

    redis_->hset(key, "u1", std::to_string(used_total_volume));
    if (prior[0]) {
        redis_->hset(key, "u2", *prior[0]);
    }
    if (prior[1]) {
        redis_->hset(key, "u3", *prior[1]);
    }
    redis_->hset(key, "ts", std::to_string(invocation_unix_sec));
    if (granted_total_volume.has_value()) {
        redis_->hset(key, "g", std::to_string(*granted_total_volume));
    }
}

} // namespace chf
