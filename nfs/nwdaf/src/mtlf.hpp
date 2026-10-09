#pragma once

// The NWDAF containing MTLF -- TS 23.288 5.1 / 6.2A / 6.2E.2, TS 29.520 4.5 (ADR-0369).
// Private to nfs/nwdaf; wired by main.cpp when `role` is mtlf or both.
//
// What it does, in the spec's terms:
//   * serves Nnwdaf_MLModelProvision (subscribe / modify / unsubscribe / notify) for the
//     analytics IDs it trains models for (config `mtlf.events`);
//   * trains through a TrainingExecutor (training_executor.hpp) on the data the ADRF holds --
//     the MTLF asks the ADRF to store the AnLF's NRF-status data (6.2E.2 step 8, 6.2B.3:
//     Nadrf_DataManagement_StorageSubscriptionRequest towards the NWDAF containing AnLF, tagged
//     with a DataSetTag) and retrieves it by that data set (Nadrf_DataManagement_RetrievalRequest);
//   * stores every trained model through Nadrf_MLModelManagement (6.2B.5), allowing the
//     subscribed consumers to retrieve it (allowConsumerList = the token subjects of the
//     subscriptions), and notifies them with the ADRF (Set) information, the file address and the
//     unique model id (6.2A.2, 4.5.2.4.2), modelUpdateInd on a re-trained model;
//   * re-trains when the data set has grown by `retrain_min_new_windows` usable windows since
//     the last training or `retrain_interval_seconds` elapsed (6.2A.1: "may determine whether
//     triggering further training ... is needed"), or when an AnLF reported the model's
//     accuracy below threshold (ADR-0370);
//   * serves the MTLF half of Nnwdaf_MLModelMonitor (ADR-0370, TS 23.288 6.2E.3, TS 29.520
//     4.7.2.2-4.7.2.3): an AnLF registers the model it uses; the MTLF resolves that AnLF's
//     monitor endpoint from its NRF profile (nfServices[].ipEndPoints) and subscribes there for
//     accuracy notifications (4.7.2.4), delivered to {self}/nwdaf-inbound/v1/ml-monitor;
//     accuMeetInd=false / mlModelAcc below `accuracy_threshold` marks the model degraded
//     (6.2E.3.3 step 8) and the loop re-trains it (step 9), after `retrain_cooldown_seconds`.
//
// One replica trains an event at a time (a Valkey lease); every replica serves subscriptions
// and notifies from the shared model record (ADR-0359: no in-process state).

#include "sbi_core/http2_client.hpp"
#include "sbi_core/http2_server.hpp"
#include "sbi_core/jwt.hpp"
#include "sbi_core/oauth2_client.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "fl_client.hpp"
#include "fl_server.hpp"
#include "ml_store.hpp"
#include "training_executor.hpp"

namespace nwdaf {

struct MtlfOptions {
    std::string instance_id;
    std::string nrf_base;
    std::string self_base;           // where AnLFs deliver accuracy notifications
    std::string adrf_base_url;       // "" -> discovered at the NRF (nfType ADRF)
    std::vector<std::string> events; // analytics IDs this MTLF trains for
    std::string data_set_id;         // the ADRF DataSetTag the training data is kept under
    std::int64_t min_samples = 0;
    double accuracy_tolerance = 0;
    std::int64_t check_interval_seconds = 0;
    std::int64_t retrain_min_new_windows = 0;
    std::int64_t retrain_interval_seconds = 0; // 0: never on time alone
    std::int64_t training_lease_seconds = 0;
    std::int64_t accuracy_threshold = 0;       // percent; below it a model is degraded
    std::int64_t retrain_cooldown_seconds = 0; // between accuracy-triggered re-trainings
};

class Mtlf {
public:
    Mtlf(MtlfOptions options,
         sbi_core::http2::Client& client,
         std::mutex& client_mutex,
         sbi_core::OAuth2Client& oauth_disc,
         sbi_core::OAuth2Client& oauth_adrf_dm,
         sbi_core::OAuth2Client& oauth_adrf_ml,
         sbi_core::jwt::Verifier& verifier,
         MlStore& store,
         TrainingExecutor& executor);

    // Enables the federated-learning client (ADR-0471 increment 3). Without it a subscription with
    // an mlCorreId is handled like any training subscription (the trained model is notified).
    void set_fl_client(FlRoundExecutor& executor, FlClientOptions options) {
        fl_executor_ = &executor;
        fl_options_ = options;
    }

