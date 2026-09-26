#include "bff.hpp"

#include "sbi_core/json_body.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace oam_gui_bff {

namespace {

namespace fs = std::filesystem;
using sbi_core::http2::Client;
using sbi_core::http2::ClientRequest;
using sbi_core::http2::Request;
using sbi_core::http2::Response;

constexpr const char* kCsrfHeader = "x-requested-by";
constexpr const char* kCsrfValue = "oam-gui";

// Headers on every response. no-store: nothing the GUI shows (orders, catalog) is meant to live in
// a browser cache on a shared operator workstation.
void harden(Response& r) {
    r.headers.emplace("cache-control", "no-store");
    r.headers.emplace("x-content-type-options", "nosniff");
    r.headers.emplace("referrer-policy", "no-referrer");
}

std::string header(const Request& req, const std::string& name) {
    const auto it = req.headers.find(name);
    return it == req.headers.end() ? std::string() : it->second;
}

std::string content_type_for(const std::string& file) {
    const auto ext = fs::path(file).extension().string();
    if (ext == ".html") return "text/html; charset=utf-8";
    if (ext == ".js") return "text/javascript; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".json") return "application/json";
    if (ext == ".png") return "image/png";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".woff2") return "font/woff2";
    return "application/octet-stream";
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        throw std::runtime_error("oam-gui-bff: cannot read " + p.string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// One access-log line per request. Never the body, never header values: a provisioning order's
// body carries SIM K/OPc (ADR-0422). The operator's certificate CN is the audit identity.
void access_log(const Request& req, const char* route, int status,
                std::chrono::steady_clock::time_point start) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    spdlog::info("oam-gui-bff: operator='{}' {} {} -> {} ({} ms)", req.peer_cert_cn, req.method,
                 route, status, ms);
}

// Forwards one allow-listed request. `upstream_url` is fully built by the caller from config +
// a spec path + (optionally) a validated id; nothing from the browser is spliced in unchecked.
Response forward(Client& client, const Request& req, const std::string& upstream_url,
                 const std::string& upstream_root, const std::string& browser_root) {
    ClientRequest out;
    out.method = req.method;
    out.url = upstream_url;
    out.body = req.body; // bytes, never parsed
    out.headers.emplace("accept", "application/json");
    if (const auto ct = header(req, "content-type"); !ct.empty()) {
        out.headers.emplace("content-type", ct);
    }
    auto result = client.send(out);
    if (!result.has_value()) {
        // libcurl's error text names the transport failure, not the payload.
        spdlog::warn("oam-gui-bff: upstream {} unreachable: {}", upstream_root, result.error());
        return sbi_core::http2::problem_response(502, "Bad Gateway",
                                                 "the upstream service is unreachable");
    }
    Response resp;
    resp.status = static_cast<int>(result->status);
    resp.body = std::move(result->body);
    for (const auto& [name, value] : result->headers) {
        if (name == "content-type") {
            resp.headers.emplace(name, value);
        } else if (name == "location" && value.rfind(upstream_root, 0) == 0) {
            // TMF620 Location is the service's own path; hand the browser the path it can use.
            resp.headers.emplace(name, browser_root + value.substr(upstream_root.size()));
        }
    }
    return resp;
}

Response reject(int status, const std::string& title, const std::string& detail) {
    return sbi_core::http2::problem_response(status, title, detail);
}

// Browser-side CSRF gate for every state-changing request (see bff.hpp).
std::optional<Response> csrf_check(const Request& req) {
    if (header(req, kCsrfHeader) != kCsrfValue) {
        return reject(403, "Forbidden", "missing x-requested-by header");
    }
    if (header(req, "content-type").rfind("application/json", 0) != 0) {
        return reject(415, "Unsupported Media Type", "content-type must be application/json");
    }
    return std::nullopt;
}

} // namespace

bool is_safe_id(const std::string& id) {
    if (id.empty() || id.size() > 256 || id == "." || id == "..") {
        return false;
    }
    for (const char c : id) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                        c == '-' || c == '.' || c == '_' || c == '~';
        if (!ok) {
            return false;
        }
    }
    return true;
}

StaticFiles load_static_files(const std::string& dir) {
    StaticFiles files;
    const fs::path root(dir);
    const auto index = root / "index.html";
    if (!fs::is_regular_file(index)) {
        throw std::runtime_error("oam-gui-bff: " + index.string() +
                                 " not found -- build the web app first (gui/web: npm run build)");
    }
    files["index.html"] = StaticFile{content_type_for("index.html"), read_file(index)};
    const auto assets = root / "assets";
    if (fs::is_directory(assets)) {
        for (const auto& entry : fs::directory_iterator(assets)) {
            if (entry.is_regular_file()) {
                const auto name = entry.path().filename().string();
                files["assets/" + name] = StaticFile{content_type_for(name), read_file(entry.path())};
            }
        }
    }
    return files;
}

