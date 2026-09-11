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
// TS 32.299 and RFC 4006 are NOT vendored in this repository, and this project's Diameter
// dictionary carries no constant for Called-Station-Id, Service-Context-Id or Service-Identifier.
// Writing `attributes["calledStationId"] = avp(30)` would be a code-to-name mapping recalled from
// memory -- exactly the fabrication this project forbids, and exactly the kind that is invisible
// until an operator's tariff silently prices the wrong thing.
//
// So attributes are keyed by what the WIRE actually carries: the AVP code, and the vendor id when
// one is present. `Gy.avp.432`, `Gy.avp.10415.21`. An operator scoping a product knows the codes
// their own network sends; nothing here has to guess. Where this project's own verified dictionary
// does name a code, the friendly name is emitted ALONGSIDE the numeric one, never instead of it.
//
// The result is total coverage with zero invention: every AVP the peer sends becomes scopable the
// moment it arrives, including AVPs added by a future release or by a vendor extension.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace diameter_core {
struct Avp;
}

namespace chf {

// Every AVP in the message, flattened to scopable attributes. Grouped AVPs are walked so a nested
// value (an MSCC's Service-Identifier, say) is reachable as "Gy.avp.456.avp.439".
nlohmann::json gy_attributes(const std::vector<diameter_core::Avp>& avps, const std::string& supi);

// CAMEL/CAP: the parsed operation's own parameters, by their real field names -- which come from
// this project's own cap_core, built against vendored material, so naming them invents nothing.
nlohmann::json cap_attributes(const std::string& service_key,
                              const std::string& calling_party,
                              const std::string& called_party,
                              const std::string& supi);

} // namespace chf
