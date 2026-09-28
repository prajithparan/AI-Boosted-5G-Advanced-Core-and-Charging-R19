#include "stores.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <thread>

namespace chf {

namespace {

using nlohmann::json;

// docs/DECISIONS.md ADR-0444: these are GLOBAL keys (the active-ref set every session's create/
// release/is_active touches, and the sequence counters new refs/subs are allocated from) -- NOT
// per-subscriber, so they are deliberately NEVER passed through RedisRouter::tag(). Tagging a
// global set per-ref would SPLIT it into many separate per-ref sets, breaking is_active/release's
// own "one shared set of everything currently active" contract -- a real correctness bug, not an
// optimisation choice. In cluster mode each of these lives on exactly one shard, same as UDSF's
// own due-timer ZSETs; CHF's own real Tier-1 throughput ceiling for Create/Release/is_active is
// this one shard's throughput, which cluster mode does not raise -- disclosed, not fixed, here.
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
// comment) for the "configuration parameters... to create from GUI later" requirement. Shared
// operator config, not sharded per subscriber -- deliberately untagged, same reasoning as the
// global sets above.
constexpr const char* kPolicyCounterConfigKeyPrefix = "chf:policycounter:";

// docs/DECISIONS.md ADR-0444: `id`/`ref`/`supi`/`key` below are each this store's own real
// per-entity identity (a subscriber, a charging session, an idempotency request) -- router.tag()
// wraps them in a Valkey Cluster hash tag in cluster mode (a no-op, byte-identical key, in single
// mode, the default this ADR does not change).
std::string spending_limit_key(const nf_config::RedisRouter& router, const std::string& id) {
    return "chf:sub:" + router.tag(id);
}

std::string policy_counter_config_key(const std::string& policy_counter_id) {
    // Untagged -- see kPolicyCounterConfigKeyPrefix's own comment.
    return std::string(kPolicyCounterConfigKeyPrefix) + policy_counter_id;
}

// P4.8 (ADR-0074): real per-SUPI/per-ratingGroup rolling feature key. Tagged by supi (CHF's own
// natural sharding/subscriber identity), not by supi+ratingGroup, so every rating group of one
// subscriber colocates on one shard.
std::string quota_feature_key(const nf_config::RedisRouter& router,
                              const std::string& supi,
                              std::int64_t rating_group) {
    return "chf:quotafeat:" + router.tag(supi) + ":" + std::to_string(rating_group);
}

std::string charging_data_content_key(const nf_config::RedisRouter& router,
                                      const std::string& ref) {
    return "chf:cdr:content:" + router.tag(ref);
}

} // namespace

namespace {
std::string idempotency_key_redis_key(const nf_config::RedisRouter& router,
                                      const std::string& key) {
    return "chf:idem:" + router.tag(key);
}
} // namespace

Claim IdempotencyStore::claim(const std::string& key) {
    if (!enabled() || key.empty()) {
        return Claim::Owned; // detection off, or a client that sent no key: process normally
    }
    const auto redis_key = idempotency_key_redis_key(redis_, key);
    // HSETNX is the atomic part: exactly one concurrent request can create the field, so exactly
    // one owns the key. Doing this BEFORE processing is what closes the window in which a
    // retransmission arriving mid-flight would otherwise be processed as a fresh request.
    const bool owned =
        redis_.with([&](auto& r) { return r.hsetnx(redis_key, "state", "pending"); });
    // The TTL bounds every entry, including one whose owner dies before publishing -- otherwise a
    // crashed request would wedge its key permanently.
    redis_.with([&](auto& r) { r.expire(redis_key, std::chrono::seconds(ttl_seconds_)); });
    return owned ? Claim::Owned : Claim::Duplicate;
}

std::optional<IdempotentResponse> IdempotencyStore::await_response(const std::string& key) {
    if (!enabled() || key.empty()) {
        return std::nullopt;
    }
    const auto redis_key = idempotency_key_redis_key(redis_, key);
    // Bounded, because a charging request must not block on a peer's retransmission. The original
    // is executing on this same cluster and publishes as soon as it answers; if it has not within
    // this window the caller processes normally, which is exactly the pre-ADR-0352 behaviour.
    for (int waited = 0; waited < max_wait_ms_; waited += poll_ms_) {
        const auto status = redis_.with([&](auto& r) { return r.hget(redis_key, "status"); });
        if (status) {
            IdempotentResponse response;
            try {
                response.status = std::stoi(*status);
            } catch (const std::exception&) {
                return std::nullopt;
            }
            if (const auto location =
                    redis_.with([&](auto& r) { return r.hget(redis_key, "location"); });
                location) {
                response.location = *location;
            }
            return response;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms_));
    }
    return std::nullopt;
}

