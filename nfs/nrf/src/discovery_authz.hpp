#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

// Discovery-time authorization of NF profiles against the requester (TS 33.501 13.3.1.3 via
// TS 29.510 6.1.6.2.2 / 6.1.6.2.3 allowedNfTypes / allowedPlmns / allowedNssais, and
// 6.2.3.2.3.1 requester-nf-type / requester-plmn-list / requester-snssais). Private to nfs/nrf.
//
// Simplification, disclosed: allowedNfDomains (+ requester-nf-instance-fqdn) is NOT evaluated --
// the spec does not say which part of the FQDN is "the domain" the ECMA-262 pattern is matched
// against; asked, not invented. allowedSnpns / allowedNfDomains therefore never restrict.
namespace nrf {

struct DiscoveryRequester {
    std::optional<std::string> nf_type; // requester-nf-type
    nlohmann::json plmns;               // requester-plmn-list (array of PlmnId) or null
    nlohmann::json snssais;             // requester-snssais (array of Snssai) or null
};

// Absent allowed* list = unrestricted. Present list + requester info missing = not allowed
// (TS 29.510 NOTE 12 permits returning only unrestricted instances in that case).
// A service's own list prevails over the profile's; the profile is allowed if the profile passes
// and it has no nfServices, or at least one service passes.
bool profile_discoverable_by(const nlohmann::json& profile, const DiscoveryRequester& requester);

} // namespace nrf
