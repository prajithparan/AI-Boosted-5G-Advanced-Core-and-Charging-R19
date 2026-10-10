## ADR-0424: Operator authentication -- OIDC (authorization code + PKCE) to a self-hosted IdP, Keycloak recommended

**Date:** 2026-09-26. **Status:** accepted; BFF side implemented and tested against a fake IdP;
a live Keycloak realm is deferred. **Update (ADR-0441, 2026-09-27):** the live Keycloak realm
deferral is closed -- compose + Helm entries, a real end-to-end login proven against a real
container. Back-channel logout and per-action step-up remain deferred; see ADR-0441 for what
narrowed and what did not.

**Decision.** The BFF is an OIDC confidential client (`OidcAuthenticator` behind an `Authenticator`
interface): /auth/login stores state + nonce + PKCE verifier (single use, browser-bound) and
redirects; /auth/callback exchanges the code (client_secret_post + code_verifier), verifies the ID
token itself (JWKS fetched and refreshed on unknown kid; RS256 or ES256 only; iss, aud, azp, exp
with leeway, nonce), maps (iss, sub) to `operator_user`, refuses non-ACTIVE and dormant users, and
for `mfa_required` users demands MFA evidence (`amr` or `acr` values from config). Session cookie
`__Host-oam_session` (Secure, HttpOnly, SameSite=Strict; the callback answers 200 + meta refresh
because a Strict cookie set on a redirect chain that began at the IdP would not be sent). All
endpoints, client id, secret FILE and IdP CA are config (`config/oam-gui-bff.json`).
**Recommended IdP: Keycloak** (Apache-2.0, self-hosted, OIDC + TOTP/WebAuthn MFA, LDAP/AD
federation, brokering to an operator's existing IdP). Tests use a fake IdP that signs real ES256
tokens and enforces PKCE: success, no-MFA refusal, nonce mismatch, foreign browser, replayed state,
dormant account, unknown subject -- each audited.

**Rejected.** *Home-grown passwords + TOTP in the BFF* -- credential storage, reset flows and MFA
enrolment are an IdP's job and a liability here. *SAML* -- heavier, XML-DSig; can be brokered by
Keycloak if an operator needs it. *Authelia / Dex* -- Dex has no MFA of its own; Authelia is
forward-auth-oriented; either can sit behind the same interface. *Operator client certificate as
the only login* -- kept as the terminal/device factor (layer 1), not the person.
**Deferred:** Keycloak realm export + compose/Helm entry (a live IdP was not run on the shared
16 GB machine); back-channel logout; step-up (acr) per sensitive action.