    // Enables the federated-learning server (ADR-0471 increment 4): the federations of the
    // config run on their own thread once run() starts.
    struct FlServerOptions {
        std::vector<FlFederation> federations;
        long round_grace_seconds = 0;     // wait beyond maxResTime for the clients' notifications
        long repeat_interval_seconds = 0; // 0 = each federation runs once per process
    };
    void set_fl_server(FlAggregator& aggregator, FlServerOptions options) {
        fl_aggregator_ = &aggregator;
        fl_server_options_ = std::move(options);
    }

    void install_routes(sbi_core::http2::Server& server);
    // Nnwdaf_MLModelTraining subscription CRUD (ADR-0471); called by install_routes.
    void install_training_routes(sbi_core::http2::Server& server);
    // What this role adds to the NRF profile's nwdafInfo (TS 29.510 MlAnalyticsInfo).
    nlohmann::json nrf_profile_info() const;
    // The training / notification loop; returns when `running` clears. `pause(seconds)` sleeps
    // in shutdown-aware slices and returns false when the NF is stopping.
    void run(std::atomic<bool>& running, const std::function<bool(std::int64_t)>& pause);

private:
    struct Call {
        long status = -1;
        std::string body;
        std::string location;
        std::string error;
    };
    Call call(sbi_core::OAuth2Client& oauth,
              const std::string& method,
              const std::string& url,
              const nlohmann::json* body,
              const std::optional<std::string>& callback = std::nullopt);
    std::optional<std::string> adrf_base();
    std::optional<std::string> adrf_instance_id();
    std::optional<std::string> anlf_instance_id(const std::string& event);
    bool ensure_storage_subscription(const std::string& event);
    nlohmann::json training_dataset(const std::string& event, std::int64_t& windows);
    bool train(const std::string& event, const char* why);
    bool reconcile_consumers(const std::string& event, const nlohmann::json& model);
    void deliver(const std::string& sub_id, nlohmann::json sub);
    // Nnwdaf_MLModelTraining_Notify with the trained model (ADR-0471, increment 2).
    void deliver_training(const std::string& sub_id, nlohmann::json sub);
    // One FL round for a subscription carrying an mlCorreId, once per roundInd (fl_client.hpp).
    void deliver_fl_round(const std::string& sub_id, nlohmann::json sub);
    // One federation, rounds 1..N (TS 23.288 6.2C.2.2 steps 0-9). Returns a one-line outcome.
    std::string run_federation(const FlFederation& fed, std::atomic<bool>& running);
    void fl_server_loop(std::atomic<bool>& running);
    // The immediate report of a training subscription (immReport), when asked and available.
    nlohmann::json training_representation(const nlohmann::json& sub) const;
    nlohmann::json event_notif(const std::string& event,
                               const nlohmann::json& model,
                               const nlohmann::json& sub,
                               bool update) const;
    bool trains(const std::string& event) const;
    std::optional<std::string> anlf_monitor_base(const std::string& nf_instance_id);
    void reconcile_registrations();
    void unsubscribe_monitor(const nlohmann::json& registration);
    void on_monitor_notification(const nlohmann::json& body);

    MtlfOptions options_;
    sbi_core::http2::Client& client_;
    std::mutex& client_mutex_;
    sbi_core::OAuth2Client& oauth_disc_;
    sbi_core::OAuth2Client& oauth_adrf_dm_;
    sbi_core::OAuth2Client& oauth_adrf_ml_;
    sbi_core::jwt::Verifier& verifier_;
    MlStore& store_;
    TrainingExecutor& executor_;
    FlRoundExecutor* fl_executor_ = nullptr;
    FlClientOptions fl_options_;
    FlAggregator* fl_aggregator_ = nullptr;
    FlServerOptions fl_server_options_;
    sbi_core::OAuth2Client oauth_fl_client_;
    // Notification slots of the round in progress, keyed by the per-client notifCorreId that is
    // also the callback path segment (a notification carries no client identity of its own).
    struct FlSlot {
        int round = 0;
        std::string notif_corre_id;
        FlFederation fed;
        std::optional<tl::expected<FlUpdate, std::string>> result;
    };
    std::mutex fl_mutex_;
    std::condition_variable fl_cv_;
    std::map<std::string, FlSlot> fl_slots_;
    std::string adrf_base_cached_;
    std::string adrf_id_cached_;
    sbi_core::OAuth2Client oauth_anlf_monitor_;
};

} // namespace nwdaf
