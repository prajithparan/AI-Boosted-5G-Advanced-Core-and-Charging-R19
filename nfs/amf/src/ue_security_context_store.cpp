#include "ue_security_context_store.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <iterator>

namespace amf {

namespace {

// docs/DECISIONS.md ADR-0444: tmsi is this store's whole key -- the natural sharding unit is the
// UE itself, identified by the same tmsi ServiceRequest presents back. router.tag() is a no-op in
// single-node mode (byte-identical key, unchanged since before ADR-0444) and wraps the hex tmsi in
// a Valkey Cluster hash tag in cluster mode.
std::string context_key(const nf_config::RedisRouter& router, std::uint32_t tmsi) {
    char hex[16];
    std::snprintf(hex, sizeof(hex), "%08x", tmsi);
    return "amf:uesecctx:" + router.tag(hex);
}

std::string to_hex(const std::vector<std::uint8_t>& bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const auto b : bytes) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

std::vector<std::uint8_t> from_hex(const std::string& hex) {
    std::vector<std::uint8_t> out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

} // namespace

void UeSecurityContextStore::put(std::uint32_t tmsi, const UeSecurityContext& context) {
    const auto key = context_key(redis_, tmsi);
    redis_.with([&](auto& r) {
        r.hset(key, "supi", context.supi);
        r.hset(key, "kamf", to_hex({context.kamf.begin(), context.kamf.end()}));
        r.hset(key, "ngksi", std::to_string(context.ngksi));
        r.hset(key, "uplink_count", std::to_string(context.uplink_count));
        r.hset(key, "downlink_count", std::to_string(context.downlink_count));
        r.hset(key, "ue_sec_cap", to_hex(context.ue_security_capability));
    });
}

std::optional<UeSecurityContext> UeSecurityContextStore::get(std::uint32_t tmsi) {
    const auto key = context_key(redis_, tmsi);
    std::vector<sw::redis::OptionalString> values;
    redis_.with([&](auto& r) {
        r.hmget(key,
                {"supi", "kamf", "ngksi", "uplink_count", "downlink_count", "ue_sec_cap"},
                std::back_inserter(values));
    });
    // sw::redis::Optional<T> is redis-plus-plus's own pre-C++17 Optional (`explicit operator
    // bool()`), not std::optional -- same real API confirmed via compilation this project's own
    // P4.8 pass already found (nfs/chf/src/stores.cpp).
    if (!values[0]) {
        return std::nullopt;
    }
    UeSecurityContext context;
    context.supi = *values[0];
    const auto kamf_bytes = values[1] ? from_hex(*values[1]) : std::vector<std::uint8_t>{};
    if (kamf_bytes.size() != context.kamf.size()) {
        spdlog::warn("amf: UeSecurityContext for tmsi={:08x} has malformed kamf, dropping", tmsi);
        return std::nullopt;
    }
    std::copy(kamf_bytes.begin(), kamf_bytes.end(), context.kamf.begin());
    context.ngksi = values[2] ? static_cast<std::uint8_t>(std::stoul(*values[2])) : 0;
    context.uplink_count = values[3] ? static_cast<std::uint32_t>(std::stoul(*values[3])) : 0;
    context.downlink_count = values[4] ? static_cast<std::uint32_t>(std::stoul(*values[4])) : 0;
    context.ue_security_capability = values[5] ? from_hex(*values[5]) : std::vector<std::uint8_t>{};
    return context;
}

std::uint32_t UeSecurityContextStore::next_uplink_count(std::uint32_t tmsi) {
    const auto key = context_key(redis_, tmsi);
    const auto new_value = redis_.with([&](auto& r) { return r.hincrby(key, "uplink_count", 1); });
    return static_cast<std::uint32_t>(new_value - 1);
}

std::uint32_t UeSecurityContextStore::next_downlink_count(std::uint32_t tmsi) {
    const auto key = context_key(redis_, tmsi);
    const auto new_value =
        redis_.with([&](auto& r) { return r.hincrby(key, "downlink_count", 1); });
    return static_cast<std::uint32_t>(new_value - 1);
}

void UeSecurityContextStore::remove(std::uint32_t tmsi) {
    const auto key = context_key(redis_, tmsi);
    redis_.with([&](auto& r) { r.del(key); });
}

std::uint32_t UeSecurityContextStore::allocate_tmsi() {
    // A single, deliberately untagged, global counter -- not per-UE, so there is no "one UE's own
    // slot" for it to join; it lives on whatever slot CRC16("amf:next_tmsi") maps to in cluster
    // mode, same as CHF's own global sequence counters (docs/DECISIONS.md ADR-0444).
    return static_cast<std::uint32_t>(redis_.with([](auto& r) { return r.incr("amf:next_tmsi"); }));
}

} // namespace amf
