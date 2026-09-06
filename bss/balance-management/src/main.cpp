// bss/balance-management: a real, standalone TM Forum TMF654 Prepay Balance Management service --
// the ABMF (Account Balance Management Function) half of CHARGING_PROMPT.md's P4.3
// ("Rating engine (E5) + ABMF (E6)"). Source: TM Forum's real public TMF654 v4.0.0 swagger
// (github.com/tmforum-apis/TMF654_PrepayBalanceManagement,
// TMF654-PrepayBalance-v4.0.0.swagger.json, fetched and parsed directly, not vendored -- see
// libs/bss-sid/include/bss_sid/balance.hpp's own header for why). Real basePath:
// /tmf-api/prepayBalanceManagement/v4.
//
// Why a standalone BSS component, not code inside nfs/chf: same reasoning as bss/product-catalog
// (its own file header) -- TMF654 is a TM Forum Open API, a different ecosystem from the 3GPP SBI
// stack CHF's Nchf_* APIs live in, and CLAUDE.md's "align to TM Forum ODA component boundaries"
// goal argues for a real component boundary here too.
//
// Scope, per CHARGING_PROMPT.md's P4.3 real requirements ("unit reservation and refund, multi-
// balance..., explicit currency and rounding rules...strong consistency on balance mutation --
// prove it under concurrent debit tests"): Bucket (GET only -- the real TMF654 API has NO
// `POST /bucket` at all, confirmed directly against the swagger), TopupBalance (POST/GET -- the
// real credit/bucket-creation path, see store.hpp's own disclosed interpretation),
// AdjustBalance (POST/GET -- real signed debit/credit), ReserveBalance (POST/GET -- real signed
// reserve/unreserve, sign convention disclosed as this project's own interpretation, not
// confirmed from spec prose), AccumulatedBalance (GET, filtered by `partyAccount.id` -- disclosed:
// this specific query parameter is NOT itemized in the real swagger's own `parameters` list for
// this operation (only fields/offset/limit are), but matches TM Forum's well-known general
// attribute-path filtering convention for Open APIs -- not confirmed from this specific spec
// file's text, flagged rather than silently assumed). Deliberately NOT modeled this pass:
// TransferBalance, BalanceActionHistory (real resources, not needed to prove P4.3's core ask).
//
// Persistence: real PostgreSQL (libpqxx) alone -- see schema.sql's own header for why this
// deliberately deviates from docs/DATA_MODEL.md's original two-store (Redis hot + PostgreSQL
// ledger) E6 sketch: a single-statement atomic `UPDATE ... WHERE balance >= amount` already gives
// genuine strong consistency via PostgreSQL's row-level locking, with no two-store desync risk.
//
// Disclosed simplifications:
// - Not a 3GPP NF: no NRF registration, no OAuth2 bearer-token verification -- same reasoning and
//   same mTLS-only security boundary as bss/product-catalog's own disclosure.
// - Not wired into deploy/docker/docker-compose.yml yet -- verified via a manually-run postgres
//   container, same disclosed gap bss/product-catalog had before its own follow-up closed it.
// - Not yet wired to CHF's rating engine (build_rating_grant) -- CHF still does not call this
//   service to actually debit a real balance when it grants units. That real integration (closing
//   ADR-0048/0050's own disclosed "no balance/wallet deduction against what was already consumed"
//   gap) is P4.3's Rating Function (E5) half, a separate, not-yet-built piece.
// - Real, disclosed sign-convention interpretations for TopupBalance's implicit bucket-creation
//   and ReserveBalance's reserve-vs-unreserve direction -- see the file-level comment above and
//   bss_sid/balance.hpp's own header for the full reasoning.

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

#include "bss_sid/balance.hpp"
#include "store.hpp"

// docs/DECISIONS.md ADR-0077 -- no hardcoded DB URL/deployment literal in source.
#include "nf_config/nf_config.hpp"

#ifndef CERTS_DIR
#error "CERTS_DIR must be defined by CMake (see bss/balance-management/CMakeLists.txt)"
#endif
#ifndef CONFIG_DIR
#error "CONFIG_DIR must be defined by CMake (see bss/balance-management/CMakeLists.txt)"
#endif

