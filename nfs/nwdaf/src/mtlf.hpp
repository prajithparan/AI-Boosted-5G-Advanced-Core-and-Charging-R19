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
//     triggering further training ... is needed") -- ADR-0370 adds the accuracy-monitoring
//     trigger.
//
// One replica trains an event at a time (a Valkey lease); every replica serves subscriptions
// and notifies from the shared model record (ADR-0359: no in-process state).

#include "sbi_core/http2_client.hpp"
#include "sbi_core/http2_server.hpp"
#include "sbi_core/jwt.hpp"
#include "sbi_core/oauth2_client.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "ml_store.hpp"
#include "training_executor.hpp"

namespace nwdaf {

struct MtlfOptions {
    std::string instance_id;
    std::string nrf_base;
    std::string adrf_base_url;       // "" -> discovered at the NRF (nfType ADRF)
    std::vector<std::string> events; // analytics IDs this MTLF trains for
    std::string data_set_id;         // the ADRF DataSetTag the training data is kept under
    std::int64_t min_samples = 0;
    double accuracy_tolerance = 0;
    std::int64_t check_interval_seconds = 0;
    std::int64_t retrain_min_new_windows = 0;
    std::int64_t retrain_interval_seconds = 0; // 0: never on time alone
    std::int64_t training_lease_seconds = 0;
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

    void install_routes(sbi_core::http2::Server& server);
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
    nlohmann::json event_notif(const std::string& event,
                               const nlohmann::json& model,
                               const nlohmann::json& sub,
                               bool update) const;
    bool trains(const std::string& event) const;

    MtlfOptions options_;
    sbi_core::http2::Client& client_;
    std::mutex& client_mutex_;
    sbi_core::OAuth2Client& oauth_disc_;
    sbi_core::OAuth2Client& oauth_adrf_dm_;
    sbi_core::OAuth2Client& oauth_adrf_ml_;
    sbi_core::jwt::Verifier& verifier_;
    MlStore& store_;
    TrainingExecutor& executor_;
    std::string adrf_base_cached_;
    std::string adrf_id_cached_;
};

} // namespace nwdaf
