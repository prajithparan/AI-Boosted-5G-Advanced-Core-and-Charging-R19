// ADR-0303: offering-scope matching, in its own translation unit for the same reason
// proportional_debit.cpp is (ADR-0297): it is pure predicate logic over two JSON values, and a
// test for it should not have to link the HTTP/2 client, the CDR writer and the ONNX quota sizer
// that charging_engine.cpp pulls in.

#include "charging_engine.hpp"

namespace chf {

bool charging_scope_matches(const nlohmann::json& scope, const nlohmann::json& attributes) {
    // No scope at all constrains nothing -- every offering configured before this feature existed
    // keeps behaving exactly as it did.
    if (scope.is_null() || !scope.is_object() || scope.empty()) {
        return true;
    }
    for (const auto& [key, required] : scope.items()) {
        const auto actual = attributes.find(key);
        if (actual == attributes.end()) {
            return false; // the request does not carry an attribute this offering constrains
        }
        if (required.is_array()) {
            // "usable on slice 1 OR slice 10" -- one offering, not two.
            bool any = false;
            for (const auto& candidate : required) {
                if (candidate == *actual) {
                    any = true;
                    break;
                }
            }
            if (!any) {
                return false;
            }
        } else if (required != *actual) {
            return false;
        }
    }
    return true;
}

bool rating_group_matches(const nlohmann::json& characteristic, std::int64_t rating_group) {
    if (characteristic.is_number_integer()) {
        return characteristic.get<std::int64_t>() == rating_group;
    }
    if (characteristic.is_array()) {
        for (const auto& candidate : characteristic) {
            if (candidate.is_number_integer() && candidate.get<std::int64_t>() == rating_group) {
                return true;
            }
        }
        return false;
    }
    // Anything else -- a string, an object, null -- is a misconfigured characteristic. It matches
    // NOTHING rather than everything: an offering whose rating-group scope cannot be read must not
    // silently become the offering that rates every request.
    return false;
}

} // namespace chf
