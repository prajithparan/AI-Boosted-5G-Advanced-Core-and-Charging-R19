#pragma once

// AnLF-side analytics accuracy monitoring of an ML model -- TS 23.288 5C.1 and 6.2E.3.3, the
// AnLF half of Nnwdaf_MLModelMonitor (TS 29.520 4.7.2.4-4.7.2.6) (ADR-0370). Private to
// nfs/nwdaf.
//
// What is measured. Every prediction the AnLF makes with a provisioned model is logged; when the
// observation it forecast has arrived (the instance's next observed load after the prediction
// was made -- the model is a one-step forecast, ADR-0369) the prediction is judged: correct when
// within `tolerance` load points of the observed value, the same rule the MTLF trained and
// reported with. 5C.1: "The accuracy value is computed as the number of correct predictions
// divided by the total number of predictions"; the rule for "correct" is implementation-defined
// (NOTE 3) and this is it. Outcomes are kept for `window`; accuracy, the number of inferences
// and the mean absolute deviation are computed over that window per model.
//
// State, all in Valkey so every replica logs, judges and reports the same numbers:
//   nwdaf:anlf:pred:<event>        ZSET of pending predictions, score = time made (epoch ms)
//   nwdaf:anlf:outcome:<event>     ZSET of judged predictions, score = time of the observation
//   nwdaf:anlf:monsub:<id>         an Nnwdaf_MLModelMonitor subscription (the MTLF's), plus the
//                                  reporting state (last report, last accuMeetInd); index
//                                  nwdaf:anlf:monsubs; nwdaf:anlf:monlease:<id> per-tick lease

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <sw/redis++/redis++.h>
#include <utility>
#include <vector>

namespace nwdaf {

struct LoadObservation {
    std::string nf_instance_id;
    std::chrono::system_clock::time_point at;
    std::int64_t load = 0;
};

struct AccuracySummary {
    std::int64_t model_id = 0;
    std::int64_t inferences = 0; // judged predictions in the window
    std::int64_t correct = 0;
    double mean_abs_deviation = 0;
    std::int64_t accuracy_pct() const {
        return inferences == 0 ? 0 : (100 * correct + inferences / 2) / inferences;
    }
};

class AccuracyMonitor {
public:
    AccuracyMonitor(std::shared_ptr<sw::redis::Redis> redis,
                    double tolerance,
                    std::chrono::seconds window,
                    std::chrono::seconds truth_timeout)
        : redis_(std::move(redis)), tolerance_(tolerance), window_(window),
          truth_timeout_(truth_timeout) {}

    void record_prediction(const std::string& event,
                           const std::string& nf_instance_id,
                           std::int64_t model_id,
                           double predicted,
                           std::chrono::system_clock::time_point made_at);
    // Judges every pending prediction whose instance has an observation after it; drops the
    // ones older than truth_timeout without one. Returns the number judged.
    int evaluate(const std::string& event, const std::vector<LoadObservation>& observations);
    // Accuracy over the window for one model (or every model when model_id is 0).
    AccuracySummary summary(const std::string& event, std::int64_t model_id);

    // Monitor subscriptions (the MTLF's), served by this AnLF.
    std::string create_subscription(const nlohmann::json& record);
    std::optional<nlohmann::json> get_subscription(const std::string& id);
    bool replace_subscription(const std::string& id, const nlohmann::json& record);
    bool remove_subscription(const std::string& id);
    std::vector<std::pair<std::string, nlohmann::json>> all_subscriptions();
    bool claim_report(const std::string& id, std::chrono::milliseconds ttl);

private:
    std::shared_ptr<sw::redis::Redis> redis_;
    double tolerance_;
    std::chrono::seconds window_;
    std::chrono::seconds truth_timeout_;
};

} // namespace nwdaf
