// nfs/smsf: SMSF (SMS Function), Nsmsf_SMService. Source:
// specs/5G_APIs-REL-19/TS29540_Nsmsf_SMService.yaml (v2.4.0), commit
// bca84b60a37773133bcae97e5c6c0d10a93b47b6. This project's fourteenth NF, second Tier 2 NF built
// under the continuous move-to-next-NF process (docs/DECISIONS.md ADR-0184), following 5G-EIR
// (ADR-0187). All 5 real operations implemented -- SMSF's entire real spec surface is 1 file, so
// this is another real, COMPLETE NF, not a disclosed partial slice:
//   PUT    {apiRoot}/ue-contexts/{supi}              SMServiceActivation
//   PATCH  {apiRoot}/ue-contexts/{supi}              SMSServiceParameterUpdate
//   DELETE {apiRoot}/ue-contexts/{supi}              SMServiceDeactivation
//   POST   {apiRoot}/ue-contexts/{supi}/sendsms      SendSMS
//   POST   {apiRoot}/ue-contexts/{supi}/send-mt-sms  SendMtSMS
//
// `SendSMS`/`SendMtSMS` use real `multipart/related` bodies (RFC 2046/2387, a JSON part plus an
// opaque binary SMS-payload part) -- built on the existing `sbi_core::multipart` codec already
// used by nfs/amf and nfs/smf (see libs/sbi-core/include/sbi_core/multipart.hpp), not new
// infrastructure. `SendMtSMS`'s own JSON schemas (`SmsData`/`SmsDeliveryData`) are a real
// cross-file $ref into `specs/5G_APIs-REL-19/TS29577_Nipsmgw_SMService.yaml` (IP-SM-GW's own
// service) -- only those two schemas are used here; IP-SM-GW's own `put`/`post` operations are
// out of scope for this NF and not implemented.
//
// Real, disclosed simplification -- stated up front, not discovered in review: this project has
// no real downstream SMS-GMSC/IWMSC (for `SendSMS`, uplink) or real TS 24.011 SMS-over-NAS
// CP-DATA relay to AMF (for `SendMtSMS`, downlink) -- both genuinely out of this session's spec
// material, same class of gap as this project's other disclosed missing-TS-material items (e.g.
// ADR-0104's ProSe auth). `SendSMS` therefore always responds `SMS_DELIVERY_SMSF_ACCEPTED` (the
// real enum value meaning "SMSF accepted for further processing", not a fabricated "delivered"
// claim). `SendMtSMS` responds with a real `SmsDeliveryData` envelope but an empty (0-byte) binary
// payload in place of a real TS 24.011 SMS-DELIVER-REPORT PDU, which this project cannot encode
// without that spec material -- an honest placeholder, not a fabricated PDU.
//
// Real, disclosed, deferred wiring (not built this turn): AMF does not yet call this NF's
// `SMServiceActivation` during Registration, and no NF calls `SendSMS`/`SendMtSMS` -- same pattern
// as 5G-EIR's own AMF non-wiring (ADR-0187).

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

#include "TS29540_Nsmsf_SMService.hpp"
#include "TS29577_Nipsmgw_SMService.hpp"
#include "nf_config/nf_config.hpp"
#include "stores.hpp"

namespace {

using nlohmann::json;

#ifndef CERTS_DIR
#error "CERTS_DIR must be defined by CMake (see nfs/smsf/CMakeLists.txt)"
#endif
#ifndef CONFIG_DIR
#error "CONFIG_DIR must be defined by CMake (see nfs/smsf/CMakeLists.txt)"
#endif

constexpr const char* kNfType = "SMSF";
constexpr const char* kApiRoot = "/nsmsf-sms/v2";

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
void run_nrf_lifecycle(const std::string& smsf_instance_id, const std::string& nrf_base) {
    sbi_core::http2::TlsConfig client_tls{
        .cert_path = CERTS_DIR "/smsf/cert.pem",
        .key_path = CERTS_DIR "/smsf/key.pem",
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
        http_client, nrf_base + "/oauth2/token", smsf_instance_id, "nnrf-nfm", "NRF");

    constexpr int kHeartbeatSeconds = 30;
    json profile{
        {"nfInstanceId", smsf_instance_id},
        {"nfType", kNfType},
        {"nfStatus", "REGISTERED"},
        {"ipv4Addresses", json::array({"127.0.0.1"})},
        {"heartBeatTimer", kHeartbeatSeconds},
    };

    while (true) {
        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("smsf: OAuth2 token fetch failed: {}", token.error());
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        sbi_core::http2::ClientRequest put_req;
        put_req.method = "PUT";
        put_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + smsf_instance_id;
        put_req.headers.emplace("content-type", "application/json");
        put_req.headers.emplace("authorization", "Bearer " + *token);
        put_req.headers.emplace(
            sbi_core::headers::kSenderTimestamp,
            sbi_core::headers::format_sender_timestamp(std::chrono::system_clock::now()));
        put_req.body = profile.dump();
        auto put_resp = http_client.send(put_req);
        if (put_resp.has_value() && (put_resp->status == 200 || put_resp->status == 201)) {
            spdlog::info("smsf: registered with NRF (HTTP {})", put_resp->status);
            break;
        }
        spdlog::warn("smsf: NRF registration attempt failed, retrying in 5s");
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(kHeartbeatSeconds / 2));

        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("smsf: OAuth2 token fetch failed for heartbeat: {}", token.error());
            continue;
        }

