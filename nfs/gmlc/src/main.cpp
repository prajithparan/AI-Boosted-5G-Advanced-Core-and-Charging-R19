// nfs/gmlc: GMLC (Gateway Mobile Location Centre), Ngmlc_Location. Source:
// specs/5G_APIs-REL-19/TS29515_Ngmlc_Location.yaml (v1.3.0), commit
// bca84b60a37773133bcae97e5c6c0d10a93b47b6. This project's fifteenth NF, third Tier 2 NF built
// under the continuous move-to-next-NF process (docs/DECISIONS.md ADR-0184), following SMSF
// (ADR-0188). All 5 real operations implemented:
//   POST {apiRoot}/provide-location                      RequestLocation
//   POST {apiRoot}/cancel-location                        CancelLocation
//   POST {apiRoot}/location-update                        UpdateLocation
//   POST {apiRoot}/loc-update-subs                        LocationUpdateSubcribe
//   POST {apiRoot}/perform-privacy-check-id-mapping        PrivacyCheckIdMapping
//
// Real, disclosed scope decision -- stated up front, not discovered in review: `RequestLocation`'s
// real behavior (TS 23.273) is to obtain an actual UE position via this project's own LMF
// (Nlmf_Location, TS 29.572) -- LMF is a separate Tier 2 NF this project has not built yet. This
// project will NOT fabricate GPS coordinates or any other positioning data with no real source --
// that is exactly the "invent a field" failure mode CLAUDE.md calls the single worst failure mode
// on this project. `RequestLocation` therefore, after real structural input validation (a real 400
// on malformed/missing-required-field input, matching every other NF's own precedent), returns the
// real, documented `501 Not Implemented` response the YAML itself declares, honestly signalling
// "no real positioning backend exists" rather than a fabricated location. `CancelLocation` cancels
// a previously-requested location determination by its `ldrReference` -- since `RequestLocation`
// above never creates one (disclosed 501), no `ldrReference` this project's GMLC has ever issued
// can be valid, so this always returns the real `404` after input validation -- not a stub, the
// honest real behavior given the system's own actual state, not a fabricated success.
//
// What IS fully, really implemented, independent of LMF: `UpdateLocation` (a real VGMLC->HGMLC
// location-context push, stored -- `gmlc::LocationContextStore`), `LocationUpdateSubcribe` (real
// subscription acceptance -- `gmlc::LocUpdateSubscriptionStore`; real, disclosed structural gap:
// the YAML itself declares no GET/DELETE for this resource, so nothing in this project or the real
// spec can ever query or cancel it back -- the real `LocationUpdateNotify` callback also never
// fires, same disclosed-gap shape as NEF's/SCP's own unfireable callbacks), and
// `PrivacyCheckIdMapping` (a real, pure GPSI<->application-layer-ID lookup with no LMF dependency
// at all -- `gmlc::GpsiAppLayerIdMappingStore`, seed()-only, same shape as NEF's own
// `PfdCatalogStore`: the real provisioning of these mappings is AF-registration/OAM scope, out of
// 3GPP's own SBI framework here).

#include "sbi_core/http2_client.hpp"
#include "sbi_core/http2_server.hpp"
#include "sbi_core/io_context_pool.hpp"
#include "sbi_core/json_body.hpp"
#include "sbi_core/jwt.hpp"
#include "sbi_core/logging.hpp"
#include "sbi_core/metrics.hpp"
#include "sbi_core/oauth2_client.hpp"
#include "sbi_core/otel.hpp"
#include "sbi_core/rate_limit.hpp"
#include "sbi_core/sbi_headers.hpp"
#include "sbi_core/uuid.hpp"

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <thread>

