#pragma once

// Operator authentication (ADR-0424). The BFF never sees a password: it delegates to an OIDC
// provider (Keycloak recommended, Apache-2.0, self-hosted) with the authorization-code flow +
// PKCE, validates the ID token itself, and maps the IdP subject to an operator_iam user. MFA is
// the IdP's job; the BFF *demands evidence of it* (amr/acr from config) for users flagged
// mfa_required. Behind an interface so a second IdP protocol (e.g. SAML via a broker) can be added
// without touching the handlers.

#include "iam.hpp"

#include "sbi_core/http2_client.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace oam_gui_bff {

struct LoginOutcome {
    bool ok = false;
    std::string reason;          // for the audit trail when !ok; never shown verbatim to the user
    std::optional<UserRecord> user;
    std::string idp_session_id;  // `sid`
    std::string acr;
    std::vector<std::string> amr;
    std::string subject;         // for auditing a denied login of an unknown subject
};

class Authenticator {
public:
    virtual ~Authenticator() = default;
    // Starts a login: stores state/nonce/PKCE bound to `browser_binding`, returns the IdP URL.
    virtual std::string begin(const std::string& browser_binding,
                              const std::string& terminal_cn) = 0;
    virtual LoginOutcome complete(const std::string& code, const std::string& state,
                                  const std::string& browser_binding) = 0;
    // Where the browser goes after local logout (IdP end-session), or "" if none.
    virtual std::string logout_url() const = 0;
};

struct OidcConfig {
    std::string issuer;
    std::string authorization_endpoint;
    std::string token_endpoint;
    std::string jwks_uri;
    std::string end_session_endpoint; // optional
    std::string client_id;
    std::string client_secret;        // read from a file named in config, never inline
    std::string redirect_uri;
    std::string post_logout_redirect_uri;
    std::string scope = "openid";
    std::string acr_values;           // optional, sent to the IdP to request step-up
    std::vector<std::string> mfa_amr; // any one of these in `amr` satisfies MFA
    std::vector<std::string> mfa_acr; // or `acr` equal to one of these
    int leeway_seconds = 60;
};

class OidcAuthenticator final : public Authenticator {
public:
    // `idp_tls`: the BFF's client identity + the CA that signs the IdP's server certificate.
    OidcAuthenticator(OidcConfig config, IamStore& iam, sbi_core::http2::TlsConfig idp_tls);
    std::string begin(const std::string& browser_binding, const std::string& terminal_cn) override;
    LoginOutcome complete(const std::string& code, const std::string& state,
                          const std::string& browser_binding) override;
    std::string logout_url() const override;

private:
    // PEM public key for `kid`, refreshing the JWKS once on a miss (key rotation).
    std::optional<std::pair<std::string, std::string>> key_for(const std::string& kid,
                                                               const std::string& alg);
    bool fetch_jwks();

    OidcConfig config_;
    IamStore& iam_;
    sbi_core::http2::Client idp_;
    std::mutex jwks_mutex_;
    nlohmann::json jwks_;
};

} // namespace oam_gui_bff
