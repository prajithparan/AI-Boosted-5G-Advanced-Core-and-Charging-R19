#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// ADR-0445/ADR-0446: an exact, fixed-point money representation, introduced because
// bss_sid::Money::value / Quantity::amount (product.hpp) are `double` -- the real TMF620/654 wire
// shape, confirmed against the actual swagger, and NOT changed by this file. Changing those field
// types would ripple into gui/schema-gen's derived TMF620 schemas, bill_run, billing_items,
// rating_decision_store and balance-management -- out of scope for this increment (ADR-0445 scopes
// that to increment 2/3). This type exists so CHF's new in-memory catalog/policy snapshot
// (ADR-0446) can hold prices exactly internally, converting to/from the existing double wire shape
// only at the boundary, rather than carrying `double` through the snapshot's own arithmetic.
//
// Scale: 6 decimal places (micro-units), user-decided 2026-10-02 -- see docs/DECISIONS.md ADR-0445.
// No committed catalog seed data with real per-unit rates exists in this repo to confirm the scale
// empirically against; 6dp (the same convention Google Ads API's "micros" and several real telco
// billing engines use) was chosen as a documented default headroom for sub-cent metered rates.
//
// Real, disclosed limitation of the exactness guarantee (found while writing this -- see ADR-0446):
// `bss_sid::Money`/`Quantity` are already `double` on the wire, and `bss/product-catalog`'s own
// schema stores `product_offering_price` as JSONB, round-tripped through nlohmann's default
// double-based (de)serialization before this code ever sees it. This type is therefore exact ONLY
// for values it receives as decimal text (`from_decimal_string`); `from_double_rounded` documents,
// rather than hides, that precision may already be lost upstream of it. Fixing that end-to-end
// (a SAX raw-string parse at the SBI boundary, and converting product-catalog's own storage) is a
// separate, larger change, not done in this pass.
namespace bss_sid {

// A monetary/quantity amount in exact micro-units (1 Micros unit = 1e-6 of a major currency unit
// or of whatever base unit a Quantity is denominated in). Backed by int64_t: range is
// approximately +/-9.2 * 10^12 major units at this scale, far beyond any real balance or tariff
// this project handles.
class Micros {
public:
    constexpr Micros() = default;

    // Constructs directly from an already-scaled integer (value * 1,000,000). Prefer
    // from_decimal_string/from_double_rounded at a real boundary; this constructor is for code
    // that already has a micro-unit integer (e.g. a value coming back out of another Micros).
    static constexpr Micros from_raw(std::int64_t raw_micros) { return Micros{raw_micros}; }

    // Exact parse of a decimal string (optional leading '-', digits, optional '.', up to 6
    // fractional digits). Returns std::nullopt on malformed input, more than 6 fractional digits
    // (a real precision loss this function refuses to silently round away), or overflow. This is
    // the ONLY path that is actually exact end-to-end -- use it wherever the caller can get raw
    // decimal text instead of a pre-parsed double.
    static std::optional<Micros> from_decimal_string(std::string_view text);

    // Rounds a double (round-half-away-from-zero at the 6th decimal place, via std::llround) into
    // Micros. NOT exact if the double already lost precision before reaching this call -- see this
    // file's header. Use only at the boundary with existing double-typed fields
    // (bss_sid::Money::value, Quantity::amount) that this increment does not change.
    static Micros from_double_rounded(double value);

    constexpr std::int64_t raw() const { return raw_micros_; }

    // Lossy by construction (the wire shape this project still uses today) -- for passing a
    // Micros value into bss_sid::Money/Quantity or an existing double-typed API.
    double to_double() const;

    // Exact decimal text (e.g. "12.340000"), no floating-point formatting involved.
    std::string to_decimal_string() const;

    constexpr Micros operator+(Micros other) const {
        return Micros{raw_micros_ + other.raw_micros_};
    }
    constexpr Micros operator-(Micros other) const {
        return Micros{raw_micros_ - other.raw_micros_};
    }
    constexpr Micros operator-() const { return Micros{-raw_micros_}; }

    constexpr auto operator<=>(const Micros&) const = default;

private:
    explicit constexpr Micros(std::int64_t raw_micros) : raw_micros_(raw_micros) {}

    std::int64_t raw_micros_ = 0;
};

inline constexpr Micros kMicrosScale = Micros::from_raw(1'000'000);

} // namespace bss_sid
