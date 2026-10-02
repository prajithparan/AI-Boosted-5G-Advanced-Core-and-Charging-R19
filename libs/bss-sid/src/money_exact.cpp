#include "bss_sid/money_exact.hpp"

#include <charconv>
#include <cmath>
#include <string>
#include <system_error>

namespace bss_sid {

namespace {

constexpr int kScaleDigits = 6;
constexpr std::int64_t kScale = 1'000'000;

} // namespace

std::optional<Micros> Micros::from_decimal_string(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }

    bool negative = false;
    std::size_t pos = 0;
    if (text[pos] == '+' || text[pos] == '-') {
        negative = (text[pos] == '-');
        ++pos;
    }

    const std::size_t int_start = pos;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        ++pos;
    }
    const std::string_view int_digits = text.substr(int_start, pos - int_start);
    if (int_digits.empty()) {
        return std::nullopt; // no integer part at all -- not a valid decimal (".5" rejected)
    }

    std::string_view frac_digits;
    if (pos < text.size()) {
        if (text[pos] != '.') {
            return std::nullopt; // trailing garbage that isn't a decimal point
        }
        ++pos;
        const std::size_t frac_start = pos;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            ++pos;
        }
        frac_digits = text.substr(frac_start, pos - frac_start);
        if (frac_digits.empty() || frac_digits.size() > kScaleDigits) {
            // "12." with nothing after, or more fractional precision than this type's scale --
            // the latter is a real precision loss this function refuses to silently round away.
            return std::nullopt;
        }
    }

    if (pos != text.size()) {
        return std::nullopt; // unconsumed trailing characters
    }

    // Normalize to exactly kScaleDigits fractional digits, then parse the whole thing (integer
    // part + padded fraction) as one integer -- never via a float at any point.
    std::string normalized(int_digits);
    normalized.append(frac_digits);
    normalized.append(kScaleDigits - frac_digits.size(), '0');

    std::int64_t magnitude = 0;
    const auto [ptr, ec] =
        std::from_chars(normalized.data(), normalized.data() + normalized.size(), magnitude);
    if (ec != std::errc{} || ptr != normalized.data() + normalized.size()) {
        return std::nullopt; // overflow (int64) or, defensively, a stray non-digit
    }

    return Micros::from_raw(negative ? -magnitude : magnitude);
}

Micros Micros::from_double_rounded(double value) {
    // Round-half-away-from-zero at the 6th decimal place. Not exact if `value` already lost
    // precision before this call -- see money_exact.hpp's own header comment.
    const double scaled = value * static_cast<double>(kScale);
    return Micros::from_raw(static_cast<std::int64_t>(std::llround(scaled)));
}

double Micros::to_double() const {
    return static_cast<double>(raw_micros_) / static_cast<double>(kScale);
}

std::string Micros::to_decimal_string() const {
    std::int64_t magnitude = raw_micros_;
    std::string sign;
    if (magnitude < 0) {
        sign = "-";
        magnitude = -magnitude;
    }
    const std::int64_t whole = magnitude / kScale;
    const std::int64_t frac = magnitude % kScale;

    std::string frac_str = std::to_string(frac);
    frac_str.insert(0, kScaleDigits - frac_str.size(), '0');

    return sign + std::to_string(whole) + "." + frac_str;
}

} // namespace bss_sid
