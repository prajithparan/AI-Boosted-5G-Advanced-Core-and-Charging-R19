#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

// Discovery-time authorization of NF profiles against the requester (TS 33.501 13.3.1.3 via
// TS 29.510 6.1.6.2.2 / 6.1.6.2.3 allowedNfTypes / allowedPlmns / allowedNssais, and
// 6.2.3.2.3.1 requester-nf-type / requester-plmn-list / requester-snssais). Private to nfs/nrf.
//
// allowedNfDomains: each entry is an ECMA-262 regex matched (search semantics, as ECMAScript
// RegExp.test) against the WHOLE requester-nf-instance-fqdn; no hostname/domain splitting
// (architect decision 2026-10-09). An invalid pattern matches nothing. Simplification, disclosed:
// the "ignore the FQDN for a requester in a different PLMN" rule is not applied, and allowedSnpns
// never restricts.
namespace nrf {

struct DiscoveryRequester {
    std::optional<std::string> nf_type; // requester-nf-type
    nlohmann::json plmns;               // requester-plmn-list (array of PlmnId) or null
    nlohmann::json snssais;             // requester-snssais (array of Snssai) or null
    std::optional<std::string> fqdn;    // requester-nf-instance-fqdn
};

// Absent allowed* list = unrestricted. Present list + requester info missing = not allowed
// (TS 29.510 NOTE 12 permits returning only unrestricted instances in that case).
// A service's own list prevails over the profile's; the profile is allowed if the profile passes
// and it has no nfServices, or at least one service passes.
bool profile_discoverable_by(const nlohmann::json& profile, const DiscoveryRequester& requester);

} // namespace nrf
