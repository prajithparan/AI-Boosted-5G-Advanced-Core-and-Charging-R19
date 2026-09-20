// Redis/Valkey startup connection with the architecture-bonded fail-fast rule (see nf_config.hpp
// fatal()). sw::redis::Redis connects lazily -- constructing it never fails, so a bad URL or a down
// server is only discovered on the first command, far from startup. This forces a real connection
// with PING at startup and terminates the process on failure, so no NF ever runs without its
// in-memory store.
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <sw/redis++/redis++.h>

#include "nf_config/nf_config.hpp"

namespace nf_config {

// Construct a Redis client and prove connectivity now (PING). On any failure, log and exit --
// never return a client that is not known to be connected.
inline std::shared_ptr<sw::redis::Redis> connect_redis_or_die(const std::string& url,
                                                              std::string_view nf) {
    try {
        auto redis = std::make_shared<sw::redis::Redis>(url);
        redis->ping();
        spdlog::info("{}: connected to Redis/Valkey", nf);
        return redis;
    } catch (const std::exception& e) {
        fatal(std::string(nf) + ": Redis/Valkey unavailable at startup: " + e.what());
    }
}

} // namespace nf_config
