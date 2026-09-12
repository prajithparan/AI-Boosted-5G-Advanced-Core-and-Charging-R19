#include "protocol_attributes.hpp"

#include <algorithm>
#include <cctype>

#include "diameter_core/avp.hpp"
#include "diameter_core/avp_names.hpp"

namespace chf {
namespace {

// An AVP's data is bytes on the wire; its type is only known from a dictionary. ADR-0351 added a
// real name<->code dictionary (TS 32.299 V19.0.0), but that names the AVP, not its encoding -- the
// value is still offered in the forms a scope could plausibly need, and the operator picks the one
// matching their own network:
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

std::string numeric_segment(const diameter_core::Avp& avp) {
    // Vendor-specific AVPs carry the vendor id, because code 21 from 3GPP and code 21 from another
    // vendor are different attributes and must not collide into one scope key.
    return avp.vendor_id != 0
               ? "avp." + std::to_string(avp.vendor_id) + "." + std::to_string(avp.code)
               : "avp." + std::to_string(avp.code);
}

// ADR-0351: the AVP's real name when this project's dictionary carries it, and the numeric segment
// when it does not. Falling back to the numeric segment (rather than to an empty string) is what
// keeps a named path well-formed when an unnamed AVP sits between two named ones: the path stays
// "Gy.Service-Information.avp.10415.9999.PS-Information", never "Gy.Service-Information..".
std::string named_segment(const diameter_core::Avp& avp) {
    const auto name = diameter_core::avp_name(avp.vendor_id, avp.code);
    return name.empty() ? numeric_segment(avp) : std::string(name);
}

void emit(nlohmann::json& out, const std::string& key, const diameter_core::Avp& avp) {
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
}

// Two prefixes are carried through the recursion rather than one, so every AVP is reachable by
// BOTH a fully-numeric path and a named path -- exactly two paths per AVP, not two per level.
// Branching per level would make a depth-3 grouped AVP produce eight spellings of the same value,
// which is real cost on the charging hot path for no extra reach.
void add_avp(const diameter_core::Avp& avp,
             const std::string& numeric_prefix,
             const std::string& named_prefix,
             nlohmann::json& out,
             int depth) {
    const auto numeric_key = numeric_prefix + "." + numeric_segment(avp);
    const auto named_key = named_prefix + "." + named_segment(avp);

    emit(out, numeric_key, avp);
    // The numeric key is always present, so a name this dictionary lacks costs nothing and a name
    // it has is purely additive. `charging_scope_matches` (ADR-0303) looks up only the keys an
    // offering's own scope names, so extra keys can never change which offerings already matched.
    if (named_key != numeric_key) {
        emit(out, named_key, avp);
    }

    // Grouped AVPs: decode and walk, so a nested Service-Identifier inside an MSCC is scopable.
    // Depth-bounded -- a malformed or hostile message must not turn one charging request into
    // unbounded work on the hot path.
    if (depth > 0 && avp.data.size() >= 8) {
        if (const auto nested = diameter_core::decode_avps(avp.data); nested.has_value()) {
            for (const auto& child : *nested) {
                add_avp(child, numeric_key, named_key, out, depth - 1);
            }
        }
    }
}

std::string to_hex(const std::vector<std::uint8_t>& bytes) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const auto b : bytes) {
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 0x0f]);
    }
    return out;
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
        add_avp(avp, "Gy", "Gy", out, 3);
    }
    return out;
}

nlohmann::json cap_attributes(std::int32_t service_key,
                              const std::vector<std::uint8_t>& called_party_number,
                              const std::optional<std::vector<std::uint8_t>>& calling_party_number,
                              const std::string& supi) {
    nlohmann::json out = nlohmann::json::object();
    out["protocol"] = "CAP";
    out["chargingInformationType"] = "CAP";
    if (!supi.empty()) {
        out["subscriberIdentifier"] = supi;
    }
    // Named directly: these come from this project's own cap_core, which was built against
    // vendored CAMEL material, so naming them invents nothing. serviceKey is an ASN.1 INTEGER and
    // is emitted as one.
    out["CAP.serviceKey"] = service_key;
    // The party numbers are opaque octets here, and cap_operations.hpp says so for a real reason:
    // this repository has no decoder for CalledPartyNumber's own nature-of-address + TBCD layout.
    // They are offered as the raw wire bytes in hex rather than as digits, because a wrong decode
    // would silently scope a tariff onto the wrong calls -- and a reader would take the digits for
    // an E.164 number and never check. Hex is honest about being undecoded.
    if (!called_party_number.empty()) {
        out["CAP.calledPartyNumber.hex"] = to_hex(called_party_number);
    }
    if (calling_party_number.has_value() && !calling_party_number->empty()) {
        out["CAP.callingPartyNumber.hex"] = to_hex(*calling_party_number);
    }
    return out;
}

} // namespace chf
