#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Authorization of an Nnrf_AccessToken_Get request before a token is issued (ADR-0499 phase 2;
// REVIEW-5ae0ef1 follow-up "the NRF issues any token to anyone").
//
// TS 33.501 13.4.1.1.2 step 2 (TS_33.501_j70.txt:3990-3991) and TS 29.510 5.4.2.2.1
// (TS_29.510_j80.txt:1318), errors per TS 29.510 Table 6.3.6.3-1 (400 + AccessTokenErr):
//  - grant_type not client_credentials: unsupported_grant_type; nfInstanceId or scope missing:
//    invalid_request;
//  - the consumer is not registered (a pre-requisite of 13.4.1.1.2), its request nfType differs
//    from its NF profile, or its client certificate does not name its NF type: invalid_client;
//  - a requested service name the consumer may not use on any registered producer of
//    targetNfType (allowedNfTypes etc., TS 29.510 6.1.6.2.2/6.1.6.2.3): invalid_scope.
//
// Certificate binding is local policy fitted to the lab PKI (scripts/gen-lab-pki.sh issues
// CN=<nf>, DNS:<nf>): the certificate must carry the lowercase NF type as its CN or a dNSName.
// TS 33.501 binds the NF instance ID instead; the lab certificates carry none (ADR-0499).
namespace nrf {

struct TokenRequester {
    std::string cert_cn;
    std::vector<std::string> cert_dns_names;
};

struct TokenDecision {
    // Empty: authorized. Otherwise an AccessTokenErr "error" value.
    std::string error;
    std::string description;
};

// The AccessTokenReq form checks alone (grant_type, nfInstanceId, scope).
TokenDecision check_token_request_form(const std::map<std::string, std::string>& fields);

// The form checks, then the requester and scope checks. get_profile: the registered NF profile of
// an NfInstanceId. producers_of: the registered profiles of an NF type. The NRF's own services
// (targetNfType NRF) are open to every authenticated, registered consumer.
TokenDecision authorize_token_request(
    const std::map<std::string, std::string>& fields,
    const TokenRequester& requester,
    const std::function<std::optional<nlohmann::json>(const std::string&)>& get_profile,
    const std::function<std::vector<nlohmann::json>(const std::string&)>& producers_of);

// The AccessTokenErr response: 400, application/json, Cache-Control: no-store and
// Pragma: no-cache (RFC 6749 5.2, the headers TS29510_Nnrf_AccessToken.yaml lists).
nlohmann::json access_token_err(const TokenDecision& d);

} // namespace nrf
