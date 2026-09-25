#pragma once

// ADR-0382: the UDR's project-owned OAM subscriber-provisioning API.
//
//   PUT /oam-provisioning/v1/subscribers/{ueId}
//
// NOT a 3GPP API, and deliberately not under any nudr-* root so it cannot be mistaken for one. The
// R19 Nudr YAML (TS29505_Subscription_Data.yaml, TS29519_Policy_Data.yaml) defines no create
// operation for provisioned-data and only GET/PATCH for authentication-subscription and
// policy-data am-data/sm-data -- 3GPP leaves subscriber creation to OSS/BSS, which commercial UDRs
// answer with a vendor OAM provisioning interface. This is that interface, for this project's own
// bss/provisioning workflow. The documents it accepts ARE the 3GPP schemas (field names from the
// YAML); only the envelope and path are ours.
//
// Access control: not an SBI consumer, so no OAuth2 bearer. The route admits only mTLS peers whose
// verified certificate CN or dNSName SAN is on the configured allow-list
// (config/udr.json oam_provisioning_allowed_clients) -- every NF cert chains to the same CA, so
// mTLS alone would let any NF write K/OPc.

#include <nlohmann/json.hpp>

#include <sbi_core/http2_server.hpp>
#include <string>
#include <tl/expected.hpp>
#include <vector>

#include "stores.hpp"

namespace udr::oam {

inline constexpr const char* kOamProvisioningRoot = "/oam-provisioning/v1";

// True if the request's verified mTLS peer (CN or any dNSName SAN) is on the allow-list.
bool peer_allowed(const sbi_core::http2::Request& req, const std::vector<std::string>& allowed);

// Validates the PUT body against the YAML constraints this API relies on and builds the documents.
// Error strings name the offending field but never echo its value (the body carries K/OPc).
tl::expected<SubscriberDocuments, std::string>
parse_subscriber_documents(const std::string& ue_id, const nlohmann::json& body);

void register_routes(sbi_core::http2::Server& server,
                     SubscriberProvisioningStore& store,
                     std::vector<std::string> allowed_clients);

} // namespace udr::oam
