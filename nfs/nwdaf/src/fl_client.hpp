// NWDAF federated-learning CLIENT round logic (TS 23.288 6.2C.2.2, TS 29.520
// Nnwdaf_MLModelTraining; ADR-0471 increment 3). Pure logic over a stored subscription: no network,
// no Valkey, so it is unit-testable. Mtlf calls it when an FL subscription (mlCorreId present)
// needs a round and does the delivery.
//
// Project choices, NOT 3GPP requirements (3GPP leaves the model encoding open, ADR-0471):
//   * the global model arrives in the subscription's mLModelInfos[0].mlFile and the interim local
//     model leaves in the notification's mLModelInfos[0].mlFile, both as base64 of the
//     linear-fl-v1 JSON (nfs/nwdaf/training/fl_local_round.py); mLFileAddr / ADRF exchange of the
//     interim model is NOT implemented.
#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <string>

#include "training_executor.hpp"

namespace nwdaf {

struct FlClientOptions {
    int epochs = 0;                // config/nwdaf.json mtlf.fl_client.epochs
    double learning_rate = 0;      // config/nwdaf.json mtlf.fl_client.learning_rate
    double accuracy_tolerance = 0; // load points within which a prediction counts as accurate
    std::chrono::seconds default_max_response{0}; // used when mLTrainRepInfo.maxResTime is absent
};

enum class FlRoundKind {
    NotFl,   // no mlCorreId: an ordinary training subscription
    Skipped, // skipFlInd or mLPreFlag: nothing to train or send
    Model,   // notification carries the interim local model
    Delay,   // notification is a DelayEventNotif
    Failure, // training failed for a reason other than time; no notification
};

struct FlRoundOutcome {
    FlRoundKind kind = FlRoundKind::NotFl;
    nlohmann::json notification; // NwdafMLModelTrainNotif for Model / Delay
    std::string detail;          // why, for logs
};

// `sub` is the stored NwdafMLModelTrainSubsc, `dataset` the same series document the model trainer
// takes. The round is timed with the steady clock; a round that fails, or outlasts maxResTime,
// yields a DelayEventNotif.
FlRoundOutcome run_fl_round(const nlohmann::json& sub,
                            const nlohmann::json& dataset,
                            FlRoundExecutor& executor,
                            const FlClientOptions& options);

std::string fl_base64_encode(const std::string& in);
bool fl_base64_decode(const std::string& in, std::string& out);

} // namespace nwdaf
