#include "nas_algorithm_selection.hpp"

namespace amf {

std::uint8_t algorithm_mask(std::uint8_t identity) {
    return identity <= 7 ? static_cast<std::uint8_t>(0x80u >> identity) : 0;
}

namespace {

std::optional<std::uint8_t> first_allowed(const std::vector<std::uint8_t>& priority,
                                          std::uint8_t ue_bitmap,
                                          std::uint8_t implemented) {
    for (const auto id : priority) {
        const auto mask = algorithm_mask(id);
        if (mask != 0 && (ue_bitmap & mask) != 0 && (implemented & mask) != 0) {
            return id;
        }
    }
    return std::nullopt;
}

} // namespace

std::optional<NasAlgorithms>
select_nas_algorithms(const std::vector<std::uint8_t>& ue_security_capability,
                      const NasAlgorithmPriority& priority,
                      std::uint8_t implemented_ciphering,
                      std::uint8_t implemented_integrity) {
    if (ue_security_capability.size() < 2) {
        return std::nullopt;
    }
    const auto ciphering =
        first_allowed(priority.ciphering, ue_security_capability[0], implemented_ciphering);
    const auto integrity =
        first_allowed(priority.integrity, ue_security_capability[1], implemented_integrity);
    if (!ciphering || !integrity) {
        return std::nullopt;
    }
    return NasAlgorithms{*ciphering, *integrity};
}

} // namespace amf
