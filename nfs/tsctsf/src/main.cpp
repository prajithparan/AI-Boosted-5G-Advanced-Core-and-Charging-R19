// nfs/tsctsf: TSCTSF -- Time Sensitive Communication and Time Synchronization Function, increment 1
// (ADR-0498): Ntsctsf_QoSandTSCAssistance for AF requested QoS.
//
// Stage 2: TS 23.502 4.15.6.6 / 4.15.6.14, TS 23.503. Stage 3: TS 29.565 V19.7.0
// (specs/3gpp/TS_29.565_j70.txt) and TS29565_Ntsctsf_QoSandTSCAssistance.yaml
// (specs/5G_APIs-REL-19, forge.3gpp.org REL-19, commit bca84b6), DTOs generated into sbi_gen.
// {apiRoot}/ntsctsf-qos-tscai/v1:
//
//   POST   /tsc-app-sessions                        _Create (5.3.2.2.2; 5.3.2.2.8 with GMEC)
//   GET    /tsc-app-sessions/{appSessionId}         read the context
//   PATCH  /tsc-app-sessions/{appSessionId}         _Update (5.3.2.3) -- flows / QoS reference
//   POST   /tsc-app-sessions/{appSessionId}/delete  _Delete (5.3.2.4)
//
// Create: the target UE is the "ueIpAddr" (IPv4) or, with the GMEC feature (bit 6), the GPSI in
// "ueId" (5.3.2.2.8: SUPI from the UDM, Nudm_SDM). The PDU session(s) come from the BSF
// (Nbsf_Management_Discovery, by IPv4 address or by SUPI + DNN + S-NSSAI); for each, an AF session
// is created at its PCF (Npcf_PolicyAuthorization_Create) with one media component carrying the
// AF's flows and QoS reference. No binding: 500 PDU_SESSION_NOT_AVAILABLE (table 6.2.7.3-1).
//
// DISCLOSED, not hidden:
//   * The spec's GMEC association is notification-driven (the PCF notifies the TSCTSF of new PDU
//     sessions, NOTE 1); this PCF does not, so the TSCTSF reads the BSF bindings that exist when
//     the request arrives (5.3.2.2.8 NOTE 2's "retrieves from the BSF the PCF binding
//     information"). PDU sessions established later are not associated. External groups
//     (externalGroupId) are refused 400; Ethernet (ueMac / ethFlowInfo) too.
//   * Not built: TSC assistance container / BAT / 5GS delay handling (tscQosReq is relayed into
//     nothing), alternative QoS, events subscription (evSubsc accepted and not acted on), sponsored
//     connectivity, notifications to the consumer, Time Synchronization and ASTI services.
//   * Contexts are kept in memory (single replica, deploy/helm/tsctsf).

#include "sbi_core/authorizer.hpp"
#include "sbi_core/http2_client.hpp"
#include "sbi_core/http2_server.hpp"
#include "sbi_core/io_context_pool.hpp"
#include "sbi_core/jwt.hpp"
#include "sbi_core/logging.hpp"
#include "sbi_core/metrics.hpp"
#include "sbi_core/oauth2_client.hpp"
#include "sbi_core/otel.hpp"
#include "sbi_core/problem_details.hpp"
#include "sbi_core/sbi_headers.hpp"
#include "sbi_core/uuid.hpp"

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "TS26510_CommonData_grp.hpp"
#include "nf_config/nf_config.hpp"

