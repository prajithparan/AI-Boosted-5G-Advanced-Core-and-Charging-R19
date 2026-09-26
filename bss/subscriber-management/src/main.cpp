// bss/subscriber-management: a real, standalone TM Forum TMF632 Party Management service, plus
// this project's own internal Account/Subscriber linking resources -- CHARGING_PROMPT.md's P4.7
// ("BSS layer + master/consumer/enterprise model, E1/E2/E9/E10").
//
// Real basePath, confirmed directly against TM Forum's own public swagger (fetched live, same
// sourcing discipline as bss/product-catalog's own TMF620 citation --
// github.com/tmforum-apis/TMF632_PartyManagement, TMF632-Party-v4.0.0.swagger.json):
// `/tmf-api/party/v4/` with real `/individual` and `/organization` collections.
//
// Why this exists: bss/subscriber-management's own store library (src/store.hpp/.cpp,
// docs/DECISIONS.md ADR-0060) built the real PostgreSQL persistence for E1 (Subscriber) and E10
// (Account/master-consumer-enterprise) but deliberately deferred the HTTP/REST service to P4.7 --
// this turn is that deferred service, following bss/product-catalog/src/main.cpp's own established
// pattern (sbi_core HTTP/2 server, mTLS-only security boundary, real PostgreSQL via the existing
// store classes).
//
// Scope, approved before implementation: real Individual/Organization/Account/Subscriber
// Create/Get/List (matching product-catalog's own disclosed CRUD bar; PATCH/DELETE deferred, same
// as product-catalog left PATCH deferred).
//
// Account/Subscriber basePath: these are project-internal resources (docs/DATA_MODEL.md's own
// explicit "not itself a spec-mandated shape" disclosure for both) -- no real TM Forum API owns
// them, so they get a clearly project-internal basePath (`/bss-api/subscriberManagement/v1/`) that
// cannot be confused with a real TMF collection, rather than inventing a fake TMF-looking path for
// something that isn't one.
//
// Disclosed simplifications (same class as product-catalog's own):
// - Not a 3GPP NF: mTLS-only security boundary, no NRF registration, no OAuth2 -- same reasoning
//   as every other bss/* component (no NRF-issued token source for a non-3GPP ODA component).
// - One libpqxx connection per store, serialized behind a mutex -- not a connection pool (same
//   disclosed limitation as every other bss/* store, ADR-0049's standing "nothing benchmarked
//   yet").
// - PATCH/DELETE not implemented this turn, same real, disclosed narrowing product-catalog used.
// - Subscriber's real trigger (something in nfs/udm or nfs/udr actually calling this service to
//   look up/create a Subscriber record from a real SUPI) does not exist yet -- this is the data
//   model + API, not yet wired into the control-plane NFs. A real, disclosed gap, not silently
//   assumed done.

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

#include "bss_sid/party.hpp"
#include "store.hpp"

// docs/DECISIONS.md ADR-0077 -- no hardcoded DB URL/deployment literal in source.
#include "nf_config/nf_config.hpp"

#ifndef CERTS_DIR
#error "CERTS_DIR must be defined by CMake (see bss/subscriber-management/CMakeLists.txt)"
#endif
#ifndef CONFIG_DIR
#error "CONFIG_DIR must be defined by CMake (see bss/subscriber-management/CMakeLists.txt)"
#endif

namespace {

using nlohmann::json;

constexpr const char* kPartyApiRoot = "/tmf-api/party/v4";
constexpr const char* kProjectApiRoot = "/bss-api/subscriberManagement/v1";

} // namespace

// ADR-0386: relational-model rejections are client errors, not 500s.
template <typename Handler> auto guarded(Handler handler) {
    return [handler = std::move(handler)](
               const sbi_core::http2::Request& req) -> sbi_core::http2::Response {
        try {
            return handler(req);
        } catch (const subscriber_management::InvalidRequest& e) {
            return sbi_core::http2::problem_response(400, "Bad Request", e.what());
        } catch (const subscriber_management::Conflict& e) {
            return sbi_core::http2::problem_response(409, "Conflict", e.what());
        }
    };
}

