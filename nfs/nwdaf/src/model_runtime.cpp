#include "model_runtime.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <onnxruntime/onnxruntime_cxx_api.h>

namespace nwdaf {

namespace {
// One Ort::Env per process (ONNX Runtime's contract; the CHF's ai_inference.cpp found the
// schema-registry failure a second Env causes).
Ort::Env& shared_env() {
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "nwdaf-anlf");
    return env;
}
} // namespace

std::optional<NfLoadFeatures> nf_load_features(const std::vector<LoadSample>& history) {
    if (history.size() < kNfLoadLags) {
        return std::nullopt;
    }
    NfLoadFeatures f{};
    float sum = 0;
    float registered = 0;
    for (std::size_t i = 0; i < kNfLoadLags; ++i) {
        const auto& s = history[history.size() - kNfLoadLags + i];
        if (!s.load) {
            return std::nullopt;
        }
        f[i] = static_cast<float>(*s.load);
        sum += f[i];
        registered += s.registered ? 1.0F : 0.0F;
    }
    f[kNfLoadLags] = sum / static_cast<float>(kNfLoadLags);
    f[kNfLoadLags + 1] = registered / static_cast<float>(kNfLoadLags);
    return f;
}

ModelRuntime::ModelRuntime() = default;
ModelRuntime::~ModelRuntime() = default;

bool ModelRuntime::load(const std::string& onnx_bytes, std::int64_t model_unique_id) {
    try {
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(1);
        auto session = std::make_unique<Ort::Session>(
            shared_env(), onnx_bytes.data(), onnx_bytes.size(), options);
        session_ = std::move(session);
        model_unique_id_ = model_unique_id;
        spdlog::info("nwdaf: ML model {} loaded ({} bytes)", model_unique_id, onnx_bytes.size());
        return true;
    } catch (const std::exception& e) {
        spdlog::warn("nwdaf: ML model {} rejected by ONNX Runtime: {}", model_unique_id, e.what());
        return false;
    }
}

void ModelRuntime::unload() {
    session_.reset();
    model_unique_id_ = 0;
}

std::optional<double> ModelRuntime::predict(const NfLoadFeatures& features) {
    if (!session_) {
        return std::nullopt;
    }
    try {
        Ort::AllocatorWithDefaultOptions allocator;
        auto input_name = session_->GetInputNameAllocated(0, allocator);
        auto output_name = session_->GetOutputNameAllocated(0, allocator);
        std::array<float, kNfLoadFeatureCount> input = features;
        std::array<std::int64_t, 2> shape{1, static_cast<std::int64_t>(kNfLoadFeatureCount)};
        auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        auto tensor = Ort::Value::CreateTensor<float>(
            memory, input.data(), input.size(), shape.data(), shape.size());
        const char* inputs[] = {input_name.get()};
        const char* outputs[] = {output_name.get()};
        auto out = session_->Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, outputs, 1);
        if (out.empty() || !out[0].IsTensor()) {
            return std::nullopt;
        }
        const double v = static_cast<double>(out[0].GetTensorData<float>()[0]);
        return std::clamp(v, 0.0, 100.0);
    } catch (const std::exception& e) {
        spdlog::warn("nwdaf: ML inference failed: {} -- statistics only", e.what());
        return std::nullopt;
    }
}

} // namespace nwdaf