namespace {

using nlohmann::json;

constexpr const char* kApiRoot = "/tmf-api/prepayBalanceManagement/v4";

sbi_core::http2::Response not_found(const std::string& resource, const std::string& id) {
    return sbi_core::http2::problem_response(404, "Not Found", "No " + resource + " " + id);
}

} // namespace

int main() {
    const auto config = nf_config::load("balance-management", CONFIG_DIR);
    const auto port = nf_config::require<unsigned short>(config, "port");
    const auto metrics_bind_address =
        nf_config::require<std::string>(config, "metrics_bind_address");
    const auto self_base = nf_config::require<std::string>(
        config, "self_base_url", "BALANCE_MANAGEMENT_SELF_BASE_URL");
    const auto conninfo =
        nf_config::require<std::string>(config, "database_url", "BALANCE_MANAGEMENT_DATABASE_URL");

    sbi_core::init_logging("balance-management");
    sbi_core::init_tracing("balance-management");
    sbi_core::init_metrics(metrics_bind_address);

    spdlog::info("balance-management: starting (TM Forum TMF654 Prepay Balance Management)");

    sbi_core::http2::TlsConfig server_tls{
        .cert_path = CERTS_DIR "/balance-management/cert.pem",
        .key_path = CERTS_DIR "/balance-management/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };

    balance_management::BalanceStore store(self_base + kApiRoot, conninfo);
    spdlog::info("balance-management: connected to PostgreSQL");

    auto meter = sbi_core::get_meter("balance-management");
    auto topup_counter =
        meter->CreateUInt64Counter("balance_management_topup_total", "Total TopupBalance creates");
    auto adjust_counter = meter->CreateUInt64Counter("balance_management_adjust_total",
                                                     "Total AdjustBalance creates");
    auto adjust_rejected_counter =
        meter->CreateUInt64Counter("balance_management_adjust_rejected_total",
                                   "Total AdjustBalance creates rejected for insufficient balance");
    auto reserve_counter = meter->CreateUInt64Counter("balance_management_reserve_total",
                                                      "Total ReserveBalance creates");
    auto reserve_rejected_counter = meter->CreateUInt64Counter(
        "balance_management_reserve_rejected_total",
        "Total ReserveBalance creates rejected for insufficient balance");

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

    // --- Bucket (GET only -- see file header) ---

    server.add_route(
        "GET", std::string(kApiRoot) + "/bucket", [&store](const sbi_core::http2::Request& req) {
            // ADR-0307: `?relatedParty.id=` is TM Forum's own standard dot-path collection filter,
            // not a bespoke query parameter -- which is why C1 needed no new resource. It returns
            // the SHARED buckets that party belongs to, so CHF can ask "does this subscriber draw
            // from a family bucket?" in one call.
            const auto filter = req.query_params.find("relatedParty.id");
            if (filter != req.query_params.end() && !filter->second.empty()) {
                const auto shared = store.find_shared_bucket_for(filter->second);
                json out = json::array();
                if (shared.has_value()) {
                    out.push_back(*shared);
                }
                return sbi_core::http2::Response::json(200, out.dump());
            }
            return sbi_core::http2::Response::json(200, json(store.list_buckets()).dump());
        });

    server.add_route("GET",
                     std::string(kApiRoot) + "/bucket/{id}",
                     [&store](const sbi_core::http2::Request& req) {
                         const auto id = req.path_params.at("id");
                         const auto bucket = store.get_bucket(id);
                         if (!bucket.has_value()) {
                             return not_found("Bucket", id);
                         }
                         return sbi_core::http2::Response::json(200, json(*bucket).dump());
                     });

    // --- AccumulatedBalance (GET, real query filter -- see file header's disclosure) ---

    server.add_route("GET",
                     std::string(kApiRoot) + "/accumulatedBalance",
                     [&store](const sbi_core::http2::Request& req) {
                         const auto it = req.query_params.find("partyAccount.id");
                         if (it == req.query_params.end()) {
                             return sbi_core::http2::problem_response(
                                 400, "Bad Request", "partyAccount.id query parameter is required");
                         }
                         const auto accumulated = store.get_accumulated_balance(it->second);
                         return sbi_core::http2::Response::json(200, json(accumulated).dump());
                     });

    // --- TopupBalance ---

    server.add_route("POST",
                     std::string(kApiRoot) + "/topupBalance",
                     [&store, &topup_counter](const sbi_core::http2::Request& req) {
                         sbi_core::http2::Response err;
                         auto body =
                             sbi_core::http2::parse_json_body<bss_sid::TopupBalance>(req, err);
                         if (!body.has_value()) {
                             return err;
                         }
                         if (!body->bucket.has_value() || body->bucket->id.empty()) {
                             return sbi_core::http2::problem_response(
                                 400, "Bad Request", "bucket.id is required");
                         }
                         const auto result = store.topup(*body);
                         topup_counter->Add(1);

                         sbi_core::http2::Response resp;
                         resp.status = 201;
                         resp.headers.emplace("content-type", "application/json");
                         resp.headers.emplace("location", *result.record.href);
                         resp.body = json(result.record).dump();
                         return resp;
                     });

    server.add_route("GET",
                     std::string(kApiRoot) + "/topupBalance/{id}",
                     [&store](const sbi_core::http2::Request& req) {
                         const auto id = req.path_params.at("id");
                         const auto record = store.get_topup(id);
                         if (!record.has_value()) {
                             return not_found("TopupBalance", id);
                         }
                         return sbi_core::http2::Response::json(200, json(*record).dump());
                     });

    // --- AdjustBalance (real signed debit/credit -- the strong-consistency-under-concurrency
    // mutation P4.3 asks to be proven) ---

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/adjustBalance",
        [&store, &adjust_counter, &adjust_rejected_counter](const sbi_core::http2::Request& req) {
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<bss_sid::AdjustBalance>(req, err);
            if (!body.has_value()) {
                return err;
            }
            if (!body->bucket.has_value() || body->bucket->id.empty()) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", "bucket.id is required");
            }
            const auto result = store.adjust(*body);
            adjust_counter->Add(1);
            if (!result.succeeded) {
                adjust_rejected_counter->Add(1);
            }

            // Real TMF654 semantics: an insufficient-balance rejection is a real business
            // outcome (ActionStatusType "failed"), not an HTTP error -- the resource is still
            // created (an audit record of the attempt), same as a declined real-world charge.
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location", *result.record.href);
            resp.body = json(result.record).dump();
            return resp;
        });

    server.add_route("GET",
                     std::string(kApiRoot) + "/adjustBalance/{id}",
                     [&store](const sbi_core::http2::Request& req) {
                         const auto id = req.path_params.at("id");
                         const auto record = store.get_adjust(id);
                         if (!record.has_value()) {
                             return not_found("AdjustBalance", id);
                         }
                         return sbi_core::http2::Response::json(200, json(*record).dump());
                     });

    // --- ReserveBalance (real signed reserve/unreserve) ---

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/reserveBalance",
        [&store, &reserve_counter, &reserve_rejected_counter](const sbi_core::http2::Request& req) {
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<bss_sid::ReserveBalance>(req, err);
            if (!body.has_value()) {
                return err;
            }
            if (!body->bucket.has_value() || body->bucket->id.empty()) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", "bucket.id is required");
            }
            const auto result = store.reserve(*body);
            reserve_counter->Add(1);
            if (!result.succeeded) {
                reserve_rejected_counter->Add(1);
            }

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location", *result.record.href);
            resp.body = json(result.record).dump();
            return resp;
        });

    server.add_route("GET",
                     std::string(kApiRoot) + "/reserveBalance/{id}",
                     [&store](const sbi_core::http2::Request& req) {
                         const auto id = req.path_params.at("id");
                         const auto record = store.get_reserve(id);
                         if (!record.has_value()) {
                             return not_found("ReserveBalance", id);
                         }
                         return sbi_core::http2::Response::json(200, json(*record).dump());
                     });

    server.start();
    spdlog::info("balance-management: listening on https://0.0.0.0:{} (TLS 1.3 + mTLS)", port);
    spdlog::info("balance-management: Prometheus metrics at http://{}/metrics",
                 metrics_bind_address);
    ioc.run();
    return 0;
}
