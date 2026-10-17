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
// `SendSMS` is the stage-3 form of stage-2 Nsmsf_SMService_UplinkSMS (the YAML's own SmsRecordData
// description). Since ADR-0486 it implements TS 23.502 clause 4.13.3.3's MO SMS over NAS at the
// SMSF: the binary part is decoded as TS 24.011 CP/RP and TS 23.040 TPDU (libs/sms-core,
// sms_procedures.hpp), and the CP-ACK (step 2c) and the RP reply (steps 6a-6b) go back to the UE
// via Namf_Communication_N1N2MessageTransfer (n1n2_sender.hpp). The SMSF's LI IRI-POI (li_poi.hpp,
// TS 33.128 clause 6.2.5.3) reports SMSMessage and SMSReport for a target UE.
//
// `SendMtSMS` is the stage-3 form of Nsmsf_SMService_MtForwardSm (TS 23.540 clause 6.4.2.2). Since
// ADR-0486 increment 4 it implements TS 23.502 clauses 4.13.3.6-4.13.3.7 (MT SMS over NAS, SMSF
// side): Namf_MT_EnableUEReachability, the CP-DATA(RP-DATA) to the UE via N1N2MessageTransfer, the
// UE's CP-ACK and delivery report back through UplinkSMS, the closing CP-ACK, and the delivery
// report in the 200 response (mt_sms.hpp, which also lists its disclosed interpretations).
//
// Real, disclosed limits: this project has no SMS-IWMSC/SMS-GMSC, so an SMS-SUBMIT is answered with
// an RP-ERROR + SMS-SUBMIT-REPORT (configurable causes, config/smsf.json `mo_sms`), never an RP-ACK
// that would claim a Service Centre took it; `SendSMS` still answers SMS_DELIVERY_SMSF_ACCEPTED
// ("accepted for further processing", not "delivered"). Nothing in this project calls `SendMtSMS`
// except tests: there is no SMS-GMSC (and no Nudm_UECM_SendRoutingInfoForSM consumer). The AMF
// address is configuration (`amf_base_url`), not UeSmsContextData.amfId. The SMS management
// subscription data check of TS 23.502 clause 4.13.3.6 step 4 is not made.

#include "sbi_core/authorizer.hpp"
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
#include "sbi_core/problem_details.hpp"
#include "sbi_core/rate_limit.hpp"
#include "sbi_core/sbi_headers.hpp"
#include "sbi_core/uuid.hpp"

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "TS29518_Namf_MT.hpp"
#include "TS29540_Nsmsf_SMService.hpp"
#include "TS29577_Nipsmgw_SMService.hpp"
#include "li_poi.hpp"
#include "mt_sms.hpp"
#include "n1n2_sender.hpp"
#include "nf_config/nf_config.hpp"
#include "sms_procedures.hpp"
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

// ProblemDetails with an application error `cause` (TS 29.540 table 6.1.7.3-1, TS 29.500 table
// 5.2.7.2-1).
sbi_core::http2::Response problem_with_cause(int status,
                                             const std::string& title,
                                             const std::string& detail,
                                             const std::string& cause) {
    sbi_core::http2::Response r;
    r.status = status;
    r.headers.emplace("content-type", "application/problem+json");
    r.body = json(sbi_core::make_problem_details(status, title, detail, cause)).dump();
    return r;
}

