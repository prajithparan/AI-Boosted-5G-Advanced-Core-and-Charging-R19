#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace smf {

// ADR-0308: parse a 3GPP AMBR string into the kbps a PFCP MBR IE carries.
//
// TS 29.571's `BitRate` is a number, a space and a unit -- "1 Mbps", "200 Kbps", "5 Gbps" -- while
// TS 29.244's MBR IE is in kbps (1000 bps, per session_ies.hpp's own confirmed comment, not
// assumed). Returns std::nullopt for anything unrecognised rather than guessing a magnitude: a
// mis-scaled rate limit is invisible until a subscriber is throttled a thousand times too hard or
// not at all, and neither surfaces as an error anywhere.
std::optional<std::uint64_t> ambr_to_kbps(const std::string& ambr);

} // namespace smf
