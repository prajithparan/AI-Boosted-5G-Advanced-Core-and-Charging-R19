#pragma once

// UDR node adapter for the provisioning saga (ADR-0382). Builds one subscriber's 3GPP documents --
// AccessAndMobilitySubscriptionData, SessionManagementSubscriptionData,
// SmfSelectionSubscriptionData (TS29503_Nudm_SDM.yaml), AuthenticationSubscription
// (TS29505_Subscription_Data.yaml), AmPolicyData and SmPolicyData (TS29519_Policy_Data.yaml) -- and
// writes them through the UDR's project-owned OAM provisioning API (PUT
// /oam-provisioning/v1/subscribers/{ueId}), authenticated by this service's mTLS certificate. Nudr
// has no create operation for this data; see the UDR side's nfs/udr/src/oam_provisioning.hpp.
//
// Network content comes from config (network_profiles keyed by product offering id, plus
// serving PLMN, AMF field, initial SQN) -- never from literals here. K/OPc come from the order
// (the SIM vendor's input file in real operations) and are never logged or persisted by this
// service.

#include <nlohmann/json.hpp>

#include <map>
#include <sbi_core/http2_client.hpp>
#include <string>
#include <tl/expected.hpp>
#include <vector>

namespace provisioning {

struct Snssai {
    int sst = 0;
    std::string sd; // empty = no SD
};

// Per-offering network profile. Proper home is the TMF620 ProductSpecification's characteristics;
// config-keyed until the catalog carries them (disclosed, ADR-0382).
struct NetworkProfile {
    std::vector<Snssai> snssais; // first one is the default S-NSSAI
    std::string dnn;
    std::string ue_ambr_uplink;   // BitRate, e.g. "100 Mbps"
    std::string ue_ambr_downlink; // BitRate
};

struct UdrAdapterConfig {
    std::string udr_base_url;
    std::string serving_plmn_id;
    std::string authentication_method;           // AuthMethod, e.g. "5G_AKA"
    std::string authentication_management_field; // 4 hex digits
    std::string initial_sqn;                     // 12 hex digits, used when the order has none
    std::map<std::string, NetworkProfile> network_profiles; // key: offering id, or "default"
};

UdrAdapterConfig parse_udr_adapter_config(const nlohmann::json& config);

// SIM credentials from the order. Held only for the duration of one request.
struct SimCredentials {
    std::string k;   // 32 hex
    std::string opc; // 32 hex
    std::string sqn; // 12 hex or empty (config initial_sqn applies)
};

struct SubscriberSpec {
    std::string supi;
    std::string msisdn; // digits, may be empty
    std::string offering_id;
    SimCredentials sim;
};

// The profile used, returned so the saga can record it (non-secret) in the task response.
struct UdrProvisionOutcome {
    int http_status = 0;
    std::string profile_key;
};

class UdrAdapter {
public:
    UdrAdapter(UdrAdapterConfig config, sbi_core::http2::TlsConfig tls);

    // Builds the PUT body; exposed for tests. Error if no profile applies.
    tl::expected<nlohmann::json, std::string> build_documents(const SubscriberSpec& spec,
                                                              std::string* profile_key) const;

    tl::expected<UdrProvisionOutcome, std::string> provision(const SubscriberSpec& spec);

private:
    UdrAdapterConfig config_;
    sbi_core::http2::Client client_;
};

} // namespace provisioning
