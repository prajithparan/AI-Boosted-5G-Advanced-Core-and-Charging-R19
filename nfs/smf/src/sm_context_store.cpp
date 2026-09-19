#include "sm_context_store.hpp"

namespace smf {

std::string SmContextStore::create(nlohmann::json context) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string ref = "smctx-" + std::to_string(next_id_++);
    contexts_.emplace(ref, std::move(context));
    return ref;
}

std::optional<nlohmann::json> SmContextStore::get(const std::string& sm_context_ref) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = contexts_.find(sm_context_ref);
    if (it == contexts_.end()) {
        return std::nullopt;
    }
    return std::make_optional(it->second);
}

bool SmContextStore::update(const std::string& sm_context_ref, nlohmann::json context) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = contexts_.find(sm_context_ref);
    if (it == contexts_.end()) {
        return false;
    }
    it->second = std::move(context);
    return true;
}

bool SmContextStore::remove(const std::string& sm_context_ref) {
    std::lock_guard<std::mutex> lock(mutex_);
    return contexts_.erase(sm_context_ref) > 0;
}

std::vector<nlohmann::json> SmContextStore::snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<nlohmann::json> out;
    out.reserve(contexts_.size());
    for (const auto& [ref, ctx] : contexts_) {
        out.push_back(ctx);
    }
    return out;
}

} // namespace smf
