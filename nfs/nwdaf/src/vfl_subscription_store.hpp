#pragma once

#include <nlohmann/json.hpp>

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

// In-memory store for the NWDAF's VFL (vertical federated learning) subscriptions -- ADR-0380, the
// VFL hook. Backs the Nnwdaf_VFLTraining and Nnwdaf_VFLInference subscription resources
// (TS 29.520). One instance per resource family. Stores the subscription body as received (JSON),
// the same assign-id / get / replace / remove shape as the SMF's EventSubscriptionStore. In-memory
// only, no persistence across restarts -- a disclosed lab simplification, and the VFL coordination
// itself (multi-party model exchange between MTLFs) is Phase D, not implemented here; this is the
// SBI seam where it will attach.

namespace nwdaf {

class VflSubscriptionStore {
public:
    explicit VflSubscriptionStore(std::string id_prefix) : prefix_(std::move(id_prefix)) {}

    std::string create(nlohmann::json subscription);
    std::optional<nlohmann::json> get(const std::string& sub_id);
    bool replace(const std::string& sub_id, nlohmann::json subscription);
    // Applies an RFC 7386 JSON merge patch to an existing subscription; returns the merged body.
    std::optional<nlohmann::json> merge_patch(const std::string& sub_id,
                                              const nlohmann::json& patch);
    bool remove(const std::string& sub_id);

private:
    std::mutex mutex_;
    std::string prefix_;
    std::unordered_map<std::string, nlohmann::json> subscriptions_;
    std::uint64_t next_id_ = 1;
};

} // namespace nwdaf
