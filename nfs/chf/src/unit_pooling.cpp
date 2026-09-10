// ADR-0330. Separate translation unit for the same reason proportional_debit.cpp is one: this is
// arithmetic plus a small amount of JSON parsing, and a test that checks a conversion should not
// have to link the HTTP/2 client, the CDR writer and the ONNX quota sizer.

#include "unit_pooling.hpp"

#include <cmath>

namespace chf {
namespace {

// A factor is only usable if it is a real, finite, strictly positive number. Zero is rejected
// along with the rest: a configured rate of zero means "this traffic is free", which an operator
// expresses by omitting the field, and accepting it here would make a typo indistinguishable from
// a deliberate giveaway.
std::optional<double> usable_factor(const nlohmann::json& value, const char* key) {
    if (!value.contains(key)) {
        return std::nullopt;
    }
    const auto& field = value.at(key);
    if (!field.is_number()) {
        return std::nullopt;
    }
    const double factor = field.get<double>();
    if (!std::isfinite(factor) || factor <= 0.0) {
        return std::nullopt;
    }
    return factor;
}

} // namespace

UnitPooling parse_unit_pooling(const std::optional<nlohmann::json>& characteristic_value) {
    UnitPooling out;
    if (!characteristic_value.has_value() || !characteristic_value->is_object()) {
        return out;
    }
    out.octets_per_second = usable_factor(*characteristic_value, "octetsPerSecond");
    out.octets_per_service_unit = usable_factor(*characteristic_value, "octetsPerServiceUnit");
    return out;
}

double pooled_used_volume(const UnitPooling& pooling,
                          double used_volume,
                          double used_service_units,
                          double used_time) {
    double total = used_volume;
    if (pooling.octets_per_second.has_value() && used_time > 0.0) {
        total += used_time * *pooling.octets_per_second;
    }
    if (pooling.octets_per_service_unit.has_value() && used_service_units > 0.0) {
        total += used_service_units * *pooling.octets_per_service_unit;
    }
    return total;
}

} // namespace chf
