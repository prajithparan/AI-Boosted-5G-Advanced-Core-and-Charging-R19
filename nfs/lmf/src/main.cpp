// nfs/lmf: LMF (Location Management Function), Nlmf_Location + Nlmf_Broadcast +
// Nlmf_DataExposure. Source: specs/5G_APIs-REL-19/TS29572_Nlmf_Location.yaml (v1.4.1),
// TS29572_Nlmf_Broadcast.yaml (v1.3.0), TS29572_Nlmf_DataExposure.yaml (v1.0.0), commit
// bca84b60a37773133bcae97e5c6c0d10a93b47b6. This project's sixteenth NF, eighth built under the
// continuous move-to-next-NF process (docs/DECISIONS.md ADR-0184), fourth Tier 2 NF, following
// GMLC (ADR-0189). Nlmf_Broadcast/Nlmf_DataExposure added later as ADR-0193 audit gap-closures
// (ADR-0196). All 11 real operations implemented:
//   POST   {apiRoot}/determine-location                DetermineLocation
//   POST   {apiRoot}/up-subscriptions                   UpSubscriptions
//   DELETE {apiRoot}/up-subscriptions/{subscriptionId}   DeleteSubscription
//   POST   {apiRoot}/cancel-location                     CancelLocation
//   POST   {apiRoot}/location-context-transfer           LocationContextTransfer
//   POST   {apiRoot}/measure-location                    LocationMeasure
//   POST   {apiRoot}/configure-up                        UpConfig
//   POST   /nlmf-broadcast/v1/cipher-key-data             CipheringKeyData
//   POST   /nlmf-dataexposure/v1/subscriptions            CreateSubscription
//   PATCH  /nlmf-dataexposure/v1/subscriptions/{id}        ModifySubscription
//   DELETE /nlmf-dataexposure/v1/subscriptions/{id}        DeleteSubscription
//
// Real, disclosed scope decision -- stated up front, not discovered in review, and the direct
// continuation of GMLC's own disclosed gap (ADR-0189): `DetermineLocation`'s real behavior (TS
// 23.273/38.305) is to obtain an actual UE position via real LPP (LTE Positioning Protocol, TS
// 37.355) message exchange over NG-RAN, or real GNSS assistance data delivery -- neither of which
// this project has any spec material or a real/simulated RAN/UE stack for. `LocationMeasure`
// similarly needs real PRU (Positioning Reference Unit) measurement data from a real NRPPa/gNB
// positioning stack (TS 38.305/38.455), also absent. This project will NOT fabricate GPS
// coordinates, LPP PDUs, or PRU measurement bytes with no real source -- CLAUDE.md's single worst
// failure mode. Both operations therefore, after real structural input validation (a real `400`
// on malformed input, matching every other NF's own precedent -- `DetermineLocation` additionally
// enforces the real YAML's own `not: required: [ecgi, ncgi]` mutual-exclusivity constraint, a
// real declared rule the codegen's own structural typing doesn't enforce automatically), return
// the real, documented `501 Not Implemented` response the YAML itself declares. `CancelLocation`
// cancels a previously-requested location determination by its `ldrReference` -- since
// `DetermineLocation` never creates one (disclosed above), no cancellation request can ever have
// one to match, so this always returns the real `404` -- the honest real behavior, not a
// fabricated success.
//
// What IS fully, really implemented, independent of any RF/GNSS/LPP capability:
// `UpSubscriptions`/`DeleteSubscription` (a real, full create+delete lifecycle -- unlike GMLC's
// own create-only `/loc-update-subs`, ADR-0189 -- `lmf::UpSubscriptionStore`; real, disclosed
// spec gap: the real `201` response declares no id-bearing header or body field, so this
// implementation adds a real `Location` header at the HTTP layer, not a fabricated JSON field, to
// make the resource actually usable), `LocationContextTransfer` (a real LMF-relocation
// context push, stored in `lmf::LocationContextStore` keyed by the real, required
// `ldrReference`), and `UpConfig` (a real secure-LCS-UP-connection setup/modify/terminate,
// `lmf::UpConfigStore` keyed by supi/gpsi per the real YAML's own declared `anyOf` requirement --
// enforced as a real `400`, not invented -- `TERMINATION` real-behaviorally removes the stored
// entry rather than just accepting and discarding the request). `CreateSubscription`/
// `ModifySubscription`/`DeleteSubscription` (Nlmf_DataExposure,
// `lmf::DataExposureSubscriptionStore`
// -- a real, full create+modify+delete lifecycle, `ModifySubscription` using the same real RFC
// 6902 JSON Patch application as `nfs/nrf`'s own `NfRegistry::apply_patch`).
//
// `CipheringKeyData` (Nlmf_Broadcast) is real but structurally limited by its own YAML: it is the
// AMF-facing QUERY interface only -- no write/provisioning operation exists anywhere in this real
// spec surface for populating actual `CipheringDataSet` records (real LTE/NR positioning SIB
// ciphering key material is O&M/vendor-provisioned, outside this specific SBI service). This
// project has no such provisioning path, so `dataAvailability` honestly always reports
// `CIPHERING_KEY_DATA_NOT_AVAILABLE`, and the real declared `CipheringKeyData` async callback
// notification never fires -- not a fabricated key, not a silently-dropped feature.
//
// `Nlmf_DataExposure`'s own real notification path (`LmfDataExposureNotification`, carrying
// `LmfDataExposureReport.samplingDataList` -> real `locMeasureData`/`groundTruth` location
// measurements) shares the exact same disclosed RF/LPP/PRU capability gap as `LocationMeasure`
// above -- this project has no real measurement data to ever populate a report with, so no
// notification is ever generated in this build, even though the subscription lifecycle managing
// it is fully real.

