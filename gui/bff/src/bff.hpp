#pragma once

// oam-gui-bff: the backend-for-frontend between the operator's browser and the BSS services /
// NF configuration (ADR-0422, ADR-0423, ADR-0424, ADR-0425).
//
// Trust, outermost first:
//   1. operator mTLS: the browser's TLS client certificate (terminal identity) must chain to the
//      separate OPERATOR CA -- an NF certificate from the lab CA cannot even connect;
//   2. an OIDC login (IdP-side MFA) creates a server-side session bound to that terminal cert;
//   3. every /api call is authorized here from operator_iam (permission + org-unit scope) and
//      audited (allowed AND denied) before anything is forwarded;
//   4. the BFF calls the services with its OWN lab-CA identity (CN=oam-gui-bff). NF mTLS is not
//      weakened: no service gains a trust anchor or an anonymous port.
//
// It is not a generic reverse proxy: only the routes in app.cpp exist. CSRF: browsers attach
// cookies and client certificates automatically, so every state-changing request must carry
// `x-requested-by: oam-gui` and `content-type: application/json`; both force a CORS preflight
// cross-origin, and this server answers no OPTIONS, so a foreign page can never complete one.

#include "auth.hpp"
#include "config_mgmt.hpp"
#include "iam.hpp"

#include "sbi_core/http2_client.hpp"
#include "sbi_core/http2_server.hpp"

#include <map>
#include <string>

namespace oam_gui_bff {

struct Config {
    std::string product_catalog_base_url; // e.g. https://127.0.0.1:7785 (no trailing path)
    std::string provisioning_base_url;    // e.g. https://127.0.0.1:7790
    std::string static_dir;               // directory holding index.html + assets/
    // No upstream timeout: sbi_core::http2::Client sets none (a hung service holds one BFF worker
    // thread). Disclosed in ADR-0422, not papered over here.
};

// The spec's own API roots (TMF620 basePath; the project-owned provisioning root). Paths are not
// deployment configuration -- the same line ADR-0273/ADR-0274 drew for the 3GPP service paths.
inline constexpr const char* kTmf620Root = "/tmf-api/productCatalogManagement/v4";
inline constexpr const char* kProvisioningRoot = "/provisioning/v1";

inline constexpr const char* kSessionCookie = "__Host-oam_session";
inline constexpr const char* kLoginCookie = "__Host-oam_login";

// Static files served from memory; loaded once, so a request path can never reach the filesystem.
struct StaticFile {
    std::string content_type;
    std::string body;
};
using StaticFiles = std::map<std::string, StaticFile>; // key: "index.html", "assets/<file>"

// Loads <dir>/index.html and every regular file directly in <dir>/assets/. Throws if index.html
// is missing (fail fast: a BFF with no app is a misconfiguration, not a degraded mode).
StaticFiles load_static_files(const std::string& dir);

// True for a path segment safe to splice into an upstream URL: RFC 3986 unreserved characters
// only. Anything else (/, %, .., ?, #) is rejected with 400 rather than encoded and forwarded.
bool is_safe_id(const std::string& id);

// Everything the routes need. All references must outlive the server.
struct Deps {
    sbi_core::http2::Client& services; // BFF's lab-CA identity towards the BSS services
    IamStore& iam;
    Authenticator& auth;
    NfConfigManager& configs;
    Config config;
};

void register_routes(sbi_core::http2::Server& server, Deps& deps, StaticFiles static_files);

} // namespace oam_gui_bff
