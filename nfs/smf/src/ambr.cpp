#include "ambr.hpp"

#include <cctype>
#include <exception>

namespace smf {

std::optional<std::uint64_t> ambr_to_kbps(const std::string& ambr) {
    const auto space = ambr.find(' ');
    if (space == std::string::npos) {
        return std::nullopt;
    }
    double value = 0.0;
    try {
        value = std::stod(ambr.substr(0, space));
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (value < 0.0) {
        return std::nullopt;
    }
    std::string unit = ambr.substr(space + 1);
    for (auto& c : unit) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (unit == "bps") {
        return static_cast<std::uint64_t>(value / 1000.0);
    }
    if (unit == "kbps") {
        return static_cast<std::uint64_t>(value);
    }
    if (unit == "mbps") {
        return static_cast<std::uint64_t>(value * 1000.0);
    }
    if (unit == "gbps") {
        return static_cast<std::uint64_t>(value * 1000000.0);
    }
    if (unit == "tbps") {
        return static_cast<std::uint64_t>(value * 1000000000.0);
    }
    return std::nullopt;
}

} // namespace smf
