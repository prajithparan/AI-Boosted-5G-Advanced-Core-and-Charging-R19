#pragma once

#include <cstdint>
#include <optional>
#include <vector>

// NAS security algorithm selection, TS 33.501 clause 6.7.1.1 (ADR-0480). Private to nfs/amf.
//
// "Each AMF shall be configured via network management with lists of algorithms which are
// allowed for usage. There shall be one list for NAS integrity algorithms, and one for NAS
// ciphering algorithms. These lists shall be ordered according to a priority decided by the
// operator. ... The AMF shall select the NAS algorithm which have the highest priority according
// to the ordered lists." The chosen algorithm must also be one the UE supports (UE security
// capability IE, TS 24.501 9.11.3.54: octet 3 bits 8..1 = 5G-EA0..EA7, octet 4 bits 8..1 =
// 5G-IA0..IA7, i.e. algorithm identity n <-> mask 0x80 >> n) and one this AMF can actually run.
//
// Pure functions: no I/O, no state. Not yet called by the registration path (that still uses the
// fixed 128-NEA2/128-NIA2 pair) -- wiring is the next ADR-0480 step.
namespace amf {

struct NasAlgorithmPriority {
    // Algorithm identities (0..7), highest priority first.
    std::vector<std::uint8_t> ciphering;
    std::vector<std::uint8_t> integrity;
};

struct NasAlgorithms {
    std::uint8_t ciphering = 0;
    std::uint8_t integrity = 0;
    bool operator==(const NasAlgorithms&) const = default;
};

// Bitmask for identity n in the same layout as the capability octets (0x80 >> n); n > 7 -> 0.
std::uint8_t algorithm_mask(std::uint8_t identity);

// ue_security_capability: the IE value as captured at registration (octet 3 = EA bitmap, octet 4 =
// IA bitmap; shorter than 2 octets -> no selection). implemented_*: bitmasks of what this AMF can
// execute (NEA0/NEA2/NIA2 today, plus a provider's NEA1/NIA1 when one loaded).
// Returns nullopt when either direction has no common algorithm (the caller must then reject the
// procedure; this function never falls back to a weaker choice).
std::optional<NasAlgorithms>
select_nas_algorithms(const std::vector<std::uint8_t>& ue_security_capability,
                      const NasAlgorithmPriority& priority,
                      std::uint8_t implemented_ciphering,
                      std::uint8_t implemented_integrity);

} // namespace amf
