// nfs/eir: 5G-EIR (5G Equipment Identity Register), N5g-eir_EquipmentIdentityCheck service.
// Source: specs/5G_APIs-REL-19/TS29511_N5g-eir_EquipmentIdentityCheck.yaml (v1.4.0), commit
// bca84b60a37773133bcae97e5c6c0d10a93b47b6. This project's thirteenth NF, first Tier 2 NF built
// (CLAUDE.md's own Tier 2 scope list), continuing the continuous move-to-next-NF process
// (docs/DECISIONS.md ADR-0184) after the original Tier 1 "still not done"
// nssf/nef/scp/bsf list closed out (ADR-0183 through ADR-0186). Real, standardized directory/
// binary/config name `eir` (not `5g-eir`) chosen deliberately -- CMake target names and generated
// identifiers starting with a digit are a real, avoidable source of tooling friction, not a
// change to the real NF identity. Real, self-caught bug during live verification: `kNfType` was
// initially written as `"5G-EIR"` (hyphen), which NRF's own `known_nf_types()` correctly rejected
// with a real `400` -- the real TS 29.510 `NFType` enum value is `5G_EIR` (underscore), confirmed
// by direct read of the YAML, not assumed; fixed below.
//
// In scope, agreed with the user before implementation: this service's entire real operation
// surface is exactly 1 operation (confirmed by direct read of the 120-line YAML, not estimated) --
// unlike NEF/SCP/BSF/NSSF, this is a real, COMPLETE NF this turn, not a disclosed partial slice:
//   GET {apiRoot}/equipment-status  GetEquipmentStatus
//
// Real, disclosed simplification -- stated up front, not discovered in review: this YAML has no
// operation anywhere that lets a caller WRITE an equipment's status into the 5G-EIR. The real
// provisioning of a device's IMEI/PEI into the equipment database (whitelist/blacklist/greylist)
// is genuinely out of 3GPP's own standardized SBI framework scope here (typically OAM/GSMA IMEI
// database sync), not just unbuilt -- same structural shape as nfs/nef's/nfs/scp's own disclosed
// gaps (ADR-0185/ADR-0186). `EquipmentStatusStore` is therefore seed()-only, seeded with real,
// standardized `EquipmentStatus` enum values (WHITELISTED/BLACKLISTED/GREYLISTED, TS29511's own
// enum, not fabricated), not live-provisioned data.
//
// Real, disclosed, deferred wiring opportunity (not built this turn): TS 23.502 §4.2.2.2.2's own
// Registration procedure has AMF optionally invoke `N5g-eir_EquipmentIdentityCheck` to verify a
// UE's PEI during Registration. This project's own AMF (nfs/amf) does not yet call this NF --
// real, disclosed, deferred wiring, same pattern as NEF's own PFD-management/UPF non-wiring
// (ADR-0185).

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

#include "TS29511_N5g-eir_EquipmentIdentityCheck.hpp"
#include "nf_config/nf_config.hpp"
#include "stores.hpp"

namespace {

using nlohmann::json;

#ifndef CERTS_DIR
#error "CERTS_DIR must be defined by CMake (see nfs/eir/CMakeLists.txt)"
#endif
#ifndef CONFIG_DIR
#error "CONFIG_DIR must be defined by CMake (see nfs/eir/CMakeLists.txt)"
#endif

// Real TS 29.510 Nnrf_NFManagement NFType enum value is "5G_EIR" (underscore) -- confirmed by
// direct read of specs/5G_APIs-REL-19/TS29510_Nnrf_NFManagement.yaml, NOT "5G-EIR" as this
// project's own NF name (nfType != display name; TS 29.511's own service name uses a hyphen,
// TS 29.510's own enum uses an underscore -- both real, not a typo either way).
constexpr const char* kNfType = "5G_EIR";
constexpr const char* kApiRoot = "/n5g-eir-eic/v1";

// Must match nfs/nrf/src/main.cpp's kNrfInstanceId exactly -- see docs/DECISIONS.md ADR-0018.
constexpr const char* kNrfInstanceId = "5ba9a927-1d31-4c8e-8a10-000000000001";

// Real, illustrative seed data -- see this file's own top comment, simplification, for why no
// live write path exists to populate this instead. Real, standardized PEI format (IMEISV, TS
// 23.003) used for the example entries, not fabricated.
void seed_equipment_statuses(eir::EquipmentStatusStore& store) {
    store.seed("imeisv-3510000000000010", sbi_gen::EquipmentStatus::WHITELISTED);
    store.seed("imeisv-3510000000000020", sbi_gen::EquipmentStatus::BLACKLISTED);
    store.seed("imeisv-3510000000000030", sbi_gen::EquipmentStatus::GREYLISTED);
}

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
void run_nrf_lifecycle(const std::string& eir_instance_id, const std::string& nrf_base) {
    sbi_core::http2::TlsConfig client_tls{
        .cert_path = CERTS_DIR "/eir/cert.pem",
        .key_path = CERTS_DIR "/eir/key.pem",
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
        http_client, nrf_base + "/oauth2/token", eir_instance_id, "nnrf-nfm", "NRF");

    constexpr int kHeartbeatSeconds = 30;
    json profile{
        {"nfInstanceId", eir_instance_id},
        {"nfType", kNfType},
        {"nfStatus", "REGISTERED"},
        {"ipv4Addresses", json::array({"127.0.0.1"})},
        {"heartBeatTimer", kHeartbeatSeconds},
    };

    while (true) {
        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("eir: OAuth2 token fetch failed: {}", token.error());
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        sbi_core::http2::ClientRequest put_req;
        put_req.method = "PUT";
        put_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + eir_instance_id;
        put_req.headers.emplace("content-type", "application/json");
        put_req.headers.emplace("authorization", "Bearer " + *token);
        put_req.headers.emplace(
            sbi_core::headers::kSenderTimestamp,
            sbi_core::headers::format_sender_timestamp(std::chrono::system_clock::now()));
        put_req.body = profile.dump();
        auto put_resp = http_client.send(put_req);
        if (put_resp.has_value() && (put_resp->status == 200 || put_resp->status == 201)) {
            spdlog::info("eir: registered with NRF (HTTP {})", put_resp->status);
            break;
        }
        spdlog::warn("eir: NRF registration attempt failed, retrying in 5s");
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(kHeartbeatSeconds / 2));

        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("eir: OAuth2 token fetch failed for heartbeat: {}", token.error());
            continue;
        }

