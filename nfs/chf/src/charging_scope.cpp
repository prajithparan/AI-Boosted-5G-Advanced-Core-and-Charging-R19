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

} // namespace chf
