// nfs/bsf: BSF (Binding Support Function), Nbsf_Management service. Source:
// specs/5G_APIs-REL-19/TS29521_Nbsf_Management.yaml (v1.5.0), commit
// bca84b60a37773133bcae97e5c6c0d10a93b47b6. This project's tenth NF -- second of the whole-new-NF
// gap-closure moves off UDR's exhausted task #106 backlog (ADR-0182), user-directed continuous
// move-to-the-next-NF pattern (no per-NF confirmation needed, see docs/DECISIONS.md ADR-0184).

// In scope, agreed with the user before implementation (docs/CAPABILITY_GAP_ANALYSIS.md's own
// "still not done" nssf/nef/scp/bsf list, BSF chosen as the cleanest-scoped of the three
// remaining) -- all 15 real operations across Nbsf_Management's 4 resource families:
//   POST   {apiRoot}/pcfBindings                          CreatePCFBinding
//   GET    {apiRoot}/pcfBindings                          GetPCFBindings
//   DELETE {apiRoot}/pcfBindings/{bindingId}               DeleteIndPCFBinding
//   PATCH  {apiRoot}/pcfBindings/{bindingId}               UpdateIndPCFBinding
//   POST   {apiRoot}/subscriptions                        CreateIndividualSubcription
//   PUT    {apiRoot}/subscriptions/{subId}                ReplaceIndividualSubscription
//   DELETE {apiRoot}/subscriptions/{subId}                DeleteIndividualSubscription
//   POST   {apiRoot}/pcf-ue-bindings                       CreatePCFforUEBinding
//   GET    {apiRoot}/pcf-ue-bindings                       GetPCFForUeBindings
//   DELETE {apiRoot}/pcf-ue-bindings/{bindingId}           DeleteIndPCFforUEBinding
//   PATCH  {apiRoot}/pcf-ue-bindings/{bindingId}           UpdateIndPCFforUEBinding
//   POST   {apiRoot}/pcf-mbs-bindings                      CreatePCFMbsBinding
//   GET    {apiRoot}/pcf-mbs-bindings                      GetPCFMbsBinding
//   PATCH  {apiRoot}/pcf-mbs-bindings/{bindingId}          ModifyIndPCFMbsBinding
//   DELETE {apiRoot}/pcf-mbs-bindings/{bindingId}          DeleteIndPCFMbsBinding
// Plus the real `myNotification` callback CreateIndividualSubcription declares
// (`{$request.body#/notifUri}`), delivering `BsfNotification` on binding create/delete.