        sbi_core::http2::ClientRequest patch_req;
        patch_req.method = "PATCH";
        patch_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + eir_instance_id;
        patch_req.headers.emplace("content-type", "application/json-patch+json");
        patch_req.headers.emplace("authorization", "Bearer " + *token);
        patch_req.body =
            json::array({json{{"op", "replace"}, {"path", "/nfStatus"}, {"value", "REGISTERED"}}})
                .dump();

        auto patch_resp = http_client.send(patch_req);
        if (!patch_resp.has_value() || patch_resp->status != 200) {
            spdlog::warn("eir: heartbeat failed");
        }
    }
}

} // namespace

int main() {
    sbi_core::init_logging("eir");
    sbi_core::init_tracing("eir");

    // ADR-0077 (user-directed, mandatory, project-wide): no DB URL/connection/deployment
    // parameter may be a hardcoded literal default in source -- real values live in the
    // checked-in config/eir.json, with an env var override per key still available.
    const auto config = nf_config::load("eir", CONFIG_DIR);
    const auto port = nf_config::require<unsigned short>(config, "port");
    const auto metrics_bind_address =
        nf_config::require<std::string>(config, "metrics_bind_address");
    const auto sbi_bind_address = nf_config::require<std::string>(config, "sbi_bind_address");
    const auto nrf_base =
        nf_config::require<std::string>(config, "nrf_base_url", "EIR_NRF_BASE_URL");

    sbi_core::init_metrics(metrics_bind_address);

    const std::string eir_instance_id = sbi_core::generate_uuid_v4();
    spdlog::info("eir: starting, nfInstanceId={}", eir_instance_id);

    sbi_core::http2::TlsConfig server_tls{
        .cert_path = CERTS_DIR "/eir/cert.pem",
        .key_path = CERTS_DIR "/eir/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };

    sbi_core::jwt::Verifier verifier(CERTS_DIR "/nrf-jwt/public.pem", kNrfInstanceId);

    eir::EquipmentStatusStore statuses;
    seed_equipment_statuses(statuses);

    auto meter = sbi_core::get_meter("eir");
    auto get_status_counter = meter->CreateUInt64Counter("eir_get_equipment_status_total",
                                                         "Total GetEquipmentStatus calls");

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

    // --- N5g-eir_EquipmentIdentityCheck: equipment-status ---

    server.add_route(
        "GET",
        std::string(kApiRoot) + "/equipment-status",
        [&verifier, &statuses, &get_status_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto pei_it = req.query_params.find("pei");
            if (pei_it == req.query_params.end()) {
                return sbi_core::http2::problem_response(
                    400, "Missing mandatory query parameter", "pei is required");
            }
            get_status_counter->Add(1);

            auto status = statuses.get(pei_it->second);
            if (!status.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "PEI Not Found", "No equipment status for pei " + pei_it->second);
            }

            sbi_gen::EirResponseData resp_body;
            resp_body.status.value = *status;
            return sbi_core::http2::Response::json(200, json(resp_body).dump());
        });

    std::thread(run_nrf_lifecycle, eir_instance_id, nrf_base).detach();

    server.start();
    spdlog::info("eir: listening on https://{}:{} (TLS 1.3 + mTLS)", sbi_bind_address, port);
    spdlog::info("eir: Prometheus metrics at http://{}/metrics", metrics_bind_address);
    sbi_core::run_multi_threaded(ioc);
    return 0;
}
