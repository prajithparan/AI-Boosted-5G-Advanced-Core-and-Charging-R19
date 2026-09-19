#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Private to nfs/smf -- not shared with any other NF, per CLAUDE.md's "no NF includes another
// NF's private headers" rule.
//
// Backs Nsmf_EventExposure's /subscriptions collection (ADR-0201): CreateIndividualSubcription
// assigns a server-generated subId; Get/Replace/DeleteIndividualSubcription look it up by that
// ref. Same real assign-id/store/remove shape as sm_context_store.hpp's SmContextStore, kept as a
// separate type since this is a genuinely distinct resource (an event subscription, not an SM
// context) -- same "distinct resource, not a rename" precedent already used elsewhere in this
// project (e.g. nfs/udr's OperatorSpecificDataStore vs PolicyOperatorSpecificDataStore). In-memory
// only, no persistence across restarts -- same disclosed simplification as SmContextStore.

namespace smf {

class EventSubscriptionStore {
public:
    // Assigns and returns a new subId, stores the subscription under it.
    std::string create(nlohmann::json subscription);

    std::optional<nlohmann::json> get(const std::string& sub_id);

    // Replaces the stored subscription for an existing subId. Returns false if it doesn't exist.
    bool update(const std::string& sub_id, nlohmann::json subscription);

    bool remove(const std::string& sub_id);

    // A copy of every stored subscription, for the QOS_MON producer to iterate (ADR-0379).
    std::vector<nlohmann::json> snapshot();

private:
    std::mutex mutex_;
    std::unordered_map<std::string, nlohmann::json> subscriptions_;
    std::uint64_t next_id_ = 1;
};

} // namespace smf
