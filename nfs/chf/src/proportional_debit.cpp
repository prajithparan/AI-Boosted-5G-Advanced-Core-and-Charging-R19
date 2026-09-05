// ADR-0297: the consumed-fraction arithmetic, in its own translation unit ON PURPOSE.
//
// It is declared in charging_engine.hpp with the rest of the charging API, because that is where a
// reader looks for it. It is DEFINED here because charging_engine.cpp pulls in the HTTP/2 client,
// the CDR writer, the rating-decision store and the ONNX quota sizer, and a test that wants to
// check a division should not have to link all of that. Pure arithmetic, no dependencies beyond
// <algorithm>, compiled into both the chf binary and the test binary.

#include <algorithm>

#include "charging_engine.hpp"

namespace chf {

double proportional_debit(double total_reserved,
                          double granted_volume,
                          double used_volume,
                          double granted_service_units,
                          double used_service_units,
                          double granted_time,
                          double used_time) {
    if (total_reserved <= 0.0) {
        return 0.0;
    }
    // No grant recorded at all -> nothing to proportion against. Charge the full reservation
    // rather than nothing: an unmeasurable session is not a free session.
    const bool has_volume_grant = granted_volume > 0.0;
    const bool has_unit_grant = granted_service_units > 0.0;
    const bool has_time_grant = granted_time > 0.0;
    if (!has_volume_grant && !has_unit_grant && !has_time_grant) {
        return total_reserved;
    }

    double granted_weight = 0.0;
    double consumed_weight = 0.0;
    if (has_volume_grant) {
        granted_weight += granted_volume;
        consumed_weight += std::min(used_volume, granted_volume);
    }
    if (has_unit_grant) {
        granted_weight += granted_service_units;
        consumed_weight += std::min(used_service_units, granted_service_units);
    }
    // ADR-0304: seconds, proportioned against their own grant like the other two dimensions and
    // never summed with them -- octets, service units and seconds are not commensurable.
    if (has_time_grant) {
        granted_weight += granted_time;
        consumed_weight += std::min(used_time, granted_time);
    }
    if (granted_weight <= 0.0) {
        return total_reserved;
    }
    const double fraction = consumed_weight / granted_weight;
    return std::clamp(total_reserved * fraction, 0.0, total_reserved);
}

} // namespace chf
