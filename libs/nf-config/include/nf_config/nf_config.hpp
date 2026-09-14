// nf_config: shared, minimal config-file loader used by every NF/BSS service's main.cpp.
//
// docs/DECISIONS.md ADR-0077 (user-directed, mandatory, project-wide coding decision): no DB
// URL, connection string, or deployment parameter may be a hardcoded literal default inside a
// source file. Real values live in a checked-in per-service JSON config file
// (config/<service>.json at the repo root); an env var of the form <SERVICE>_<KEY> may override
// any single key at deployment time (container/k8s convenience, same override mechanism every
// NF already used for things like AMF_REDIS_URL -- what changes is that the *default* now lives
// in the config file, not a string literal in main.cpp). There is no third, hardcoded fallback.
//
// This does not apply to CERTS_DIR (a CMake-supplied build-time path, not a runtime deployment
// parameter -- same distinction the project already drew for that constant) or to fixed protocol
// identity constants that ADR-0018 already justifies as deliberately fixed (e.g. AMF's own
// kNrfInstanceId, which must match NRF's own hardcoded identity, not a per-deployment value).

#pragma once

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace nf_config {

// Resolves and loads a service's JSON config file. Path resolution order: the
// <SERVICE_NAME>_CONFIG_FILE env var if set, else "<config_dir>/<service_name>.json". Throws
// std::runtime_error with the resolved path on failure -- fail fast, same discipline this
// project already applies to Redis::ping() at startup, rather than silently falling back to any
// value not actually present in a config source.
inline nlohmann::json load(const std::string& service_name, const std::string& config_dir) {
    std::string env_var_name = service_name;
    for (auto& c : env_var_name) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    env_var_name += "_CONFIG_FILE";

    std::string path = config_dir + "/" + service_name + ".json";
    if (const char* env = std::getenv(env_var_name.c_str())) {
        path = env;
    }

    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("nf_config: could not open config file: " + path);
    }
    nlohmann::json j;
    in >> j;
    return j;
}

// Returns the value for `key`: an env var named `env_name` wins if set (deployment-time
// override), otherwise the config file's own value for `key`. Throws if neither source has it --
// there is deliberately no third, in-source literal default.
template <typename T>
T require(const nlohmann::json& config, const std::string& key, const char* env_name = nullptr) {
    if (env_name != nullptr) {
        if (const char* env = std::getenv(env_name)) {
            if constexpr (std::is_same_v<T, std::string>) {
                return T(env);
            } else {
                return nlohmann::json::parse(env).template get<T>();
            }
        }
    }
    if (!config.contains(key)) {
        throw std::runtime_error("nf_config: missing required config key: " + key);
    }
    return config.at(key).get<T>();
}

// ADR-0355: the same precedence as require<> -- environment first, then the config file -- for a
// key that is allowed to be absent. Returns nullopt only when NEITHER supplies it, so a caller's
// .value_or(default) is the single place the default lives. Added rather than reaching for
// config.value(): that ignores the environment, which is precisely what left the 3M-CDR run's
// CHF instances on a dead PostgreSQL port (ADR-0352).
template <typename T>
std::optional<T>
optional(const nlohmann::json& config, const std::string& key, const char* env_name = nullptr) {
    if (env_name != nullptr) {
        if (const char* env = std::getenv(env_name)) {
            if constexpr (std::is_same_v<T, std::string>) {
                return T(env);
            } else {
                return nlohmann::json::parse(env).template get<T>();
            }
        }
    }
    if (!config.contains(key)) {
        return std::nullopt;
    }
    return config.at(key).get<T>();
}

} // namespace nf_config
