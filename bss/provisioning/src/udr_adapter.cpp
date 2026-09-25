#include "udr_adapter.hpp"

#include <stdexcept>

namespace provisioning {

using nlohmann::json;

namespace {

json snssai_json(const Snssai& s) {
    json j{{"sst", s.sst}};
    if (!s.sd.empty()) {
        j["sd"] = s.sd;
    }
    return j;
}

// The smPolicySnssaiData map key. TS 29.519 says only "the key of the map is the S-NSSAI" with no
// wire encoding; this is the project's own "<sst>-<sd>" form, the one the PCF reads with
// (nfs/pcf/src/main.cpp snssai_map_key, ADR-0072).
std::string snssai_map_key(const Snssai& s) {
    return std::to_string(s.sst) + "-" + s.sd;
}

} // namespace

UdrAdapterConfig parse_udr_adapter_config(const json& config) {
    UdrAdapterConfig c;
    c.udr_base_url = config.at("udr_base_url").get<std::string>();
    c.serving_plmn_id = config.at("serving_plmn_id").get<std::string>();
    const json& auth = config.at("authentication");
    c.authentication_method = auth.at("method").get<std::string>();
    c.authentication_management_field = auth.at("management_field").get<std::string>();
    c.initial_sqn = auth.at("initial_sqn").get<std::string>();
    for (const auto& [key, p] : config.at("network_profiles").items()) {
        NetworkProfile np;
        for (const auto& s : p.at("snssais")) {
            np.snssais.push_back(Snssai{s.at("sst").get<int>(), s.value("sd", "")});
        }
        if (np.snssais.empty()) {
            throw std::runtime_error("network_profiles." + key + ".snssais must not be empty");
        }
        np.dnn = p.at("dnn").get<std::string>();
        np.ue_ambr_uplink = p.at("ue_ambr").at("uplink").get<std::string>();
        np.ue_ambr_downlink = p.at("ue_ambr").at("downlink").get<std::string>();
        c.network_profiles.emplace(key, std::move(np));
    }
    return c;
}

UdrAdapter::UdrAdapter(UdrAdapterConfig config, sbi_core::http2::TlsConfig tls)
    : config_(std::move(config)), client_(std::move(tls)) {}

tl::expected<json, std::string> UdrAdapter::build_documents(const SubscriberSpec& spec,
                                                            std::string* profile_key) const {
    auto it = config_.network_profiles.find(spec.offering_id);
    if (it == config_.network_profiles.end()) {
        it = config_.network_profiles.find("default");
    }
    if (it == config_.network_profiles.end()) {
        return tl::unexpected("no network profile for offering '" + spec.offering_id +
                              "' and no 'default' profile configured");
    }
    if (profile_key != nullptr) {
        *profile_key = it->first;
    }
    const NetworkProfile& p = it->second;

    // AccessAndMobilitySubscriptionData (TS29503_Nudm_SDM.yaml): nssai.defaultSingleNssais is
    // required inside Nssai; gpsis carries the MSISDN (Gpsi "msisdn-<digits>"); subscribedUeAmbr
    // is AmbrRm {uplink, downlink}.
    json default_nssais = json::array();
    for (const auto& s : p.snssais) {
        default_nssais.push_back(snssai_json(s));
    }
    json am_data{
        {"nssai", {{"defaultSingleNssais", default_nssais}}},
        {"subscribedUeAmbr", {{"uplink", p.ue_ambr_uplink}, {"downlink", p.ue_ambr_downlink}}}};
    if (!spec.msisdn.empty()) {
        am_data["gpsis"] = json::array({"msisdn-" + spec.msisdn});
    }

    // SessionManagementSubscriptionData: singleNssai required. One document, for the default
    // S-NSSAI -- the same floor the UDR's seeded subscribers carry. dnnConfigurations is left
    // unset, as it is for the seeded subscribers (disclosed, ADR-0382).
    json sm_data{{"singleNssai", snssai_json(p.snssais.front())}};

    // AuthenticationSubscription. encPermanentKey/encOpcKey carry K/OPc in clear hex with no
    // protectionParameterId -- the lab has no key-protection (HSM/transport-key) scheme yet;
    // disclosed in ADR-0382, not presented as encryption.
    json auth{
        {"authenticationMethod", config_.authentication_method},
        {"encPermanentKey", spec.sim.k},
        {"encOpcKey", spec.sim.opc},
        {"authenticationManagementField", config_.authentication_management_field},
        {"sequenceNumber", {{"sqn", spec.sim.sqn.empty() ? config_.initial_sqn : spec.sim.sqn}}},
        {"supi", spec.supi}};

    // SmPolicyData: smPolicySnssaiData (required, minProperties 1) -> SmPolicySnssaiData{snssai
    // required} -> smPolicyDnnData{dnn: SmPolicyDnnData{dnn required}}.
    json snssai_map = json::object();
    for (const auto& s : p.snssais) {
        snssai_map[snssai_map_key(s)] = {
            {"snssai", snssai_json(s)},
            {"smPolicyDnnData", {{p.dnn, {{"dnn", p.dnn}}}}},
        };
    }

    return json{
        {"servingPlmnId", config_.serving_plmn_id},
        {"amData", am_data},
        // SmfSelectionSubscriptionData has no required members; subscribedSnssaiInfos left unset,
        // matching the seeded subscribers (disclosed).
        {"smfSelectionData", json::object()},
        {"smData", sm_data},
        {"authenticationSubscription", auth},
        // AmPolicyData has no required members; nothing product-derived to put in it yet.
        {"amPolicyData", json::object()},
        {"smPolicyData", {{"smPolicySnssaiData", snssai_map}}},
    };
}

tl::expected<UdrProvisionOutcome, std::string> UdrAdapter::provision(const SubscriberSpec& spec) {
    UdrProvisionOutcome out;
    auto body = build_documents(spec, &out.profile_key);
    if (!body.has_value()) {
        return tl::unexpected(body.error());
    }
    sbi_core::http2::ClientRequest req;
    req.method = "PUT";
    req.url = config_.udr_base_url + "/oam-provisioning/v1/subscribers/" + spec.supi;
    req.headers.emplace("content-type", "application/json");
    req.body = body->dump();
    auto resp = client_.send(req);
    if (!resp.has_value()) {
        return tl::unexpected("UDR unreachable: " + resp.error());
    }
    out.http_status = resp->status;
    if (resp->status != 201 && resp->status != 204) {
        // The UDR's ProblemDetails never echoes field values, so its body is safe to record.
        return tl::unexpected("UDR returned " + std::to_string(resp->status) + ": " +
                              resp->body.substr(0, 512));
    }
    return out;
}

} // namespace provisioning
