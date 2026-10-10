// nfs/scp: SCP (Service Communication Proxy), Nscp_EventExposure service. Source:
// specs/5G_APIs-REL-19/TS29570_Nscp_EventExposure.yaml (v1.0.1), commit
// bca84b60a37773133bcae97e5c6c0d10a93b47b6. This project's twelfth NF, fourth built under the
// continuous move-to-next-NF process (docs/DECISIONS.md ADR-0184).
//
// In scope, agreed with the user before implementation (real, explicit design question, not a
// default assumption -- see docs/DECISIONS.md ADR-0186 for the full disclosure): SCP's REAL
// defining role per TS 29.500 SS6.10-6.11 is as an inline HTTP/2 message-forwarding proxy for
// "indirect communication" (Model C/D) between NF service consumers and producers -- every
// existing NF in this project currently calls every other NF directly
// (sbi_core::http2::Client -> the target NF's own real address), and making SCP a genuine
// intermediary would mean re-routing every one of those calls through a new SCP process, a
// cross-cutting architecture change touching every NF already built, not a same-shape addition
// like every other NF this project has built so far. That real forwarding behavior is NOT
// implemented in this turn -- deliberately, disclosed up front, chosen explicitly by the user
// over building it and over designing it first (see ADR-0186's own recorded options).
//
// What IS implemented: `Nscp_EventExposure`, the one real YAML-backed SCP service that exists as
// an actual origin-server REST API (SCP's own operational-statistics exposure to subscribers --
// e.g. request/response counts, failure causes, reselection stats an SCP instance would report
// about its own forwarding activity). All 3 real operations:
//   POST  {apiRoot}/subscriptions                  CreateSubscription
//   PATCH {apiRoot}/subscriptions/{subscriptionId}  ModifySubscription
//   DELETE {apiRoot}/subscriptions/{subscriptionId} DeleteSubscription
// Plus the real `onScpEventExposureNotification` callback CreateSubscription's own POST declares.
//
// Real, disclosed gap -- stated up front, not discovered in review: because this project's SCP
// does not perform real message forwarding, it never generates genuine `ScpSignallingInfo`
// activity (request/response counts, failure causes, reselection stats) to report on.
// `CreateSubscription`/`ModifySubscription`/`DeleteSubscription` are real, live, tested CRUD on
// the subscription resource itself, but the real `onScpEventExposureNotification` callback is
// never fired -- same disclosed-gap shape and same reasoning as nfs/nef's own
// `PfdChangeNotification` gap (ADR-0185): this project does not build a notification-delivery
// function with no real caller, since dead code presented as live infrastructure would be worse
// than disclosing the gap plainly.

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

#include "TS29570_Nscp_EventExposure.hpp"
#include "nf_config/nf_config.hpp"
#include "stores.hpp"

namespace {

using nlohmann::json;

#ifndef CERTS_DIR
#error "CERTS_DIR must be defined by CMake (see nfs/scp/CMakeLists.txt)"
#endif
#ifndef CONFIG_DIR
#error "CONFIG_DIR must be defined by CMake (see nfs/scp/CMakeLists.txt)"
#endif

constexpr const char* kNfType = "SCP";
constexpr const char* kApiRoot = "/nscp-ee/v1";

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
void run_nrf_lifecycle(const std::string& scp_instance_id, const std::string& nrf_base) {
    sbi_core::http2::TlsConfig client_tls{
        .cert_path = CERTS_DIR "/scp/cert.pem",
        .key_path = CERTS_DIR "/scp/key.pem",
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
        http_client, nrf_base + "/oauth2/token", scp_instance_id, "nnrf-nfm", "NRF");

    constexpr int kHeartbeatSeconds = 30;
    json profile{
        {"nfInstanceId", scp_instance_id},
        {"nfType", kNfType},
        {"nfStatus", "REGISTERED"},
        {"ipv4Addresses", json::array({"127.0.0.1"})},
        {"heartBeatTimer", kHeartbeatSeconds},
    };

    while (true) {
        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("scp: OAuth2 token fetch failed: {}", token.error());
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        sbi_core::http2::ClientRequest put_req;
        put_req.method = "PUT";
        put_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + scp_instance_id;
        put_req.headers.emplace("content-type", "application/json");
        put_req.headers.emplace("authorization", "Bearer " + *token);
        put_req.headers.emplace(
            sbi_core::headers::kSenderTimestamp,
            sbi_core::headers::format_sender_timestamp(std::chrono::system_clock::now()));
        put_req.body = profile.dump();
        auto put_resp = http_client.send(put_req);
        if (put_resp.has_value() && (put_resp->status == 200 || put_resp->status == 201)) {
            spdlog::info("scp: registered with NRF (HTTP {})", put_resp->status);
            break;
        }
        spdlog::warn("scp: NRF registration attempt failed, retrying in 5s");
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(kHeartbeatSeconds / 2));

        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("scp: OAuth2 token fetch failed for heartbeat: {}", token.error());
            continue;
        }

