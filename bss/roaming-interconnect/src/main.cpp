// bss/roaming-interconnect: a real, standalone TM Forum TMF651 Agreement Management service --
// InterconnectAgreement (E7) only, CHARGING_PROMPT.md's P4.7 ("BSS layer + master/consumer/
// enterprise model") scope.
//
// Real basePath, confirmed directly against TM Forum's own public swagger (fetched live, same
// sourcing discipline as bss/product-catalog's own TMF620 citation --
// github.com/tmforum-apis/TMF651_AgreementManagement, TMF651-Agreement-v4.0.0.swagger.json):
// `/tmf-api/agreementManagement/v4/` with a real `/agreement` collection.
//
// Real, disclosed scope split (a deliberate refinement of this project's own earlier phase
// assignment -- see schema.sql's own updated header comment for the full disclosure): this
// project's earlier E7 schema work (docs/DECISIONS.md ADR-0060) originally assigned E7's ENTIRE
// HTTP service, InterconnectAgreement AND RoamingCdrFile together, to P4.11 (Roaming and
// interconnect SETTLEMENT), since P4.11 is genuinely blocked on real GSMA TAP3/RAP/NRTRDE spec
// text CLAUDE.md forbids fabricating. On review, `InterconnectAgreement` itself -- WHO the partner
// operator is and what a real TMF651 Agreement between the two operators looks like -- is not
// GSMA-blocked at all; it's ordinary master data, squarely part of P4.7's own "master model" layer
// (docs/DATA_MODEL.md's E10 framing already treats Agreement as part of the enterprise/SLA
// hierarchy). `RoamingCdrFile` (real GSMA-formatted CDR ingestion, the actually-blocked piece)
// stays unexposed here -- P4.11's own real scope, still deferred pending the user supplying real
// GSMA spec text, not worked around.
//
// Scope, approved before implementation: real Agreement Create/Get/List (matching
// product-catalog's own disclosed CRUD bar; PATCH/DELETE deferred, same as product-catalog).
//
// Disclosed simplifications (same class as product-catalog's own):
// - Not a 3GPP NF: mTLS-only security boundary, no NRF registration, no OAuth2.
// - One libpqxx connection per store, serialized behind a mutex -- not a connection pool.
// - PATCH/DELETE not implemented this turn.
// - RoamingCdrFileStore exists (store.hpp) but has no HTTP route here -- real, disclosed, P4.11's
//   own scope, not silently narrowed away.

#include "sbi_core/http2_server.hpp"
#include "sbi_core/json_body.hpp"
#include "sbi_core/logging.hpp"
#include "sbi_core/metrics.hpp"
#include "sbi_core/otel.hpp"
#include "sbi_core/rate_limit.hpp"

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cstdlib>

#include "bss_sid/agreement.hpp"
#include "store.hpp"

// docs/DECISIONS.md ADR-0077 -- no hardcoded DB URL/deployment literal in source.
#include "nf_config/nf_config.hpp"

#ifndef CERTS_DIR
#error "CERTS_DIR must be defined by CMake (see bss/roaming-interconnect/CMakeLists.txt)"
#endif
#ifndef CONFIG_DIR
#error "CONFIG_DIR must be defined by CMake (see bss/roaming-interconnect/CMakeLists.txt)"
#endif

namespace {

using nlohmann::json;

constexpr const char* kApiRoot = "/tmf-api/agreementManagement/v4";

} // namespace

// ADR-0387: relational-model rejections are client errors, not 500s.
template <typename Handler> auto guarded(Handler handler) {
    return [handler = std::move(handler)](
               const sbi_core::http2::Request& req) -> sbi_core::http2::Response {
        try {
            return handler(req);
        } catch (const roaming_interconnect::InvalidRequest& e) {
            return sbi_core::http2::problem_response(400, "Bad Request", e.what());
        }
    };
}