void register_routes(sbi_core::http2::Server& server,
                     Client& client,
                     const Config& config,
                     StaticFiles static_files) {
    // A resource the GUI may list/create/read, bound to one upstream collection.
    struct Collection {
        const char* browser_path;  // e.g. /api/tmf620/productOffering
        std::string upstream_root; // e.g. /tmf-api/productCatalogManagement/v4 (for Location)
        std::string upstream_base; // base URL + root
        std::string collection;    // e.g. /productOffering
        bool allow_list;           // GET on the collection
    };
    const std::string catalog_base = config.product_catalog_base_url + kTmf620Root;
    const std::string prov_base = config.provisioning_base_url + kProvisioningRoot;
    const std::vector<Collection> collections = {
        {"/api/tmf620/productOffering", kTmf620Root, catalog_base, "/productOffering", true},
        {"/api/tmf620/productOfferingPrice", kTmf620Root, catalog_base, "/productOfferingPrice",
         true},
        // The provisioning API has no list operation (bss/provisioning/src/main.cpp).
        {"/api/provisioning/customerOrder", kProvisioningRoot, prov_base, "/customerOrder", false},
    };

    for (const auto& c : collections) {
        const std::string browser_root =
            std::string(c.browser_path).substr(0, std::string(c.browser_path).rfind('/'));
        if (c.allow_list) {
            server.add_route("GET", c.browser_path,
                             [&client, c, browser_root](const Request& req) {
                                 const auto start = std::chrono::steady_clock::now();
                                 auto r = forward(client, req, c.upstream_base + c.collection,
                                                  c.upstream_root, browser_root);
                                 harden(r);
                                 access_log(req, c.browser_path, r.status, start);
                                 return r;
                             });
        }
        server.add_route("POST", c.browser_path,
                         [&client, c, browser_root](const Request& req) {
                             const auto start = std::chrono::steady_clock::now();
                             Response r;
                             if (auto bad = csrf_check(req)) {
                                 r = std::move(*bad);
                             } else {
                                 r = forward(client, req, c.upstream_base + c.collection,
                                             c.upstream_root, browser_root);
                             }
                             harden(r);
                             access_log(req, c.browser_path, r.status, start);
                             return r;
                         });
        server.add_route("GET", std::string(c.browser_path) + "/{id}",
                         [&client, c, browser_root](const Request& req) {
                             const auto start = std::chrono::steady_clock::now();
                             const auto& id = req.path_params.at("id");
                             Response r;
                             if (!is_safe_id(id)) {
                                 r = reject(400, "Bad Request", "invalid resource id");
                             } else {
                                 r = forward(client, req,
                                             c.upstream_base + c.collection + "/" + id,
                                             c.upstream_root, browser_root);
                             }
                             harden(r);
                             access_log(req, c.browser_path, r.status, start);
                             return r;
                         });
    }

    // --- static web app ---
    auto files = std::make_shared<const StaticFiles>(std::move(static_files));
    const auto serve = [files](const std::string& key, const Request& req) {
        const auto start = std::chrono::steady_clock::now();
        Response r;
        const auto it = files->find(key);
        if (it == files->end()) {
            r = reject(404, "Not Found", "no such file");
        } else {
            r.status = 200;
            r.headers.emplace("content-type", it->second.content_type);
            r.body = it->second.body;
            if (key == "index.html") {
                // Everything the app needs comes from this origin; no inline script, no framing.
                r.headers.emplace("content-security-policy",
                                  "default-src 'self'; script-src 'self'; style-src 'self'; "
                                  "img-src 'self' data:; connect-src 'self'; object-src 'none'; "
                                  "base-uri 'none'; form-action 'self'; frame-ancestors 'none'");
                r.headers.emplace("x-frame-options", "DENY");
            }
        }
        harden(r);
        access_log(req, "static", r.status, start);
        return r;
    };
    server.add_route("GET", "/", [serve](const Request& req) { return serve("index.html", req); });
    server.add_route("GET", "/index.html",
                     [serve](const Request& req) { return serve("index.html", req); });
    server.add_route("GET", "/assets/{file}", [serve](const Request& req) {
        // The map only holds files enumerated at start-up, so no path can escape static_dir;
        // is_safe_id is belt and braces against an odd key ever being inserted.
        const auto& f = req.path_params.at("file");
        return serve(is_safe_id(f) ? "assets/" + f : std::string(), req);
    });
}

} // namespace oam_gui_bff