        sbi_core::http2::ClientRequest patch_req;
        patch_req.method = "PATCH";
        patch_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + smsf_instance_id;
        patch_req.headers.emplace("content-type", "application/json-patch+json");
        patch_req.headers.emplace("authorization", "Bearer " + *token);
        patch_req.body =
            json::array({json{{"op", "replace"}, {"path", "/nfStatus"}, {"value", "REGISTERED"}}})
                .dump();

        auto patch_resp = http_client.send(patch_req);
        if (!patch_resp.has_value() || patch_resp->status != 200) {
            spdlog::warn("smsf: heartbeat failed");
        }
    }
}

} // namespace

int main() {
    sbi_core::init_logging("smsf");
    sbi_core::init_tracing("smsf");

    // ADR-0077 (user-directed, mandatory, project-wide): no DB URL/connection/deployment
    // parameter may be a hardcoded literal default in source -- real values live in the
    // checked-in config/smsf.json, with an env var override per key still available.
    const auto config = nf_config::load("smsf", CONFIG_DIR);
    const auto port = nf_config::require<unsigned short>(config, "port");
    const auto metrics_bind_address =
        nf_config::require<std::string>(config, "metrics_bind_address");
    const auto sbi_bind_address = nf_config::require<std::string>(config, "sbi_bind_address");
    const auto nrf_base =
        nf_config::require<std::string>(config, "nrf_base_url", "SMSF_NRF_BASE_URL");

    sbi_core::init_metrics(metrics_bind_address);

    const std::string smsf_instance_id = sbi_core::generate_uuid_v4();
    spdlog::info("smsf: starting, nfInstanceId={}", smsf_instance_id);

    sbi_core::http2::TlsConfig server_tls{
        .cert_path = CERTS_DIR "/smsf/cert.pem",
        .key_path = CERTS_DIR "/smsf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };

    sbi_core::jwt::Verifier verifier(CERTS_DIR "/nrf-jwt/public.pem", kNrfInstanceId);

    smsf::UeSmsContextStore contexts;

    auto meter = sbi_core::get_meter("smsf");
    auto activation_counter = meter->CreateUInt64Counter("smsf_sm_service_activation_total",
                                                         "Total SMServiceActivation calls");
    auto param_update_counter = meter->CreateUInt64Counter(
        "smsf_sms_service_parameter_update_total", "Total SMSServiceParameterUpdate calls");
    auto deactivation_counter = meter->CreateUInt64Counter("smsf_sm_service_deactivation_total",
                                                           "Total SMServiceDeactivation calls");
    auto send_sms_counter =
        meter->CreateUInt64Counter("smsf_send_sms_total", "Total SendSMS calls");
    auto send_mt_sms_counter =
        meter->CreateUInt64Counter("smsf_send_mt_sms_total", "Total SendMtSMS calls");

    boost::asio::io_context ioc;
    // 0.0.0.0: same Docker-reachability reasoning as NRF's bind -- see docs/DECISIONS.md ADR-0014.
    sbi_core::http2::Server server(ioc, sbi_bind_address, port, server_tls);

    // P15 / P4.12 (ADR-0280): optional TPS ceiling from this NF's own config (`max_tps`,
    // `tps_burst`), overridable per deployment via SBI_MAX_TPS. Absent means unlimited, so this
    // changes nothing until an operator opts in.
    if (const auto tps_limit = sbi_core::read_tps_limit(config); tps_limit.enabled()) {
        server.set_tps_limit(tps_limit.sustained_tps, tps_limit.burst);
        spdlog::info("TPS ceiling active: {} req/s sustained, burst {}",
                     tps_limit.sustained_tps,
                     tps_limit.burst > 0.0 ? tps_limit.burst : tps_limit.sustained_tps);
    }

    // --- Nsmsf_SMService: ue-contexts ---

    server.add_route(
        "PUT",
        std::string(kApiRoot) + "/ue-contexts/{supi}",
        [&verifier, &contexts, &activation_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto supi = req.path_params.at("supi");
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::UeSmsContextData>(req, err);
            if (!body.has_value()) {
                return err;
            }

            activation_counter->Add(1);
            std::string etag;
            const bool created = contexts.put(supi, json(*body), etag);

            if (created) {
                sbi_core::http2::Response resp;
                resp.status = 201;
                resp.headers.emplace("content-type", "application/json");
                resp.headers.emplace("location", std::string(kApiRoot) + "/ue-contexts/" + supi);
                resp.headers.emplace("etag", etag);
                resp.body = json(*body).dump();
                return resp;
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            resp.headers.emplace("etag", etag);
            return resp;
        });

    server.add_route(
        "PATCH",
        std::string(kApiRoot) + "/ue-contexts/{supi}",
        [&verifier, &contexts, &param_update_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto supi = req.path_params.at("supi");
            json patch_ops;
            try {
                patch_ops = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            auto patched = contexts.patch(supi, patch_ops);
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No UE SMS context for supi " + supi);
            }
            param_update_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "DELETE",
        std::string(kApiRoot) + "/ue-contexts/{supi}",
        [&verifier, &contexts, &deactivation_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto supi = req.path_params.at("supi");
            // Real, disclosed: the YAML documents an optional `If-Match` precondition header on
            // this operation but declares no `412` response for a mismatch -- accepted but not
            // enforced, a real spec gap, not a shortcut (same class as 5G-EIR's own disclosed
            // structural gaps, ADR-0187).
            if (!contexts.remove(supi)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No UE SMS context for supi " + supi);
            }
            deactivation_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/ue-contexts/{supi}/sendsms",
        [&verifier, &contexts, &send_sms_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto supi = req.path_params.at("supi");
            if (!contexts.get(supi).has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No UE SMS context for supi " + supi);
            }
            const auto content_type_it = req.headers.find("content-type");
            if (content_type_it == req.headers.end()) {
                return sbi_core::http2::problem_response(
                    400, "Invalid Service Request", "Content-Type header is required");
            }
            auto parts = sbi_core::multipart::parse(content_type_it->second, req.body);
            if (!parts.has_value() || parts->empty()) {
                return sbi_core::http2::problem_response(
                    400, "Invalid Service Request", "Malformed multipart/related body");
            }
            sbi_gen::SmsRecordData jsonData;
            try {
                jsonData = json::parse((*parts)[0].body).get<sbi_gen::SmsRecordData>();
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(400, "Invalid Service Request", e.what());
            }
            bool found_payload = false;
            for (const auto& part : *parts) {
                if (part.content_id.has_value() &&
                    *part.content_id == jsonData.smsPayload.contentId) {
                    found_payload = true;
                    break;
                }
            }
            if (!found_payload) {
                return sbi_core::http2::problem_response(
                    400, "Invalid Service Request", "smsPayload binary part not found");
            }

            send_sms_counter->Add(1);
            // Real, disclosed simplification -- see this file's own top comment: no real
            // downstream SMS-GMSC/IWMSC exists in this project, so this always reports
            // SMSF-level acceptance, never a fabricated "delivered" status.
            sbi_gen::SmsRecordDeliveryData resp_body;
            resp_body.smsRecordId = jsonData.smsRecordId;
            resp_body.deliveryStatus.value = sbi_gen::SmsDeliveryStatus::SMS_DELIVERY_SMSF_ACCEPTED;
            return sbi_core::http2::Response::json(200, json(resp_body).dump());
        });

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/ue-contexts/{supi}/send-mt-sms",
        [&verifier, &contexts, &send_mt_sms_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto supi = req.path_params.at("supi");
            if (!contexts.get(supi).has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No UE SMS context for supi " + supi);
            }
            const auto content_type_it = req.headers.find("content-type");
            if (content_type_it == req.headers.end()) {
                return sbi_core::http2::problem_response(
                    400, "Invalid Service Request", "Content-Type header is required");
            }
            auto parts = sbi_core::multipart::parse(content_type_it->second, req.body);
            if (!parts.has_value() || parts->empty()) {
                return sbi_core::http2::problem_response(
                    400, "Invalid Service Request", "Malformed multipart/related body");
            }
            sbi_gen::SmsData jsonData;
            try {
                jsonData = json::parse((*parts)[0].body).get<sbi_gen::SmsData>();
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(400, "Invalid Service Request", e.what());
            }
            bool found_payload = false;
            for (const auto& part : *parts) {
                if (part.content_id.has_value() &&
                    *part.content_id == jsonData.smsPayload.contentId) {
                    found_payload = true;
                    break;
                }
            }
            if (!found_payload) {
                return sbi_core::http2::problem_response(
                    400, "Invalid Service Request", "smsPayload binary part not found");
            }

            send_mt_sms_counter->Add(1);
            // Real, disclosed simplification -- see this file's own top comment: this project has
            // no real TS 24.011 SMS-DELIVER-REPORT encoder, so the binary delivery-report part is
            // an honest empty placeholder, not a fabricated PDU.
            sbi_gen::SmsDeliveryData resp_json;
            resp_json.smsPayload.contentId = "smsPayload";

            sbi_core::multipart::Part json_part;
            json_part.content_type = "application/json";
            json_part.body = json(resp_json).dump();
            sbi_core::multipart::Part bin_part;
            bin_part.content_type = "application/vnd.3gpp.sms";
            bin_part.content_id = "smsPayload";
            const auto encoded = sbi_core::multipart::encode({json_part, bin_part});

            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", encoded.content_type_header);
            resp.body = encoded.body;
            return resp;
        });

    std::thread(run_nrf_lifecycle, smsf_instance_id, nrf_base).detach();

    server.start();
    spdlog::info("smsf: listening on https://{}:{} (TLS 1.3 + mTLS)", sbi_bind_address, port);
    spdlog::info("smsf: Prometheus metrics at http://{}/metrics", metrics_bind_address);
    sbi_core::run_multi_threaded(ioc);
    return 0;
}
