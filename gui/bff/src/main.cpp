// oam-gui-bff: backend-for-frontend for the Phase 7 operator GUI (ADR-0420..0425). See bff.hpp
// for the trust layering and app.cpp for the route table. Not a 3GPP NF: no NRF registration.

#include "auth.hpp"
#include "bff.hpp"
#include "config_mgmt.hpp"
#include "iam.hpp"

#include "sbi_core/http2_client.hpp"
#include "sbi_core/http2_server.hpp"
#include "sbi_core/io_context_pool.hpp"
#include "sbi_core/logging.hpp"
#include "sbi_core/metrics.hpp"

#include <boost/asio/io_context.hpp>
#include <nf_config/nf_config.hpp>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
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

std::string read_secret_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) nf_config::fatal("oam-gui-bff: cannot read the OIDC client secret file " + path);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    if (s.empty()) nf_config::fatal("oam-gui-bff: the OIDC client secret file is empty");
    return s;
}

std::vector<std::string> strings(const nlohmann::json& j) {
    std::vector<std::string> out;
    for (const auto& v : j) out.push_back(v.get<std::string>());
    return out;
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
    bff.static_dir = resolve(
        nf_config::require<std::string>(config, "static_dir", "OAM_GUI_BFF_STATIC_DIR"));

    // Browser-facing listener: this server's certificate + the OPERATOR CA (which terminals may
    // connect at all).
    const sbi_core::http2::TlsConfig browser_tls{
        .cert_path = resolve(nf_config::require<std::string>(config, "server_cert_path",
                                                             "OAM_GUI_BFF_SERVER_CERT_PATH")),
        .key_path = resolve(nf_config::require<std::string>(config, "server_key_path",
                                                            "OAM_GUI_BFF_SERVER_KEY_PATH")),
        .ca_path = resolve(nf_config::require<std::string>(config, "operator_ca_path",
                                                           "OAM_GUI_BFF_OPERATOR_CA_PATH")),
    };
    // Service-facing client: the BFF's own lab-CA identity + the lab CA (whom it calls).
    const auto client_cert = resolve(
        nf_config::require<std::string>(config, "client_cert_path", "OAM_GUI_BFF_CLIENT_CERT_PATH"));
    const auto client_key = resolve(
        nf_config::require<std::string>(config, "client_key_path", "OAM_GUI_BFF_CLIENT_KEY_PATH"));
    const sbi_core::http2::TlsConfig service_tls{
        .cert_path = client_cert,
        .key_path = client_key,
        .ca_path = resolve(nf_config::require<std::string>(config, "service_ca_path",
                                                           "OAM_GUI_BFF_SERVICE_CA_PATH")),
    };

    // operator_iam, as the least-privileged `oam_gui_bff` role (INSERT-only on the audit trail).
    const auto iam_url = nf_config::require<std::string>(config, "iam_database_url",
                                                         "OAM_GUI_BFF_IAM_DATABASE_URL");
    const auto iam_pool = nf_config::require<int>(config, "iam_db_pool_size",
                                                  "OAM_GUI_BFF_IAM_DB_POOL_SIZE");
    const auto chain_key = nf_config::require<std::string>(config, "audit_chain_key",
                                                           "OAM_GUI_BFF_AUDIT_CHAIN_KEY");

    // OIDC (ADR-0424).
    const auto oidc_cfg = nf_config::require<nlohmann::json>(config, "oidc");
    oam_gui_bff::OidcConfig oidc;
    oidc.issuer = oidc_cfg.at("issuer").get<std::string>();
    oidc.authorization_endpoint = oidc_cfg.at("authorization_endpoint").get<std::string>();
    oidc.token_endpoint = oidc_cfg.at("token_endpoint").get<std::string>();
    oidc.jwks_uri = oidc_cfg.at("jwks_uri").get<std::string>();
    oidc.end_session_endpoint = oidc_cfg.value("end_session_endpoint", "");
    oidc.client_id = oidc_cfg.at("client_id").get<std::string>();
    oidc.redirect_uri = oidc_cfg.at("redirect_uri").get<std::string>();
    oidc.post_logout_redirect_uri = oidc_cfg.value("post_logout_redirect_uri", "");
    oidc.acr_values = oidc_cfg.value("acr_values", "");
    oidc.mfa_amr = strings(oidc_cfg.at("mfa_amr"));
    oidc.mfa_acr = strings(oidc_cfg.at("mfa_acr"));
    oidc.client_secret =
        read_secret_file(resolve(oidc_cfg.at("client_secret_file").get<std::string>()));
    const sbi_core::http2::TlsConfig idp_tls{client_cert, client_key,
                                             resolve(oidc_cfg.at("idp_ca_path").get<std::string>())};

    // NF configuration management (ADR-0425).
    const auto nfc = nf_config::require<nlohmann::json>(config, "nf_config");
    std::set<std::string> editable;
    for (const auto& s : strings(nfc.at("editable"))) editable.insert(s);

    sbi_core::init_metrics(metrics_bind_address);

    oam_gui_bff::IamStore iam(iam_url, static_cast<std::size_t>(iam_pool), chain_key);
    iam.ensure_audit_partitions();
    oam_gui_bff::OidcAuthenticator auth(oidc, iam, idp_tls);
    oam_gui_bff::NfConfigManager configs(resolve(nfc.at("config_dir").get<std::string>()),
                                         resolve(nfc.at("schema_dir").get<std::string>()),
                                         editable);

    auto static_files = oam_gui_bff::load_static_files(bff.static_dir);
    spdlog::info("oam-gui-bff: serving {} static file(s) from {}", static_files.size(),
                 bff.static_dir);

    sbi_core::http2::Client services(service_tls);
    oam_gui_bff::Deps deps{services, iam, auth, configs, bff};
    boost::asio::io_context ioc;
    sbi_core::http2::Server server(ioc, bind_address, port, browser_tls);
    oam_gui_bff::register_routes(server, deps, std::move(static_files));

    spdlog::info("oam-gui-bff: listening on https://{}:{} (TLS 1.3 + operator mTLS + OIDC "
                 "sessions); catalog={} provisioning={} idp={}",
                 bind_address, port, bff.product_catalog_base_url, bff.provisioning_base_url,
                 oidc.issuer);
    server.start();
    sbi_core::run_multi_threaded(ioc);
    return 0;
}
