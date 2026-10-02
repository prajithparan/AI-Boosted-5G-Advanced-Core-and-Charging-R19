// Unit tests for libs/bss-sid's money_exact.hpp -- the fixed-point Micros type ADR-0446
// introduces. Property-style round-trip coverage, not just known-value spot checks, because the
// claim being tested ("exact for decimal text") is exactly the kind of thing reasoning about the
// code cannot settle on its own -- see money_exact.hpp's own header for why this exists.

#include <cstdint>
#include <random>

#include "bss_sid/money_exact.hpp"

#include <gtest/gtest.h>

using bss_sid::Micros;

TEST(BssSidMoneyExact, DecimalStringRoundTripsExactly) {
    struct Case {
        const char* text;
        std::int64_t expected_raw;
    };
    const Case cases[] = {
        {"0", 0},
        {"0.000000", 0},
        {"12.34", 12'340'000},
        {"12.340000", 12'340'000},
        {"0.000150", 150},
        {"-0.000150", -150},
        {"-12.34", -12'340'000},
        {"9223372036854.775807", 9223372036854775807LL},
        {"1", 1'000'000},
        {"0.1", 100'000},
        {"+5", 5'000'000},
    };
    for (const auto& c : cases) {
        const auto parsed = Micros::from_decimal_string(c.text);
        ASSERT_TRUE(parsed.has_value()) << "failed to parse: " << c.text;
        EXPECT_EQ(parsed->raw(), c.expected_raw) << "mismatch for: " << c.text;
    }
}

TEST(BssSidMoneyExact, RejectsMoreThanSixFractionalDigits) {
    EXPECT_FALSE(Micros::from_decimal_string("0.0000001").has_value());
    EXPECT_FALSE(Micros::from_decimal_string("12.1234567").has_value());
}

TEST(BssSidMoneyExact, RejectsMalformedInput) {
    EXPECT_FALSE(Micros::from_decimal_string("").has_value());
    EXPECT_FALSE(Micros::from_decimal_string(".5").has_value());
    EXPECT_FALSE(Micros::from_decimal_string("12.").has_value());
    EXPECT_FALSE(Micros::from_decimal_string("12.34.56").has_value());
    EXPECT_FALSE(Micros::from_decimal_string("abc").has_value());
    EXPECT_FALSE(Micros::from_decimal_string("12a").has_value());
    EXPECT_FALSE(Micros::from_decimal_string("--5").has_value());
    EXPECT_FALSE(Micros::from_decimal_string("12 ").has_value());
}

TEST(BssSidMoneyExact, ToDecimalStringRoundTrips) {
    // Property test, not a handful of guesses: random decimal strings with at most 6 fractional
    // digits, parsed then re-rendered, must produce byte-identical text (modulo the canonical
    // padding to 6 digits this type always emits).
    std::mt19937 rng(0xB55); // fixed seed -- deterministic CI, not flaky-by-design
    std::uniform_int_distribution<std::int64_t> whole_dist(0, 999'999);
    std::uniform_int_distribution<int> frac_dist(0, 999'999);
    std::uniform_int_distribution<int> sign_dist(0, 1);

    for (int i = 0; i < 2000; ++i) {
        const std::int64_t whole = whole_dist(rng);
        const int frac = frac_dist(rng);
        const bool negative = sign_dist(rng) == 1 && (whole != 0 || frac != 0);

        char frac_buf[8];
        std::snprintf(frac_buf, sizeof(frac_buf), "%06d", frac);
        const std::string text = (negative ? "-" : "") + std::to_string(whole) + "." + frac_buf;

        const auto parsed = Micros::from_decimal_string(text);
        ASSERT_TRUE(parsed.has_value()) << "failed to parse generated text: " << text;
        EXPECT_EQ(parsed->to_decimal_string(), text) << "round-trip mismatch for: " << text;

        // And the raw integer must match hand-computed micros, independent of the string path.
        const std::int64_t expected_raw = (negative ? -1 : 1) * (whole * 1'000'000 + frac);
        EXPECT_EQ(parsed->raw(), expected_raw) << "raw mismatch for: " << text;
    }
}

TEST(BssSidMoneyExact, FromDoubleRoundedMatchesExpectedMicros) {
    EXPECT_EQ(Micros::from_double_rounded(12.34).raw(), 12'340'000);
    EXPECT_EQ(Micros::from_double_rounded(0.0).raw(), 0);
    EXPECT_EQ(Micros::from_double_rounded(-5.5).raw(), -5'500'000);
    // Nearest-micro rounding, not truncation.
    EXPECT_EQ(Micros::from_double_rounded(0.0000004).raw(), 0);
    EXPECT_EQ(Micros::from_double_rounded(0.0000006).raw(), 1);
}

TEST(BssSidMoneyExact, ToDoubleRoundTripsWithinFloatingPointTolerance) {
    const auto m = Micros::from_decimal_string("1234.567891");
    ASSERT_TRUE(m.has_value());
    EXPECT_NEAR(m->to_double(), 1234.567891, 1e-9);
}

TEST(BssSidMoneyExact, ArithmeticIsExact) {
    const auto a = Micros::from_decimal_string("10.5").value();
    const auto b = Micros::from_decimal_string("0.5").value();
    EXPECT_EQ((a + b).to_decimal_string(), "11.000000");
    EXPECT_EQ((a - b).to_decimal_string(), "10.000000");
    EXPECT_EQ((-a).to_decimal_string(), "-10.500000");
}

TEST(BssSidMoneyExact, OrderingWorks) {
    const auto small = Micros::from_decimal_string("1.000000").value();
    const auto large = Micros::from_decimal_string("1.000001").value();
    EXPECT_LT(small, large);
    EXPECT_EQ(small, Micros::from_raw(1'000'000));
}
