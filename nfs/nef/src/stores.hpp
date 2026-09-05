#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Private to nfs/nef -- not shared with any other NF, per CLAUDE.md's "no NF includes another
// NF's private headers" rule. In-memory only, no persistence across restarts -- same disclosed
// simplification as every other NF's store built so far.

namespace nef {

// Backs Nnef_PFDmanagement's `/applications`/`/applications/{appId}`/`/applications/partialpull`
// GET-only resource family. Keyed by `applicationId`. Value is the raw `PfdDataForApp` (TS29551)
// as JSON. Real, disclosed: this YAML has no operation anywhere that lets a caller WRITE PFD
// content into NEF (the real 3GPP AF-to-NEF PFD provisioning path is out of 3GPP's own SBI
// framework scope, not just unbuilt here) -- so this store is seed()-only, same precedent as
// several of UDR's own real "no live write path exists" stores (e.g. `SponsorConnectivityDataStore`
// before ADR-0182's own turn).
class PfdCatalogStore {
public:
    void seed(const std::string& application_id, nlohmann::json pfd_data_for_app);
    std::optional<nlohmann::json> get(const std::string& application_id);
    std::vector<nlohmann::json> get_many(const std::vector<std::string>& application_ids);
    // Real AppFetchPartialUpdate semantics: returns the stored PfdDataForApp only if its own
    // `pfdTimestamp` is later than `since` (ISO8601 UTC strings, lexicographically comparable in
    // this project's own generated DateTime format -- same real, disclosed string-compare
    // precedent used elsewhere), else nullopt ("not changed").
    std::optional<nlohmann::json> get_if_changed_since(const std::string& application_id,
                                                       const std::optional<std::string>& since);

private:
    std::mutex mutex_;
    std::unordered_map<std::string, nlohmann::json> catalog_;
};

// Backs Nnef_PFDmanagement's `/subscriptions` collection + `/subscriptions/{subscriptionId}`
// individual resource. Keyed by an NEF-generated subscriptionId. Value is the raw
// `PfdSubscription` (TS29551) as JSON.
class PfdSubscriptionStore {
public:
    std::string create(nlohmann::json subscription);
    std::optional<nlohmann::json> get(const std::string& sub_id);
    bool put(const std::string& sub_id, nlohmann::json subscription);
    bool remove(const std::string& sub_id);

private:
    std::mutex mutex_;
    std::unordered_map<std::string, nlohmann::json> subscriptions_;
    std::uint64_t next_id_ = 1;
};

// ADR-0207 (gap-closure task #164, first NEF slice). Backs Nnef_DNAIMapping's individual DNAI
// Mapping Subscription resource (TS29591 paths + TS29522_DNAIMapping.yaml schemas) -- keyed by an
// NEF-generated subscriptionId. Same real create/get/remove shape as `PfdSubscriptionStore`
// above, deliberately re-implemented as its own class (not shared) since each real resource type
// has its own real id namespace/lifecycle.
class DnaiMapSubStore {
public:
    std::string create(nlohmann::json subscription);
    std::optional<nlohmann::json> get(const std::string& sub_id);
    bool remove(const std::string& sub_id);

private:
    std::mutex mutex_;
    std::unordered_map<std::string, nlohmann::json> subscriptions_;
    std::uint64_t next_id_ = 1;
};

// ADR-0207 (gap-closure task #164, first NEF slice). Backs Nnef_EASDeployment's individual EAS
// Deployment Event Subscription resource (TS29591). Keyed by an NEF-generated subscriptionId.
class EasDeploySubStore {
public:
    std::string create(nlohmann::json subscription);
    std::optional<nlohmann::json> get(const std::string& sub_id);
    bool remove(const std::string& sub_id);

private:
    std::mutex mutex_;
    std::unordered_map<std::string, nlohmann::json> subscriptions_;
    std::uint64_t next_id_ = 1;
};

// ADR-0208 (gap-closure task #164, second NEF slice). Backs Nnef_SMContext's Individual SM
// Context resource (TS29541) -- keyed by an NEF-generated smContextId. `update` applies the real
// `SmContextUpdateData` fields present in the request on top of the stored representation (all
// its own fields are optional per the real YAML, i.e. a partial update), not a full replace.
class SmContextStore {
public:
    std::string create(nlohmann::json created_data);
    std::optional<nlohmann::json> get(const std::string& sm_context_id);
    bool update(const std::string& sm_context_id, const nlohmann::json& partial_update);
    bool remove(const std::string& sm_context_id);

private:
    std::mutex mutex_;
    std::unordered_map<std::string, nlohmann::json> contexts_;
    std::uint64_t next_id_ = 1;
};

// ADR-0208 (gap-closure task #164, second NEF slice). Backs Nnef_ECSAddress's Individual ECS
// Address Configuration Information Subscription resource (TS29591). `patch` applies a real RFC
// 7396 JSON Merge Patch (matches the YAML's own declared `application/merge-patch+json` request
// content type for `ModifyIndividualSubcription` exactly -- nlohmann::json's own `merge_patch`
// implements RFC 7396 natively, not a hand-rolled approximation).
class EcsAddrCfgInfoSubStore {
public:
    std::string create(nlohmann::json subscription);
    std::optional<nlohmann::json> get(const std::string& sub_id);
    bool put(const std::string& sub_id, nlohmann::json subscription);
    bool patch(const std::string& sub_id, const nlohmann::json& merge_patch);
    bool remove(const std::string& sub_id);

private:
    std::mutex mutex_;
    std::unordered_map<std::string, nlohmann::json> subscriptions_;
    std::uint64_t next_id_ = 1;
};

// ADR-0209 (gap-closure task #164, third NEF slice). Backs Nnef_EventExposure's Individual
// Network Exposure Event Subscription resource (TS29591). Same real create/get/put/remove shape
// as `PfdSubscriptionStore` -- no PATCH exists on this real resource (only Create/Get/Replace/
// Delete per its own YAML).
class NefEventExposureSubStore {
public:
    std::string create(nlohmann::json subscription);
    std::optional<nlohmann::json> get(const std::string& sub_id);
    bool put(const std::string& sub_id, nlohmann::json subscription);
    bool remove(const std::string& sub_id);

private:
    std::mutex mutex_;
    std::unordered_map<std::string, nlohmann::json> subscriptions_;
    std::uint64_t next_id_ = 1;
};

// ADR-0210 (gap-closure task #164, fourth and final NEF slice). Backs Nnef_TrafficInfluenceData's
// Individual Traffic Influence Subscription resource (TS29591). Same real create/get/put/remove
// shape as `PfdSubscriptionStore` -- no PATCH exists on this real resource.
class TrafficInfluDataSubStore {
public:
    std::string create(nlohmann::json subscription);
    std::optional<nlohmann::json> get(const std::string& sub_id);
    bool put(const std::string& sub_id, nlohmann::json subscription);
    bool remove(const std::string& sub_id);

private:
    std::mutex mutex_;
    std::unordered_map<std::string, nlohmann::json> subscriptions_;
    std::uint64_t next_id_ = 1;
};

// Shared real create/get/put/patch(RFC 7396 merge-patch)/remove shape backing
// Nnef_Inference/Nnef_Training/Nnef_VFLInference/Nnef_VFLTraining's own Individual Subscription
// resources -- each deliberately its own class (not templated/shared) since each real resource has
// its own real id namespace/lifecycle, same precedent as every other NEF subscription store.
// ADR-0302: the AF-facing TS 29.522 TrafficInfluence subscriptions, keyed by "{afId}/{subId}" so
// one AF cannot read or delete another's -- the resource is genuinely per-AF in the spec's own
// path, and a flat id namespace would quietly make it global.
class AfTrafficInfluenceSubStore {
public:
    std::string create(const std::string& af_id, nlohmann::json subscription);
    std::optional<nlohmann::json> get(const std::string& af_id, const std::string& sub_id);
    std::vector<nlohmann::json> list(const std::string& af_id);
    bool put(const std::string& af_id, const std::string& sub_id, nlohmann::json subscription);
    std::optional<nlohmann::json>
    merge_patch(const std::string& af_id, const std::string& sub_id, const nlohmann::json& patch);
    bool remove(const std::string& af_id, const std::string& sub_id);

private:
    static std::string key(const std::string& af_id, const std::string& sub_id) {
        return af_id + "/" + sub_id;
    }
    std::mutex mutex_;
    std::unordered_map<std::string, nlohmann::json> subscriptions_;
    std::uint64_t next_id_ = 1;
};

class InferEventSubStore {
public:
    std::string create(nlohmann::json subscription);
    std::optional<nlohmann::json> get(const std::string& sub_id);
    bool put(const std::string& sub_id, nlohmann::json subscription);
    bool patch(const std::string& sub_id, const nlohmann::json& merge_patch);
    bool remove(const std::string& sub_id);

private:
    std::mutex mutex_;
    std::unordered_map<std::string, nlohmann::json> subscriptions_;
    std::uint64_t next_id_ = 1;
};

class TrainEventsSubStore {
public:
    std::string create(nlohmann::json subscription);
    std::optional<nlohmann::json> get(const std::string& sub_id);
    bool put(const std::string& sub_id, nlohmann::json subscription);
    bool patch(const std::string& sub_id, const nlohmann::json& merge_patch);
    bool remove(const std::string& sub_id);

private:
    std::mutex mutex_;
    std::unordered_map<std::string, nlohmann::json> subscriptions_;
    std::uint64_t next_id_ = 1;
};

class VflInferSubStore {
public:
    std::string create(nlohmann::json subscription);
    std::optional<nlohmann::json> get(const std::string& sub_id);
    bool put(const std::string& sub_id, nlohmann::json subscription);
    bool patch(const std::string& sub_id, const nlohmann::json& merge_patch);
    bool remove(const std::string& sub_id);

private:
    std::mutex mutex_;
    std::unordered_map<std::string, nlohmann::json> subscriptions_;
    std::uint64_t next_id_ = 1;
};

class NefVflTrainSubStore {
public:
    std::string create(nlohmann::json subscription);
    std::optional<nlohmann::json> get(const std::string& sub_id);
    bool put(const std::string& sub_id, nlohmann::json subscription);
    bool patch(const std::string& sub_id, const nlohmann::json& merge_patch);
    bool remove(const std::string& sub_id);

private:
    std::mutex mutex_;
    std::unordered_map<std::string, nlohmann::json> subscriptions_;
    std::uint64_t next_id_ = 1;
};

} // namespace nef
