#pragma once

// The MTLF's training backend (ADR-0369). CLAUDE.md: "Design the training-sidecar interface to
// be swappable (e.g. pluggable backend/executor) rather than hardcoding for toy-scale local
// training only." This is that interface: the MTLF hands an executor a dataset file and gets a
// model file and a report back; where and how the training ran is the executor's business.
//
//   SubprocessExecutor  runs nfs/nwdaf/training/train_nf_load.py on this host with the
//                       interpreter from config. What the lab and CI use.
//   (later)             a Kubeflow / Flyte / remote-queue executor that ships the dataset off the
//                       NF's host and waits for the artifact -- same TrainingJob/TrainingResult.
//
// Private to nfs/nwdaf.

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <tl/expected.hpp>
#include <vector>

namespace nwdaf {

struct TrainingJob {
    std::string event;      // NwdafEvent, e.g. "NF_LOAD"
    nlohmann::json dataset; // the sidecar's input document (see train_nf_load.py)
    std::int64_t min_samples = 0;
    double accuracy_tolerance = 0;
};

struct TrainingResult {
    std::string onnx_bytes;
    nlohmann::json report; // the sidecar's report JSON, verbatim
};

class TrainingExecutor {
public:
    virtual ~TrainingExecutor() = default;
    virtual tl::expected<TrainingResult, std::string> train(const TrainingJob& job) = 0;
};

struct SubprocessExecutorOptions {
    std::string python;  // interpreter (a venv's, normally)
    std::string script;  // absolute path of train_nf_load.py
    std::string workdir; // where datasets, models, reports and MLflow's DB land
    std::string mlflow_tracking_uri;
    std::chrono::seconds timeout{600};
};

class SubprocessExecutor final : public TrainingExecutor {
public:
    explicit SubprocessExecutor(SubprocessExecutorOptions options);
    tl::expected<TrainingResult, std::string> train(const TrainingJob& job) override;

private:
    SubprocessExecutorOptions options_;
};

// Federated-learning client step (ADR-0471 increment 3): one local round of the linear model
// from the sidecar's fl_local_round.py, starting from the FL Server's global model.
struct FlRoundJob {
    std::string event;
    nlohmann::json dataset;      // same document as TrainingJob::dataset (the "series")
    nlohmann::json global_model; // null = start from zeros
    int epochs = 0;
    double learning_rate = 0;
    double accuracy_tolerance = 0;
};

struct FlRoundResult {
    nlohmann::json model;  // linear-fl-v1 local model
    nlohmann::json report; // n_samples, accuracy_global_pct, accuracy_local_pct, loss, ...
};

class FlRoundExecutor {
public:
    virtual ~FlRoundExecutor() = default;
    virtual tl::expected<FlRoundResult, std::string> train_round(const FlRoundJob& job) = 0;
};

struct SubprocessFlRoundExecutorOptions {
    std::string python;
    std::string script; // absolute path of fl_local_round.py
    std::string workdir;
    std::chrono::seconds timeout{600};
};

class SubprocessFlRoundExecutor final : public FlRoundExecutor {
public:
    explicit SubprocessFlRoundExecutor(SubprocessFlRoundExecutorOptions options);
    tl::expected<FlRoundResult, std::string> train_round(const FlRoundJob& job) override;

private:
    SubprocessFlRoundExecutorOptions options_;
};

// Federated-learning SERVER aggregation (ADR-0471 increment 4): sample-weighted FedAvg of the
// clients' interim models by the sidecar's fl_aggregate.py.
class FlAggregator {
public:
    virtual ~FlAggregator() = default;
    // Each update: {"model": <linear-fl-v1>, "n_samples": <int>}. Returns the global model JSON.
    virtual tl::expected<nlohmann::json, std::string>
    aggregate(const std::vector<nlohmann::json>& updates) = 0;
};

struct SubprocessFlAggregatorOptions {
    std::string python;
    std::string script; // absolute path of fl_aggregate.py
    std::string workdir;
    std::chrono::seconds timeout{600};
};

class SubprocessFlAggregator final : public FlAggregator {
public:
    explicit SubprocessFlAggregator(SubprocessFlAggregatorOptions options);
    tl::expected<nlohmann::json, std::string>
    aggregate(const std::vector<nlohmann::json>& updates) override;

private:
    SubprocessFlAggregatorOptions options_;
};

} // namespace nwdaf
