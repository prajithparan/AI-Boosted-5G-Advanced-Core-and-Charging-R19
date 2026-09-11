#include "charging_information.hpp"

#include "TS26510_CommonData_grp.hpp"

namespace chf {

DetectedChargingInformation
detect_charging_information(const sbi_gen::ChargingDataRequest_Nchf_ConvergedCharging& request) {
    DetectedChargingInformation out;

    // One line per charging-information block TS 32.291 defines. Ordered as the specification
    // lists them, and exhaustive: every optional block on the generated request DTO appears here.
    //
    // The macro keeps that exhaustiveness checkable by eye -- twenty-five near-identical branches
    // written out by hand is where a type gets quietly omitted and a service stops being billable.
#define CHF_DETECT(field, name)                                                                    \
    if (request.field.has_value()) {                                                               \
        if (out.type.empty()) {                                                                    \
            out.type = (name);                                                                     \
        }                                                                                          \
        out.payload[(name)] = nlohmann::json(*request.field);                                      \
    }

    CHF_DETECT(pDUSessionChargingInformation, "PDUSession")
    CHF_DETECT(sMSChargingInformation, "SMS")
    CHF_DETECT(mMTelChargingInformation, "MMTel")
    CHF_DETECT(iMSChargingInformation, "IMS")
    CHF_DETECT(mMSChargingInformation, "MMS")
    CHF_DETECT(registrationChargingInformation, "Registration")
    CHF_DETECT(n2ConnectionChargingInformation, "N2Connection")
    CHF_DETECT(locationReportingChargingInformation, "LocationReporting")
    CHF_DETECT(nEFChargingInformation, "NEF")
    CHF_DETECT(mBSSessionChargingInformation, "MBSSession")
    CHF_DETECT(proSeChargingInformation, "ProSe")
    CHF_DETECT(lCSInformation, "LCS")
    CHF_DETECT(roamingQBCInformation, "RoamingQBC")
    CHF_DETECT(nSPAChargingInformation, "NSPA")
    CHF_DETECT(networkSharingChargingInformation, "NetworkSharing")
    CHF_DETECT(nSMChargingInformation, "NSM")
    CHF_DETECT(nSACFChargingInformation, "NSACF")
    CHF_DETECT(nSSAAChargingInformation, "NSSAA")
    CHF_DETECT(tSNChargingInformation, "TSN")
    CHF_DETECT(rangingSLChargingInformation, "RangingSL")
    CHF_DETECT(edgeInfrastructureUsageChargingInformation, "EdgeInfrastructureUsage")
    CHF_DETECT(eASDeploymentChargingInformation, "EASDeployment")
    CHF_DETECT(directEdgeEnablingServiceChargingInformation, "DirectEdgeEnablingService")
    CHF_DETECT(exposedEdgeEnablingServiceChargingInformation, "ExposedEdgeEnablingService")
    CHF_DETECT(interCHFInformation, "InterCHF")

#undef CHF_DETECT

    return out;
}

void flatten_attributes(const nlohmann::json& value,
                        const std::string& prefix,
                        nlohmann::json& out,
                        int max_depth) {
    if (max_depth <= 0) {
        return;
    }
    if (value.is_object()) {
        for (const auto& [key, child] : value.items()) {
            flatten_attributes(
                child, prefix.empty() ? key : prefix + "." + key, out, max_depth - 1);
        }
        return;
    }
    if (value.is_array()) {
        // Indexed, not collapsed: a tariff priced on the first recipient of a multi-recipient MMS
        // is a real product, and collapsing the array would make it unexpressible.
        for (std::size_t i = 0; i < value.size(); ++i) {
            flatten_attributes(value[i], prefix + "." + std::to_string(i), out, max_depth - 1);
        }
        return;
    }
    if (value.is_null()) {
        // An absent value is not an attribute. Recording it as null would let a scope "match"
        // something the request never carried.
        return;
    }
    if (!prefix.empty()) {
        out[prefix] = value;
    }
}

} // namespace chf
