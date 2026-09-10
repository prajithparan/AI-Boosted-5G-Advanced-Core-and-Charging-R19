#pragma once

// ADR-0330: unit pooling -- one allowance drawn down by traffic measured in different units.
//
// ADR-0309 made a voice+data bundle a single product for MONEY: one offering price whose
// `ratingGroup` is an array, so both kinds of traffic debit the same balance. What it explicitly
// did NOT do was pool a single *unit* allowance -- "10 GB usable as data or as minutes" -- and the
// stated reason was that converting minutes to octets requires a rate no specification defines.
//
// That reasoning was right about the specification and wrong about the conclusion. 3GPP does not
// define the rate because it is not 3GPP's to define: it is a commercial term the operator sets,
// exactly like the price itself. So the rate is CONFIGURATION, carried on the offering price as a
// `unitPooling` characteristic, and this project supplies none of its own:
//
//   {"name": "unitPooling",
//    "value": {"octetsPerSecond": 87381, "octetsPerServiceUnit": 1000000}}
//
// Each field is optional. Absent means "traffic in that unit does not draw on the pooled
// allowance", which leaves the existing per-dimension behaviour exactly as it was -- so an
// offering with no `unitPooling` characteristic is unaffected by any of this.
//
// The direction is deliberately one-way: other units convert INTO the pooled volume, never the
// reverse. A bundle is sold as "10 GB, and voice draws from it at this rate", not as a mutually
// convertible currency pair, and a two-way conversion would let rounding move allowance back and
// forth across a boundary the operator never agreed to.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>

namespace chf {

struct UnitPooling {
    // Octets drawn from the pooled volume allowance per second of measured time. The operator's
    // commercial rate, not a bitrate this project inferred from anything.
    std::optional<double> octets_per_second;
    // Octets drawn per service-specific unit (a message, an event, a session).
    std::optional<double> octets_per_service_unit;

    bool any() const {
        return octets_per_second.has_value() || octets_per_service_unit.has_value();
    }
};

// Parses the `unitPooling` characteristic value. Returns a UnitPooling with nothing set when the
// value is absent, malformed, or carries no usable factor -- never a guessed rate. A negative or
// non-finite factor is rejected the same way: it would silently turn consumption into a refund.
UnitPooling parse_unit_pooling(const std::optional<nlohmann::json>& characteristic_value);

// Folds time and service-unit usage into an octet-equivalent, at the operator's configured rate.
// Returns `used_volume` unchanged when no rate is configured for a dimension, which is what makes
// this safe to call unconditionally on every Release.
double pooled_used_volume(const UnitPooling& pooling,
                          double used_volume,
                          double used_service_units,
                          double used_time);

} // namespace chf
