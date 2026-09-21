// Provisioning module (project_customer_onboarding_orchestration). Order-to-activation runner:
// accepts a customer order (from the operator GUI / sales point), decomposes it (TMF622->641->652),
// and provisions the customer across nodes -- BSS SID chain (this increment, real) + UDR network
// subscription/auth/policy (task recorded, executed by the UDR adapter, next increment). Its own
// domain DB (orchestration) + writes the consolidated charging DB. Fail-fast on any dependency.

#include "sbi_core/http2_server.hpp"
#include "sbi_core/logging.hpp"
#include "sbi_core/metrics.hpp"
#include "sbi_core/otel.hpp"
#include "sbi_core/io_context_pool.hpp"

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <nf_config/nf_config.hpp>

#include "provisioning_store.hpp"

using nlohmann::json;

int main() {
    sbi_core::init_logging("provisioning");
    sbi_core::init_tracing("provisioning");

    const auto config = nf_config::load("provisioning", CONFIG_DIR);
    const auto port = nf_config::require<unsigned short>(config, "port", "PROVISIONING_PORT");
    const auto metrics_bind_address =
        nf_config::require<std::string>(config, "metrics_bind_address", "PROVISIONING_METRICS_BIND_ADDRESS");
    const auto orchestration_url =
        nf_config::require<std::string>(config, "orchestration_database_url", "PROVISIONING_ORCHESTRATION_DATABASE_URL");
    const auto charging_url =
        nf_config::require<std::string>(config, "charging_database_url", "PROVISIONING_CHARGING_DATABASE_URL");
    const auto db_pool_size =
        static_cast<std::size_t>(nf_config::require<int>(config, "db_pool_size", "PROVISIONING_DB_POOL_SIZE"));

    sbi_core::init_metrics(metrics_bind_address);

    provisioning::ProvisioningStore store(orchestration_url, charging_url, db_pool_size);
    spdlog::info("provisioning: connected to orchestration + charging PostgreSQL");

    auto meter = sbi_core::get_meter("provisioning");
    auto order_counter = meter->CreateUInt64Counter("provisioning_customer_orders_total",
                                                     "Customer provisioning orders processed");

    sbi_core::http2::TlsConfig server_tls{
        .cert_path = CERTS_DIR "/provisioning/cert.pem",
        .key_path = CERTS_DIR "/provisioning/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };

    boost::asio::io_context ioc;
    sbi_core::http2::Server server(ioc, "0.0.0.0", port, server_tls);

    server.add_route(
        "POST", "/provisioning/v1/customerOrder",
        [&store, &order_counter](const sbi_core::http2::Request& req) {
            json body;
            try {
                body = json::parse(req.body);
            } catch (const std::exception& e) {
                return sbi_core::http2::Response::json(
                    400, json{{"error", std::string("invalid JSON: ") + e.what()}}.dump());
            }
            const auto result = store.create_customer_order(body);
            order_counter->Add(1);
            json out{{"orderId", result.order_id}, {"accountId", result.customer_account_id},
                     {"subscriberId", result.subscriber_id}, {"supi", result.supi},
                     {"msisdn", result.msisdn}, {"status", result.status}};
            if (result.error) out["error"] = *result.error;
            return sbi_core::http2::Response::json(result.status == "completed" ? 201 : 502, out.dump());
        });

    server.add_route(
        "GET", "/provisioning/v1/customerOrder/{id}",
        [&store](const sbi_core::http2::Request& req) {
            const auto id = req.path_params.at("id");
            const auto order = store.get_order(id);
            if (!order) {
                return sbi_core::http2::Response::json(404, json{{"error", "order not found"}}.dump());
            }
            return sbi_core::http2::Response::json(200, order->dump());
        });

    spdlog::info("provisioning: listening on https://0.0.0.0:{} (TLS 1.3 + mTLS)", port);
    server.start();
    sbi_core::run_multi_threaded(ioc);
    return 0;
}
