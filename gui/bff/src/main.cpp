// oam-gui-bff: backend-for-frontend for the Phase 7 operator GUI (ADR-0420, ADR-0422). See bff.hpp
// for the route allow-list and the security model. Not a 3GPP NF: no NRF registration, no OAuth2
// -- the operator is authenticated by client certificate (separate operator CA), and the BFF
// authenticates to the BSS services with its own lab-CA identity exactly like any other client.

#include "bff.hpp"

#include "sbi_core/http2_client.hpp"
#include "sbi_core/http2_server.hpp"
#include "sbi_core/io_context_pool.hpp"
#include "sbi_core/logging.hpp"
#include "sbi_core/metrics.hpp"

#include <boost/asio/io_context.hpp>
#include <nf_config/nf_config.hpp>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <string>

#ifndef REPO_ROOT
#error "REPO_ROOT must be defined by CMake (see gui/bff/CMakeLists.txt)"
#endif

namespace {

// Relative paths in config/oam-gui-bff.json are relative to the repository root, so the same
// file works from any working directory; absolute paths (containers, Helm) are used as-is.
std::string resolve(const std::string& p) {
    const std::filesystem::path path(p);
    return path.is_absolute() ? p : (std::filesystem::path(REPO_ROOT) / path).string();
}

std::string path_key(const nlohmann::json& config, const char* key, const char* env) {
    return resolve(nf_config::require<std::string>(config, key, env));
}

} // namespace

int main() {
    sbi_core::init_logging("oam-gui-bff");

    const auto config = nf_config::load("oam-gui-bff", REPO_ROOT "/config");
    const auto port = nf_config::require<unsigned short>(config, "port");
    const auto bind_address =
        nf_config::require<std::string>(config, "bind_address", "OAM_GUI_BFF_BIND_ADDRESS");
    const auto metrics_bind_address = nf_config::require<std::string>(
        config, "metrics_bind_address", "OAM_GUI_BFF_METRICS_BIND_ADDRESS");

    oam_gui_bff::Config bff;
    bff.product_catalog_base_url = nf_config::require<std::string>(
        config, "product_catalog_base_url", "OAM_GUI_BFF_PRODUCT_CATALOG_BASE_URL");
    bff.provisioning_base_url = nf_config::require<std::string>(
        config, "provisioning_base_url", "OAM_GUI_BFF_PROVISIONING_BASE_URL");
    bff.static_dir = path_key(config, "static_dir", "OAM_GUI_BFF_STATIC_DIR");

    // Browser-facing listener: this server's certificate + the OPERATOR CA (who may use the GUI).
    const sbi_core::http2::TlsConfig browser_tls{
        .cert_path = path_key(config, "server_cert_path", "OAM_GUI_BFF_SERVER_CERT_PATH"),
        .key_path = path_key(config, "server_key_path", "OAM_GUI_BFF_SERVER_KEY_PATH"),
        .ca_path = path_key(config, "operator_ca_path", "OAM_GUI_BFF_OPERATOR_CA_PATH"),
    };
    // Service-facing client: the BFF's own lab-CA identity + the lab CA (whom it calls).
    const sbi_core::http2::TlsConfig service_tls{
        .cert_path = path_key(config, "client_cert_path", "OAM_GUI_BFF_CLIENT_CERT_PATH"),
        .key_path = path_key(config, "client_key_path", "OAM_GUI_BFF_CLIENT_KEY_PATH"),
        .ca_path = path_key(config, "service_ca_path", "OAM_GUI_BFF_SERVICE_CA_PATH"),
    };

    sbi_core::init_metrics(metrics_bind_address);

    auto static_files = oam_gui_bff::load_static_files(bff.static_dir);
    spdlog::info("oam-gui-bff: serving {} static file(s) from {}", static_files.size(),
                 bff.static_dir);

    sbi_core::http2::Client client(service_tls);
    boost::asio::io_context ioc;
    sbi_core::http2::Server server(ioc, bind_address, port, browser_tls);
    oam_gui_bff::register_routes(server, client, bff, std::move(static_files));

    spdlog::info("oam-gui-bff: listening on https://{}:{} (TLS 1.3 + operator mTLS); "
                 "catalog={} provisioning={}",
                 bind_address, port, bff.product_catalog_base_url, bff.provisioning_base_url);
    server.start();
    sbi_core::run_multi_threaded(ioc);
    return 0;
}
