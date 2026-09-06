// ADR-0308 (C3): the AMBR-string-to-kbps conversion that decides how hard a subscriber is
// throttled.
//
// This is worth its own tests because every failure mode is SILENT. A factor-of-1000 scaling error
// throttles a subscriber a thousand times too hard, or not at all, and neither shows up as an
// error anywhere -- not in a log, not in a metric, not in a failed request. The only symptom is a
// bill or a complaint weeks later.

#include "ambr.hpp"

#include <gtest/gtest.h>

TEST(AmbrToKbps, ConvertsEveryRealBitRateUnit) {
    // TS 29.244's MBR is kbps, so kbps is the identity case and everything else scales to it.
    EXPECT_EQ(smf::ambr_to_kbps("200 Kbps").value_or(0), 200u);
    EXPECT_EQ(smf::ambr_to_kbps("1 Mbps").value_or(0), 1'000u);
    EXPECT_EQ(smf::ambr_to_kbps("5 Gbps").value_or(0), 5'000'000u);
    EXPECT_EQ(smf::ambr_to_kbps("2 Tbps").value_or(0), 2'000'000'000u);
    EXPECT_EQ(smf::ambr_to_kbps("8000 bps").value_or(0), 8u);
}

TEST(AmbrToKbps, IsCaseInsensitiveOnTheUnit) {
    // TS 29.571's own examples use "Kbps"/"Mbps", but a real PCF may send any casing and being
    // wrong here means silently not throttling.
    EXPECT_EQ(smf::ambr_to_kbps("1 MBPS").value_or(0), 1'000u);
    EXPECT_EQ(smf::ambr_to_kbps("1 mbps").value_or(0), 1'000u);
    EXPECT_EQ(smf::ambr_to_kbps("300 KBPS").value_or(0), 300u);
}

TEST(AmbrToKbps, HandlesFractionalRates) {
    EXPECT_EQ(smf::ambr_to_kbps("1.5 Mbps").value_or(0), 1'500u);
    EXPECT_EQ(smf::ambr_to_kbps("0.5 Gbps").value_or(0), 500'000u);
}

TEST(AmbrToKbps, RefusesWhatItDoesNotUnderstandRatherThanGuessing) {
    // Each of these would otherwise become some plausible-looking number, and a wrong rate limit
    // is worse than no rate limit because it looks deliberate.
    EXPECT_FALSE(smf::ambr_to_kbps("1Mbps").has_value()) << "no separator";
    EXPECT_FALSE(smf::ambr_to_kbps("fast").has_value());
    EXPECT_FALSE(smf::ambr_to_kbps("").has_value());
    EXPECT_FALSE(smf::ambr_to_kbps("1 furlongs-per-fortnight").has_value());
    EXPECT_FALSE(smf::ambr_to_kbps("-5 Mbps").has_value()) << "a negative rate is not a rate";
}

TEST(AmbrToKbps, ZeroIsARealRateNotAnError) {
    // 0 is a legitimate AMBR value and must round-trip as 0 rather than being confused with the
    // "could not parse" answer -- the two mean opposite things to the QER-building code.
    const auto zero = smf::ambr_to_kbps("0 Mbps");
    ASSERT_TRUE(zero.has_value());
    EXPECT_EQ(*zero, 0u);
}
