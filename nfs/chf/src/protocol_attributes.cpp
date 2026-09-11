#include "protocol_attributes.hpp"

#include <algorithm>
#include <cctype>

#include "diameter_core/avp.hpp"

namespace chf {
namespace {

// An AVP's data is bytes on the wire; its type is only known from a dictionary this project does
// not have for the charging AVPs. So each value is offered in the forms a scope could plausibly
// need, and the operator picks the one matching their own network:
//
//   * the UTF-8 string, when every byte is printable (APNs, Service-Context-Id, MSISDNs)
//   * the unsigned 32-bit integer, when the value is exactly 4 bytes (Rating-Group, enums)
//
// Offering both rather than guessing one is the honest choice: a wrong type guess would make a
// tariff match nothing, silently, and the operator would have no way to see why.
bool printable(const std::vector<std::uint8_t>& data) {
    if (data.empty()) {
        return false;
    }
    return std::all_of(data.begin(), data.end(), [](std::uint8_t b) {
        return b == '\t' || b == ' ' || (b >= 0x21 && b < 0x7f);
    });
}

std::string key_for(const diameter_core::Avp& avp, const std::string& prefix) {
    // Vendor-specific AVPs carry the vendor id, because code 21 from 3GPP and code 21 from another
    // vendor are different attributes and must not collide into one scope key.
    return avp.vendor_id != 0
               ? prefix + ".avp." + std::to_string(avp.vendor_id) + "." + std::to_string(avp.code)
               : prefix + ".avp." + std::to_string(avp.code);
}

void add_avp(const diameter_core::Avp& avp,
             const std::string& prefix,
             nlohmann::json& out,
             int depth) {
    const auto key = key_for(avp, prefix);
    if (printable(avp.data)) {
        out[key] = std::string(avp.data.begin(), avp.data.end());
    }
    if (avp.data.size() == 4) {
        const std::uint32_t v = (static_cast<std::uint32_t>(avp.data[0]) << 24) |
                                (static_cast<std::uint32_t>(avp.data[1]) << 16) |
                                (static_cast<std::uint32_t>(avp.data[2]) << 8) |
                                static_cast<std::uint32_t>(avp.data[3]);
        out[key + ".u32"] = v;
    }
    // Grouped AVPs: decode and walk, so a nested Service-Identifier inside an MSCC is scopable.
    // Depth-bounded -- a malformed or hostile message must not turn one charging request into
    // unbounded work on the hot path.
    if (depth > 0 && avp.data.size() >= 8) {
        if (const auto nested = diameter_core::decode_avps(avp.data); nested.has_value()) {
            for (const auto& child : *nested) {
                add_avp(child, key, out, depth - 1);
            }
        }
    }
}

} // namespace

nlohmann::json gy_attributes(const std::vector<diameter_core::Avp>& avps, const std::string& supi) {
    nlohmann::json out = nlohmann::json::object();
    // The protocol itself is scopable, so an operator can price legacy traffic differently from
    // 5G without having to find a distinguishing AVP.
    out["protocol"] = "Gy";
    out["chargingInformationType"] = "Gy";
    if (!supi.empty()) {
        out["subscriberIdentifier"] = supi;
    }
    for (const auto& avp : avps) {
        add_avp(avp, "Gy", out, 3);
    }
    return out;
}

nlohmann::json cap_attributes(const std::string& service_key,
                              const std::string& calling_party,
                              const std::string& called_party,
                              const std::string& supi) {
    nlohmann::json out = nlohmann::json::object();
    out["protocol"] = "CAP";
    out["chargingInformationType"] = "CAP";
    if (!supi.empty()) {
        out["subscriberIdentifier"] = supi;
    }
    // Named directly: these come from this project's own cap_core, which was built against
    // vendored CAMEL material, so naming them invents nothing.
    if (!service_key.empty()) {
        out["CAP.serviceKey"] = service_key;
    }
    if (!calling_party.empty()) {
        out["CAP.callingPartyNumber"] = calling_party;
    }
    if (!called_party.empty()) {
        out["CAP.calledPartyNumber"] = called_party;
    }
    return out;
}

} // namespace chf
