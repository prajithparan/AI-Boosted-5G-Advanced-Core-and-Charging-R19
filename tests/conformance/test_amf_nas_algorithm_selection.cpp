// ADR-0480: TS 33.501 6.7.1.1 NAS algorithm selection (pure function; no ports).
#include "nas_algorithm_selection.hpp"

#include <gtest/gtest.h>

using amf::NasAlgorithmPriority;
using amf::NasAlgorithms;
using amf::select_nas_algorithms;

namespace {
// Bitmask helpers in the TS 24.501 9.11.3.54 layout.
constexpr std::uint8_t kEa0 = 0x80, kEa1 = 0x40, kEa2 = 0x20, kEa3 = 0x10;
constexpr std::uint8_t kIa0 = 0x80, kIa1 = 0x40, kIa2 = 0x20, kIa3 = 0x10;
const NasAlgorithmPriority kPrio{.ciphering = {2, 1, 3, 0}, .integrity = {2, 1, 3}};
} // namespace

TEST(AmfNasAlgorithmSelection, MaskMatchesTheCapabilityBitLayout) {
    EXPECT_EQ(amf::algorithm_mask(0), 0x80);
    EXPECT_EQ(amf::algorithm_mask(1), 0x40);
    EXPECT_EQ(amf::algorithm_mask(2), 0x20);
    EXPECT_EQ(amf::algorithm_mask(7), 0x01);
    EXPECT_EQ(amf::algorithm_mask(8), 0x00);
}

TEST(AmfNasAlgorithmSelection, PicksTheHighestPriorityAlgorithmTheUeAndAmfBothHave) {
    // UE supports everything; AMF implements EA0/EA2 and IA2 only -> priority 2 wins.
    auto r = select_nas_algorithms(
        {kEa0 | kEa1 | kEa2 | kEa3, kIa0 | kIa1 | kIa2 | kIa3}, kPrio, kEa0 | kEa2, kIa2);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, (NasAlgorithms{2, 2}));
}

TEST(AmfNasAlgorithmSelection, FallsDownTheListWhenTheUeLacksTheTopChoice) {
    // UE has no 128-5G-EA2/IA2 -> EA1/IA1 is next, available only if the AMF implements it.
    auto r = select_nas_algorithms({kEa0 | kEa1, kIa1}, kPrio, kEa0 | kEa1 | kEa2, kIa1 | kIa2);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, (NasAlgorithms{1, 1}));
    // Same UE, AMF without a NEA1/NIA1 provider: no common integrity algorithm -> no selection.
    EXPECT_FALSE(select_nas_algorithms({kEa0 | kEa1, kIa1}, kPrio, kEa0 | kEa2, kIa2).has_value());
}

TEST(AmfNasAlgorithmSelection, NeverChoosesAnAlgorithmOutsideTheConfiguredLists) {
    // NIA0 is supported by both sides but is not in the integrity list: it must not be picked.
    EXPECT_FALSE(select_nas_algorithms({kEa2, kIa0}, kPrio, kEa2, kIa0 | kIa2).has_value());
}

TEST(AmfNasAlgorithmSelection, CipheringAndIntegrityAreIndependent) {
    NasAlgorithmPriority p{.ciphering = {0, 2}, .integrity = {2}}; // operator prefers NEA0
    auto r = select_nas_algorithms({kEa0 | kEa2, kIa2}, p, kEa0 | kEa2, kIa2);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, (NasAlgorithms{0, 2}));
}

TEST(AmfNasAlgorithmSelection, MalformedOrEmptyInputsGiveNoSelection) {
    EXPECT_FALSE(select_nas_algorithms({}, kPrio, 0xFF, 0xFF).has_value());
    EXPECT_FALSE(select_nas_algorithms({kEa2}, kPrio, 0xFF, 0xFF).has_value()); // IA octet missing
    NasAlgorithmPriority bad{.ciphering = {9, 200}, .integrity = {9}};          // identities > 7
    EXPECT_FALSE(select_nas_algorithms({0xFF, 0xFF}, bad, 0xFF, 0xFF).has_value());
    EXPECT_FALSE(
        select_nas_algorithms({0xFF, 0xFF}, NasAlgorithmPriority{}, 0xFF, 0xFF).has_value());
}
