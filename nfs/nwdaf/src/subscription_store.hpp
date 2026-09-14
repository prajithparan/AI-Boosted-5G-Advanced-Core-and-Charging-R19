#pragma once

#include <memory>
#include <optional>
#include <string>
#include <sw/redis++/redis++.h>
#include <utility>
#include <vector>

#include "TS26510_CommonData_grp.hpp"

// Private to nfs/nwdaf. ADR-0360: Nnwdaf_EventsSubscription state lives in Valkey, not in the
// process, so that N AnLF instances behind the NRF are interchangeable -- a subscription created on
// one instance can be updated, deleted and notified by any other. Phase A (ADR-0358) held this in
// an in-process map; ADR-0359 named replacing it the precondition for every other NWDAF function.
//
// Same "hot-path state, not a system-of-record" pattern as the AMF's and CHF's Valkey stores.
// Keys: nwdaf:sub:<id> and nwdaf:transfer:<id> hold the resource JSON exactly as the YAML shapes
// it; nwdaf:subs / nwdaf:transfers are the index sets the notifier enumerates (never KEYS);
// nwdaf:next_id is an INCR counter, which is what makes IDs unique across replicas without a
// coordinator.

namespace nwdaf {

class SubscriptionStore {
public:
    explicit SubscriptionStore(std::shared_ptr<sw::redis::Redis> redis)
        : redis_(std::move(redis)) {}

    // Returns the new resource id ("nwdaf-sub-<n>").
    std::string create_subscription(const sbi_gen::NnwdafEventsSubscription& sub);
    std::optional<sbi_gen::NnwdafEventsSubscription> get_subscription(const std::string& id);
    // false when no such subscription exists (the PUT is then a 404, never an upsert).
    bool replace_subscription(const std::string& id, const sbi_gen::NnwdafEventsSubscription& sub);
    bool remove_subscription(const std::string& id);
    // Every subscription, for the notifier. A snapshot: the map may change under it, which is fine
    // -- a subscription deleted after the snapshot gets at most one more notification.
    std::vector<std::pair<std::string, sbi_gen::NnwdafEventsSubscription>> all_subscriptions();

    std::string create_transfer(const sbi_gen::AnalyticsSubscriptionsTransfer& transfer);
    bool replace_transfer(const std::string& id,
                          const sbi_gen::AnalyticsSubscriptionsTransfer& transfer);
    bool remove_transfer(const std::string& id);

private:
    std::string next_id(const char* prefix);
    std::shared_ptr<sw::redis::Redis> redis_;
};

} // namespace nwdaf
