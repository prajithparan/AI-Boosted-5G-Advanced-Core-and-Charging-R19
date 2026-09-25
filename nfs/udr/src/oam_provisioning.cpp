#include "oam_provisioning.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <regex>
#include <sbi_core/json_body.hpp>

namespace udr::oam {

using nlohmann::json;

namespace {

bool matches(const json& v, const std::regex& re) {
    return v.is_string() && std::regex_match(v.get<std::string>(), re);
}

// AuthMethod (TS29505_Subscription_Data.yaml) is an anyOf enum-or-string; only the enum values are
// accepted here, since UDM can only act on those.
bool known_auth_method(const json& v) {
    static const std::vector<std::string> kMethods = {
        "5G_AKA", "EAP_AKA_PRIME", "EAP_TLS", "EAP_TTLS", "NONE"};
    return v.is_string() &&
           std::find(kMethods.begin(), kMethods.end(), v.get<std::string>()) != kMethods.end();
}

tl::unexpected<std::string> bad(const std::string& msg) {
    return tl::unexpected(msg);
}

} // namespace

bool peer_allowed(const sbi_core::http2::Request& req, const std::vector<std::string>& allowed) {
    const auto on_list = [&allowed](const std::string& id) {
        return !id.empty() && std::find(allowed.begin(), allowed.end(), id) != allowed.end();
    };
    if (on_list(req.peer_cert_cn)) {
        return true;
    }
    return std::any_of(req.peer_cert_dns_names.begin(), req.peer_cert_dns_names.end(), on_list);
}

tl::expected<SubscriberDocuments, std::string> parse_subscriber_documents(const std::string& ue_id,
                                                                          const json& body) {
    // Supi pattern (TS29571_CommonData.yaml) also admits nai-/gci-/gli- forms; this API accepts
    // the IMSI form only, the one bss/provisioning allocates today (disclosed in ADR-0382).
    static const std::regex kImsiSupi("^imsi-[0-9]{5,15}$");
    static const std::regex kPlmn("^[0-9]{5,6}$");
    static const std::regex kHex128("^[A-Fa-f0-9]{32}$");
    static const std::regex kAmf("^[A-Fa-f0-9]{4}$");
    static const std::regex kSqn("^[A-Fa-f0-9]{12}$");

    if (!std::regex_match(ue_id, kImsiSupi)) {
        return bad("ueId must be an IMSI-form SUPI (imsi-<5..15 digits>)");
    }
    if (!body.is_object()) {
        return bad("body must be a JSON object");
    }
    SubscriberDocuments docs;
    if (!matches(body.value("servingPlmnId", json()), kPlmn)) {
        return bad("servingPlmnId must be MCC+MNC (5 or 6 digits)");
    }
    docs.serving_plmn_id = body["servingPlmnId"].get<std::string>();

    for (const char* key : {"amData",
                            "smfSelectionData",
                            "smData",
                            "authenticationSubscription",
                            "amPolicyData",
                            "smPolicyData"}) {
        if (!body.contains(key) || !body[key].is_object()) {
            return bad(std::string(key) + " is required and must be an object");
        }
    }

    // SessionManagementSubscriptionData: singleNssai is required (TS29503_Nudm_SDM.yaml).
    if (!body["smData"].contains("singleNssai") || !body["smData"]["singleNssai"].is_object()) {
        return bad("smData.singleNssai is required (SessionManagementSubscriptionData)");
    }
    // SmPolicyData: smPolicySnssaiData is required, minProperties 1 (TS29519_Policy_Data.yaml).
    const json& sm_pol = body["smPolicyData"];
    if (!sm_pol.contains("smPolicySnssaiData") || !sm_pol["smPolicySnssaiData"].is_object() ||
        sm_pol["smPolicySnssaiData"].empty()) {
        return bad("smPolicyData.smPolicySnssaiData is required with at least one S-NSSAI");
    }

    // AuthenticationSubscription (TS29505_Subscription_Data.yaml): authenticationMethod required.
    const json& auth = body["authenticationSubscription"];
    if (!known_auth_method(auth.value("authenticationMethod", json()))) {
        return bad("authenticationSubscription.authenticationMethod missing or not an AuthMethod");
    }
    const auto method = auth["authenticationMethod"].get<std::string>();
    if (method == "5G_AKA" || method == "EAP_AKA_PRIME") {
        // AKA needs K and OPc (TS 35.206: 128-bit each). The YAML types both as plain strings;
        // 32 hex digits is this API's own stricter requirement.
        if (!matches(auth.value("encPermanentKey", json()), kHex128)) {
            return bad("authenticationSubscription.encPermanentKey must be 32 hex digits for AKA");
        }
        if (!matches(auth.value("encOpcKey", json()), kHex128)) {
            return bad("authenticationSubscription.encOpcKey must be 32 hex digits for AKA");
        }
    }
    if (auth.contains("authenticationManagementField") &&
        !matches(auth["authenticationManagementField"], kAmf)) {
        return bad("authenticationSubscription.authenticationManagementField must be 4 hex digits");
    }
    if (auth.contains("sequenceNumber")) {
        const json& sn = auth["sequenceNumber"];
        if (!sn.is_object() || (sn.contains("sqn") && !matches(sn["sqn"], kSqn))) {
            return bad("authenticationSubscription.sequenceNumber.sqn must be 12 hex digits");
        }
    }
    if (auth.contains("supi") && auth["supi"] != ue_id) {
        return bad("authenticationSubscription.supi does not match the ueId in the path");
    }

    docs.am_data = body["amData"];
    docs.smf_selection_data = body["smfSelectionData"];
    docs.sm_data = body["smData"];
    docs.authentication_subscription = auth;
    docs.am_policy_data = body["amPolicyData"];
    docs.sm_policy_data = sm_pol;
    return docs;
}

void register_routes(sbi_core::http2::Server& server,
                     SubscriberProvisioningStore& store,
                     std::vector<std::string> allowed_clients) {
    server.add_route(
        "PUT",
        std::string(kOamProvisioningRoot) + "/subscribers/{ueId}",
        [&store, allowed = std::move(allowed_clients)](const sbi_core::http2::Request& req) {
            if (!peer_allowed(req, allowed)) {
                spdlog::warn("udr: OAM provisioning refused for mTLS peer CN='{}'",
                             req.peer_cert_cn);
                return sbi_core::http2::problem_response(
                    403, "Forbidden", "mTLS peer is not an authorised provisioning client");
            }
            json body;
            try {
                body = json::parse(req.body);
            } catch (const json::parse_error&) {
                // parse_error's what() can quote the offending input, which may contain K/OPc.
                return sbi_core::http2::problem_response(
                    400, "Malformed JSON", "request body is not valid JSON");
            }
            const auto& ue_id = req.path_params.at("ueId");
            auto docs = parse_subscriber_documents(ue_id, body);
            if (!docs.has_value()) {
                return sbi_core::http2::problem_response(400, "Bad Request", docs.error());
            }
            const bool created = store.replace(ue_id, *docs);
            spdlog::info(
                "udr: OAM provisioning {} subscriber {}", created ? "created" : "replaced", ue_id);
            sbi_core::http2::Response resp;
            resp.status = created ? 201 : 204;
            return resp;
        });
}

} // namespace udr::oam