int main() {
    const auto config = nf_config::load("roaming-interconnect", CONFIG_DIR);
    const auto port = nf_config::require<unsigned short>(config, "port");
    const auto metrics_bind_address =
        nf_config::require<std::string>(config, "metrics_bind_address");
    const auto self_base = nf_config::require<std::string>(
        config, "self_base_url", "ROAMING_INTERCONNECT_SELF_BASE_URL");
    const auto conninfo = nf_config::require<std::string>(
        config, "database_url", "ROAMING_INTERCONNECT_DATABASE_URL");

    sbi_core::init_logging("roaming-interconnect");
    sbi_core::init_tracing("roaming-interconnect");
    sbi_core::init_metrics(metrics_bind_address);

    spdlog::info("roaming-interconnect: starting (TM Forum TMF651 Agreement Management, E7 "
                 "InterconnectAgreement)");

    sbi_core::http2::TlsConfig server_tls{
        .cert_path = CERTS_DIR "/roaming-interconnect/cert.pem",
        .key_path = CERTS_DIR "/roaming-interconnect/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };

    roaming_interconnect::InterconnectAgreementStore agreement_store(
        self_base + kApiRoot + "/agreement", conninfo);
    spdlog::info("roaming-interconnect: connected to PostgreSQL");

    auto meter = sbi_core::get_meter("roaming-interconnect");
    auto agreement_create_counter =
        meter->CreateUInt64Counter("roaming_interconnect_agreement_create_total",
                                   "Total TMF651 InterconnectAgreement creates");

    boost::asio::io_context ioc;
    sbi_core::http2::Server server(ioc, "0.0.0.0", port, server_tls);

    // P15 / P4.12 (ADR-0280): optional TPS ceiling from this NF's own config (`max_tps`,
    // `tps_burst`), overridable per deployment via SBI_MAX_TPS. Absent means unlimited, so this
    // changes nothing until an operator opts in.
    if (const auto tps_limit = sbi_core::read_tps_limit(config); tps_limit.enabled()) {
        server.set_tps_limit(tps_limit.sustained_tps, tps_limit.burst);
        spdlog::info("TPS ceiling active: {} req/s sustained, burst {}",
                     tps_limit.sustained_tps,
                     tps_limit.burst > 0.0 ? tps_limit.burst : tps_limit.sustained_tps);
    }

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/agreement",
        guarded([&agreement_store, &agreement_create_counter](const sbi_core::http2::Request& req) {
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<roaming_interconnect::InterconnectAgreement>(req,
                                                                                              err);
            if (!body.has_value()) {
                return err;
            }
            const auto id = agreement_store.create(*body);
            agreement_create_counter->Add(1);
            const auto stored = agreement_store.get(id);

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location", std::string(kApiRoot) + "/agreement/" + id);
            resp.body = json(*stored).dump();
            return resp;
        }));

    server.add_route("GET",
                     std::string(kApiRoot) + "/agreement",
                     guarded([&agreement_store](const sbi_core::http2::Request&) {
                         const auto agreements = agreement_store.list();
                         return sbi_core::http2::Response::json(200, json(agreements).dump());
                     }));

    server.add_route("GET",
                     std::string(kApiRoot) + "/agreement/{id}",
                     guarded([&agreement_store](const sbi_core::http2::Request& req) {
                         const auto id = req.path_params.at("id");
                         const auto agreement = agreement_store.get(id);
                         if (!agreement.has_value()) {
                             return sbi_core::http2::problem_response(
                                 404, "Not Found", "No InterconnectAgreement " + id);
                         }
                         return sbi_core::http2::Response::json(200, json(*agreement).dump());
                     }));

    server.start();
    spdlog::info("roaming-interconnect: listening on https://0.0.0.0:{} (TLS 1.3 + mTLS)", port);
    spdlog::info("roaming-interconnect: Prometheus metrics at http://{}/metrics",
                 metrics_bind_address);
    ioc.run();
    return 0;
}
