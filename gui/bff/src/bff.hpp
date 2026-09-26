#pragma once

// oam-gui-bff: the backend-for-frontend between the operator's browser and the BSS services
// (ADR-0422). The NFs/BSS services speak HTTP/2 + mutual TLS only and never see a browser: the
// browser authenticates to THIS process with an operator client certificate issued by a separate
// operator CA, and this process calls the services with its own lab-CA identity (CN=oam-gui-bff).
// NF mTLS is not weakened anywhere -- no service gains a new trust anchor or an anonymous port.
//
// It is deliberately NOT a generic reverse proxy: only the routes below are forwarded, bodies are
// passed through as bytes (never parsed, so SIM keys in a provisioning order are never
// materialised into a log string, a JSON error message or a span attribute), and only
// content-type/accept travel upstream.
//
//   browser path                                    upstream (base URL from config)
//   GET|POST /api/tmf620/productOffering            {catalog}/tmf-api/productCatalogManagement/v4/productOffering
//   GET      /api/tmf620/productOffering/{id}       .../productOffering/{id}
//   GET|POST /api/tmf620/productOfferingPrice       .../productOfferingPrice
//   GET      /api/tmf620/productOfferingPrice/{id}  .../productOfferingPrice/{id}
//   POST     /api/provisioning/customerOrder        {provisioning}/provisioning/v1/customerOrder
//   GET      /api/provisioning/customerOrder/{id}   .../customerOrder/{id}
//   GET      /, /index.html, /assets/{file}         the built web app (static_dir), loaded at start
//
// CSRF: browsers attach client certificates automatically, so a state-changing request must carry
// `x-requested-by: oam-gui` and `content-type: application/json`. Both force a CORS preflight on a
// cross-origin request, and this server answers no OPTIONS / sends no CORS headers, so a foreign
// page can never complete one.

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

// Registers every allow-listed route. `client` must outlive `server`; it carries the BFF's own
// lab-CA identity towards the services.
void register_routes(sbi_core::http2::Server& server,
                     sbi_core::http2::Client& client,
                     const Config& config,
                     StaticFiles static_files);

} // namespace oam_gui_bff
