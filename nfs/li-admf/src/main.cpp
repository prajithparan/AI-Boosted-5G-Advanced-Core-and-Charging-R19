// ADMF -- the Administration Function of TS 33.127 clause 5.3.5 (ADR-0462). This process is the LI_HI1
// receiver (ETSI TS 103 120, XML over HTTPS + mTLS) and, from the build's step 4 on, the X1 client
// that provisions the POIs and the MDF2. Like li-mdf it is deliberately NOT an SBI NF: it does not
// register with the NRF and exposes no 3GPP service API (TS 33.127 keeps the LI architecture off the
// service-based plane).
//
// Warrant state lives in its own PostgreSQL (schema.sql), not in this process, so a second replica
// sees the same warrants (ADR-0359).

#include "sbi_core/http2_server.hpp"
#include "sbi_core/io_context_pool.hpp"
#include "sbi_core/logging.hpp"
#include "sbi_core/metrics.hpp"
#include "sbi_core/otel.hpp"

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <string>

#include "hi1_service.hpp"
#include "hi1_store.hpp"
#include "lifecycle.hpp"
#include "lipf.hpp"
#include "nf_config/nf_config.hpp"
#include "nf_config/pg_pool.hpp"

namespace {

// TS 103 120 Annex H.5 table H.0b: the default relative paths of the LI lifecycle workflow
// endpoints, plus the API base URL itself ("/"), which also serves GETCSPCONFIG.
constexpr const char* kHi1Paths[] = {"/",
                                     "/li/authorisation/new",
                                     "/li/authorisation/extension",
                                     "/li/authorisation/cancellation",
                                     "/li/task/addition",
                                     "/li/task/cancellation",
                                     "/li/task/change-delivery"};

} // namespace