// Real, disclosed simplifications -- stated up front, not discovered in review:
// 1. `CreatePCFBinding`/`CreatePCFMbsBinding`'s real duplicate-combination check (a second create
//    for the same supi+dnn+snssai, or the same mbsSessionId, must return 403 with the EXISTING
//    binding's own `pcfSmFqdn`/`pcfSmIpEndPoints` (or `pcfFqdn`/`pcfIpEndPoints` for MBS) rather
//    than create a second binding) is implemented exactly as the spec documents it -- this is the
//    one piece of real BSF business logic beyond plain CRUD, and it is NOT a simplification.
// 2. Real `BsfEvent` notification coverage: `PCF_PDU_SESSION_BINDING_REGISTRATION`/
//    `_DEREGISTRATION` and `SNSSAI_DNN_BINDING_REGISTRATION`/`_DEREGISTRATION` fire together on
//    every successful `CreatePCFBinding`/`DeleteIndPCFBinding` -- real, not a simplification,
//    because the spec's own combination-uniqueness rule (simplification 1 above) means creating a
//    NEW binding for a given supi+dnn+snssai combination always IS "the first PDU session for
//    that DNN+S-NSSAI", so both event types describe the same real occurrence, not two different
//    conditions this project would need separate tracking for. `PCF_UE_BINDING_REGISTRATION`/
//    `_DEREGISTRATION` fire on `CreatePCFforUEBinding`/`DeleteIndPCFforUEBinding`. No event fires
//    for `pcf-mbs-bindings` create/delete: `BsfEvent`'s own real enum (confirmed by direct read of
//    the YAML) has no MBS-shaped event value -- a real, disclosed absence in the spec itself, not
//    an omission this project introduces.
// 3. `PATCH` on all three binding families uses RFC 7396 merge-patch (the real
//    `application/merge-patch+json` content-type the spec declares for these three operations,
//    different from other NFs' RFC 6902 `application/json-patch+json` PATCH bodies) via
//    `nlohmann::json::merge_patch()`.
// 4. `CreateIndividualSubcription`'s own response never proactively computes "already-met events"
//    (the real `BsfSubscriptionResp`'s own doc: "may contain the notification of the already met
//    events") -- this project always echoes the created `BsfSubscription` back, a real, disclosed
//    narrower scope than the spec's optional immediate-notification path.
// 5. `GetPCFForUeBindings` enforces at most one `PcfForUeBinding` per real `supi` (no documented
//    multi-binding-per-UE case exists in the spec, unlike per-PDU-session bindings, which are
//    genuinely one-per-combination by the same uniqueness rule) -- `CreatePCFforUEBinding` reuses
//    the same 403-duplicate pattern as `pcfBindings` for this reason, even though the spec's own
//    `CreatePCFforUEBinding` operation doesn't document a `403` response the way `CreatePCFBinding`
//    does. This is a real, disclosed project-chosen invariant, not a literal spec requirement.

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

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "TS29521_Nbsf_Management.hpp"
#include "nf_config/nf_config.hpp"
#include "stores.hpp"