void IdempotencyStore::remember(const std::string& key, const IdempotentResponse& response) {
    if (!enabled() || key.empty()) {
        return;
    }
    const auto redis_key = idempotency_key_redis_key(redis_, key);
    redis_.with([&](auto& r) {
        if (!response.location.empty()) {
            r.hset(redis_key, "location", response.location);
        }
        // Written LAST: await_response keys off "status", so it must not become visible before
        // the location it belongs with, or a waiter could replay a 201 with no Location.
        r.hset(redis_key, "status", std::to_string(response.status));
        r.expire(redis_key, std::chrono::seconds(ttl_seconds_));
    });
}

std::string ChargingDataStore::create(const std::string& supi) {
    const auto id = redis_.with([&](auto& r) { return r.incr(kChargingDataNextIdKey); });
    auto ref = "chg-" + std::to_string(id);
    const auto content_key = charging_data_content_key(redis_, ref);
    redis_.with([&](auto& r) {
        r.sadd(kChargingDataActiveSet, ref);
        r.hset(content_key, "supi", supi);
        r.hset(content_key, "reserved_total", "0");
    });
    return ref;
}

bool ChargingDataStore::release(const std::string& ref) {
    // Content (chf:cdr:content:{ref}) is deliberately left behind after release -- a real,
    // disclosed audit trail (which SUPI/how much was reserved for this now-closed session) that
    // the balance-management side's own AdjustBalance/ReserveBalance ledger rows independently
    // corroborate. Only the active-set membership is removed, matching this method's own existing
    // "no longer active" contract.
    return redis_.with([&](auto& r) { return r.srem(kChargingDataActiveSet, ref); }) > 0;
}

bool ChargingDataStore::is_active(const std::string& ref) {
    return redis_.with([&](auto& r) { return r.sismember(kChargingDataActiveSet, ref); });
}

std::optional<std::string> ChargingDataStore::get_supi(const std::string& ref) {
    const auto key = charging_data_content_key(redis_, ref);
    const auto value = redis_.with([&](auto& r) { return r.hget(key, "supi"); });
    if (!value) {
        return std::nullopt;
    }
    return *value;
}

double ChargingDataStore::add_reserved(const std::string& ref, double amount) {
    const auto key = charging_data_content_key(redis_, ref);
    return redis_.with([&](auto& r) { return r.hincrbyfloat(key, "reserved_total", amount); });
}

double ChargingDataStore::get_reserved_total(const std::string& ref) {
    const auto key = charging_data_content_key(redis_, ref);
    const auto value = redis_.with([&](auto& r) { return r.hget(key, "reserved_total"); });
    if (!value) {
        return 0.0;
    }
    return std::stod(*value);
}

// ADR-0304. Same field/key/atomicity discipline as the others.
void ChargingDataStore::add_granted_time(const std::string& ref, double seconds) {
    const auto key = charging_data_content_key(redis_, ref);
    redis_.with([&](auto& r) { r.hincrbyfloat(key, "granted_time", seconds); });
}

double ChargingDataStore::get_granted_time(const std::string& ref) {
    const auto key = charging_data_content_key(redis_, ref);
    const auto value = redis_.with([&](auto& r) { return r.hget(key, "granted_time"); });
    return value ? std::stod(*value) : 0.0;
}

