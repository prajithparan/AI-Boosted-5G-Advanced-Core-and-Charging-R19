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
#include <expected>
#include <string>
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
    virtual std::expected<TrainingResult, std::string> train(const TrainingJob& job) = 0;
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
    std::expected<TrainingResult, std::string> train(const TrainingJob& job) override;

private:
    SubprocessExecutorOptions options_;
};

} // namespace nwdaf
