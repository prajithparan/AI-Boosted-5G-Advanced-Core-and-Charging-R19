#include "li_poi.hpp"

#include <spdlog/spdlog.h>

namespace upf {

namespace {

li_poi::Hooks make_hooks() {
    li_poi::Hooks hooks;
    hooks.log_name = "upf-li-poi";
    hooks.supported_kinds = {li_core::x1::TargetIdentifierKind::UpfFseid};
    hooks.normalise = [](const std::string& value) {
        // A SEID compared as a number: "007" and "7" are the same session.
        const auto first = value.find_first_not_of('0');
        return first == std::string::npos ? std::string("0") : value.substr(first);
    };
    return hooks;
}

} // namespace

UpfLiPoi::UpfLiPoi(li_poi::Config config) : runtime_(std::move(config), make_hooks()) {}

UpfLiPoi::~UpfLiPoi() = default;

void UpfLiPoi::start() {
    runtime_.start();
}

void UpfLiPoi::stop() {
    runtime_.stop();
}

void UpfLiPoi::on_session_established(std::uint64_t cp_seid,
                                      const std::string& cp_address,
                                      std::uint64_t up_seid,
                                      std::uint32_t uplink_teid) {
    const std::lock_guard<std::mutex> lock(mutex_);
    // The map is keyed by TEID, so a SEID reused after a deletion cannot inherit a stale tunnel.
    session_by_teid_[uplink_teid] = Session{cp_seid, cp_address, up_seid};
}

void UpfLiPoi::on_session_deleted(std::uint64_t up_seid) {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::erase_if(session_by_teid_,
                  [&](const auto& entry) { return entry.second.up_seid == up_seid; });
}

void UpfLiPoi::on_packet(std::uint32_t teid,
                         PacketDirection direction,
                         std::span<const std::uint8_t> gtpu_packet) {
    Session session;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto it = session_by_teid_.find(teid);
        if (it == session_by_teid_.end()) {
            return;
        }
        session = it->second;
    }
    const auto matches = runtime_.matches(std::to_string(session.seid),
                                          {li_core::x1::TargetIdentifierKind::UpfFseid});
    for (const auto& match : matches) {
        if (!match.target.address.empty() && match.target.address != session.address) {
            continue; // the same SEID, but another SMF's session
        }
        runtime_.emit_cc(match.xid,
                         li_core::PayloadFormat::GtpUMessage,
                         direction == PacketDirection::Uplink
                             ? li_core::PayloadDirection::FromTarget
                             : li_core::PayloadDirection::ToTarget,
                         gtpu_packet);
    }
}

} // namespace upf
