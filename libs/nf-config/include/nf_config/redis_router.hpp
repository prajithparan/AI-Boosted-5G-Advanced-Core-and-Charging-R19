#pragma once

// Routes a store operation to the right Valkey connection, transparently over a single-node
// server or a real Valkey Cluster deployment (docs/DECISIONS.md ADR-0443/ADR-0444).
//
// History: this class was originally private to nfs/udsf/src/store.hpp (ADR-0443, UDSF -- the
// first and, until ADR-0444, only NF converted to real cluster routing). ADR-0444 converted AMF
// and CHF the same way and promoted the class here, into libs/nf-config, rather than copy-pasting
// it a second and third time -- this project's own "shared code lives in libs/" convention
// (mirrored by nf_config::connect_redis_or_die/connect_redis_cluster_or_die already living here).
//
// Two, deliberately different, ways to use it -- because UDSF and AMF/CHF have genuinely
// different needs, not because one is an incomplete version of the other:
//
//  - with(hash_tag, f): UDSF's own original entry point (ADR-0443). Calls f(Redis&) once, on a
//    connection PINNED to that one hash tag's shard in cluster mode
//    (RedisCluster::redis(hash_tag, new_connection=false)) or the shared single-node client
//    otherwise. This exists because UDSF does a SEQUENCE of commands (read, decide, WATCH,
//    MULTI/EXEC) that must all land on the same connection/shard. The real, disclosed hazard this
//    carries (ADR-0443's "one real bug this proof work caught"): a with(hash_tag, f) must never be
//    called again, nested, from inside another with(hash_tag, f) on the same shard -- with a
//    connection pool sized by redis_pool_size and ConnectionPoolOptions::wait_timeout defaulting
//    to 0 (wait forever), enough concurrent nested calls hang the process rather than erroring.
//    UDSF's own call sites (store.cpp) already inline what would otherwise be a nested call
//    (list_subs() inlines get_sub()'s body) for exactly this reason.
//
//  - with(f): ADR-0444's own new entry point, for a store whose every operation is genuinely
//    single-key (AMF's UeSecurityContextStore/AmfUeIdIndexStore, CHF's ChargingDataStore and
//    siblings -- ADR-0443's own survey already found "none" under multi-key ops for both). Calls
//    f(Redis&) or f(RedisCluster&) directly -- NOT through RedisCluster::redis(hash_tag), so NO
//    connection is ever pinned across the call: every single-key command method on
//    sw::redis::RedisCluster (hset/hget/hincrby/hincrbyfloat/incr/sadd/srem/sismember/smembers/
//    set/get/del/expire/hsetnx/... -- confirmed by reading redis_cluster.h, not assumed) computes
//    its own hash slot from its own key argument and fetches+releases its own connection
//    per-command, the same way sw::redis::Redis's own internal pool already works in single-node
//    mode. This makes with(f) safe to call with a lambda that issues several sequential commands,
//    and safe even if the caller happens to nest it (there is no pinned connection to exhaust) --
//    a real, deliberate design difference from with(hash_tag, f), not an oversight; AMF/CHF do not
//    need with(hash_tag, f)'s shard-pinning because they never need more than "this one command,
//    right now" to be atomic with anything else.
//
//  - transaction(hash_tag, piped, new_connection): UDSF's own real WATCH/MULTI/EXEC entry point
//    (ADR-0443 Decision 1's own finding: a Redis object built from RedisCluster::redis() throws on
//    ::transaction() because it has no pool of its own left to hand the Transaction --
//    RedisCluster::transaction(hash_tag, ...) is the library's own, separate, correct entry
//    point). AMF/CHF have no multi-key transaction and do not use this.
//
// tag(id): the single source of truth for whether a per-entity key gets a Valkey Cluster hash tag
// ("{...}") at all. Returns `id` unchanged in single-node mode (so every existing single-node
// deployment's key bytes are exactly what they were before ADR-0444 -- byte-identical default
// behaviour, ADR-0077/ADR-0443's own precedent), and "{" + id + "}" in cluster mode. AMF/CHF's own
// key-builder functions call this so the hash-tag decision lives in exactly one place, not
// hand-duplicated at every call site (docs/DECISIONS.md ADR-0444 names the real, disclosed reason
// this is mode-gated rather than unconditional like UDSF's own baked-in-since-ADR-0400 prefix).

#include <memory>
#include <string>
#include <sw/redis++/redis++.h>
#include <type_traits>

namespace nf_config {

class RedisRouter {
public:
    explicit RedisRouter(std::shared_ptr<sw::redis::Redis> single) : single_(std::move(single)) {}
    explicit RedisRouter(std::shared_ptr<sw::redis::RedisCluster> cluster)
        : cluster_(std::move(cluster)) {}

    bool is_cluster() const { return static_cast<bool>(cluster_); }

    // Hash-tag wrapper for a per-entity key segment. A no-op (returns `id` unchanged) in
    // single-node mode; wraps in Valkey Cluster hash-tag braces in cluster mode. See this file's
    // own header comment for why this is mode-gated.
    std::string tag(const std::string& id) const { return is_cluster() ? "{" + id + "}" : id; }

    // ADR-0444's own entry point: no hash tag, no pinned connection -- see header comment. `f`
    // must be generic over the connection type (e.g. a lambda taking `auto&`), since it is called
    // with either a `sw::redis::Redis&` or a `sw::redis::RedisCluster&` depending on mode; both
    // expose the same single-key command surface for every command this project's AMF/CHF stores
    // use.
    template <typename F> auto with(F&& f) const {
        if (cluster_) {
            return f(*cluster_);
        }
        return f(*single_);
    }

    // UDSF's own original entry point (ADR-0443). See header comment for the pinned-connection
    // semantics and the nesting hazard this carries.
    template <typename F>
    auto with(const std::string& hash_tag,
              F&& f) const -> std::invoke_result_t<F&, sw::redis::Redis&> {
        if (cluster_) {
            auto r = cluster_->redis(hash_tag, /*new_connection=*/false);
            return f(r);
        }
        return f(*single_);
    }

    // UDSF's own original WATCH/MULTI/EXEC entry point (ADR-0443).
    sw::redis::Transaction
    transaction(const std::string& hash_tag, bool piped, bool new_connection) const {
        if (cluster_) {
            return cluster_->transaction(hash_tag, piped, new_connection);
        }
        return single_->transaction(piped, new_connection);
    }

private:
    std::shared_ptr<sw::redis::Redis> single_;
    std::shared_ptr<sw::redis::RedisCluster> cluster_;
};

} // namespace nf_config