int main() {
    sbi_core::init_logging("li-admf");
    const auto config = nf_config::load("li-admf", CONFIG_DIR);

    const auto hi1_port = nf_config::require<std::uint16_t>(config, "hi1_port", "LI_ADMF_HI1_PORT");
    const auto metrics_bind_address = nf_config::require<std::string>(
        config, "metrics_bind_address", "LI_ADMF_METRICS_BIND_ADDRESS");
    const auto database_url =
        nf_config::require<std::string>(config, "database_url", "LI_ADMF_DATABASE_URL");
    const auto pool_size = nf_config::require<std::size_t>(config, "db_pool_size", "LI_ADMF_DB_POOL_SIZE");
    const auto public_base_url =
        nf_config::require<std::string>(config, "public_base_url", "LI_ADMF_PUBLIC_BASE_URL");

    li_admf::Hi1Config hi1;
    hi1.self = {config.at("endpoint_id").at("country_code").get<std::string>(),
                config.at("endpoint_id").at("unique_identifier").get<std::string>()};
    hi1.national_profile_owner = config.at("national_profile").at("owner").get<std::string>();
    hi1.national_profile_version = config.at("national_profile").at("version").get<std::string>();
    hi1.supported_etsi_versions = config.at("supported_etsi_versions").get<std::vector<std::string>>();
    hi1.public_base_url = public_base_url;
    for (const auto& lea : config.at("lea_bindings")) {
        hi1.leas.push_back({lea.at("peer_cert_cn").get<std::string>(),
                            {lea.at("country_code").get<std::string>(),
                             lea.at("unique_identifier").get<std::string>()}});
    }
    if (hi1.leas.empty()) {
        // Fail closed and say so: with no onboarded LEA every request is refused, which is correct
        // but is also never what an operator who started this process meant.
        spdlog::warn("li-admf: config lists no lea_bindings -- every HI1 request will be refused (403)");
    }

    sbi_core::init_metrics(metrics_bind_address);
    spdlog::info("li-admf: starting, endpoint {}/{}", hi1.self.country_code, hi1.self.unique_identifier);

    nf_config::PgPool pool(database_url, pool_size);
    li_admf::Hi1Store store(pool);
    store.ensure_schema(); // fail fast: an ADMF that cannot record its audit trail must not serve

    // The LIPF: the X1 client that provisions the POIs and the MDF2, over the same mTLS identity.
    li_admf::LipfConfig lipf_config;
    lipf_config.admf_identifier = config.at("x1_admf_identifier").get<std::string>();
    lipf_config.x1_version = config.at("x1_version").get<std::string>();
    lipf_config.cc_capable = config.at("cc_capable").get<bool>();
    if (const auto ia = config.at("poi_identifier_association_events").get<std::string>(); ia == "All") {
        lipf_config.poi_identifier_association = li_core::x1::IdentifierAssociationEventsGenerated::All;
    } else if (ia == "IdentifierAssociation") {
        lipf_config.poi_identifier_association = li_core::x1::IdentifierAssociationEventsGenerated::IdentifierAssociation;
    } else if (ia != "Absent") {
        nf_config::fatal("poi_identifier_association_events must be All, IdentifierAssociation or Absent");
    }
    std::vector<li_admf::NetworkElement> elements;
    for (const auto& ne : config.at("network_elements")) {
        elements.push_back({ne.at("name").get<std::string>(),
                            ne.at("role").get<std::string>(),
                            ne.at("ne_identifier").get<std::string>(),
                            ne.at("x1_url").get<std::string>()});
    }
    li_admf::HttpX1Transport x1_transport(sbi_core::http2::TlsConfig{
        .cert_path = CERTS_DIR "/li-admf/cert.pem",
        .key_path = CERTS_DIR "/li-admf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    });
    li_admf::Lipf lipf(lipf_config, std::move(elements), x1_transport);

    li_admf::LifecycleConfig lifecycle_config;
    lifecycle_config.self = hi1.self;
    lifecycle_config.reconcile_interval = std::chrono::seconds(config.at("reconcile_interval_seconds").get<int>());
    lifecycle_config.retry_interval = std::chrono::seconds(config.at("retry_interval_seconds").get<int>());
    lifecycle_config.maximum_list_records = config.at("maximum_list_records").get<std::uint64_t>();
    lifecycle_config.extra_document_content_types =
        config.at("extra_document_content_types").get<std::vector<std::string>>();
    li_admf::Lifecycle lifecycle(lifecycle_config, store, lipf);

    li_admf::Hi1Service service(std::move(hi1), &lifecycle);

    auto meter = sbi_core::get_meter("li-admf");
    auto requests = meter->CreateUInt64Counter("li_admf_hi1_requests_total", "HI1 requests received");
    auto rejected = meter->CreateUInt64Counter(
        "li_admf_hi1_rejected_total", "HI1 requests refused (unbound peer or top-level error)");

    const sbi_core::http2::TlsConfig tls{
        .cert_path = CERTS_DIR "/li-admf/cert.pem",
        .key_path = CERTS_DIR "/li-admf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    boost::asio::io_context ioc;
    sbi_core::http2::Server server(ioc, "0.0.0.0", hi1_port, tls);
    for (const char* path : kHi1Paths) {
        server.add_route("POST", path, [&, path](const sbi_core::http2::Request& request) {
            requests->Add(1);
            std::string content_type;
            if (const auto it = request.headers.find("content-type"); it != request.headers.end()) {
                content_type = it->second;
            }
            const auto reply = service.handle(request.peer_cert_cn, path, content_type, request.body);
            if (reply.outcome != "ok") {
                rejected->Add(1);
            }
            try {
                store.audit({request.peer_cert_cn,
                             path,
                             reply.transaction_id,
                             reply.sender,
                             reply.actions,
                             reply.outcome,
                             reply.detail});
            } catch (const std::exception& e) {
                // An unauditable action must not be performed (TS 33.127): refuse rather than serve.
                spdlog::error("li-admf: audit write failed, refusing the request: {}", e.what());
                sbi_core::http2::Response unavailable;
                unavailable.status = 503;
                return unavailable;
            }
            sbi_core::http2::Response response;
            response.status = reply.http_status;
            if (!reply.body.empty()) {
                response.headers.emplace("content-type", reply.content_type);
                response.body = reply.body;
            }
            return response;
        });
    }
    lifecycle.start();
    server.start();
    spdlog::info("li-admf: LI_HI1 on https://0.0.0.0:{} (TLS 1.3 + mTLS)", hi1_port);
    sbi_core::run_multi_threaded(ioc);
    lifecycle.stop();
    return 0;
}
