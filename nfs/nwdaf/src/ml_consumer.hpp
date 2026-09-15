#pragma once

// The AnLF as a consumer of Nnwdaf_MLModelProvision (TS 23.288 6.2A.1, 6.2B.7; ADR-0369).
// Private to nfs/nwdaf; wired by main.cpp when `role` is anlf or both and an MTLF is configured.
//
//   * ONE replica subscribes at the NWDAF containing MTLF for each analytics ID this AnLF
//     computes with a model (Valkey SET NX on nwdaf:anlf:mlsub:<event>, heartbeat, takeover --
//     the collection-holder rule of ADR-0368), with this NWDAF's own inbound URI as notifUri;
//     a graceful shutdown of the holder unsubscribes.
//   * Every replica handles Nnwdaf_MLModelProvision_Notify: the notified model becomes the
//     active model for its event in Valkey (nwdaf:anlf:model:<event>), so every replica infers
//     with the same model.
//   * Every replica retrieves the model bytes from the ADRF on first use (the file address the
//     MTLF notified -- Nadrf_MLModelManagement_RetrievalRequest, 6.2B.7) and keeps the ONNX
//     session in-process; a new active model replaces it.
//   * ADR-0370: the holder registers the model it uses at the MTLF (Nnwdaf_MLModelMonitor_
//     Register, 6.2E.3.2) -- once per model, deregistering the previous one and on shutdown --
//     and every prediction is logged with the AccuracyMonitor so the AnLF can judge it later.

#include "sbi_core/http2_client.hpp"
#include "sbi_core/oauth2_client.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "accuracy_monitor.hpp"
#include "ml_store.hpp"
#include "model_runtime.hpp"

namespace nwdaf {

struct MlConsumerOptions {
    std::string instance_id;
    std::string nrf_base;      // for the monitor-registration token
    std::string mtlf_base_url; // "" -> no MTLF: statistics only, never a guess
    std::string notif_uri;     // this NWDAF's inbound URI for the MTLF's notifications
    std::vector<std::string> events;
    std::vector<std::string> nf_types;   // mLEventFilter.nfTypes, when non-empty
    std::int64_t accuracy_threshold = 0; // mlEvRepCon.mlAccuracyThreshold (percent)
    std::int64_t holder_heartbeat_seconds = 0;
    std::int64_t retry_seconds = 0;
};

class MlConsumer {
public:
    MlConsumer(MlConsumerOptions options,
               sbi_core::http2::Client& client,
               std::mutex& client_mutex,
               sbi_core::OAuth2Client& oauth_mtlf,
               sbi_core::OAuth2Client& oauth_adrf_ml,
               MlStore& store);

    bool enabled() const { return !options_.mtlf_base_url.empty(); }
    // The subscription-holder loop; returns when `running` clears (unsubscribing if holding).
    void run(std::atomic<bool>& running, const std::function<bool(std::int64_t)>& pause);
    // Nnwdaf_MLModelProvision_Notify body (array of NwdafMLModelProvNotif). Returns the number
    // of models taken into use.
    int on_notification(const nlohmann::json& body);
    // The active model for an event, as notified (modelUniqueId, fileUrl, accuracy...).
    std::optional<nlohmann::json> active_model(const std::string& event);
    // One prediction with the active model, loading its bytes from the ADRF on first use;
    // logged for accuracy monitoring when a monitor is attached.
    std::optional<double> predict(const std::string& event,
                                  const std::string& nf_instance_id,
                                  const NfLoadFeatures& features);
    void attach_monitor(AccuracyMonitor* monitor) { monitor_ = monitor; }

private:
    bool subscribe(const std::string& event, std::string& resource_uri);
    void unsubscribe(const std::string& resource_uri);
    std::optional<std::string> register_use(const std::string& event, std::int64_t model_id);
    void deregister(const std::string& registration_uri);
    bool ensure_loaded(const std::string& event, const nlohmann::json& model);

    MlConsumerOptions options_;
    sbi_core::http2::Client& client_;
    std::mutex& client_mutex_;
    sbi_core::OAuth2Client& oauth_mtlf_;
    sbi_core::OAuth2Client& oauth_adrf_ml_;
    MlStore& store_;
    sbi_core::OAuth2Client oauth_monitor_;
    AccuracyMonitor* monitor_ = nullptr;
    int subscribe_failures_ = 0;
    std::mutex runtimes_mutex_;
    std::map<std::string, std::unique_ptr<ModelRuntime>> runtimes_; // per event, in-process cache
};

} // namespace nwdaf