#include "sbi_core/http2_client.hpp"
#include "sbi_core/http2_server.hpp"
#include "sbi_core/io_context_pool.hpp"
#include "sbi_core/json_body.hpp"
#include "sbi_core/jwt.hpp"
#include "sbi_core/logging.hpp"
#include "sbi_core/metrics.hpp"
#include "sbi_core/multipart.hpp"
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

// LMF's own types (InputData_Nlmf_Location, CancelLocData_Nlmf_Location, LocContextData,
// LocMeasurementReq, UpSubscription, UpConfig) end up merged into this shared,
// strongly-connected-component-grouped header rather than a standalone TS29572_Nlmf_Location.hpp
// -- same real cross-file $ref-cycle mechanism already documented in nfs/gmlc/src/main.cpp's own
// include comment (ADR-0189), not specific to this NF.
#include "TS26510_CommonData_grp.hpp"
// Gap-closure (ADR-0193 audit, ADR-0196): Nlmf_Broadcast/Nlmf_DataExposure's own types stayed in
// their own standalone headers (no cross-file $ref cycle pulled them into the shared group
// header above).
#include "TS26510_CommonData_grp.hpp" // was TS29572_Nlmf_DataExposure.hpp; merged into the group by ADR-0358's regeneration
#include "TS29572_Nlmf_Broadcast.hpp"
#include "nf_config/nf_config.hpp"
#include "stores.hpp"

namespace {

using nlohmann::json;

#ifndef CERTS_DIR
#error "CERTS_DIR must be defined by CMake (see nfs/lmf/CMakeLists.txt)"
#endif
#ifndef CONFIG_DIR
#error "CONFIG_DIR must be defined by CMake (see nfs/lmf/CMakeLists.txt)"
#endif

constexpr const char* kNfType = "LMF";
constexpr const char* kApiRoot = "/nlmf-loc/v1";
// Gap-closure (ADR-0193 audit, ADR-0196).
constexpr const char* kBroadcastApiRoot = "/nlmf-broadcast/v1";
constexpr const char* kDataExposureApiRoot = "/nlmf-dataexposure/v1";

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

// Runs on a dedicated thread, never on the server's io_context -- same reasoning as
// nfs/ausf/src/main.cpp's run_nrf_lifecycle (docs/DECISIONS.md ADR-0006/ADR-0019).
void run_nrf_lifecycle(const std::string& lmf_instance_id, const std::string& nrf_base) {
    sbi_core::http2::TlsConfig client_tls{
        .cert_path = CERTS_DIR "/lmf/cert.pem",
        .key_path = CERTS_DIR "/lmf/key.pem",
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
        http_client, nrf_base + "/oauth2/token", lmf_instance_id, "nnrf-nfm", "NRF");

    constexpr int kHeartbeatSeconds = 30;
    json profile{
        {"nfInstanceId", lmf_instance_id},
        {"nfType", kNfType},
        {"nfStatus", "REGISTERED"},
        {"ipv4Addresses", json::array({"127.0.0.1"})},
        {"heartBeatTimer", kHeartbeatSeconds},
    };

    while (true) {
        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("lmf: OAuth2 token fetch failed: {}", token.error());
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        sbi_core::http2::ClientRequest put_req;
        put_req.method = "PUT";
        put_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + lmf_instance_id;
        put_req.headers.emplace("content-type", "application/json");
        put_req.headers.emplace("authorization", "Bearer " + *token);
        put_req.headers.emplace(
            sbi_core::headers::kSenderTimestamp,
            sbi_core::headers::format_sender_timestamp(std::chrono::system_clock::now()));
        put_req.body = profile.dump();
        auto put_resp = http_client.send(put_req);
        if (put_resp.has_value() && (put_resp->status == 200 || put_resp->status == 201)) {
            spdlog::info("lmf: registered with NRF (HTTP {})", put_resp->status);
            break;
        }
        spdlog::warn("lmf: NRF registration attempt failed, retrying in 5s");
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(kHeartbeatSeconds / 2));

        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("lmf: OAuth2 token fetch failed for heartbeat: {}", token.error());
            continue;
        }