namespace {

using nlohmann::json;

#ifndef CERTS_DIR
#error "CERTS_DIR must be defined by CMake (see nfs/bsf/CMakeLists.txt)"
#endif
#ifndef CONFIG_DIR
#error "CONFIG_DIR must be defined by CMake (see nfs/bsf/CMakeLists.txt)"
#endif

constexpr const char* kNfType = "BSF";
constexpr const char* kApiRoot = "/nbsf-management/v1";

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

// A query parameter declared `content: application/json` in the YAML carries a JSON-encoded
// value in the query string -- same helper/reasoning as nfs/nssf's own get_json_query_param.
template <typename T>
std::optional<T> get_json_query_param(const sbi_core::http2::Request& req,
                                      const std::string& name) {
    auto it = req.query_params.find(name);
    if (it == req.query_params.end()) {
        return std::nullopt;
    }
    try {
        // Two-step construction (not a direct `return ...get<T>();`): when T is itself
        // nlohmann::json, the direct form is ambiguous between constructing a T and constructing
        // an optional<T> (real -Wconversion warning, not a hypothetical one -- caught by this
        // project's own zero-warnings build discipline). Binding to a named T first resolves it.
        T value = nlohmann::json::parse(it->second).get<T>();
        return std::optional<T>(std::move(value));
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

std::optional<std::string> get_string_query_param(const sbi_core::http2::Request& req,
                                                  const std::string& name) {
    auto it = req.query_params.find(name);
    if (it == req.query_params.end()) {
        return std::nullopt;
    }
    return it->second;
}

// Real GetPCFBindings filter: every provided query parameter must match the stored PcfBinding's
// own field exactly. All filters are optional; a GetPCFBindings call with none of them would
// match the first stored binding found (a real, disclosed corner case the spec itself doesn't
// forbid -- GetPCFBindings never marks any filter as `required`).
bool matches_pcf_binding_filter(const json& binding, const sbi_core::http2::Request& req) {
    if (auto v = get_string_query_param(req, "ipv4Addr");
        v.has_value() && binding.value("ipv4Addr", "") != *v) {
        return false;
    }
    if (auto v = get_string_query_param(req, "ipv6Prefix");
        v.has_value() && binding.value("ipv6Prefix", "") != *v) {
        return false;
    }
    if (auto v = get_string_query_param(req, "macAddr48");
        v.has_value() && binding.value("macAddr48", "") != *v) {
        return false;
    }
    if (auto v = get_string_query_param(req, "dnn");
        v.has_value() && binding.value("dnn", "") != *v) {
        return false;
    }
    if (auto v = get_string_query_param(req, "supi");
        v.has_value() && binding.value("supi", "") != *v) {
        return false;
    }
    if (auto v = get_string_query_param(req, "gpsi");
        v.has_value() && binding.value("gpsi", "") != *v) {
        return false;
    }
    if (auto v = get_string_query_param(req, "ipDomain");
        v.has_value() && binding.value("ipDomain", "") != *v) {
        return false;
    }
    if (auto v = get_json_query_param<json>(req, "snssai");
        v.has_value() && binding.value("snssai", json::object()) != *v) {
        return false;
    }
    return true;
}

// Real `myNotification` delivery (the callback CreateIndividualSubcription's own POST declares:
// `{$request.body#/notifUri}`). Same synchronous, best-effort, non-blocking-to-the-caller
// precedent as nfs/udr's onDataChange (ADR-0171) and nfs/nssf's nssaiAvailabilityNotification. A
// subscription matches a fired event when: the event type is in its own `events` list, its own
// `supi` equals the binding's `supi`, and -- for PDU-session/SNSSAI-DNN events only -- if the
// subscription named specific `snssaiDnnPairs`/`addSnssaiDnnPairs`, the fired dnn+snssai pair is
// one of them (no snssaiDnnPairs at all means "every DNN/S-NSSAI for this supi", a real, spec-
// documented optional field, not a fabricated wildcard).
void deliver_bsf_notification(sbi_core::http2::Client& notify_client,
                              bsf::BsfSubscriptionStore& subs,
                              const std::string& event,
                              const std::string& supi,
                              const std::optional<std::string>& dnn,
                              const json& snssai_or_null) {
    for (const auto& [sub_id, sub_json] : subs.list_all()) {
        sbi_gen::BsfSubscription sub;
        try {
            sub = sub_json.get<sbi_gen::BsfSubscription>();
        } catch (const nlohmann::json::exception&) {
            continue;
        }
        if (sub.supi != supi) {
            continue;
        }
        bool event_wanted = std::any_of(
            sub.events.begin(), sub.events.end(), [&](const auto& e) { return e.value == event; });
        if (!event_wanted) {
            continue;
        }
        if (dnn.has_value()) {
            std::vector<sbi_gen::SnssaiDnnPair> pairs;
            if (sub.snssaiDnnPairs.has_value()) {
                pairs.push_back(*sub.snssaiDnnPairs);
            }
            if (sub.addSnssaiDnnPairs.has_value()) {
                for (const auto& p : *sub.addSnssaiDnnPairs) {
                    pairs.push_back(p);
                }
            }
            if (!pairs.empty()) {
                const bool matched = std::any_of(pairs.begin(), pairs.end(), [&](const auto& p) {
                    return p.dnn == *dnn && json(p.snssai) == snssai_or_null;
                });
                if (!matched) {
                    continue;
                }
            }
        }

        sbi_gen::BsfEventNotification event_notif;
        event_notif.event.value = event;
        if (dnn.has_value()) {
            sbi_gen::PcfForPduSessionInfo info;
            info.dnn = *dnn;
            info.snssai = snssai_or_null.get<sbi_gen::Snssai>();
            event_notif.pcfForPduSessInfos = std::vector<sbi_gen::PcfForPduSessionInfo>{info};
        }

        sbi_gen::BsfNotification notif;
        notif.notifCorreId = sub.notifCorreId;
        notif.eventNotifs = std::vector<sbi_gen::BsfEventNotification>{event_notif};

        sbi_core::http2::ClientRequest req;
        req.method = "POST";
        req.url = sub.notifUri;
        req.headers.emplace("content-type", "application/json");
        req.body = json(notif).dump();
        if (auto resp = notify_client.send(req); !resp.has_value() || resp->status != 204) {
            spdlog::warn(
                "bsf: myNotification delivery to {} failed or non-204 (subId={}, event={})",
                req.url,
                sub_id,
                event);
        }
    }
}

// Runs on a dedicated thread, never on the server's io_context -- same reasoning as
// nfs/ausf/src/main.cpp's run_nrf_lifecycle (docs/DECISIONS.md ADR-0006/ADR-0019).
void run_nrf_lifecycle(const std::string& bsf_instance_id, const std::string& nrf_base) {
    sbi_core::http2::TlsConfig client_tls{
        .cert_path = CERTS_DIR "/bsf/cert.pem",
        .key_path = CERTS_DIR "/bsf/key.pem",
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
        http_client, nrf_base + "/oauth2/token", bsf_instance_id, "nnrf-nfm", "NRF");

    constexpr int kHeartbeatSeconds = 30;
    json profile{
        {"nfInstanceId", bsf_instance_id},
        {"nfType", kNfType},
        {"nfStatus", "REGISTERED"},
        {"ipv4Addresses", json::array({"127.0.0.1"})},
        {"heartBeatTimer", kHeartbeatSeconds},
    };

    while (true) {
        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("bsf: OAuth2 token fetch failed: {}", token.error());
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        sbi_core::http2::ClientRequest put_req;
        put_req.method = "PUT";
        put_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + bsf_instance_id;
        put_req.headers.emplace("content-type", "application/json");
        put_req.headers.emplace("authorization", "Bearer " + *token);
        put_req.headers.emplace(
            sbi_core::headers::kSenderTimestamp,
            sbi_core::headers::format_sender_timestamp(std::chrono::system_clock::now()));
        put_req.body = profile.dump();
        auto put_resp = http_client.send(put_req);
        if (put_resp.has_value() && (put_resp->status == 200 || put_resp->status == 201)) {
            spdlog::info("bsf: registered with NRF (HTTP {})", put_resp->status);
            break;
        }
        spdlog::warn("bsf: NRF registration attempt failed, retrying in 5s");
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(kHeartbeatSeconds / 2));

        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("bsf: OAuth2 token fetch failed for heartbeat: {}", token.error());
            continue;
        }

        sbi_core::http2::ClientRequest patch_req;
        patch_req.method = "PATCH";
        patch_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + bsf_instance_id;
        patch_req.headers.emplace("content-type", "application/json-patch+json");
        patch_req.headers.emplace("authorization", "Bearer " + *token);
        patch_req.body =
            json::array({json{{"op", "replace"}, {"path", "/nfStatus"}, {"value", "REGISTERED"}}})
                .dump();

        auto patch_resp = http_client.send(patch_req);
        if (!patch_resp.has_value() || patch_resp->status != 200) {
            spdlog::warn("bsf: heartbeat failed");
        }
    }
}

} // namespace

