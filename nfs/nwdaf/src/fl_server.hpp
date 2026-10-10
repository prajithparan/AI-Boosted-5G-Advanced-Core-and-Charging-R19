// NWDAF federated-learning SERVER building blocks (TS 23.288 6.2C.2.2, TS 29.520
// Nnwdaf_MLModelTraining; ADR-0471 increment 4). Pure logic over JSON: no network, no Valkey, so it
// is unit-testable. The orchestration (discovery calls, callbacks, round loop) is Mtlf's.
//
// Project choices, NOT 3GPP requirements (ADR-0471):
//   * a client's interim model is the linear-fl-v1 JSON in the notification's
//     mLModelInfos[0].mlFile (base64), carrying `n_samples` (the FedAvg weight);
//   * the FL client is found by filtering the NRF's NWDAF profiles client-side on
//     nwdafInfo.mlAnalyticsList[].flCapabilityType (FL_CLIENT / FL_SERVER_AND_CLIENT) and
//     mlAnalyticsIds, because no FL query parameter was found in TS29510_Nnrf_NFDiscovery.yaml.
#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <tl/expected.hpp>
#include <vector>

namespace nwdaf {

struct FlFederation {
    std::string event;       // NwdafEvent the federation trains, e.g. "NF_LOAD"
    std::string ml_corre_id; // mlCorreId: names the federation in every message
    int rounds = 0;          // FL rounds (config)
    int min_clients = 0;     // quorum: fewer participants abort the federation (config)
    long max_res_time_s = 0; // mLTrainRepInfo.maxResTime per round (config)
};

struct FlClientRef {
    std::string nf_instance_id;
    std::string base_url; // scheme://ip:port of nnwdaf-mlmodeltraining, no apiRoot
};

// `nf_instances` is the NRF discovery result's nfInstances array.
std::vector<FlClientRef> select_fl_clients(const nlohmann::json& nf_instances,
                                           const std::string& event,
                                           const std::string& self_instance_id);

// NwdafMLModelTrainSubsc for `round`; `global_model` null = round 1 (the client starts from zeros).
nlohmann::json build_round_subscription(const FlFederation& fed,
                                        int round,
                                        const nlohmann::json& global_model,
                                        const std::string& notif_uri,
                                        const std::string& notif_corre_id);

// NwdafMLModelTrainSubscPatch (RFC 7396) that starts `round` with the new global model.
nlohmann::json
build_round_patch(const FlFederation& fed, int round, const nlohmann::json& global_model);

struct FlUpdate {
    nlohmann::json model; // linear-fl-v1 local model
    long n_samples = 0;
    long accuracy_of_global = -1; // statusReport.mlModelAcc, -1 when absent
};

// A client's NwdafMLModelTrainNotif for `round`: the interim model, or an error naming why the
// client could not deliver one (DelayEventNotif cause, wrong correlation, wrong round, bad model).
tl::expected<FlUpdate, std::string> parse_fl_notification(const nlohmann::json& body,
                                                          const FlFederation& fed,
                                                          int round,
                                                          const std::string& notif_corre_id);

} // namespace nwdaf