        sbi_core::http2::ClientRequest patch_req;
        patch_req.method = "PATCH";
        patch_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + lmf_instance_id;
        patch_req.headers.emplace("content-type", "application/json-patch+json");
        patch_req.headers.emplace("authorization", "Bearer " + *token);
        patch_req.body =
            json::array({json{{"op", "replace"}, {"path", "/nfStatus"}, {"value", "REGISTERED"}}})
                .dump();

        auto patch_resp = http_client.send(patch_req);
        if (!patch_resp.has_value() || patch_resp->status != 200) {
            spdlog::warn("lmf: heartbeat failed");
        }
    }
}

} // namespace

int main() {
    sbi_core::init_logging("lmf");
    sbi_core::init_tracing("lmf");

    // ADR-0077 (user-directed, mandatory, project-wide): no DB URL/connection/deployment
    // parameter may be a hardcoded literal default in source -- real values live in the
    // checked-in config/lmf.json, with an env var override per key still available.
    const auto config = nf_config::load("lmf", CONFIG_DIR);
    const auto port = nf_config::require<unsigned short>(config, "port");
    const auto metrics_bind_address =
        nf_config::require<std::string>(config, "metrics_bind_address");
    const auto nrf_base =
        nf_config::require<std::string>(config, "nrf_base_url", "LMF_NRF_BASE_URL");

    sbi_core::init_metrics(metrics_bind_address);

    const std::string lmf_instance_id = sbi_core::generate_uuid_v4();
    spdlog::info("lmf: starting, nfInstanceId={}", lmf_instance_id);

    sbi_core::http2::TlsConfig server_tls{
        .cert_path = CERTS_DIR "/lmf/cert.pem",
        .key_path = CERTS_DIR "/lmf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };

    sbi_core::jwt::Verifier verifier(CERTS_DIR "/nrf-jwt/public.pem", kNrfInstanceId);

    lmf::UpSubscriptionStore up_subscriptions;
    lmf::LocationContextStore location_contexts;
    lmf::UpConfigStore up_configs;
    lmf::DataExposureSubscriptionStore data_exposure_subscriptions;

    auto meter = sbi_core::get_meter("lmf");
    auto determine_location_counter =
        meter->CreateUInt64Counter("lmf_determine_location_total", "Total DetermineLocation calls");
    auto up_subscriptions_counter =
        meter->CreateUInt64Counter("lmf_up_subscriptions_total", "Total UpSubscriptions calls");
    auto delete_subscription_counter = meter->CreateUInt64Counter("lmf_delete_subscription_total",
                                                                  "Total DeleteSubscription calls");
    auto cancel_location_counter =
        meter->CreateUInt64Counter("lmf_cancel_location_total", "Total CancelLocation calls");
    auto location_context_transfer_counter = meter->CreateUInt64Counter(
        "lmf_location_context_transfer_total", "Total LocationContextTransfer calls");
    auto location_measure_counter =
        meter->CreateUInt64Counter("lmf_location_measure_total", "Total LocationMeasure calls");
    auto up_config_counter =
        meter->CreateUInt64Counter("lmf_up_config_total", "Total UpConfig calls");
    // Gap-closure (ADR-0193 audit, ADR-0196).
    auto ciphering_key_data_counter = meter->CreateUInt64Counter(
        "lmf_ciphering_key_data_total", "Total Nlmf_Broadcast CipheringKeyData calls");
    auto data_exposure_create_counter =
        meter->CreateUInt64Counter("lmf_data_exposure_create_subscription_total",
                                   "Total Nlmf_DataExposure CreateSubscription calls");
    auto data_exposure_modify_counter =
        meter->CreateUInt64Counter("lmf_data_exposure_modify_subscription_total",
                                   "Total Nlmf_DataExposure ModifySubscription calls");
    auto data_exposure_delete_counter =
        meter->CreateUInt64Counter("lmf_data_exposure_delete_subscription_total",
                                   "Total Nlmf_DataExposure DeleteSubscription calls");

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

    // --- Nlmf_Location ---

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/determine-location",
        [&verifier, &determine_location_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            std::string json_body_str = req.body;
            if (const auto content_type_it = req.headers.find("content-type");
                content_type_it != req.headers.end() &&
                sbi_core::multipart::is_multipart_related(content_type_it->second)) {
                auto parts = sbi_core::multipart::parse(content_type_it->second, req.body);
                if (!parts.has_value() || parts->empty()) {
                    return sbi_core::http2::problem_response(
                        400, "Invalid Service Request", "Malformed multipart/related body");
                }
                json_body_str = (*parts)[0].body;
            }
            sbi_gen::InputData_Nlmf_Location body;
            try {
                body = json::parse(json_body_str).get<sbi_gen::InputData_Nlmf_Location>();
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(
                    400, "Missing or invalid mandatory IE", e.what());
            }
            // Real, declared YAML constraint (`not: required: [ecgi, ncgi]`): ecgi and ncgi
            // must not both be present. The codegen's own structural typing doesn't enforce
            // this automatically, so it's checked explicitly here.
            if (body.ecgi.has_value() && body.ncgi.has_value()) {
                return sbi_core::http2::problem_response(
                    400, "Invalid Service Request", "ecgi and ncgi must not both be present");
            }
            determine_location_counter->Add(1);
            // Real, disclosed -- see this file's own top comment: no real LPP/GNSS positioning
            // capability exists in this project. The real, documented 501 response, not a
            // fabricated location.
            return sbi_core::http2::problem_response(
                501,
                "Not Implemented",
                "LMF DetermineLocation requires a real LPP (TS 37.355) UE positioning exchange "
                "or real GNSS assistance data, neither implemented in this project -- see "
                "docs/DECISIONS.md ADR-0190");
        });

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/up-subscriptions",
        [&verifier, &up_subscriptions, &up_subscriptions_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::UpSubscription>(req, err);
            if (!body.has_value()) {
                return err;
            }
            up_subscriptions_counter->Add(1);
            const auto id = up_subscriptions.create(json(*body));

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            // Real, disclosed: the YAML declares no header for this 201 -- added for real
            // usability, see this file's own top comment.
            resp.headers.emplace("location", std::string(kApiRoot) + "/up-subscriptions/" + id);
            resp.body = json(*body).dump();
            return resp;
        });

    server.add_route(
        "DELETE",
        std::string(kApiRoot) + "/up-subscriptions/{subscriptionId}",
        [&verifier, &up_subscriptions, &delete_subscription_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            if (!up_subscriptions.remove(id)) {
                return sbi_core::http2::problem_response(404, "Not Found", "No subscription " + id);
            }
            delete_subscription_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
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
                sbi_core::http2::parse_json_body<sbi_gen::CancelLocData_Nlmf_Location>(req, err);
            if (!body.has_value()) {
                return err;
            }
            cancel_location_counter->Add(1);
            // Real, disclosed -- see this file's own top comment: DetermineLocation never
            // issues a real ldrReference in this project, so no cancellation request can ever
            // match one. The real, honest 404, not a fabricated success.
            return sbi_core::http2::problem_response(
                404,
                "Not Found",
                "No active location request for ldrReference " + body->ldrReference);
        });

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/location-context-transfer",
        [&verifier, &location_contexts, &location_context_transfer_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::LocContextData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            location_context_transfer_counter->Add(1);
            location_contexts.put(body->ldrReference, json(*body));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/measure-location",
        [&verifier, &location_measure_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::LocMeasurementReq>(req, err);
            if (!body.has_value()) {
                return err;
            }
            location_measure_counter->Add(1);
            // Real, disclosed -- see this file's own top comment: no real PRU/NRPPa measurement
            // capability exists in this project. The real, documented 501 response, not
            // fabricated measurement bytes.
            return sbi_core::http2::problem_response(
                501,
                "Not Implemented",
                "LMF LocationMeasure requires a real PRU/NRPPa measurement capability (TS "
                "38.305/38.455), not implemented in this project -- see docs/DECISIONS.md "
                "ADR-0190");
        });

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/configure-up",
        [&verifier, &up_configs, &up_config_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::UpConfig>(req, err);
            if (!body.has_value()) {
                return err;
            }
            // Real, declared YAML constraint (`anyOf: [required:[supi], required:[gpsi]]`) --
            // enforced here since the codegen's own structural typing leaves both optional.
            if (!body->supi.has_value() && !body->gpsi.has_value()) {
                return sbi_core::http2::problem_response(
                    400, "Invalid Service Request", "At least one of supi or gpsi is required");
            }
            up_config_counter->Add(1);
            const std::string key = body->supi.has_value() ? *body->supi : *body->gpsi;
            if (body->lcsUpConnectionInd.has_value() &&
                body->lcsUpConnectionInd->value == "TERMINATION") {
                up_configs.terminate(key);
            } else {
                up_configs.put(key, json(*body));
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nlmf_Broadcast (gap-closure, ADR-0193 audit, ADR-0196) ---

    server.add_route(
        "POST",
        std::string(kBroadcastApiRoot) + "/cipher-key-data",
        [&verifier, &ciphering_key_data_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::CipherRequestData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            ciphering_key_data_counter->Add(1);
            // Real, disclosed gap: this YAML's own single operation is the AMF-facing QUERY
            // interface only -- it declares no real write/provisioning operation anywhere for
            // populating actual CipheringDataSet records (LTE/NR positioning SIB ciphering keys
            // are real O&M/vendor-provisioned data per TS 33.501's own broader security
            // framework, outside this specific 3GPP SBI surface). This project has no such
            // provisioning path, so real ciphering key data can never exist here -- honestly
            // reporting unavailable, not fabricating key material. `body->amfCallBackURI` is
            // real and would be used to push a real async `CipheringKeyData` notification if
            // data ever became available (per this YAML's own declared callback), but that never
            // fires in this build for the same reason.
            sbi_gen::CipherResponseData resp_data{};
            resp_data.dataAvailability = "CIPHERING_KEY_DATA_NOT_AVAILABLE";
            return sbi_core::http2::Response::json(200, json(resp_data).dump());
        });

    // --- Nlmf_DataExposure (gap-closure, ADR-0193 audit, ADR-0196) ---

    server.add_route(
        "POST",
        std::string(kDataExposureApiRoot) + "/subscriptions",
        [&verifier, &data_exposure_subscriptions, &data_exposure_create_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::LmfDataExposureSubscription>(req, err);
            if (!body.has_value()) {
                return err;
            }
            data_exposure_create_counter->Add(1);
            const auto id = data_exposure_subscriptions.create(json(*body));

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kDataExposureApiRoot) + "/subscriptions/" + id);
            resp.body = json(*body).dump();
            return resp;
        });

    server.add_route(
        "PATCH",
        std::string(kDataExposureApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &data_exposure_subscriptions, &data_exposure_modify_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            json patch_ops;
            try {
                patch_ops = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            std::optional<json> patched;
            try {
                patched = data_exposure_subscriptions.apply_patch(id, patch_ops);
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(400, "Invalid JSON Patch", e.what());
            }
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(404, "Not Found", "No subscription " + id);
            }
            data_exposure_modify_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "DELETE",
        std::string(kDataExposureApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &data_exposure_subscriptions, &data_exposure_delete_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            if (!data_exposure_subscriptions.remove(id)) {
                return sbi_core::http2::problem_response(404, "Not Found", "No subscription " + id);
            }
            data_exposure_delete_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    std::thread(run_nrf_lifecycle, lmf_instance_id, nrf_base).detach();

    server.start();
    spdlog::info("lmf: listening on https://0.0.0.0:{} (TLS 1.3 + mTLS)", port);
    spdlog::info("lmf: Prometheus metrics at http://{}/metrics", metrics_bind_address);
    sbi_core::run_multi_threaded(ioc);
    return 0;
}