int main() {
    sbi_core::init_logging("bsf");
    sbi_core::init_tracing("bsf");

    // ADR-0077 (user-directed, mandatory, project-wide): no DB URL/connection/deployment
    // parameter may be a hardcoded literal default in source -- real values live in the
    // checked-in config/bsf.json, with an env var override per key still available.
    const auto config = nf_config::load("bsf", CONFIG_DIR);
    const auto port = nf_config::require<unsigned short>(config, "port");
    const auto metrics_bind_address =
        nf_config::require<std::string>(config, "metrics_bind_address");
    const auto sbi_bind_address = nf_config::require<std::string>(config, "sbi_bind_address");
    const auto nrf_base =
        nf_config::require<std::string>(config, "nrf_base_url", "BSF_NRF_BASE_URL");

    sbi_core::init_metrics(metrics_bind_address);

    const std::string bsf_instance_id = sbi_core::generate_uuid_v4();
    spdlog::info("bsf: starting, nfInstanceId={}", bsf_instance_id);

    sbi_core::http2::TlsConfig server_tls{
        .cert_path = CERTS_DIR "/bsf/cert.pem",
        .key_path = CERTS_DIR "/bsf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };

    sbi_core::jwt::Verifier verifier(CERTS_DIR "/nrf-jwt/public.pem", kNrfInstanceId);

    // BSF's own client identity for delivering myNotification callbacks -- separate
    // sbi_core::http2::Client from run_nrf_lifecycle's (which runs on its own thread; this one is
    // only ever touched from route handlers, which all run on ioc's single thread -- see
    // http2_server.hpp).
    sbi_core::http2::TlsConfig notify_client_tls{
        .cert_path = CERTS_DIR "/bsf/cert.pem",
        .key_path = CERTS_DIR "/bsf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client notify_client(std::move(notify_client_tls));

    bsf::PcfBindingStore pcf_bindings;
    bsf::BsfSubscriptionStore subscriptions;
    bsf::PcfForUeBindingStore pcf_ue_bindings;
    bsf::PcfMbsBindingStore pcf_mbs_bindings;

    auto meter = sbi_core::get_meter("bsf");
    auto create_binding_counter =
        meter->CreateUInt64Counter("bsf_create_pcf_binding_total", "Total CreatePCFBinding calls");
    auto get_bindings_counter =
        meter->CreateUInt64Counter("bsf_get_pcf_bindings_total", "Total GetPCFBindings calls");
    auto delete_binding_counter = meter->CreateUInt64Counter("bsf_delete_pcf_binding_total",
                                                             "Total DeleteIndPCFBinding calls");
    auto update_binding_counter = meter->CreateUInt64Counter("bsf_update_pcf_binding_total",
                                                             "Total UpdateIndPCFBinding calls");
    auto sub_create_counter = meter->CreateUInt64Counter("bsf_sub_create_total",
                                                         "Total CreateIndividualSubcription calls");
    auto sub_replace_counter = meter->CreateUInt64Counter(
        "bsf_sub_replace_total", "Total ReplaceIndividualSubscription calls");
    auto sub_delete_counter = meter->CreateUInt64Counter(
        "bsf_sub_delete_total", "Total DeleteIndividualSubscription calls");
    auto create_ue_binding_counter = meter->CreateUInt64Counter(
        "bsf_create_pcf_ue_binding_total", "Total CreatePCFforUEBinding calls");
    auto get_ue_bindings_counter = meter->CreateUInt64Counter("bsf_get_pcf_ue_bindings_total",
                                                              "Total GetPCFForUeBindings calls");
    auto delete_ue_binding_counter = meter->CreateUInt64Counter(
        "bsf_delete_pcf_ue_binding_total", "Total DeleteIndPCFforUEBinding calls");
    auto update_ue_binding_counter = meter->CreateUInt64Counter(
        "bsf_update_pcf_ue_binding_total", "Total UpdateIndPCFforUEBinding calls");
    auto create_mbs_binding_counter = meter->CreateUInt64Counter("bsf_create_pcf_mbs_binding_total",
                                                                 "Total CreatePCFMbsBinding calls");
    auto get_mbs_binding_counter =
        meter->CreateUInt64Counter("bsf_get_pcf_mbs_binding_total", "Total GetPCFMbsBinding calls");
    auto modify_mbs_binding_counter = meter->CreateUInt64Counter(
        "bsf_modify_pcf_mbs_binding_total", "Total ModifyIndPCFMbsBinding calls");
    auto delete_mbs_binding_counter = meter->CreateUInt64Counter(
        "bsf_delete_pcf_mbs_binding_total", "Total DeleteIndPCFMbsBinding calls");

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

    // --- Nbsf_Management: pcfBindings ---

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/pcfBindings",
        [&verifier, &pcf_bindings, &notify_client, &subscriptions, &create_binding_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::PcfBinding>(req, err);
            if (!body.has_value()) {
                return err;
            }
            if (!body->supi.has_value()) {
                return sbi_core::http2::problem_response(
                    400,
                    "Missing mandatory IE",
                    "PcfBinding requires supi for this project's own real duplicate-check (see "
                    "this file's own top comment, simplification 1)");
            }

            if (auto existing =
                    pcf_bindings.find_by_combination(*body->supi, body->dnn, json(body->snssai));
                existing.has_value()) {
                sbi_gen::ExtProblemDetails_Nbsf_Management existing_details;
                existing_details.status = 403;
                existing_details.title = "Forbidden";
                existing_details.detail =
                    "An existing PCF binding already covers this supi+dnn+snssai combination";
                json existing_binding = existing->second;
                if (existing_binding.contains("pcfSmFqdn")) {
                    existing_details.pcfSmFqdn =
                        existing_binding.at("pcfSmFqdn").get<sbi_gen::Fqdn>();
                }
                if (existing_binding.contains("pcfSmIpEndPoints")) {
                    existing_details.pcfSmIpEndPoints =
                        existing_binding.at("pcfSmIpEndPoints")
                            .get<std::vector<sbi_gen::IpEndPoint>>();
                }
                sbi_core::http2::Response resp;
                resp.status = 403;
                resp.headers.emplace("content-type", "application/problem+json");
                resp.body = json(existing_details).dump();
                return resp;
            }

            create_binding_counter->Add(1);
            json j = *body;
            const auto id = pcf_bindings.create(j);
            deliver_bsf_notification(notify_client,
                                     subscriptions,
                                     sbi_gen::BsfEvent::PCF_PDU_SESSION_BINDING_REGISTRATION,
                                     *body->supi,
                                     body->dnn,
                                     json(body->snssai));
            deliver_bsf_notification(notify_client,
                                     subscriptions,
                                     sbi_gen::BsfEvent::SNSSAI_DNN_BINDING_REGISTRATION,
                                     *body->supi,
                                     body->dnn,
                                     json(body->snssai));

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location", std::string(kApiRoot) + "/pcfBindings/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kApiRoot) + "/pcfBindings",
        [&verifier, &pcf_bindings, &get_bindings_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            get_bindings_counter->Add(1);
            for (const auto& binding : pcf_bindings.list_all()) {
                if (matches_pcf_binding_filter(binding, req)) {
                    return sbi_core::http2::Response::json(200, binding.dump());
                }
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "DELETE",
        std::string(kApiRoot) + "/pcfBindings/{bindingId}",
        [&verifier, &pcf_bindings, &notify_client, &subscriptions, &delete_binding_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("bindingId");
            auto removed = pcf_bindings.remove(id);
            if (!removed.has_value()) {
                return sbi_core::http2::problem_response(404, "Not Found", "No PCF binding " + id);
            }
            delete_binding_counter->Add(1);
            if (removed->contains("supi")) {
                const std::string supi = removed->at("supi").get<std::string>();
                const std::string dnn = removed->value("dnn", "");
                const json snssai = removed->value("snssai", json::object());
                deliver_bsf_notification(notify_client,
                                         subscriptions,
                                         sbi_gen::BsfEvent::PCF_PDU_SESSION_BINDING_DEREGISTRATION,
                                         supi,
                                         dnn,
                                         snssai);
                deliver_bsf_notification(notify_client,
                                         subscriptions,
                                         sbi_gen::BsfEvent::SNSSAI_DNN_BINDING_DEREGISTRATION,
                                         supi,
                                         dnn,
                                         snssai);
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "PATCH",
        std::string(kApiRoot) + "/pcfBindings/{bindingId}",
        [&verifier, &pcf_bindings, &update_binding_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("bindingId");
            json merge_patch;
            try {
                merge_patch = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            auto patched = pcf_bindings.patch(id, merge_patch);
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(404, "Not Found", "No PCF binding " + id);
            }
            update_binding_counter->Add(1);
            return sbi_core::http2::Response::json(200, patched->dump());
        });

    // --- Nbsf_Management: subscriptions ---

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/subscriptions",
        [&verifier, &subscriptions, &sub_create_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::BsfSubscription>(req, err);
            if (!body.has_value()) {
                return err;
            }

            sub_create_counter->Add(1);
            json j = *body;
            const auto id = subscriptions.create(j);

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location", std::string(kApiRoot) + "/subscriptions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "PUT",
        std::string(kApiRoot) + "/subscriptions/{subId}",
        [&verifier, &subscriptions, &sub_replace_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subId");
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::BsfSubscription>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            if (!subscriptions.put(id, j)) {
                return sbi_core::http2::problem_response(404, "Not Found", "No subscription " + id);
            }
            sub_replace_counter->Add(1);
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "DELETE",
        std::string(kApiRoot) + "/subscriptions/{subId}",
        [&verifier, &subscriptions, &sub_delete_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subId");
            if (!subscriptions.remove(id).has_value()) {
                return sbi_core::http2::problem_response(404, "Not Found", "No subscription " + id);
            }
            sub_delete_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nbsf_Management: pcf-ue-bindings ---

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/pcf-ue-bindings",
        [&verifier, &pcf_ue_bindings, &notify_client, &subscriptions, &create_ue_binding_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::PcfForUeBinding>(req, err);
            if (!body.has_value()) {
                return err;
            }

            if (auto existing = pcf_ue_bindings.find_by_supi(body->supi); existing.has_value()) {
                return sbi_core::http2::problem_response(
                    403,
                    "Forbidden",
                    "An existing PCF for a UE binding already covers this supi (see this file's "
                    "own top comment, simplification 5)");
            }

            create_ue_binding_counter->Add(1);
            json j = *body;
            const auto id = pcf_ue_bindings.create(j);
            deliver_bsf_notification(notify_client,
                                     subscriptions,
                                     sbi_gen::BsfEvent::PCF_UE_BINDING_REGISTRATION,
                                     body->supi,
                                     std::nullopt,
                                     json());

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location", std::string(kApiRoot) + "/pcf-ue-bindings/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kApiRoot) + "/pcf-ue-bindings",
        [&verifier, &pcf_ue_bindings, &get_ue_bindings_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            get_ue_bindings_counter->Add(1);
            const auto supi_filter = get_string_query_param(req, "supi");
            const auto gpsi_filter = get_string_query_param(req, "gpsi");
            json results = json::array();
            for (const auto& binding : pcf_ue_bindings.list_all()) {
                if (supi_filter.has_value() && binding.value("supi", "") != *supi_filter) {
                    continue;
                }
                if (gpsi_filter.has_value() && binding.value("gpsi", "") != *gpsi_filter) {
                    continue;
                }
                results.push_back(binding);
            }
            return sbi_core::http2::Response::json(200, results.dump());
        });

    server.add_route(
        "DELETE",
        std::string(kApiRoot) + "/pcf-ue-bindings/{bindingId}",
        [&verifier, &pcf_ue_bindings, &notify_client, &subscriptions, &delete_ue_binding_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("bindingId");
            auto removed = pcf_ue_bindings.remove(id);
            if (!removed.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No PCF for a UE binding " + id);
            }
            delete_ue_binding_counter->Add(1);
            if (removed->contains("supi")) {
                deliver_bsf_notification(notify_client,
                                         subscriptions,
                                         sbi_gen::BsfEvent::PCF_UE_BINDING_DEREGISTRATION,
                                         removed->at("supi").get<std::string>(),
                                         std::nullopt,
                                         json());
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "PATCH",
        std::string(kApiRoot) + "/pcf-ue-bindings/{bindingId}",
        [&verifier, &pcf_ue_bindings, &update_ue_binding_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("bindingId");
            json merge_patch;
            try {
                merge_patch = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            auto patched = pcf_ue_bindings.patch(id, merge_patch);
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No PCF for a UE binding " + id);
            }
            update_ue_binding_counter->Add(1);
            return sbi_core::http2::Response::json(200, patched->dump());
        });

    // --- Nbsf_Management: pcf-mbs-bindings ---

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/pcf-mbs-bindings",
        [&verifier, &pcf_mbs_bindings, &create_mbs_binding_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::PcfMbsBinding>(req, err);
            if (!body.has_value()) {
                return err;
            }

            if (auto existing = pcf_mbs_bindings.find_by_mbs_session_id(json(body->mbsSessionId));
                existing.has_value()) {
                sbi_gen::MbsExtProblemDetails_Nbsf_Management existing_details;
                existing_details.status = 403;
                existing_details.title = "Forbidden";
                existing_details.detail =
                    "An existing PCF for an MBS Session binding already covers this mbsSessionId";
                json existing_binding = existing->second;
                if (existing_binding.contains("pcfFqdn")) {
                    existing_details.pcfFqdn = existing_binding.at("pcfFqdn").get<sbi_gen::Fqdn>();
                }
                if (existing_binding.contains("pcfIpEndPoints")) {
                    existing_details.pcfIpEndPoints = existing_binding.at("pcfIpEndPoints")
                                                          .get<std::vector<sbi_gen::IpEndPoint>>();
                }
                sbi_core::http2::Response resp;
                resp.status = 403;
                resp.headers.emplace("content-type", "application/problem+json");
                resp.body = json(existing_details).dump();
                return resp;
            }

            create_mbs_binding_counter->Add(1);
            json j = *body;
            const auto id = pcf_mbs_bindings.create(j);

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location", std::string(kApiRoot) + "/pcf-mbs-bindings/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kApiRoot) + "/pcf-mbs-bindings",
        [&verifier, &pcf_mbs_bindings, &get_mbs_binding_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto mbs_session_id = get_json_query_param<json>(req, "mbs-session-id");
            if (!mbs_session_id.has_value()) {
                return sbi_core::http2::problem_response(
                    400, "Missing mandatory query parameter", "mbs-session-id is required");
            }
            get_mbs_binding_counter->Add(1);
            json results = json::array();
            if (auto found = pcf_mbs_bindings.find_by_mbs_session_id(*mbs_session_id);
                found.has_value()) {
                results.push_back(found->second);
            }
            return sbi_core::http2::Response::json(200, results.dump());
        });

    server.add_route(
        "PATCH",
        std::string(kApiRoot) + "/pcf-mbs-bindings/{bindingId}",
        [&verifier, &pcf_mbs_bindings, &modify_mbs_binding_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("bindingId");
            json merge_patch;
            try {
                merge_patch = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            auto patched = pcf_mbs_bindings.patch(id, merge_patch);
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No PCF for an MBS Session binding " + id);
            }
            modify_mbs_binding_counter->Add(1);
            return sbi_core::http2::Response::json(200, patched->dump());
        });

    server.add_route(
        "DELETE",
        std::string(kApiRoot) + "/pcf-mbs-bindings/{bindingId}",
        [&verifier, &pcf_mbs_bindings, &delete_mbs_binding_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("bindingId");
            if (!pcf_mbs_bindings.remove(id).has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No PCF for an MBS Session binding " + id);
            }
            delete_mbs_binding_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    std::thread(run_nrf_lifecycle, bsf_instance_id, nrf_base).detach();

    server.start();
    spdlog::info("bsf: listening on https://{}:{} (TLS 1.3 + mTLS)", sbi_bind_address, port);
    spdlog::info("bsf: Prometheus metrics at http://{}/metrics", metrics_bind_address);
    sbi_core::run_multi_threaded(ioc);
    return 0;
}