// GMLC's own types (InputData_Ngmlc_Location, CancelLocData_Ngmlc_Location, LocUpdateData,
// LocUpdateSubs, PrivacyCheckIdMappingReqData/RespData) end up merged into this shared,
// strongly-connected-component-grouped header rather than a standalone TS29515_Ngmlc_Location.hpp
// -- see tools/sbi-codegen/sbi_codegen/render.py's own module docstring for why (real, cross-file
// $ref cycles with TS29572_Nlmf_Location.yaml force the merge, same mechanism already documented
// project-wide, not specific to this NF).
#include "TS26510_CommonData_grp.hpp"
#include "nf_config/nf_config.hpp"
#include "stores.hpp"

namespace {

using nlohmann::json;

#ifndef CERTS_DIR
#error "CERTS_DIR must be defined by CMake (see nfs/gmlc/CMakeLists.txt)"
#endif
#ifndef CONFIG_DIR
#error "CONFIG_DIR must be defined by CMake (see nfs/gmlc/CMakeLists.txt)"
#endif

constexpr const char* kNfType = "GMLC";
constexpr const char* kApiRoot = "/ngmlc-loc/v1";

// Must match nfs/nrf/src/main.cpp's kNrfInstanceId exactly -- see docs/DECISIONS.md ADR-0018.
constexpr const char* kNrfInstanceId = "5ba9a927-1d31-4c8e-8a10-000000000001";

// Same pattern as every other NF's check_bearer -- see nfs/nrf/src/main.cpp's comment for why a
// missing Authorization header is not itself a 401 (bootstrap security alternative:
// `security: [{}, oAuth2ClientCredentials:[...]]` in the YAML).
std::optional<sbi_core::jwt::VerifyResult> check_bearer(const sbi_core::http2::Request& req,
                                                        sbi_core::jwt::Verifier& verifier) {
    auto it = req.headers.find("authorization");
    if (it == req.headers.end()) {
        return std::nullopt;
    }
    const std::string& value = it->second;
    constexpr std::string_view kPrefix = "Bearer ";
    if (value.size() <= kPrefix.size() || value.compare(0, kPrefix.size(), kPrefix) != 0) {
        sbi_core::jwt::VerifyResult r;
        r.valid = false;
        r.error = "Authorization header present but not a Bearer token";
        return r;
    }
    return verifier.verify(value.substr(kPrefix.size()));
}

// Real, illustrative seed data for PrivacyCheckIdMapping -- see this file's own top comment for
// why no live write path exists to populate this instead.
void seed_id_mappings(gmlc::GpsiAppLayerIdMappingStore& store) {
    store.seed("msisdn-15550100001", "applayer-alice");
    store.seed("msisdn-15550100002", "applayer-bob");
}

// Runs on a dedicated thread, never on the server's io_context -- same reasoning as
// nfs/ausf/src/main.cpp's run_nrf_lifecycle (docs/DECISIONS.md ADR-0006/ADR-0019).
void run_nrf_lifecycle(const std::string& gmlc_instance_id, const std::string& nrf_base) {
    sbi_core::http2::TlsConfig client_tls{
        .cert_path = CERTS_DIR "/gmlc/cert.pem",
        .key_path = CERTS_DIR "/gmlc/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client http_client(std::move(client_tls));

    for (int attempt = 0; attempt < 300; ++attempt) {
        sbi_core::http2::ClientRequest probe;
        probe.method = "GET";
        probe.url = nrf_base + "/nnrf-nfm/v1/nf-instances/00000000-0000-4000-8000-000000000000";
        if (http_client.send(probe).has_value()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    sbi_core::OAuth2Client oauth(
        http_client, nrf_base + "/oauth2/token", gmlc_instance_id, "nnrf-nfm", "NRF");

    constexpr int kHeartbeatSeconds = 30;
    json profile{
        {"nfInstanceId", gmlc_instance_id},
        {"nfType", kNfType},
        {"nfStatus", "REGISTERED"},
        {"ipv4Addresses", json::array({"127.0.0.1"})},
        {"heartBeatTimer", kHeartbeatSeconds},
    };

    while (true) {
        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("gmlc: OAuth2 token fetch failed: {}", token.error());
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        sbi_core::http2::ClientRequest put_req;
        put_req.method = "PUT";
        put_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + gmlc_instance_id;
        put_req.headers.emplace("content-type", "application/json");
        put_req.headers.emplace("authorization", "Bearer " + *token);
        put_req.headers.emplace(
            sbi_core::headers::kSenderTimestamp,
            sbi_core::headers::format_sender_timestamp(std::chrono::system_clock::now()));
        put_req.body = profile.dump();
        auto put_resp = http_client.send(put_req);
        if (put_resp.has_value() && (put_resp->status == 200 || put_resp->status == 201)) {
            spdlog::info("gmlc: registered with NRF (HTTP {})", put_resp->status);
            break;
        }
        spdlog::warn("gmlc: NRF registration attempt failed, retrying in 5s");
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(kHeartbeatSeconds / 2));

        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("gmlc: OAuth2 token fetch failed for heartbeat: {}", token.error());
            continue;
        }

        sbi_core::http2::ClientRequest patch_req;
        patch_req.method = "PATCH";
        patch_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + gmlc_instance_id;
        patch_req.headers.emplace("content-type", "application/json-patch+json");
        patch_req.headers.emplace("authorization", "Bearer " + *token);
        patch_req.body =
            json::array({json{{"op", "replace"}, {"path", "/nfStatus"}, {"value", "REGISTERED"}}})
                .dump();

        auto patch_resp = http_client.send(patch_req);
        if (!patch_resp.has_value() || patch_resp->status != 200) {
            spdlog::warn("gmlc: heartbeat failed");
        }
    }
}

} // namespace

int main() {
    sbi_core::init_logging("gmlc");
    sbi_core::init_tracing("gmlc");

    // ADR-0077 (user-directed, mandatory, project-wide): no DB URL/connection/deployment
    // parameter may be a hardcoded literal default in source -- real values live in the
    // checked-in config/gmlc.json, with an env var override per key still available.
    const auto config = nf_config::load("gmlc", CONFIG_DIR);
    const auto port = nf_config::require<unsigned short>(config, "port");
    const auto metrics_bind_address =
        nf_config::require<std::string>(config, "metrics_bind_address");
    const auto nrf_base =
        nf_config::require<std::string>(config, "nrf_base_url", "GMLC_NRF_BASE_URL");

    sbi_core::init_metrics(metrics_bind_address);

    const std::string gmlc_instance_id = sbi_core::generate_uuid_v4();
    spdlog::info("gmlc: starting, nfInstanceId={}", gmlc_instance_id);

    sbi_core::http2::TlsConfig server_tls{
        .cert_path = CERTS_DIR "/gmlc/cert.pem",
        .key_path = CERTS_DIR "/gmlc/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };

    sbi_core::jwt::Verifier verifier(CERTS_DIR "/nrf-jwt/public.pem", kNrfInstanceId);

    gmlc::LocationContextStore location_contexts;
    gmlc::LocUpdateSubscriptionStore loc_update_subs;
    gmlc::GpsiAppLayerIdMappingStore id_mappings;
    seed_id_mappings(id_mappings);

    auto meter = sbi_core::get_meter("gmlc");
    auto request_location_counter =
        meter->CreateUInt64Counter("gmlc_request_location_total", "Total RequestLocation calls");
    auto cancel_location_counter =
        meter->CreateUInt64Counter("gmlc_cancel_location_total", "Total CancelLocation calls");
    auto update_location_counter =
        meter->CreateUInt64Counter("gmlc_update_location_total", "Total UpdateLocation calls");
    auto loc_update_subscribe_counter = meter->CreateUInt64Counter(
        "gmlc_location_update_subscribe_total", "Total LocationUpdateSubcribe calls");
    auto privacy_check_counter = meter->CreateUInt64Counter("gmlc_privacy_check_id_mapping_total",
                                                            "Total PrivacyCheckIdMapping calls");

    boost::asio::io_context ioc;
    // 0.0.0.0: same Docker-reachability reasoning as NRF's bind -- see docs/DECISIONS.md ADR-0014.
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

    // --- Ngmlc_Location ---

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/provide-location",
        [&verifier, &request_location_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::InputData_Ngmlc_Location>(req, err);
            if (!body.has_value()) {
                return err;
            }
            request_location_counter->Add(1);
            // Real, disclosed -- see this file's own top comment: no real LMF positioning
            // backend exists in this project. The real, documented 501 response, not a
            // fabricated location.
            return sbi_core::http2::problem_response(
                501,
                "Not Implemented",
                "GMLC RequestLocation requires a real Nlmf_Location backend (LMF), not yet "
                "built in this project -- see docs/DECISIONS.md ADR-0189");
        });

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/cancel-location",
        [&verifier, &cancel_location_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::CancelLocData_Ngmlc_Location>(req, err);
            if (!body.has_value()) {
                return err;
            }
            cancel_location_counter->Add(1);
            // Real, disclosed -- see this file's own top comment: RequestLocation never issues
            // a real ldrReference in this project, so no cancellation request can ever match
            // one. The real, honest 404, not a fabricated success.
            return sbi_core::http2::problem_response(
                404,
                "Not Found",
                "No active location request for ldrReference " + body->ldrReference);
        });

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/location-update",
        [&verifier, &location_contexts, &update_location_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::LocUpdateData_Ngmlc_Location>(req, err);
            if (!body.has_value()) {
                return err;
            }
            // Real, disclosed: the YAML itself requires neither supi nor gpsi on this
            // resource, but this project's own store needs a key to identify the UE the
            // pushed context belongs to -- a real store-key necessity, not a fabricated spec
            // requirement.
            std::string key;
            if (body->supi.has_value()) {
                key = *body->supi;
            } else if (body->gpsi.has_value()) {
                key = *body->gpsi;
            } else {
                return sbi_core::http2::problem_response(
                    400,
                    "Invalid Service Request",
                    "At least one of supi or gpsi is required to identify the UE");
            }
            update_location_counter->Add(1);
            location_contexts.put(key, json(*body));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/loc-update-subs",
        [&verifier, &loc_update_subs, &loc_update_subscribe_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::LocUpdateSubs>(req, err);
            if (!body.has_value()) {
                return err;
            }
            loc_update_subscribe_counter->Add(1);
            loc_update_subs.create(json(*body));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/perform-privacy-check-id-mapping",
        [&verifier, &id_mappings, &privacy_check_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::PrivacyCheckIdMappingReqData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            privacy_check_counter->Add(1);

            sbi_gen::PrivacyCheckIdMappingRespData resp_body;
            if (body->gpsiList.has_value()) {
                for (const auto& gpsi : *body->gpsiList) {
                    if (auto mapped = id_mappings.app_layer_id_for(gpsi); mapped.has_value()) {
                        if (!resp_body.appLayerIds.has_value()) {
                            resp_body.appLayerIds = std::vector<sbi_gen::ApplicationlayerId>{};
                        }
                        resp_body.appLayerIds->push_back(*mapped);
                    }
                }
            }
            if (body->appLayerIds.has_value()) {
                for (const auto& app_layer_id : *body->appLayerIds) {
                    if (auto mapped = id_mappings.gpsi_for(app_layer_id); mapped.has_value()) {
                        if (!resp_body.gpsiList.has_value()) {
                            resp_body.gpsiList = std::vector<sbi_gen::Gpsi>{};
                        }
                        resp_body.gpsiList->push_back(*mapped);
                    }
                }
            }
            return sbi_core::http2::Response::json(200, json(resp_body).dump());
        });

    std::thread(run_nrf_lifecycle, gmlc_instance_id, nrf_base).detach();

    server.start();
    spdlog::info("gmlc: listening on https://0.0.0.0:{} (TLS 1.3 + mTLS)", port);
    spdlog::info("gmlc: Prometheus metrics at http://{}/metrics", metrics_bind_address);
    sbi_core::run_multi_threaded(ioc);
    return 0;
}