namespace {

using nlohmann::json;
namespace http2 = sbi_core::http2;

#ifndef CERTS_DIR
#error "CERTS_DIR must be defined by CMake (see nfs/tsctsf/CMakeLists.txt)"
#endif
#ifndef CONFIG_DIR
#error "CONFIG_DIR must be defined by CMake (see nfs/tsctsf/CMakeLists.txt)"
#endif

constexpr const char* kApiRoot = "/ntsctsf-qos-tscai/v1";
// Must match nfs/nrf/src/main.cpp's kNrfInstanceId exactly -- see docs/DECISIONS.md ADR-0018.
constexpr const char* kNrfInstanceId = "5ba9a927-1d31-4c8e-8a10-000000000001";
// TS 29.565 table 6.2.8-1: feature 6 = GMEC -> bit 6 of the hex string -> 0x20.
constexpr unsigned kGmecBit = 0x20;

http2::TlsConfig tsctsf_tls() {
    return http2::TlsConfig{
        .cert_path = CERTS_DIR "/tsctsf/cert.pem",
        .key_path = CERTS_DIR "/tsctsf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
}

http2::Response problem(int status,
                        const std::string& title,
                        const std::string& detail,
                        const std::optional<std::string>& cause = std::nullopt) {
    http2::Response r;
    r.status = status;
    r.headers.emplace("content-type", "application/problem+json");
    r.body = json(sbi_core::make_problem_details(status, title, detail, cause)).dump();
    return r;
}

std::optional<sbi_core::jwt::VerifyResult> check_bearer(const http2::Request& req,
                                                        sbi_core::jwt::Verifier& verifier) {
    const auto it = req.headers.find("authorization");
    if (it == req.headers.end()) {
        return std::nullopt;
    }
    constexpr std::string_view kPrefix = "Bearer ";
    if (it->second.size() <= kPrefix.size() ||
        it->second.compare(0, kPrefix.size(), kPrefix) != 0) {
        sbi_core::jwt::VerifyResult r;
        r.error = "Authorization header present but not a Bearer token";
        return r;
    }
    return verifier.verify(it->second.substr(kPrefix.size()));
}

bool has_feature(const std::optional<std::string>& supp_feat, unsigned bit) {
    if (!supp_feat || supp_feat->empty()) {
        return false;
    }
    try {
        return (std::stoul(*supp_feat, nullptr, 16) & bit) != 0;
    } catch (const std::exception&) {
        return false;
    }
}

// Npcf_PolicyAuthorization media components from the AF's flows and QoS reference (TS 29.514).
json media_components(const std::optional<std::vector<sbi_gen::FlowInfo>>& flows,
                      const std::string& qos_reference) {
    json sub_comps = json::object();
    if (flows) {
        for (const auto& f : *flows) {
            json sc{{"fNum", f.flowId}};
            if (f.flowDescriptions) {
                sc["fDescs"] = *f.flowDescriptions;
            }
            sub_comps[std::to_string(f.flowId)] = sc;
        }
    }
    json mc{{"medCompN", 1}, {"fStatus", "ENABLED"}};
    if (!qos_reference.empty()) {
        mc["qosReference"] = qos_reference;
    }
    if (!sub_comps.empty()) {
        mc["medSubComps"] = sub_comps;
    }
    return json{{"1", mc}};
}

// One AF session at one PCF.
struct AfSession {
    std::string pcf_base;
    std::string app_session_id;
};

struct Context {
    json data; // TscAppSessionContextData as created / updated
    std::vector<AfSession> af_sessions;
};

void run_nrf_lifecycle(const std::string& instance_id, const std::string& nrf_base) {
    http2::Client http_client(tsctsf_tls());
    // ADR-0499 phase 3: every client of this NF sends each API's token from here on; until the
    // NRF issues one (after NFRegister, TS 33.501 13.4.1.1.2) requests go without.
    sbi_core::install_default_token_source(
        http_client.tls(), nrf_base + "/oauth2/token", instance_id);
    constexpr int kHeartbeatSeconds = 30;
    const json profile{{"nfInstanceId", instance_id},
                       {"nfType", "TSCTSF"},
                       {"nfStatus", "REGISTERED"},
                       {"ipv4Addresses", json::array({"127.0.0.1"})},
                       {"heartBeatTimer", kHeartbeatSeconds}};
    while (true) {
        http2::ClientRequest put;
        put.method = "PUT";
        put.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + instance_id;
        put.headers.emplace("content-type", "application/json");
        put.headers.emplace(
            sbi_core::headers::kSenderTimestamp,
            sbi_core::headers::format_sender_timestamp(std::chrono::system_clock::now()));
        put.body = profile.dump();
        if (const auto r = http_client.send(put); r && (r->status == 200 || r->status == 201)) {
            spdlog::info("tsctsf: registered with NRF (HTTP {})", r->status);
            // ADR-0499: a token refused before NFRegister is asked for again now.
            sbi_core::clear_default_token_backoff();
            break;
        }
        spdlog::warn("tsctsf: NRF registration attempt failed, retrying in 5s");
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(kHeartbeatSeconds / 2));
        http2::ClientRequest patch;
        patch.method = "PATCH";
        patch.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + instance_id;
        patch.headers.emplace("content-type", "application/json-patch+json");
        patch.body =
            json::array({json{{"op", "replace"}, {"path", "/nfStatus"}, {"value", "REGISTERED"}}})
                .dump();
        if (const auto r = http_client.send(patch); !r || r->status != 200) {
            spdlog::warn("tsctsf: NRF heartbeat failed");
        }
    }
}

} // namespace

