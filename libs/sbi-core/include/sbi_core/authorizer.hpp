#pragma once

#include "sbi_core/http2_server.hpp"
#include "sbi_core/jwt.hpp"

#include <memory>
#include <optional>
#include <string>

// Producer-side OAuth2 authorization of SBI requests, run by http2::Server before a route's
// handler (ADR-0499 phase 1; REVIEW-5ae0ef1 SBI-01/02). Rules, TS 29.500 6.7.3
// (TS_29.500_j70.txt:3066-3085) and TS 33.501 13.4.1.2:
//  - no Authorization header: accepted only when `required` is false ("based on local
//    configuration"); otherwise 401 with `WWW-Authenticate: Bearer realm="<API URI>"` and no
//    error attribute;
//  - a token that is not a Bearer token, fails the signature/expiry/issuer check, or whose `aud`
//    is neither this NF's NFType nor its NfInstanceId: 401, `error="invalid_token"`;
//  - a token missing a required AccessTokenClaims claim: 401, `error="invalid_token"`, with
//    ProblemDetails cause ACCESS_TOKEN_CLAIM_MISSING and the claims in invalidParams;
//  - a token whose `scope` lacks the API's service-level scope: 403,
//    `error="insufficient_scope"`, `scope="<required>"`.
// The required scope comes from the API's YAML (api_catalog.hpp); an apiName not in it (a
// project-owned API) requires the apiName itself, the service-name rule of TS 29.500 6.7.3.
//
// Not checked (disclosed in ADR-0499): `sub` against the client certificate (the lab PKI's
// certificates carry no NF instance ID), resource/operation-level scopes, and the optional
// producer-attribute claims (PLMN, S-NSSAI, NSI, NF set).
namespace sbi_core::http2 {

struct AuthorizerConfig {
    // Reject requests that carry no access token. Default per the user's decision (ADR-0499).
    bool required = true;
    // This NF's own identity, the accepted `aud` values (AccessTokenClaims.aud: an NFType, or an
    // array of NfInstanceIds).
    std::string nf_type;
    std::string nf_instance_id;
};

class Authorizer {
public:
    Authorizer(AuthorizerConfig config, std::shared_ptr<jwt::Verifier> verifier);

    // std::nullopt: authorized, run the handler. Otherwise the 401/403 to answer instead.
    [[nodiscard]] std::optional<Response> check(const Request& req) const;

    // The service-level scope a request to `path` needs ("" for a path with no apiName segment).
    [[nodiscard]] static std::string required_scope(const std::string& path);

private:
    AuthorizerConfig config_;
    std::shared_ptr<jwt::Verifier> verifier_;
};

} // namespace sbi_core::http2
