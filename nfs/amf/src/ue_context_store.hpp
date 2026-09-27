#pragma once

#include <nlohmann/json.hpp>

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

// Private to nfs/amf -- not shared with any other NF, per CLAUDE.md's "no NF includes another NF's
// private headers" rule.
//
// Backs the four per-ueContextId Namf_Communication operations implemented so far
// (ReleaseUEContext, EBIAssignment, UEContextTransfer, RegistrationStatusUpdate --
// TS29518_Namf_Communication.yaml). In-memory only, no persistence across restarts -- same
// disclosed simplification as nfs/nrf/src/registry.hpp (see docs/DECISIONS.md ADR-0015).
//
// UPDATE (ADR-0249/ADR-0393): this is stale -- ngap_task.cpp's own registration procedure has
// called put() (keyed by SUPI, storing SM context refs and the PCF AM Policy Association) since
// ADR-0249, and handle_uplink_nas_transport_deregistration calls remove() on it. CreateUEContext
// itself (the Namf_Communication operation this store's own four SBI handlers were originally
// built for) is UNRELATED to those SUPI-keyed puts -- it is still not implemented, for the
// original reason below, so the "no such UE context" (404) branch for THOSE four operations
// specifically is still the only one reachable in practice; this store is no longer empty, it is
// just never populated the way those four operations expect.
//
// CreateUEContext (the operation that would populate this store keyed by ueContextId, for real)
// requires multipart/related request bodies, which libs/sbi-core does not support yet -- deferred
// per docs/DECISIONS.md ADR-0016's multipart discussion.

namespace amf {

class UeContextStore {
public:
    void put(const std::string& ue_context_id, nlohmann::json context);
    std::optional<nlohmann::json> get(const std::string& ue_context_id);
    bool remove(const std::string& ue_context_id);

private:
    std::mutex mutex_;
    std::unordered_map<std::string, nlohmann::json> contexts_;
};

} // namespace amf