int main() {
    const auto config = nf_config::load("subscriber-management", CONFIG_DIR);
    const auto port = nf_config::require<unsigned short>(config, "port");
    const auto metrics_bind_address =
        nf_config::require<std::string>(config, "metrics_bind_address");
    const auto self_base = nf_config::require<std::string>(
        config, "self_base_url", "SUBSCRIBER_MANAGEMENT_SELF_BASE_URL");
    const auto conninfo = nf_config::require<std::string>(
        config, "database_url", "SUBSCRIBER_MANAGEMENT_DATABASE_URL");

    sbi_core::init_logging("subscriber-management");
    sbi_core::init_tracing("subscriber-management");
    sbi_core::init_metrics(metrics_bind_address);

    spdlog::info("subscriber-management: starting (TM Forum TMF632 Party Management + E1/E10)");

    sbi_core::http2::TlsConfig server_tls{
        .cert_path = CERTS_DIR "/subscriber-management/cert.pem",
        .key_path = CERTS_DIR "/subscriber-management/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };

    subscriber_management::PartyIndividualStore individual_store(
        self_base + kPartyApiRoot + "/individual", conninfo);
    subscriber_management::PartyOrganizationStore organization_store(
        self_base + kPartyApiRoot + "/organization", conninfo);
    subscriber_management::AccountStore account_store(self_base + kProjectApiRoot + "/account",
                                                      conninfo);
    subscriber_management::SubscriberStore subscriber_store(
        self_base + kProjectApiRoot + "/subscriber", conninfo);
    spdlog::info("subscriber-management: connected to PostgreSQL");

    auto meter = sbi_core::get_meter("subscriber-management");
    auto individual_create_counter = meter->CreateUInt64Counter(
        "subscriber_management_individual_create_total", "Total TMF632 Individual creates");
    auto organization_create_counter = meter->CreateUInt64Counter(
        "subscriber_management_organization_create_total", "Total TMF632 Organization creates");
    auto account_create_counter = meter->CreateUInt64Counter(
        "subscriber_management_account_create_total", "Total E10 Account creates");
    auto subscriber_create_counter = meter->CreateUInt64Counter(
        "subscriber_management_subscriber_create_total", "Total E1 Subscriber creates");

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

    // --- TMF632 Individual (E1) ---

    server.add_route(
        "POST",
        std::string(kPartyApiRoot) + "/individual",
        guarded(
            [&individual_store, &individual_create_counter](const sbi_core::http2::Request& req) {
                sbi_core::http2::Response err;
                auto body = sbi_core::http2::parse_json_body<bss_sid::Individual>(req, err);
                if (!body.has_value()) {
                    return err;
                }
                const auto id = individual_store.create(*body);
                individual_create_counter->Add(1);
                const auto stored = individual_store.get(id);

                sbi_core::http2::Response resp;
                resp.status = 201;
                resp.headers.emplace("content-type", "application/json");
                resp.headers.emplace("location", std::string(kPartyApiRoot) + "/individual/" + id);
                resp.body = json(*stored).dump();
                return resp;
            }));

    server.add_route("GET",
                     std::string(kPartyApiRoot) + "/individual",
                     guarded([&individual_store](const sbi_core::http2::Request&) {
                         const auto individuals = individual_store.list();
                         return sbi_core::http2::Response::json(200, json(individuals).dump());
                     }));

    server.add_route("GET",
                     std::string(kPartyApiRoot) + "/individual/{id}",
                     guarded([&individual_store](const sbi_core::http2::Request& req) {
                         const auto id = req.path_params.at("id");
                         const auto individual = individual_store.get(id);
                         if (!individual.has_value()) {
                             return sbi_core::http2::problem_response(
                                 404, "Not Found", "No Individual " + id);
                         }
                         return sbi_core::http2::Response::json(200, json(*individual).dump());
                     }));

    // --- TMF632 Organization (E10 enterprise hierarchy) ---

    server.add_route(
        "POST",
        std::string(kPartyApiRoot) + "/organization",
        guarded([&organization_store,
                 &organization_create_counter](const sbi_core::http2::Request& req) {
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<bss_sid::Organization>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto id = organization_store.create(*body);
            organization_create_counter->Add(1);
            const auto stored = organization_store.get(id);

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location", std::string(kPartyApiRoot) + "/organization/" + id);
            resp.body = json(*stored).dump();
            return resp;
        }));

    server.add_route("GET",
                     std::string(kPartyApiRoot) + "/organization",
                     guarded([&organization_store](const sbi_core::http2::Request&) {
                         const auto organizations = organization_store.list();
                         return sbi_core::http2::Response::json(200, json(organizations).dump());
                     }));

    server.add_route("GET",
                     std::string(kPartyApiRoot) + "/organization/{id}",
                     guarded([&organization_store](const sbi_core::http2::Request& req) {
                         const auto id = req.path_params.at("id");
                         const auto organization = organization_store.get(id);
                         if (!organization.has_value()) {
                             return sbi_core::http2::problem_response(
                                 404, "Not Found", "No Organization " + id);
                         }
                         return sbi_core::http2::Response::json(200, json(*organization).dump());
                     }));

    // --- Account (E10, project-internal) ---

    server.add_route(
        "POST",
        std::string(kProjectApiRoot) + "/account",
        guarded([&account_store, &account_create_counter](const sbi_core::http2::Request& req) {
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<subscriber_management::Account>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto id = account_store.create(*body);
            account_create_counter->Add(1);
            const auto stored = account_store.get(id);

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location", std::string(kProjectApiRoot) + "/account/" + id);
            resp.body = json(*stored).dump();
            return resp;
        }));

    server.add_route("GET",
                     std::string(kProjectApiRoot) + "/account",
                     guarded([&account_store](const sbi_core::http2::Request&) {
                         const auto accounts = account_store.list();
                         return sbi_core::http2::Response::json(200, json(accounts).dump());
                     }));

    server.add_route("GET",
                     std::string(kProjectApiRoot) + "/account/{id}",
                     guarded([&account_store](const sbi_core::http2::Request& req) {
                         const auto id = req.path_params.at("id");
                         const auto account = account_store.get(id);
                         if (!account.has_value()) {
                             return sbi_core::http2::problem_response(
                                 404, "Not Found", "No Account " + id);
                         }
                         return sbi_core::http2::Response::json(200, json(*account).dump());
                     }));

    // --- Subscriber (E1, project-internal) ---

    server.add_route(
        "POST",
        std::string(kProjectApiRoot) + "/subscriber",
        guarded([&subscriber_store,
                 &subscriber_create_counter](const sbi_core::http2::Request& req) {
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<subscriber_management::Subscriber>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto id = subscriber_store.create(*body);
            subscriber_create_counter->Add(1);
            const auto stored = subscriber_store.get(id);

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location", std::string(kProjectApiRoot) + "/subscriber/" + id);
            resp.body = json(*stored).dump();
            return resp;
        }));

    server.add_route("GET",
                     std::string(kProjectApiRoot) + "/subscriber",
                     guarded([&subscriber_store](const sbi_core::http2::Request&) {
                         const auto subscribers = subscriber_store.list();
                         return sbi_core::http2::Response::json(200, json(subscribers).dump());
                     }));

    server.add_route("GET",
                     std::string(kProjectApiRoot) + "/subscriber/{id}",
                     guarded([&subscriber_store](const sbi_core::http2::Request& req) {
                         const auto id = req.path_params.at("id");
                         const auto subscriber = subscriber_store.get(id);
                         if (!subscriber.has_value()) {
                             return sbi_core::http2::problem_response(
                                 404, "Not Found", "No Subscriber " + id);
                         }
                         return sbi_core::http2::Response::json(200, json(*subscriber).dump());
                     }));

    // Real, disclosed convenience lookup: the real trigger for this (nfs/udm or nfs/udr looking up
    // a Subscriber by its real SUPI) doesn't exist yet (see this file's own header comment), but
    // the store already supports it (SubscriberStore::get_by_supi) -- exposing it now rather than
    // leaving a store capability with no HTTP path to reach it. Real query-param convention (not a
    // separate TMF-looking subpath, since this is project-internal, not a real TM Forum resource).
    server.add_route("GET",
                     std::string(kProjectApiRoot) + "/subscriber/by-supi/{supi}",
                     guarded([&subscriber_store](const sbi_core::http2::Request& req) {
                         const auto supi = req.path_params.at("supi");
                         const auto subscriber = subscriber_store.get_by_supi(supi);
                         if (!subscriber.has_value()) {
                             return sbi_core::http2::problem_response(
                                 404, "Not Found", "No Subscriber with supi " + supi);
                         }
                         return sbi_core::http2::Response::json(200, json(*subscriber).dump());
                     }));

    server.start();
    spdlog::info("subscriber-management: listening on https://0.0.0.0:{} (TLS 1.3 + mTLS)", port);
    spdlog::info("subscriber-management: Prometheus metrics at http://{}/metrics",
                 metrics_bind_address);
    ioc.run();
    return 0;
}