        sbi_core::http2::ClientRequest patch_req;
        patch_req.method = "PATCH";
        patch_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + scp_instance_id;
        patch_req.headers.emplace("content-type", "application/json-patch+json");
        patch_req.headers.emplace("authorization", "Bearer " + *token);
        patch_req.body =
            json::array({json{{"op", "replace"}, {"path", "/nfStatus"}, {"value", "REGISTERED"}}})
                .dump();

        auto patch_resp = http_client.send(patch_req);
        if (!patch_resp.has_value() || patch_resp->status != 200) {
            spdlog::warn("scp: heartbeat failed");
        }
    }
}

} // namespace

int main() {
    sbi_core::init_logging("scp");
    sbi_core::init_tracing("scp");

    // ADR-0077 (user-directed, mandatory, project-wide): no DB URL/connection/deployment
    // parameter may be a hardcoded literal default in source -- real values live in the
    // checked-in config/scp.json, with an env var override per key still available.
    const auto config = nf_config::load("scp", CONFIG_DIR);
    const auto port = nf_config::require<unsigned short>(config, "port");
    const auto metrics_bind_address =
        nf_config::require<std::string>(config, "metrics_bind_address");
    const auto sbi_bind_address = nf_config::require<std::string>(config, "sbi_bind_address");
    const auto nrf_base =
        nf_config::require<std::string>(config, "nrf_base_url", "SCP_NRF_BASE_URL");

    sbi_core::init_metrics(metrics_bind_address);

    const std::string scp_instance_id = sbi_core::generate_uuid_v4();
    spdlog::info("scp: starting, nfInstanceId={}", scp_instance_id);

    sbi_core::http2::TlsConfig server_tls{
        .cert_path = CERTS_DIR "/scp/cert.pem",
        .key_path = CERTS_DIR "/scp/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };

    sbi_core::jwt::Verifier verifier(CERTS_DIR "/nrf-jwt/public.pem", kNrfInstanceId);

    scp::ScpEventSubscriptionStore subscriptions;

    auto meter = sbi_core::get_meter("scp");
    auto sub_create_counter =
        meter->CreateUInt64Counter("scp_ee_sub_create_total", "Total CreateSubscription calls");
    auto sub_modify_counter =
        meter->CreateUInt64Counter("scp_ee_sub_modify_total", "Total ModifySubscription calls");
    auto sub_delete_counter =
        meter->CreateUInt64Counter("scp_ee_sub_delete_total", "Total DeleteSubscription calls");

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

    // --- Nscp_EventExposure: subscriptions ---

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/subscriptions",
        [&verifier, &subscriptions, &sub_create_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::ScpEventExposureSubscription>(req, err);
            if (!body.has_value()) {
                return err;
            }

            sub_create_counter->Add(1);
            const auto id = subscriptions.create(json(*body));

            sbi_gen::ScpEventExposureSubsResp resp_body;
            resp_body.expiryTime = body->expiry;

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location", std::string(kApiRoot) + "/subscriptions/" + id);
            resp.body = json(resp_body).dump();
            return resp;
        });

    server.add_route(
        "PATCH",
        std::string(kApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &subscriptions, &sub_modify_counter](const sbi_core::http2::Request& req) {
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
            auto patched = subscriptions.patch(id, patch_ops);
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(404, "Not Found", "No subscription " + id);
            }
            sub_modify_counter->Add(1);

            sbi_gen::ScpEventExposureSubsResp resp_body;
            try {
                resp_body.expiryTime = patched->get<sbi_gen::ScpEventExposureSubscription>().expiry;
            } catch (const json::exception&) {
                // Real, disclosed: a patch that produces a document no longer matching
                // ScpEventExposureSubscription's own shape still succeeded as a raw RFC 6902
                // patch (same precedent as nfs/nrf's NfRegistry::apply_patch) -- the response is
                // just missing the optional expiryTime echo in that case, not an error.
            }
            return sbi_core::http2::Response::json(200, json(resp_body).dump());
        });

    server.add_route(
        "DELETE",
        std::string(kApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &subscriptions, &sub_delete_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            if (!subscriptions.remove(id)) {
                return sbi_core::http2::problem_response(404, "Not Found", "No subscription " + id);
            }
            sub_delete_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    std::thread(run_nrf_lifecycle, scp_instance_id, nrf_base).detach();

    server.start();
    spdlog::info("scp: listening on https://{}:{} (TLS 1.3 + mTLS)", sbi_bind_address, port);
    spdlog::info("scp: Prometheus metrics at http://{}/metrics", metrics_bind_address);
    sbi_core::run_multi_threaded(ioc);
    return 0;
}
