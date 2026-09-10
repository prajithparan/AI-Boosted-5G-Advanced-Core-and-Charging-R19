// ADR-0330: unit pooling -- one allowance drawn down by traffic measured in different units.
//
// The rate is the operator's, never this project's. These tests are mostly about what happens when
// the operator supplies something unusable, because that is where a charging system does damage
// quietly: a mis-parsed rate does not throw, it just bills the wrong amount forever.

#include <nlohmann/json.hpp>

#include <optional>

#include "unit_pooling.hpp"

#include <gtest/gtest.h>

using nlohmann::json;

TEST(UnitPooling, ParsesBothOperatorConfiguredRates) {
    const auto p = chf::parse_unit_pooling(
        std::optional<json>(json{{"octetsPerSecond", 87381}, {"octetsPerServiceUnit", 1000000}}));
    ASSERT_TRUE(p.octets_per_second.has_value());
    ASSERT_TRUE(p.octets_per_service_unit.has_value());
    EXPECT_DOUBLE_EQ(*p.octets_per_second, 87381.0);
    EXPECT_DOUBLE_EQ(*p.octets_per_service_unit, 1000000.0);
    EXPECT_TRUE(p.any());
}

TEST(UnitPooling, AnAbsentCharacteristicPoolsNothing) {
    // The overwhelmingly common case: an offering that never mentions pooling must behave exactly
    // as it did before this ADR existed.
    const auto p = chf::parse_unit_pooling(std::nullopt);
    EXPECT_FALSE(p.any());
    EXPECT_DOUBLE_EQ(chf::pooled_used_volume(p, 5000.0, 3.0, 60.0), 5000.0);
}

TEST(UnitPooling, RejectsRatesThatWouldSilentlyMisbill) {
    // Zero, negative, non-finite and non-numeric are all refused rather than coerced. A negative
    // rate would turn consumption into a refund; a zero rate makes a typo indistinguishable from a
    // deliberate giveaway. Neither surfaces as an error anywhere downstream, so it has to be
    // caught here.
    for (const auto& bad : {json{{"octetsPerSecond", 0}},
                            json{{"octetsPerSecond", -5}},
                            json{{"octetsPerSecond", "87381"}},
                            json{{"octetsPerSecond", nullptr}}}) {
        const auto p = chf::parse_unit_pooling(std::optional<json>(bad));
        EXPECT_FALSE(p.octets_per_second.has_value()) << bad.dump();
    }
    const auto not_an_object = chf::parse_unit_pooling(std::optional<json>(json::array({1, 2})));
    EXPECT_FALSE(not_an_object.any());
}

TEST(UnitPooling, OnePartialRateLeavesTheOtherDimensionAlone) {
    // "Voice draws on the bundle, events do not" is a real product, so a half-configured pooling
    // is valid rather than an error.
    const auto p = chf::parse_unit_pooling(std::optional<json>(json{{"octetsPerSecond", 1000}}));
    ASSERT_TRUE(p.octets_per_second.has_value());
    EXPECT_FALSE(p.octets_per_service_unit.has_value());
    // 60 s at 1000 octets/s folds in; the 4 service units do not.
    EXPECT_DOUBLE_EQ(chf::pooled_used_volume(p, 2000.0, 4.0, 60.0), 2000.0 + 60000.0);
}

TEST(UnitPooling, ConvertsAtExactlyTheConfiguredRate) {
    chf::UnitPooling p;
    p.octets_per_second = 87381.0;      // ~699 kbps voice, an operator's commercial choice
    p.octets_per_service_unit = 1000.0; // 1 kB per event
    // 1 MB of data + 120 s of voice + 3 events.
    const double pooled = chf::pooled_used_volume(p, 1'000'000.0, 3.0, 120.0);
    EXPECT_DOUBLE_EQ(pooled, 1'000'000.0 + (120.0 * 87381.0) + (3.0 * 1000.0));
}

TEST(UnitPooling, NoUsageInAPooledDimensionAddsNothing) {
    chf::UnitPooling p;
    p.octets_per_second = 87381.0;
    // A pure data session under a pooled product draws only what it actually used -- the rate
    // must not add a phantom conversion for zero seconds.
    EXPECT_DOUBLE_EQ(chf::pooled_used_volume(p, 4096.0, 0.0, 0.0), 4096.0);
}