// LI identities for a UE: the SUPI, and pei/gpsi from the request if it has them, else from the
// UE SMS context the AMF activated.
smsf::LiIdentities li_identities(const std::string& supi,
                                 const json& ue_context,
                                 std::optional<std::string> pei,
                                 std::optional<std::string> gpsi) {
    const auto ctx_str = [&](const char* key) -> std::optional<std::string> {
        if (ue_context.contains(key) && ue_context.at(key).is_string()) {
            return ue_context.at(key).get<std::string>();
        }
        return std::nullopt;
    };
    smsf::LiIdentities ids;
    ids.supi = supi;
    ids.pei = pei ? pei : ctx_str("pei");
    ids.gpsi = gpsi ? gpsi : ctx_str("gpsi");
    return ids;
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

    // ADR-0499 phase 3: every client of this NF sends each API's token from here on; until the
    // NRF issues one (after NFRegister, TS 33.501 13.4.1.1.2) requests go without.
    sbi_core::install_default_token_source(
        http_client.tls(), nrf_base + "/oauth2/token", smsf_instance_id);

    constexpr int kHeartbeatSeconds = 30;
    json profile{
        {"nfInstanceId", smsf_instance_id},
        {"nfType", kNfType},
        {"nfStatus", "REGISTERED"},
        {"ipv4Addresses", json::array({"127.0.0.1"})},
        {"heartBeatTimer", kHeartbeatSeconds},
    };

    while (true) {
        sbi_core::http2::ClientRequest put_req;
        put_req.method = "PUT";
        put_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + smsf_instance_id;
        put_req.headers.emplace("content-type", "application/json");
        put_req.headers.emplace(
            sbi_core::headers::kSenderTimestamp,
            sbi_core::headers::format_sender_timestamp(std::chrono::system_clock::now()));
        put_req.body = profile.dump();
        auto put_resp = http_client.send(put_req);
        if (put_resp.has_value() && (put_resp->status == 200 || put_resp->status == 201)) {
            spdlog::info("smsf: registered with NRF (HTTP {})", put_resp->status);
            // ADR-0499: a token refused before NFRegister is asked for again now.
            sbi_core::clear_default_token_backoff();
            break;
        }
        spdlog::warn("smsf: NRF registration attempt failed, retrying in 5s");
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(kHeartbeatSeconds / 2));

        sbi_core::http2::ClientRequest patch_req;
        patch_req.method = "PATCH";
        patch_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + smsf_instance_id;
        patch_req.headers.emplace("content-type", "application/json-patch+json");
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

    // ADR-0486: MO SMS over NAS. RP-Cause / TP-FCS for the "no SMS-IWMSC" reply are configuration
    // (sms_procedures.hpp explains why the reply is an RP-ERROR, never an RP-ACK).
    smsf::MoSmsPolicy mo_sms_policy;
    if (config.contains("mo_sms")) {
        const auto& mo = config.at("mo_sms");
        mo_sms_policy.no_iwmsc_rp_cause =
            mo.value("no_iwmsc_rp_cause", mo_sms_policy.no_iwmsc_rp_cause);
        mo_sms_policy.no_iwmsc_tp_failure_cause =
            mo.value("no_iwmsc_tp_failure_cause", mo_sms_policy.no_iwmsc_tp_failure_cause);
    }
    // ADR-0486 increment 4: MT SMS (mt_sms.hpp). TR1N and the in-flight cap are configuration.
    smsf::MtSmsPolicy mt_sms_policy;
    if (config.contains("mt_sms")) {
        const auto& mt = config.at("mt_sms");
        mt_sms_policy.tr1n = std::chrono::milliseconds(
            static_cast<long long>(mt.value("tr1n_seconds", 40.0) * 1000.0));
        mt_sms_policy.max_concurrent = mt.value("max_concurrent", mt_sms_policy.max_concurrent);
    }
    smsf::MtTransactions mt_transactions(mt_sms_policy);
    // LI IRI-POI (ADR-0486), disabled unless config/smsf.json's li_poi.enabled is true; nullptr
    // keeps the SMSF's behaviour unchanged, the convention of every POI in this project.
    std::unique_ptr<smsf::SmsfLiPoi> li_poi;
    if (config.contains("li_poi") && config.at("li_poi").value("enabled", false)) {
        const auto& lp = config.at("li_poi");
        li_poi::Config poi_cfg;
        poi_cfg.x1_bind_address = lp.at("x1_bind_address").get<std::string>();
        poi_cfg.x1_port = lp.at("x1_port").get<std::uint16_t>();
        poi_cfg.ne_identifier = lp.value("ne_identifier", std::string{"smsf-poi"});
        poi_cfg.network_function_id = lp.value("network_function_id", smsf_instance_id);
        poi_cfg.interception_point_id =
            lp.value("interception_point_id", std::string{"SMSF-IRI-POI-1"});
        poi_cfg.mdf2_host = lp.at("mdf2_host").get<std::string>();
        poi_cfg.mdf2_port = lp.at("mdf2_port").get<std::uint16_t>();
        poi_cfg.mdf2_sni = lp.value("mdf2_sni", std::string{});
        poi_cfg.cert_path = CERTS_DIR "/smsf/cert.pem";
        poi_cfg.key_path = CERTS_DIR "/smsf/key.pem";
        poi_cfg.ca_path = CERTS_DIR "/ca/ca.crt";
        poi_cfg.x1_keepalive_p1_seconds = lp.value("x1_keepalive_p1_seconds", 60);
        poi_cfg.x1_keepalive_p2_seconds = lp.value("x1_keepalive_p2_seconds", 180);
        poi_cfg.x1_keepalive_p3_seconds = lp.value("x1_keepalive_p3_seconds", 300);
        poi_cfg.x1_allow_deactivate_all = lp.value("x1_allow_deactivate_all", true);
        li_poi = std::make_unique<smsf::SmsfLiPoi>(std::move(poi_cfg));
        li_poi->start();
    }

    // Declared after li_poi so its worker (which may call li_poi) is joined first on teardown.
    const auto amf_base_url =
        nf_config::require<std::string>(config, "amf_base_url", "SMSF_AMF_BASE_URL");
    smsf::N1n2Sender n1n2_sender(amf_base_url,
                                 nrf_base,
                                 smsf_instance_id,
                                 sbi_core::http2::TlsConfig{
                                     .cert_path = CERTS_DIR "/smsf/cert.pem",
                                     .key_path = CERTS_DIR "/smsf/key.pem",
                                     .ca_path = CERTS_DIR "/ca/ca.crt",
                                 });

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
    // ADR-0499 phase 4: every route needs an access token for this NF (aud) and its service
    // (scope nsmsf-sms) unless `oauth2_required` is false -- the SMSF serves no notification route.
    server.set_authorizer(std::make_shared<const sbi_core::http2::Authorizer>(
        sbi_core::http2::AuthorizerConfig{
            .required = nf_config::optional<bool>(config, "oauth2_required", "SMSF_OAUTH2_REQUIRED")
                            .value_or(true),
            .nf_type = kNfType,
            .nf_instance_id = smsf_instance_id,
        },
        std::make_shared<sbi_core::jwt::Verifier>(CERTS_DIR "/nrf-jwt/public.pem",
                                                  kNrfInstanceId)));

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
        [&verifier,
         &contexts,
         &send_sms_counter,
         &mo_sms_policy,
         &mt_transactions,
         &n1n2_sender,
         &li_poi](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto supi = req.path_params.at("supi");
            const auto ue_context = contexts.get(supi);
            if (!ue_context.has_value()) {
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
            const sbi_core::multipart::Part* payload_part = nullptr;
            for (const auto& part : *parts) {
                if (part.content_id.has_value() &&
                    *part.content_id == jsonData.smsPayload.contentId) {
                    payload_part = &part;
                    break;
                }
            }
            if (payload_part == nullptr) {
                return sbi_core::http2::problem_response(
                    400, "Invalid Service Request", "smsPayload binary part not found");
            }

            // ADR-0486: TS 24.011 CP/RP and TS 23.040 TPDU handling (sms_procedures.hpp).
            const std::vector<std::uint8_t> payload(payload_part->body.begin(),
                                                    payload_part->body.end());
            auto outcome =
                smsf::process_uplink_sms(payload, mo_sms_policy, std::chrono::system_clock::now());
            if (!outcome) {
                return sbi_core::http2::problem_response(400,
                                                         "Invalid Service Request",
                                                         "smsPayload is not an SM-CP message: " +
                                                             outcome.error());
            }
            if (outcome->rp_error) {
                spdlog::warn("smsf: CP-DATA from {} acknowledged, {}", supi, *outcome->rp_error);
            }
            send_sms_counter->Add(1);

            // TS 33.128 6.2.5.3: SMSMessage on CP-DATA_RP-DATA [SMS-SUBMIT, SMS-COMMAND] from a
            // target UE. Identities: the request's own pei/gpsi, else the UE SMS context's.
            std::optional<json> location;
            if (jsonData.ueLocation) {
                location = json(*jsonData.ueLocation);
            }
            std::vector<li_poi::Match> li_matches;
            if (li_poi && outcome->rp && outcome->tpdu) {
                const auto ids = li_identities(
                    supi,
                    *ue_context,
                    jsonData.pei
                        ? std::optional<std::string>(json(*jsonData.pei).get<std::string>())
                        : std::nullopt,
                    jsonData.gpsi
                        ? std::optional<std::string>(json(*jsonData.gpsi).get<std::string>())
                        : std::nullopt);
                li_matches =
                    li_poi->report_mo_sms_message(ids, *outcome->rp, *outcome->tpdu, location);
            }

            // TS 23.502 4.13.3.6 steps 5c-6b: the UE's CP-ACK / delivery report for an MT SMS
            // (TI flag 1 -- a network-originated TI). The delivery report's CP-ACK is the last
            // message of the transfer (step 6c).
            bool mt_delivery_report = false;
            if (outcome->cp.ti_flag &&
                mt_transactions.on_uplink(
                    supi, outcome->cp, outcome->rp, location ? location->dump() : std::string{})) {
                mt_delivery_report = outcome->cp.type == sms_core::CpMessageType::Data;
            }

            // TS 23.502 4.13.3.3 steps 2c and 6a: CP-ACK, then the RP reply, to the UE via the AMF.
            if (!outcome->downlink.empty()) {
                smsf::N1n2Sender::Job job;
                job.supi = supi;
                job.payloads = std::move(outcome->downlink);
                job.last_is_final = outcome->rp_reply.has_value() || mt_delivery_report;
                if (outcome->rp_reply && !li_matches.empty()) {
                    const std::size_t reply_index = job.payloads.size() - 1;
                    job.on_sent = [&li_poi,
                                   matches = std::move(li_matches),
                                   reply = *outcome->rp_reply,
                                   reply_index](std::size_t index, bool accepted) {
                        // SMSReport: "the SMSF sends ... CP-DATA_RP-ERROR [SMS-SUBMIT-REPORT]"
                        // -- reported once the AMF has taken it, not before.
                        if (index == reply_index && accepted && li_poi) {
                            li_poi->report_sms_report(matches, reply);
                        }
                    };
                }
                n1n2_sender.enqueue(std::move(job));
            }

            // SMSF_ACCEPTED: the SMSF took the message for further processing -- never a
            // fabricated "delivered" status (there is no SMS-IWMSC; the UE gets the RP-ERROR).
            sbi_gen::SmsRecordDeliveryData resp_body;
            resp_body.smsRecordId = jsonData.smsRecordId;
            resp_body.deliveryStatus.value = sbi_gen::SmsDeliveryStatus::SMS_DELIVERY_SMSF_ACCEPTED;
            return sbi_core::http2::Response::json(200, json(resp_body).dump());
        });

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/ue-contexts/{supi}/send-mt-sms",
        [&verifier, &contexts, &send_mt_sms_counter, &mt_transactions, &n1n2_sender, &li_poi](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto supi = req.path_params.at("supi");
            const auto ue_context = contexts.get(supi);
            if (!ue_context.has_value()) {
                return problem_with_cause(
                    404, "Not Found", "No UE SMS context for supi " + supi, "CONTEXT_NOT_FOUND");
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
            const sbi_core::multipart::Part* payload_part = nullptr;
            for (const auto& part : *parts) {
                if (part.content_id.has_value() &&
                    *part.content_id == jsonData.smsPayload.contentId) {
                    payload_part = &part;
                    break;
                }
            }
            if (payload_part == nullptr) {
                return problem_with_cause(400,
                                          "Invalid Service Request",
                                          "smsPayload binary part not found",
                                          "SMS_PAYLOAD_MISSING");
            }
            const std::vector<std::uint8_t> payload(payload_part->body.begin(),
                                                    payload_part->body.end());
            auto request = smsf::parse_mt_forward_payload(payload);
            if (!request) {
                return problem_with_cause(
                    400, "Invalid Service Request", request.error(), "SMS_PAYLOAD_ERROR");
            }
            send_mt_sms_counter->Add(1);

            const auto ids = li_identities(supi, *ue_context, std::nullopt, std::nullopt);
            // TS 33.128 6.2.5.3: an SMSMessage whenever delivery to a target UE was attempted,
            // with linkTransferStatus per NOTE 5.
            const auto report_failed_attempt = [&] {
                if (li_poi) {
                    li_poi->report_mt_sms_message(ids, request->rp, request->tpdu, false);
                }
            };
            // TS 29.540 table 6.1.7.3-1 has no application error for an undeliverable MT SMS
            // (TS 23.540 clause 5.1.5 step 7 only says "with error cause"), so these failures are
            // a 504 ProblemDetails WITHOUT a cause -- disclosed, never an invented cause value.
            const auto not_delivered = [&](const std::string& why) {
                spdlog::warn("smsf: MT SMS for {} not delivered: {}", supi, why);
                report_failed_attempt();
                return sbi_core::http2::problem_response(
                    504, "MT SMS not delivered", "MT SMS for " + supi + " not delivered: " + why);
            };

            // Step 4: Namf_MT_EnableUEReachability.
            const auto reachability = n1n2_sender.enable_ue_reachability(supi);
            if (!reachability) {
                return not_delivered("Namf_MT_EnableUEReachability failed: " +
                                     reachability.error());
            }
            if (*reachability != sbi_gen::UeReachability::REACHABLE) {
                return not_delivered("the AMF reports the UE " + *reachability);
            }

            // Steps 5a-6b: CP-DATA to the UE; wait for its delivery report (TR1N).
            auto outcome = mt_transactions.run(supi, [&](std::uint8_t ti) {
                smsf::N1n2Sender::Job job;
                job.supi = supi;
                job.payloads.push_back(smsf::make_mt_cp_data(*request, ti));
                job.on_sent = [&mt_transactions, supi, ti](std::size_t, bool accepted) {
                    mt_transactions.downlink_result(supi, ti, accepted);
                };
                n1n2_sender.enqueue(std::move(job));
            });
            if (!outcome) {
                switch (outcome.error()) {
                    case smsf::MtFailure::Congestion:
                        return problem_with_cause(503,
                                                  "Service Unavailable",
                                                  "MT SMS transactions in flight at the limit",
                                                  "NF_CONGESTION");
                    case smsf::MtFailure::NotDelivered:
                        return not_delivered("the AMF did not accept the downlink CP-DATA");
                    case smsf::MtFailure::CpError:
                        return not_delivered("the UE answered CP-ERROR");
                    case smsf::MtFailure::Timeout:
                        return not_delivered("TR1N expired without a delivery report");
                }
                return not_delivered("unrecognised MT failure");
            }

            // Step 7: the delivery report (RP-ACK or RP-ERROR from the UE) in the 200 response.
            if (li_poi) {
                const auto matches =
                    li_poi->report_mt_sms_message(ids, request->rp, request->tpdu, true);
                std::optional<json> location;
                if (!outcome->ue_location_json.empty()) {
                    location = json::parse(outcome->ue_location_json, nullptr, false);
                }
                li_poi->report_sms_report(matches, outcome->rp, location);
            }
            sbi_gen::SmsDeliveryData resp_json;
            resp_json.smsPayload.contentId = "smsPayload";
            sbi_core::multipart::Part json_part;
            json_part.content_type = "application/json";
            json_part.body = json(resp_json).dump();
            sbi_core::multipart::Part bin_part;
            bin_part.content_type = "application/vnd.3gpp.sms";
            bin_part.content_id = "smsPayload";
            const auto report = smsf::make_delivery_report_payload(*request, outcome->rpdu);
            bin_part.body.assign(report.begin(), report.end());
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
