#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <sw/redis++/redis++.h>
#include <utility>
#include <vector>

// Private to nfs/nwdaf. ADR-0369: the ML-model state of both logical functions, in Valkey, so
// every replica of either role sees the same thing (ADR-0359: no in-process state).
//
// MTLF side
//   nwdaf:mlprov:sub:<id>      an Nnwdaf_MLModelProvision subscription as received, plus the
//                              consumer's nfInstanceId (the OAuth2 token subject); index
//                              nwdaf:mlprov:subs -- separate from the AnLF's nwdaf:subs so an
//                              MTLF-only instance never serves the AnLF's event subscriptions
//   nwdaf:mlmodel:<event>      the model this MTLF currently provisions for an analytics ID:
//                              modelUniqueId, where it is in the ADRF, its lineage (MLflow run,
//                              data source, sample counts, held-out accuracy)
//   nwdaf:mltrain:<event>      SET NX PX lease: one replica trains an event at a time
//   nwdaf:mlprov:lease:<id>    SET NX PX lease: one replica notifies a subscription per tick
//   nwdaf:mlmon:reg:<id>       an Nnwdaf_MLModelMonitor registration (an AnLF using a model),
//                              plus the subscription this MTLF opened at that AnLF; index
//                              nwdaf:mlmon:regs (ADR-0370)
//   nwdaf:mldegraded:<event>   the accuracy notification that declared the current model
//                              degraded -- the training loop re-trains on it (ADR-0370)
//   nwdaf:mlstoragesub:<event> the transRefId of the ADRF storage subscription feeding the
//                              training data set (reused across MTLF restarts)
// AnLF side
//   nwdaf:anlf:mlsub:<event>   the ML-model subscription ONE replica holds at the MTLF, with
//                              :alive as the holder's heartbeat (same takeover rule as the
//                              collection at the DCCF, collection_store.hpp)
//   nwdaf:anlf:model:<event>   the model the AnLF uses for that analytics ID, as the MTLF's
//                              notification described it; every replica loads its bytes from
//                              the ADRF on demand and caches the ONNX session in-process
//   nwdaf:next_id              the shared INCR counter (modelUniqueId comes from it, so it is
//                              unique across MTLF replicas with no coordinator)

namespace nwdaf {

class MlStore {
public:
    explicit MlStore(std::shared_ptr<sw::redis::Redis> redis) : redis_(std::move(redis)) {}

    std::int64_t next_number();
    std::string next_id(const char* prefix);

    // MTLF: provision subscriptions.
    std::string create_provision_subscription(const nlohmann::json& record);
    std::optional<nlohmann::json> get_provision_subscription(const std::string& id);
    bool replace_provision_subscription(const std::string& id, const nlohmann::json& record);
    bool remove_provision_subscription(const std::string& id);
    std::vector<std::pair<std::string, nlohmann::json>> all_provision_subscriptions();

    // MTLF: one replica delivers a subscription's notifications per loop tick (ADR-0365's
    // rule for the AnLF notifier, applied here): SET NX PX on nwdaf:mlprov:lease:<id>.
    bool claim_delivery(const std::string& id, std::chrono::milliseconds ttl);

    // MTLF: the provisioned model per event.
    std::optional<nlohmann::json> get_model(const std::string& event);
    void put_model(const std::string& event, const nlohmann::json& record);
    bool acquire_training_lease(const std::string& event, std::chrono::milliseconds ttl);
    void release_training_lease(const std::string& event);
    // The ADRF storage subscription (transRefId) that keeps the training data flowing.
    std::optional<std::string> get_storage_subscription(const std::string& event);
    void put_storage_subscription(const std::string& event, const std::string& trans_ref_id);

    // MTLF: monitoring registrations (ADR-0370).
    std::string create_registration(const nlohmann::json& record);
    std::optional<nlohmann::json> get_registration(const std::string& id);
    void put_registration(const std::string& id, const nlohmann::json& record);
    bool remove_registration(const std::string& id);
    std::vector<std::pair<std::string, nlohmann::json>> all_registrations();
    std::optional<nlohmann::json> get_degraded(const std::string& event);
    void put_degraded(const std::string& event, const nlohmann::json& notif);
    void clear_degraded(const std::string& event);

    // AnLF: the subscription holder and the active model.
    bool open_holder(const std::string& key, const nlohmann::json& record);
    std::optional<nlohmann::json> get_holder(const std::string& key);
    void close_holder(const std::string& key);
    void touch_holder(const std::string& key, std::chrono::milliseconds ttl);
    bool holder_alive(const std::string& key);
    std::optional<nlohmann::json> get_active_model(const std::string& event);
    void put_active_model(const std::string& event, const nlohmann::json& record);

private:
    std::shared_ptr<sw::redis::Redis> redis_;
};

} // namespace nwdaf