int main() {
    sbi_core::init_logging("tsctsf");
    sbi_core::init_tracing("tsctsf");

    const auto config = nf_config::load("tsctsf", CONFIG_DIR);
    const auto port = nf_config::require<unsigned short>(config, "port", "TSCTSF_PORT");
    const auto bind = nf_config::require<std::string>(config, "sbi_bind_address");
    const auto metrics_bind_address = nf_config::require<std::string>(
        config, "metrics_bind_address", "TSCTSF_METRICS_BIND_ADDRESS");
    const auto nrf_base =
        nf_config::require<std::string>(config, "nrf_base_url", "TSCTSF_NRF_BASE_URL");
    const auto udm_base =
        nf_config::require<std::string>(config, "udm_base_url", "TSCTSF_UDM_BASE_URL");
    const auto bsf_base =
        nf_config::require<std::string>(config, "bsf_base_url", "TSCTSF_BSF_BASE_URL");
    const auto self_base =
        nf_config::require<std::string>(config, "self_base_url", "TSCTSF_SELF_BASE_URL");

    sbi_core::init_metrics(metrics_bind_address);
    auto meter = sbi_core::get_meter("tsctsf");
    auto created_counter =
        meter->CreateUInt64Counter("tsctsf_tsc_app_sessions_created_total",
                                   "Individual TSC Application Session Contexts created");
    auto af_session_counter = meter->CreateUInt64Counter(
        "tsctsf_pcf_af_sessions_total", "AF sessions created at PCFs (Npcf_PolicyAuthorization)");

    const std::string instance_id = sbi_core::generate_uuid_v4();
    spdlog::info("tsctsf: starting, nfInstanceId={}", instance_id);
    sbi_core::jwt::Verifier verifier(CERTS_DIR "/nrf-jwt/public.pem", kNrfInstanceId);
    http2::Client client(tsctsf_tls());
    sbi_core::OAuth2Client bsf_oauth(
        client, nrf_base + "/oauth2/token", instance_id, "nbsf-management", "BSF");
    sbi_core::OAuth2Client pcf_oauth(
        client, nrf_base + "/oauth2/token", instance_id, "npcf-policyauthorization", "PCF");

    std::mutex mutex;
    std::map<std::string, Context> contexts;

    const auto bearer = [](sbi_core::OAuth2Client& o, http2::ClientRequest& r) {
        if (auto t = o.get_bearer_token()) {
            r.headers.emplace("authorization", "Bearer " + *t);
        }
    };

    // Nbsf_Management_Discovery: the binding(s) for an IPv4 address, or for a SUPI (+ DNN,
    // S-NSSAI). This BSF answers one binding per query.
    const auto discover = [&](const std::string& query) -> std::optional<json> {
        http2::ClientRequest r;
        r.method = "GET";
        r.url = bsf_base + "/nbsf-management/v1/pcfBindings?" + query;
        bearer(bsf_oauth, r);
        const auto resp = client.send(r);
        if (!resp || resp->status != 200) {
            return std::nullopt;
        }
        const auto b = json::parse(resp->body, nullptr, false);
        return b.is_object() ? std::optional{b} : std::nullopt;
    };

    const auto pcf_base_of = [](const json& binding) -> std::optional<std::string> {
        if (binding.contains("pcfIpEndPoints") && !binding.at("pcfIpEndPoints").empty()) {
            const auto& ep = binding.at("pcfIpEndPoints").at(0);
            if (ep.contains("ipv4Address")) {
                return "https://" + ep.at("ipv4Address").get<std::string>() + ":" +
                       std::to_string(ep.value("port", 443));
            }
        }
        if (binding.contains("pcfFqdn")) {
            return "https://" + binding.at("pcfFqdn").get<std::string>();
        }
        return std::nullopt;
    };

    const auto url_encode = [](const std::string& s) {
        std::string out;
        for (const unsigned char c : s) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                out += static_cast<char>(c);
            } else {
                char buf[4];
                std::snprintf(buf, sizeof(buf), "%%%02X", c);
                out += buf;
            }
        }
        return out;
    };

    boost::asio::io_context ioc;
    http2::Server server(ioc, bind, port, tsctsf_tls());
    // ADR-0499 phase 4: every route needs an access token for this NF (aud) and its service
    // (scope ntsctsf-qos-tscai) unless `oauth2_required` is false; the PCF notification route is
    // exempt (below).
    server.set_authorizer(std::make_shared<const sbi_core::http2::Authorizer>(
        sbi_core::http2::AuthorizerConfig{
            .required =
                nf_config::optional<bool>(config, "oauth2_required", "TSCTSF_OAUTH2_REQUIRED")
                    .value_or(true),
            .nf_type = "TSCTSF",
            .nf_instance_id = instance_id,
        },
        std::make_shared<sbi_core::jwt::Verifier>(CERTS_DIR "/nrf-jwt/public.pem",
                                                  kNrfInstanceId)));

    // ---- Ntsctsf_QoSandTSCAssistance_Create (TS 29.565 5.3.2.2.2 / 5.3.2.2.8) ------------------
    server.add_route(
        "POST", std::string(kApiRoot) + "/tsc-app-sessions", [&](const http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return problem(401, "Unauthorized", auth->error);
            }
            sbi_gen::TscAppSessionContextData body;
            try {
                body = json::parse(req.body).get<sbi_gen::TscAppSessionContextData>();
            } catch (const std::exception& e) {
                return problem(400, "Bad Request", e.what(), "INVALID_MSG_FORMAT");
            }
            // TS29565 TscAppSessionContextData: required [notifUri, afId, qosReference] -- an empty
            // reference would install an AF session with no QoS at the PCF.
            if (body.qosReference.empty() && !body.tscQosReq) {
                return problem(
                    400, "Bad Request", "qosReference is required", "MANDATORY_IE_MISSING");
            }
            if (body.externalGroupId || body.ueMac || body.ethFlowInfo || body.enEthFlowInfo) {
                return problem(
                    400,
                    "Bad Request",
                    "external groups and Ethernet PDU sessions are not supported by this "
                    "TSCTSF",
                    "OPTIONAL_IE_INCORRECT");
            }
            // The PDU session binding(s) this request targets.
            std::vector<json> bindings;
            std::optional<std::string> supi;
            if (body.ueIpAddr && body.ueIpAddr->ipv4Addr) {
                if (auto b = discover("ipv4Addr=" + url_encode(*body.ueIpAddr->ipv4Addr))) {
                    bindings.push_back(*b);
                }
            } else if (body.ueId) {
                if (!has_feature(body.suppFeat, kGmecBit)) {
                    return problem(400,
                                   "Bad Request",
                                   "ueId requires the GMEC feature (TS 29.565 5.3.2.2.8)",
                                   "MANDATORY_IE_INCORRECT");
                }
                // 5.3.2.2.8: the SUPI for the GPSI from the UDM (Nudm_SDM).
                http2::ClientRequest t;
                t.method = "GET";
                t.url = udm_base + "/nudm-sdm/v2/" + *body.ueId + "/id-translation-result";
                const auto tr = client.send(t);
                if (tr && tr->status == 200) {
                    const auto j = json::parse(tr->body, nullptr, false);
                    if (j.is_object() && j.contains("supi")) {
                        supi = j.at("supi").get<std::string>();
                    }
                }
                if (!supi) {
                    return problem(404, "Not Found", "no SUPI for " + *body.ueId + " at the UDM");
                }
                std::string q = "supi=" + url_encode(*supi);
                if (body.dnn) {
                    q += "&dnn=" + url_encode(*body.dnn);
                }
                if (body.snssai) {
                    q += "&snssai=" + url_encode(json(*body.snssai).dump());
                }
                if (auto b = discover(q)) {
                    bindings.push_back(*b);
                }
            } else {
                return problem(400,
                               "Bad Request",
                               "the target UE is required (ueIpAddr, or ueId with GMEC)",
                               "MANDATORY_IE_MISSING");
            }
            if (bindings.empty()) {
                return problem(500,
                               "Internal Server Error",
                               "no PDU session binding for the target UE at the BSF",
                               "PDU_SESSION_NOT_AVAILABLE");
            }

            const std::string id = sbi_core::generate_uuid_v4();
            Context ctx;
            ctx.data = json(body);
            for (const auto& binding : bindings) {
                const auto pcf = pcf_base_of(binding);
                if (!pcf) {
                    continue;
                }
                json asc{{"notifUri", self_base + std::string(kApiRoot) + "/pcf-notify/" + id},
                         {"suppFeat", "0"},
                         {"afAppId", body.appId.value_or(body.afId)},
                         {"medComponents", media_components(body.flowInfo, body.qosReference)}};
                if (binding.contains("ipv4Addr")) {
                    asc["ueIpv4"] = binding.at("ipv4Addr");
                }
                if (binding.contains("supi")) {
                    asc["supi"] = binding.at("supi");
                }
                if (body.ueId) {
                    asc["gpsi"] = *body.ueId;
                }
                if (binding.contains("dnn")) {
                    asc["dnn"] = binding.at("dnn");
                }
                if (binding.contains("snssai")) {
                    asc["sliceInfo"] = binding.at("snssai");
                }
                http2::ClientRequest r;
                r.method = "POST";
                r.url = *pcf + "/npcf-policyauthorization/v1/app-sessions";
                r.headers.emplace("content-type", "application/json");
                bearer(pcf_oauth, r);
                r.body = json{{"ascReqData", asc}}.dump();
                const auto resp = client.send(r);
                if (!resp || resp->status != 201) {
                    const int status = resp ? static_cast<int>(resp->status) : 0;
                    // TS 29.565 5.3.2.2.2 a/b: the PCF's rejection, mapped.
                    if (status == 403) {
                        return problem(403,
                                       "Forbidden",
                                       "the PCF rejected the service information",
                                       "REQUESTED_SERVICE_NOT_AUTHORIZED");
                    }
                    return problem(500,
                                   "Internal Server Error",
                                   "AF session creation at the PCF failed (" +
                                       std::to_string(status) + ")",
                                   "PDU_SESSION_NOT_AVAILABLE");
                }
                const auto loc = resp->headers.find("location");
                if (loc == resp->headers.end()) {
                    continue;
                }
                ctx.af_sessions.push_back(
                    AfSession{*pcf, loc->second.substr(loc->second.rfind('/') + 1)});
                af_session_counter->Add(1);
                spdlog::info(
                    "tsctsf: AF session {} at PCF {}", ctx.af_sessions.back().app_session_id, *pcf);
            }
            if (ctx.af_sessions.empty()) {
                return problem(500,
                               "Internal Server Error",
                               "no PCF could be reached for the target UE",
                               "PDU_SESSION_NOT_AVAILABLE");
            }
            {
                const std::lock_guard<std::mutex> lock(mutex);
                contexts[id] = ctx;
            }
            created_counter->Add(1);
            spdlog::info("tsctsf: TSC application session {} ({} AF session(s))",
                         id,
                         ctx.af_sessions.size());
            http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location", std::string(kApiRoot) + "/tsc-app-sessions/" + id);
            resp.body = ctx.data.dump();
            return resp;
        });

    server.add_route("GET",
                     std::string(kApiRoot) + "/tsc-app-sessions/{appSessionId}",
                     [&](const http2::Request& req) {
                         if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                             return problem(401, "Unauthorized", auth->error);
                         }
                         const std::lock_guard<std::mutex> lock(mutex);
                         const auto it = contexts.find(req.path_params.at("appSessionId"));
                         if (it == contexts.end()) {
                             return problem(404, "Not Found", "no such TSC application session");
                         }
                         return http2::Response::json(200, it->second.data.dump());
                     });

    // ---- _Update (TS 29.565 5.3.2.3): flows / QoS reference to every AF session ----------------
    server.add_route(
        "PATCH",
        std::string(kApiRoot) + "/tsc-app-sessions/{appSessionId}",
        [&](const http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return problem(401, "Unauthorized", auth->error);
            }
            const auto patch = json::parse(req.body, nullptr, false);
            if (!patch.is_object()) {
                return problem(400, "Bad Request", "not JSON", "INVALID_MSG_FORMAT");
            }
            Context ctx;
            {
                const std::lock_guard<std::mutex> lock(mutex);
                const auto it = contexts.find(req.path_params.at("appSessionId"));
                if (it == contexts.end()) {
                    return problem(404, "Not Found", "no such TSC application session");
                }
                ctx = it->second;
            }
            ctx.data.merge_patch(patch);
            std::optional<std::vector<sbi_gen::FlowInfo>> flows;
            if (ctx.data.contains("flowInfo")) {
                flows = ctx.data.at("flowInfo").get<std::vector<sbi_gen::FlowInfo>>();
            }
            const json update{
                {"medComponents",
                 media_components(flows, ctx.data.value("qosReference", std::string{}))}};
            for (const auto& s : ctx.af_sessions) {
                http2::ClientRequest r;
                r.method = "PATCH";
                r.url =
                    s.pcf_base + "/npcf-policyauthorization/v1/app-sessions/" + s.app_session_id;
                r.headers.emplace("content-type", "application/merge-patch+json");
                bearer(pcf_oauth, r);
                r.body = json{{"ascReqData", update}}.dump();
                if (const auto resp = client.send(r); !resp || resp->status >= 300) {
                    return problem(403,
                                   "Forbidden",
                                   "the PCF rejected the update",
                                   "REQUESTED_SERVICE_NOT_AUTHORIZED");
                }
            }
            {
                const std::lock_guard<std::mutex> lock(mutex);
                contexts[req.path_params.at("appSessionId")] = ctx;
            }
            return http2::Response::json(200, ctx.data.dump());
        });

    // ---- _Delete (TS 29.565 5.3.2.4) -----------------------------------------------------------
    server.add_route("POST",
                     std::string(kApiRoot) + "/tsc-app-sessions/{appSessionId}/delete",
                     [&](const http2::Request& req) {
                         if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                             return problem(401, "Unauthorized", auth->error);
                         }
                         Context ctx;
                         {
                             const std::lock_guard<std::mutex> lock(mutex);
                             const auto it = contexts.find(req.path_params.at("appSessionId"));
                             if (it == contexts.end()) {
                                 return problem(
                                     404, "Not Found", "no such TSC application session");
                             }
                             ctx = it->second;
                             contexts.erase(it);
                         }
                         for (const auto& s : ctx.af_sessions) {
                             http2::ClientRequest r;
                             r.method = "POST";
                             r.url = s.pcf_base + "/npcf-policyauthorization/v1/app-sessions/" +
                                     s.app_session_id + "/delete";
                             r.headers.emplace("content-type", "application/json");
                             bearer(pcf_oauth, r);
                             r.body = "{}";
                             (void)client.send(r);
                         }
                         http2::Response r;
                         r.status = 204;
                         return r;
                     });

    // Npcf_PolicyAuthorization_Notify target given to the PCFs (accepted; events not relayed).
    // TS 33.501 13.4.1.0: OAuth 2.0 does not apply to the notification operation (ADR-0499).
    server.add_route(
        "POST",
        std::string(kApiRoot) + "/pcf-notify/**",
        [](const http2::Request&) {
            http2::Response r;
            r.status = 204;
            return r;
        },
        http2::RouteAuth::None);

    std::thread(run_nrf_lifecycle, instance_id, nrf_base).detach();
    server.start();
    spdlog::info("tsctsf: listening on https://{}:{} (TLS 1.3 + mTLS)", bind, port);
    sbi_core::run_multi_threaded(ioc);
    return 0;
}
