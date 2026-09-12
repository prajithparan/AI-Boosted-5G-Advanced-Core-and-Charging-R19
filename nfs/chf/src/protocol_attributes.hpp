#pragma once

// ADR-0346: make ANY protocol attribute productisable -- 5G N40, 4G Diameter Gy, and CAMEL/CAP.
//
// The rating engine scopes a product on a flat attribute map (ADR-0303). Only the N40 path ever
// built one, so README recorded the consequence plainly: "N40 only: Diameter Gy and CAP carry no
// TS 32.291 request and so match only unscoped offerings."
//
// That is a capability gap, not a cosmetic one. An operator running 4G alongside 5G could not
// price a Gy-charged APN, a CAMEL service key, a roaming SGSN or a content Service-Identifier at
// all -- every legacy session fell through to whatever unscoped offering happened to match its
// rating group. Voice, SMS and content on the legacy stack were unproductisable.
//
// WHAT THIS DELIBERATELY DOES NOT DO: invent AVP names.
//
// Attributes are keyed by what the WIRE actually carries: the AVP code, and the vendor id when one
// is present. `Gy.avp.432`, `Gy.avp.10415.21`. An operator scoping a product knows the codes their
// own network sends; nothing here has to guess, and an AVP added by a future release or a vendor
// extension becomes scopable the moment it arrives rather than when someone updates a table.
//
// ADR-0351: the numeric key is now joined by a NAMED one -- `Gy.Rating-Group` alongside
// `Gy.avp.432` -- for every code in diameter_core::avp_name, which is generated from the real
// TS 32.299 V19.0.0 the user supplied at specs/TS_32-299.pdf and cross-checked four ways before
// it will emit (see tools/avp-dictionary/extract_avp_names.py). Alongside, never instead:
//
//   * a name this dictionary lacks costs nothing, because the numeric key is always emitted;
//   * a name it has cannot change any existing match, because `charging_scope_matches` looks up
//     only the keys an offering's own scope names (ADR-0303);
//   * so a wrong name could only ever fail to match, never silently match the wrong thing -- and
//     the generator refuses to emit at all if its three sources disagree.
//
// Each AVP gets exactly two paths, one fully numeric and one named, not two per nesting level.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace diameter_core {
struct Avp;
}

namespace chf {

// Every AVP in the message, flattened to scopable attributes. Grouped AVPs are walked so a nested
// value (an MSCC's Service-Identifier, say) is reachable as "Gy.avp.456.avp.439".
nlohmann::json gy_attributes(const std::vector<diameter_core::Avp>& avps, const std::string& supi);

// CAMEL/CAP: the parsed InitialDP's own parameters, by their real field names -- which come from
// this project's own cap_core, built against vendored material, so naming them invents nothing.
// Takes the parameters in the types cap_core actually decodes them into: the party numbers are
// opaque octets (cap_operations.hpp), NOT digit strings, and are not pretended otherwise here.
nlohmann::json cap_attributes(std::int32_t service_key,
                              const std::vector<std::uint8_t>& called_party_number,
                              const std::optional<std::vector<std::uint8_t>>& calling_party_number,
                              const std::string& supi);

} // namespace chf
