#include "amf_ue_id_index_store.hpp"

namespace amf {

namespace {

// docs/DECISIONS.md ADR-0444: tagged by amf_ue_id itself (this store's own real identity), not by
// tmsi -- see this file's own header comment for why tagging by tmsi would be a real bug here.
std::string index_key(const nf_config::RedisRouter& router, unsigned long amf_ue_id) {
    return "amf:ueidindex:" + router.tag(std::to_string(amf_ue_id));
}

} // namespace

void AmfUeIdIndexStore::put(unsigned long amf_ue_id, std::uint32_t tmsi) {
    const auto key = index_key(redis_, amf_ue_id);
    redis_.with([&](auto& r) { r.set(key, std::to_string(tmsi)); });
}

std::optional<std::uint32_t> AmfUeIdIndexStore::get(unsigned long amf_ue_id) {
    const auto key = index_key(redis_, amf_ue_id);
    const auto value = redis_.with([&](auto& r) { return r.get(key); });
    if (!value) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(std::stoul(*value));
}

void AmfUeIdIndexStore::remove(unsigned long amf_ue_id) {
    const auto key = index_key(redis_, amf_ue_id);
    redis_.with([&](auto& r) { r.del(key); });
}

} // namespace amf