// ADR-0330: set, not increment. The pooling rate is a property of the product this session was
// sold under, so a second Update carrying the same rate must not double it -- unlike the granted_*
// fields above, which genuinely accumulate across quota re-authorisations.
void ChargingDataStore::set_unit_pooling(const std::string& ref,
                                         double octets_per_second,
                                         double octets_per_service_unit) {
    const auto key = charging_data_content_key(redis_, ref);
    redis_.with([&](auto& r) {
        if (octets_per_second > 0.0) {
            r.hset(key, "pool_octets_per_second", std::to_string(octets_per_second));
        }
        if (octets_per_service_unit > 0.0) {
            r.hset(key, "pool_octets_per_service_unit", std::to_string(octets_per_service_unit));
        }
    });
}

std::pair<double, double> ChargingDataStore::get_unit_pooling(const std::string& ref) {
    const auto key = charging_data_content_key(redis_, ref);
    sw::redis::OptionalString per_second;
    sw::redis::OptionalString per_unit;
    redis_.with([&](auto& r) {
        per_second = r.hget(key, "pool_octets_per_second");
        per_unit = r.hget(key, "pool_octets_per_service_unit");
    });
    // A malformed stored value degrades to "not pooled" rather than throwing out of a Release
    // handler: the session still finalises, just on its own dimensions as before this ADR.
    // Generic parameter on purpose: redis++ returns its own sw::redis::Optional, which is not
    // necessarily std::optional, so naming the type here fails to compile against the real client.
    auto parse = [](const auto& v) -> double {
        // sw::redis::Optional exposes operator bool / operator*, not has_value() -- same idiom
        // get_granted_time and its neighbours in this file already use.
        if (!v) {
            return 0.0;
        }
        try {
            return std::stod(*v);
        } catch (const std::exception&) {
            return 0.0;
        }
    };
    return {parse(per_second), parse(per_unit)};
}

// ADR-0297. Same field/key/atomicity discipline as reserved_total above.
void ChargingDataStore::add_granted_volume(const std::string& ref, double octets) {
    const auto key = charging_data_content_key(redis_, ref);
    redis_.with([&](auto& r) { r.hincrbyfloat(key, "granted_volume", octets); });
}

void ChargingDataStore::add_granted_service_units(const std::string& ref, double units) {
    const auto key = charging_data_content_key(redis_, ref);
    redis_.with([&](auto& r) { r.hincrbyfloat(key, "granted_service_units", units); });
}

double ChargingDataStore::get_granted_volume(const std::string& ref) {
    const auto key = charging_data_content_key(redis_, ref);
    const auto value = redis_.with([&](auto& r) { return r.hget(key, "granted_volume"); });
    return value ? std::stod(*value) : 0.0;
}

double ChargingDataStore::get_granted_service_units(const std::string& ref) {
    const auto key = charging_data_content_key(redis_, ref);
    const auto value = redis_.with([&](auto& r) { return r.hget(key, "granted_service_units"); });
    return value ? std::stod(*value) : 0.0;
}

std::string OfflineChargingDataStore::create() {
    const auto id = redis_.with([&](auto& r) { return r.incr(kOfflineNextIdKey); });
    auto ref = "offchg-" + std::to_string(id);
    redis_.with([&](auto& r) { r.sadd(kOfflineActiveSet, ref); });
    return ref;
}

bool OfflineChargingDataStore::release(const std::string& ref) {
    return redis_.with([&](auto& r) { return r.srem(kOfflineActiveSet, ref); }) > 0;
}

bool OfflineChargingDataStore::is_active(const std::string& ref) {
    return redis_.with([&](auto& r) { return r.sismember(kOfflineActiveSet, ref); });
}

std::string SpendingLimitSubscriptionStore::create(sbi_gen::SpendingLimitContext context) {
    const auto id = redis_.with([&](auto& r) { return r.incr(kSpendingLimitNextIdKey); });
    auto sub_id = "sub-" + std::to_string(id);
    const json j = context;
    const auto key = spending_limit_key(redis_, sub_id);
    redis_.with([&](auto& r) {
        r.set(key, j.dump());
        r.sadd(kSpendingLimitActiveSet, sub_id);
    });
    return sub_id;
}

