#include "discovery_authz.hpp"

#include <regex>

namespace nrf {
namespace {

bool plmn_eq(const nlohmann::json& a, const nlohmann::json& b) {
    return a.is_object() && b.is_object() && a.value("mcc", "") == b.value("mcc", "#") &&
           a.value("mnc", "") == b.value("mnc", "#");
}

// S-NSSAI match per TS 29.510 NOTE 10: SST and SD identical; absent SD never matches present SD.
bool snssai_eq(const nlohmann::json& a, const nlohmann::json& b) {
    if (!a.is_object() || !b.is_object() || !a.contains("sst") ||
        a.at("sst") != b.value("sst", -1)) {
        return false;
    }
    const bool a_sd = a.contains("sd");
    const bool b_sd = b.contains("sd");
    return a_sd == b_sd && (!a_sd || a.at("sd") == b.at("sd"));
}

const nlohmann::json*
list_of(const nlohmann::json& service, const nlohmann::json& profile, const char* key) {
    if (service.is_object() && service.contains(key) && service.at(key).is_array()) {
        return &service.at(key);
    }
    if (profile.contains(key) && profile.at(key).is_array()) {
        return &profile.at(key);
    }
    return nullptr;
}

bool allowed(const nlohmann::json& service,
             const nlohmann::json& profile,
             const DiscoveryRequester& r) {
    if (const auto* types = list_of(service, profile, "allowedNfTypes"); types != nullptr) {
        if (!r.nf_type || !types->is_array()) {
            return false;
        }
        bool hit = false;
        for (const auto& t : *types) {
            hit = hit || (t.is_string() && t.get<std::string>() == *r.nf_type);
        }
        if (!hit) {
            return false;
        }
    }
    if (const auto* domains = list_of(service, profile, "allowedNfDomains"); domains != nullptr) {
        if (!r.fqdn) {
            return false;
        }
        bool hit = false;
        for (const auto& d : *domains) {
            if (!d.is_string()) {
                continue;
            }
            try {
                hit = hit || std::regex_search(
                                 *r.fqdn, std::regex(d.get<std::string>(), std::regex::ECMAScript));
            } catch (const std::regex_error&) {
                // Invalid pattern: grants nothing.
            }
        }
        if (!hit) {
            return false;
        }
    }
    if (const auto* plmns = list_of(service, profile, "allowedPlmns"); plmns != nullptr) {
        if (!r.plmns.is_array() || r.plmns.empty()) {
            return false;
        }
        bool hit = false;
        for (const auto& rp : r.plmns) {
            for (const auto& ap : *plmns) {
                hit = hit || plmn_eq(rp, ap);
            }
            // The profile's own plmnList is implicitly allowed (TS 29.510 6.1.6.2.3).
            if (profile.contains("plmnList") && profile.at("plmnList").is_array()) {
                for (const auto& pp : profile.at("plmnList")) {
                    hit = hit || plmn_eq(rp, pp);
                }
            }
        }
        if (!hit) {
            return false;
        }
    }
    if (const auto* nssais = list_of(service, profile, "allowedNssais"); nssais != nullptr) {
        if (!r.snssais.is_array() || r.snssais.empty()) {
            return false;
        }
        bool hit = false;
        for (const auto& rs : r.snssais) {
            for (const auto& as : *nssais) {
                hit = hit || snssai_eq(rs, as);
            }
        }
        if (!hit) {
            return false;
        }
    }
    return true;
}

} // namespace

bool profile_discoverable_by(const nlohmann::json& profile, const DiscoveryRequester& requester) {
    const nlohmann::json none;
    if (profile.contains("nfServices")) {
        const auto& svcs = profile.at("nfServices");
        if (svcs.is_array() && !svcs.empty()) {
            for (const auto& s : svcs) {
                if (allowed(s, profile, requester)) {
                    return true;
                }
            }
            return false;
        }
        if (svcs.is_object() && !svcs.empty()) {
            for (const auto& [k, s] : svcs.items()) {
                if (allowed(s, profile, requester)) {
                    return true;
                }
            }
            return false;
        }
    }
    return allowed(none, profile, requester);
}

} // namespace nrf
