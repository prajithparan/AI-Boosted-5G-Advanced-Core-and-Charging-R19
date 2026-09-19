#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Private to nfs/smf -- not shared with any other NF, per CLAUDE.md's "no NF includes another NF's
// private headers" rule.
//
// Backs the /sm-contexts collection (TS29502_Nsmf_PDUSession.yaml): CreateSMContext assigns a new
// smContextRef (SMF generates it, unlike AMF's ueContextId which is client-supplied -- see
// PostSmContexts' Location header: "{apiRoot}/nsmf-pdusession/<apiVersion>/sm-contexts/
// {smContextRef}"); RetrieveSMContext/UpdateSMContext/ReleaseSMContext look it up by that
// server-assigned ref. In-memory only, no persistence across restarts -- same disclosed
// simplification as nfs/nrf/src/registry.hpp (ADR-0015) and nfs/amf/src/ue_context_store.hpp.

namespace smf {

class SmContextStore {
public:
    // Assigns and returns a new smContextRef, stores context under it.
    std::string create(nlohmann::json context);

    std::optional<nlohmann::json> get(const std::string& sm_context_ref);

    // Replaces the stored context for an existing ref. Returns false if the ref doesn't exist.
    bool update(const std::string& sm_context_ref, nlohmann::json context);

    bool remove(const std::string& sm_context_ref);

    // A copy of every live SM context, for the QOS_MON producer to iterate (ADR-0379).
    std::vector<nlohmann::json> snapshot();

private:
    std::mutex mutex_;
    std::unordered_map<std::string, nlohmann::json> contexts_;
    std::uint64_t next_id_ = 1;
};

} // namespace smf