bool SpendingLimitSubscriptionStore::update(const std::string& id,
                                            sbi_gen::SpendingLimitContext context) {
    const auto key = spending_limit_key(redis_, id);
    // Real update-in-place semantics: only set if the subscription already exists. This has the
    // same non-atomic check-then-act shape as ChargingDataStore::is_active's own callers
    // elsewhere in this codebase (e.g. the Update route checking is_active before proceeding) --
    // consistent with this project's existing concurrency-simplification level, not a new gap.
    if (!redis_.with([&](auto& r) { return r.get(key); })) {
        return false;
    }
    const json j = context;
    redis_.with([&](auto& r) { r.set(key, j.dump()); });
    return true;
}

bool SpendingLimitSubscriptionStore::remove(const std::string& id) {
    const auto key = spending_limit_key(redis_, id);
    redis_.with([&](auto& r) { r.srem(kSpendingLimitActiveSet, id); });
    return redis_.with([&](auto& r) { return r.del(key); }) > 0;
}

std::optional<sbi_gen::SpendingLimitContext>
SpendingLimitSubscriptionStore::get(const std::string& id) {
    const auto key = spending_limit_key(redis_, id);
    const auto value = redis_.with([&](auto& r) { return r.get(key); });
    if (!value) {
        return std::nullopt;
    }
    return json::parse(*value).get<sbi_gen::SpendingLimitContext>();
}

std::vector<std::pair<std::string, sbi_gen::SpendingLimitContext>>
SpendingLimitSubscriptionStore::list_all() {
    std::vector<std::string> ids;
    redis_.with([&](auto& r) { r.smembers(kSpendingLimitActiveSet, std::back_inserter(ids)); });
    std::vector<std::pair<std::string, sbi_gen::SpendingLimitContext>> out;
    out.reserve(ids.size());
    for (const auto& id : ids) {
        if (auto context = get(id); context.has_value()) {
            out.emplace_back(id, std::move(*context));
        }
    }
    return out;
}

PolicyCounterConfigStore::PolicyCounterConfigStore(nf_config::RedisRouter redis)
    : redis_(std::move(redis)) {}

void PolicyCounterConfigStore::set_status(const std::string& policy_counter_id,
                                          const std::string& status) {
    const auto key = policy_counter_config_key(policy_counter_id);
    redis_.with([&](auto& r) { r.set(key, status); });
}

std::optional<std::string>
PolicyCounterConfigStore::get_status(const std::string& policy_counter_id) {
    const auto key = policy_counter_config_key(policy_counter_id);
    const auto value = redis_.with([&](auto& r) { return r.get(key); });
    if (!value) {
        return std::nullopt;
    }
    return std::make_optional(*value);
}

std::optional<QuotaHistorySnapshot> QuotaFeatureStore::get(const std::string& supi,
                                                           std::int64_t rating_group) {
    const auto key = quota_feature_key(redis_, supi, rating_group);
    std::vector<sw::redis::OptionalString> values;
    redis_.with(
        [&](auto& r) { r.hmget(key, {"u1", "u2", "u3", "ts", "g"}, std::back_inserter(values)); });
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
    const auto key = quota_feature_key(redis_, supi, rating_group);
    // Shift the rolling window: this request's own used_total_volume becomes the new "most
    // recent" (u1); the old u1/u2 slide down. Real, disclosed non-atomicity: this is a
    // read-then-write (hmget then hset), same concurrency-simplification level as
    // SpendingLimitSubscriptionStore::update's own check-then-act -- acceptable at this project's
    // real lab scale, not claimed to be race-free under concurrent Updates for the same
    // SUPI+ratingGroup.
    std::vector<sw::redis::OptionalString> prior;
    redis_.with([&](auto& r) { r.hmget(key, {"u1", "u2"}, std::back_inserter(prior)); });

    redis_.with([&](auto& r) {
        r.hset(key, "u1", std::to_string(used_total_volume));
        if (prior[0]) {
            r.hset(key, "u2", *prior[0]);
        }
        if (prior[1]) {
            r.hset(key, "u3", *prior[1]);
        }
        r.hset(key, "ts", std::to_string(invocation_unix_sec));
        if (granted_total_volume.has_value()) {
            r.hset(key, "g", std::to_string(*granted_total_volume));
        }
    });
}

} // namespace chf
