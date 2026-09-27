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

// Cluster-mode counterpart, for a real Valkey Cluster deployment (docs/DECISIONS.md ADR-0443).
// sw::redis::RedisCluster is a DIFFERENT client class from sw::redis::Redis: routing a command by
// its key's hash slot, following MOVED/ASK redirects, and pooling one connection set per shard are
// real behaviour of this class (sw/redis++/redis_cluster.h), not something added here. `url`'s
// "?pool_size=N" query parameter (the same convention connect_redis_or_die's own callers already
// use) sizes every shard's pool identically -- ConnectionPoolOptions::size defaults to 1 in the
// library, which would serialise a shard's whole traffic through one connection if left unset.
//
// for_each(...) forces a real connection to, and PING of, every node the seed URL's CLUSTER SLOTS
// response discovers -- the same fail-fast rule as connect_redis_or_die, but stronger: it proves
// every shard is reachable at startup, not just the one seed node.
inline std::shared_ptr<sw::redis::RedisCluster> connect_redis_cluster_or_die(const std::string& url,
                                                                             std::string_view nf) {
    try {
        const sw::redis::Uri parsed(url);
        auto cluster = std::make_shared<sw::redis::RedisCluster>(parsed.connection_options(),
                                                                 parsed.connection_pool_options());
        cluster->for_each([](sw::redis::Redis& r) { r.ping(); });
        spdlog::info("{}: connected to Valkey Cluster (seed {})", nf, url);
        return cluster;
    } catch (const std::exception& e) {
        fatal(std::string(nf) + ": Valkey Cluster unavailable at startup: " + e.what());
    }
}

} // namespace nf_config
