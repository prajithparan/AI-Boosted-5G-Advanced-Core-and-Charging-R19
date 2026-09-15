#pragma once

// In-process ONNX Runtime inference for the AnLF (ADR-0369). The model comes from the MTLF
// through Nnwdaf_MLModelProvision and the ADRF (TS 23.288 6.2A, 6.2B.7); this class only ever
// LOADS bytes it was handed and runs them -- CLAUDE.md's mandated split: training in the Python
// sidecar (nfs/nwdaf/training/train_nf_load.py), inference in C++, never a Python call at
// runtime. Private to nfs/nwdaf.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Ort {
struct Env;
struct Session;
} // namespace Ort

namespace nwdaf {

// The ONNX model's input contract. MUST match train_nf_load.py's FEATURE_NAMES exactly -- one
// contract, two spellings.
inline constexpr std::size_t kNfLoadLags = 4;
inline constexpr std::size_t kNfLoadFeatureCount = 6;
inline constexpr const char* kNfLoadFeatureNames[kNfLoadFeatureCount] = {
    "load_lag3",
    "load_lag2",
    "load_lag1",
    "load_lag0",
    "load_mean",
    "registered_share",
};
using NfLoadFeatures = std::array<float, kNfLoadFeatureCount>;

// One instance's observation, oldest first, as the collected NRF timeline holds it.
struct LoadSample {
    std::optional<std::int64_t> load;
    bool registered = true;
};

// The feature vector for the NEXT load of an instance from its last kNfLoadLags observations
// (the sidecar's windows_from_series, on the inference side). std::nullopt when fewer than
// kNfLoadLags observations exist or any of the last kNfLoadLags carries no load -- an instance
// without enough history gets no prediction rather than a padded one.
std::optional<NfLoadFeatures> nf_load_features(const std::vector<LoadSample>& history);

class ModelRuntime {
public:
    ModelRuntime();
    ~ModelRuntime();
    ModelRuntime(const ModelRuntime&) = delete;
    ModelRuntime& operator=(const ModelRuntime&) = delete;

    // Replaces the loaded session with one built from `onnx_bytes`. Returns false (and keeps the
    // previous session, if any) when ONNX Runtime rejects the bytes.
    bool load(const std::string& onnx_bytes, std::int64_t model_unique_id);
    void unload();
    bool loaded() const { return session_ != nullptr; }
    std::int64_t model_unique_id() const { return model_unique_id_; }

    // One prediction, clamped to the NRF's 0..100 load scale. std::nullopt when no model is
    // loaded or the run fails -- the caller falls back to statistics, never to a guess.
    std::optional<double> predict(const NfLoadFeatures& features);

private:
    std::unique_ptr<Ort::Session> session_;
    std::int64_t model_unique_id_ = 0;
};

} // namespace nwdaf
