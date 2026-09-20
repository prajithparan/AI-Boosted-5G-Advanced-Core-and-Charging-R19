// nfs/ausf: AUSF (Authentication Server Function), Nausf_UEAuthentication service.
// Source: specs/5G_APIs-REL-19/TS29509_Nausf_UEAuthentication.yaml (commit
// bca84b60a37773133bcae97e5c6c0d10a93b47b6). Phase 2's sixth NF (PROMPT.md/CLAUDE.md order:
// NRF -> AMF -> SMF -> UDM -> UDR -> AUSF -> PCF). Real MILENAGE/TS 33.501/EAP-AKA' crypto via
// libs/aka-crypto (built last turn, see docs/DECISIONS.md ADR-0026); this is the first NF whose
// own request handlers make a real synchronous SBI client call to another NF (UDM's
// Nudm_UEAU_GenerateAuthData) rather than only to NRF -- see ADR-0027.
//
// In scope, agreed with the user before implementation -- the `ue-authentications` resource
// group only (5G-AKA and EAP-AKA' for a normal UE, TS 33.501 clause 6.1.3):
//   POST   /ue-authentications                                (initiate; calls UDM)
//   PUT    /ue-authentications/{authCtxId}/5g-aka-confirmation (Confirm5gAkaAuthentication)
//   DELETE /ue-authentications/{authCtxId}/5g-aka-confirmation (Delete5gAkaAuthenticationResult)
//   POST   /ue-authentications/{authCtxId}/eap-session          (EapAuthMethod)
//   DELETE /ue-authentications/{authCtxId}/eap-session          (DeleteEapAuthenticationResult)
//   POST   /ue-authentications/deregister                       (UEAuthenticationsDeregister)
//
// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #104, ADR-0091), real TS 33.503 5G ProSe:
//   POST   /prose-authentications                                (ProseAuthenticate; calls UDM)
//   POST   /prose-authentications/{authCtxId}/prose-auth          (proseAuth)
//   DELETE /prose-authentications/{authCtxId}/prose-auth          (DeleteProSeAuthenticationResult)
// Real, disclosed scope narrowing -- see the "prose-authentications" section below, near the
// route registrations themselves, for the full disclosure (the 5gPrukId-returning-UE path and
// CP-PRUK/PAnF persistence are both out of scope, no PAnF NF exists in this project).
//
// Still deliberately deferred, not dropped: /rg-authentications (5G-RG) -- same Tier-1
// 5G-AKA-for-a-normal-UE boundary ADR-0026 already drew for UDM's Nudm_UEAU turn.
//
// Disclosed simplification, stated up front rather than discovered in review: AUSF does NOT call
// UDM's ConfirmAuth/DeleteAuth (Nudm_UEAuthentication_ResultConfirmation) after an authentication
// completes, even though UDM's ConfirmAuth/DeleteAuth were built in ADR-0026 anticipating exactly
// this caller. That wiring is a real design decision of its own (sync-in-the-response-path vs.
// fire-and-forget, what to do if UDM is unreachable at that point) that wasn't part of the scope
// agreed for this turn -- left for a dedicated future turn, not silently done or silently skipped.
// UPDATE (ADR-0070, gap-closure Tier 1c): AuthenticationInfo.supiOrSuci is still passed straight
// through to UDM unchanged -- that remains correct, since UDM is the real home of the
// Subscription Identifier De-concealing Function (SIDF, TS 33.501 clause 6.12.5) and now performs
// real SUCI de-concealment itself before generating auth data. A real SUCI-formatted id no longer
// 404s; it round-trips through UDM's own real ECIES Profile A/B decryption.

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
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <sw/redis++/redis++.h>
#include <thread>
#include <vector>

#include "TS26510_CommonData_grp.hpp"
#include "TS29509_Nausf_UEAuthentication.hpp"
#include "aka_crypto/eap_aka_prime.hpp"
#include "aka_crypto/hex.hpp"
#include "aka_crypto/kdf.hpp"
#include "aka_crypto/milenage.hpp"
#include "kausf_store.hpp"
#include "nf_config/nf_config.hpp"
#include "nf_config/redis.hpp"
#include "stores.hpp"

namespace {

using nlohmann::json;

#ifndef CERTS_DIR
#error "CERTS_DIR must be defined by CMake (see nfs/ausf/CMakeLists.txt)"
#endif
#ifndef CONFIG_DIR
#error "CONFIG_DIR must be defined by CMake (see nfs/ausf/CMakeLists.txt)"
#endif

constexpr const char* kNfType = "AUSF";
constexpr const char* kApiRoot = "/nausf-auth/v1";
// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #104, ADR-0081).
constexpr const char* kSorProtectionApiRoot = "/nausf-sorprotection/v1";
// Gap-closure (ADR-0193 audit, ADR-0195).
constexpr const char* kUpuProtectionApiRoot = "/nausf-upuprotection/v1";

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
// nfs/udm/src/main.cpp's run_nrf_lifecycle (docs/DECISIONS.md ADR-0006/ADR-0019).
void run_nrf_lifecycle(const std::string& ausf_instance_id, const std::string& nrf_base) {
    sbi_core::http2::TlsConfig client_tls{
        .cert_path = CERTS_DIR "/ausf/cert.pem",
        .key_path = CERTS_DIR "/ausf/key.pem",
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
        http_client, nrf_base + "/oauth2/token", ausf_instance_id, "nnrf-nfm", "NRF");

    constexpr int kHeartbeatSeconds = 30;
    json profile{
        {"nfInstanceId", ausf_instance_id},
        {"nfType", kNfType},
        {"nfStatus", "REGISTERED"},
        {"ipv4Addresses", json::array({"127.0.0.1"})},
        {"heartBeatTimer", kHeartbeatSeconds},
    };

    while (true) {
        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("ausf: OAuth2 token fetch failed: {}", token.error());
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        sbi_core::http2::ClientRequest put_req;
        put_req.method = "PUT";
        put_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + ausf_instance_id;
        put_req.headers.emplace("content-type", "application/json");
        put_req.headers.emplace("authorization", "Bearer " + *token);
        put_req.headers.emplace(
            sbi_core::headers::kSenderTimestamp,
            sbi_core::headers::format_sender_timestamp(std::chrono::system_clock::now()));
        put_req.body = profile.dump();

        auto put_resp = http_client.send(put_req);
        if (put_resp.has_value() && (put_resp->status == 200 || put_resp->status == 201)) {
            spdlog::info("ausf: registered with NRF (HTTP {})", put_resp->status);
            break;
        }
        spdlog::warn("ausf: NRF registration attempt failed, retrying in 5s");
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(kHeartbeatSeconds / 2));

        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("ausf: OAuth2 token fetch failed for heartbeat: {}", token.error());
            continue;
        }

        sbi_core::http2::ClientRequest patch_req;
        patch_req.method = "PATCH";
        patch_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + ausf_instance_id;
        patch_req.headers.emplace("content-type", "application/json-patch+json");
        patch_req.headers.emplace("authorization", "Bearer " + *token);
        patch_req.body =
            json::array({json{{"op", "replace"}, {"path", "/nfStatus"}, {"value", "REGISTERED"}}})
                .dump();

        auto patch_resp = http_client.send(patch_req);
        if (!patch_resp.has_value() || patch_resp->status != 200) {
            spdlog::warn("ausf: heartbeat failed");
        }
    }
}

} // namespace

int main() {
    sbi_core::init_logging("ausf");
    sbi_core::init_tracing("ausf");

    // ADR-0077 (user-directed, mandatory, project-wide): no DB URL/connection/deployment
    // parameter may be a hardcoded literal default in source -- real values live in the
    // checked-in config/ausf.json, with an env var override per key still available.
    const auto config = nf_config::load("ausf", CONFIG_DIR);
    const auto port = nf_config::require<unsigned short>(config, "port");
    const auto metrics_bind_address =
        nf_config::require<std::string>(config, "metrics_bind_address");
    const auto nrf_base =
        nf_config::require<std::string>(config, "nrf_base_url", "AUSF_NRF_BASE_URL");
    const auto udm_base =
        nf_config::require<std::string>(config, "udm_base_url", "AUSF_UDM_BASE_URL");
    const auto redis_url = nf_config::require<std::string>(config, "redis_url", "AUSF_REDIS_URL");

    sbi_core::init_metrics(metrics_bind_address);

    const std::string ausf_instance_id = sbi_core::generate_uuid_v4();
    spdlog::info("ausf: starting, nfInstanceId={}", ausf_instance_id);

    sbi_core::http2::TlsConfig server_tls{
        .cert_path = CERTS_DIR "/ausf/cert.pem",
        .key_path = CERTS_DIR "/ausf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };

    sbi_core::jwt::Verifier verifier(CERTS_DIR "/nrf-jwt/public.pem", kNrfInstanceId);

    // AUSF's own client identity + token source for calling UDM -- separate http2::Client/
    // OAuth2Client from run_nrf_lifecycle's (which runs on its own thread; these two are only ever
    // touched from route handlers, which all run on ioc's single thread -- see http2_server.hpp).
    sbi_core::http2::TlsConfig udm_client_tls{
        .cert_path = CERTS_DIR "/ausf/cert.pem",
        .key_path = CERTS_DIR "/ausf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client udm_client(std::move(udm_client_tls));
    sbi_core::OAuth2Client udm_oauth(
        udm_client, nrf_base + "/oauth2/token", ausf_instance_id, "nudm-ueau", "UDM");

    ausf::AuthContextStore auth_contexts;
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #104, ADR-0091): see stores.hpp's own
    // header comment for why this is a distinct store from auth_contexts above.
    ausf::ProSeAuthContextStore prose_auth_contexts;

    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #104, ADR-0081): real, persistent
    // per-SUPI KAUSF + CounterSoR state -- see kausf_store.hpp's own header for why this was a
    // real, load-bearing prerequisite for Nausf_SoRProtection. Same real, fail-fast PING
    // discipline every other NF's own Redis connection already uses.
    auto redis = nf_config::connect_redis_or_die(redis_url, "ausf");
    ausf::KausfStore kausf_store(redis);

    auto meter = sbi_core::get_meter("ausf");
    auto ue_auth_counter = meter->CreateUInt64Counter("ausf_ue_authentications_total",
                                                      "Total POST /ue-authentications calls");
    auto confirm_5g_aka_counter = meter->CreateUInt64Counter(
        "ausf_5g_aka_confirmation_total", "Total Confirm5gAkaAuthentication calls");
    auto eap_session_counter =
        meter->CreateUInt64Counter("ausf_eap_session_total", "Total EapAuthMethod calls");
    auto deregister_counter = meter->CreateUInt64Counter("ausf_deregister_total",
                                                         "Total UEAuthenticationsDeregister calls");
    auto sor_protection_counter = meter->CreateUInt64Counter(
        "ausf_sor_protection_total", "Total Nausf_SoRProtection ue-sor calls");
    // Gap-closure (ADR-0193 audit, ADR-0195).
    auto upu_protection_counter = meter->CreateUInt64Counter(
        "ausf_upu_protection_total", "Total Nausf_UPUProtection ue-upu calls");
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #104, ADR-0091).
    auto prose_authenticate_counter = meter->CreateUInt64Counter(
        "ausf_prose_authenticate_total", "Total POST /prose-authentications calls");
    auto prose_auth_counter =
        meter->CreateUInt64Counter("ausf_prose_auth_total", "Total proseAuth calls");

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

    // --- Nausf_UEAuthentication: ue-authentications ---

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/ue-authentications",
        [&verifier,
         &udm_client,
         &udm_oauth,
         &udm_base,
         &auth_contexts,
         &ausf_instance_id,
         &ue_auth_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::AuthenticationInfo>(req, err);
            if (!body.has_value()) {
                return err;
            }

            auto token = udm_oauth.get_bearer_token();
            if (!token.has_value()) {
                return sbi_core::http2::problem_response(500,
                                                         "Internal Server Error",
                                                         "AUSF could not obtain a token for UDM: " +
                                                             token.error());
            }

            sbi_gen::AuthenticationInfoRequest udm_req{};
            udm_req.servingNetworkName = body->servingNetworkName;
            udm_req.ausfInstanceId = ausf_instance_id;
            // SQN resynchronisation (TS 33.102 §6.3.3, ADR-0037): both AuthenticationInfo (this
            // handler's own request) and AuthenticationInfoRequest (UDM's) declare the identical
            // ResynchronizationInfo_Nudm_UEAU{rand,auts} type for this field -- AUSF is a pure
            // passthrough here, UDM is the one that actually verifies AUTS and resets SQN.
            udm_req.resynchronizationInfo = body->resynchronizationInfo;

            sbi_core::http2::ClientRequest udm_http_req;
            udm_http_req.method = "POST";
            udm_http_req.url = udm_base + "/nudm-ueau/v1/" + body->supiOrSuci +
                               "/security-information/generate-auth-data";
            udm_http_req.headers.emplace("content-type", "application/json");
            udm_http_req.headers.emplace("authorization", "Bearer " + *token);
            udm_http_req.body = json(udm_req).dump();

            auto udm_resp = udm_client.send(udm_http_req);
            if (!udm_resp.has_value()) {
                return sbi_core::http2::problem_response(
                    500, "Internal Server Error", "AUSF could not reach UDM: " + udm_resp.error());
            }
            if (udm_resp->status == 404) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "User does not exist in the HPLMN");
            }
            if (udm_resp->status != 200) {
                return sbi_core::http2::problem_response(
                    500,
                    "Internal Server Error",
                    "UDM GenerateAuthData returned unexpected status " +
                        std::to_string(udm_resp->status));
            }

            sbi_gen::AuthenticationInfoResult udm_result;
            try {
                udm_result = json::parse(udm_resp->body).get<sbi_gen::AuthenticationInfoResult>();
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(
                    500,
                    "Internal Server Error",
                    "UDM returned a malformed AuthenticationInfoResult: " + std::string(e.what()));
            }
            if (!udm_result.authenticationVector.has_value()) {
                return sbi_core::http2::problem_response(
                    500, "Internal Server Error", "UDM returned no authenticationVector");
            }
            const std::string supi = udm_result.supi.value_or(body->supiOrSuci);

            sbi_gen::UEAuthenticationCtx ctx{};
            std::string auth_ctx_id;

            if (udm_result.authType.value == sbi_gen::AuthType_Nudm_UEAU::EAP_AKA_PRIME) {
                sbi_gen::AvEapAkaPrime av;
                try {
                    av = udm_result.authenticationVector->get<sbi_gen::AvEapAkaPrime>();
                } catch (const json::exception& e) {
                    return sbi_core::http2::problem_response(
                        500, "Internal Server Error", "UDM returned a malformed AvEapAkaPrime");
                }
                const auto rand = aka_crypto::from_hex<16>(av.rand);
                const auto autn = aka_crypto::from_hex<16>(av.autn);
                const auto xres = aka_crypto::from_hex<8>(av.xres);
                const auto ck_prime = aka_crypto::from_hex<16>(av.ckPrime);
                const auto ik_prime = aka_crypto::from_hex<16>(av.ikPrime);
                if (!rand || !autn || !xres || !ck_prime || !ik_prime) {
                    return sbi_core::http2::problem_response(
                        500,
                        "Internal Server Error",
                        "UDM returned malformed hex in AvEapAkaPrime");
                }

                const auto keys = aka_crypto::eap::derive_keys(*ck_prime, *ik_prime, supi);
                // KAUSF for EAP-AKA': the same Annex A.2 KDF as 5G-AKA's (FC=0x6A,
                // aka_crypto::derive_kausf), but keyed on CK'/IK' instead of CK/IK -- NOT RFC
                // 5448's EMSK (dimensionally inconsistent with every 32-byte Kausf field in the
                // YAML; confirmed with the user rather than assumed, see docs/DECISIONS.md
                // ADR-0027). SQN xor AK is AUTN's own first 6 bytes -- AUSF never sees SQN
                // directly either, same as a real UE/USIM.
                aka_crypto::Ak48 sqn_xor_ak{};
                std::copy(autn->begin(), autn->begin() + 6, sqn_xor_ak.begin());
                const auto kausf_eap = aka_crypto::derive_kausf(
                    *ck_prime, *ik_prime, body->servingNetworkName, sqn_xor_ak);
                // Identifier: derived from the AV's own RAND rather than a separate counter/RNG --
                // deterministic given this context, and RFC 3748 only requires it be distinct
                // across concurrent exchanges, not globally unique.
                const uint8_t identifier = (*rand)[0];
                const auto packet = aka_crypto::eap::build_challenge_request(
                    identifier, *rand, *autn, body->servingNetworkName, keys.k_aut);
                const auto eap_payload_b64 = aka_crypto::eap::base64_encode(packet);

                ausf::AuthContext store_ctx{};
                store_ctx.supi = supi;
                store_ctx.serving_network_name = body->servingNetworkName;
                store_ctx.auth_type = "EAP_AKA_PRIME";
                store_ctx.k_aut = keys.k_aut;
                store_ctx.xres = *xres;
                store_ctx.kausf_eap = kausf_eap;
                store_ctx.msk = keys.msk;
                auth_ctx_id = auth_contexts.create(std::move(store_ctx));

                ctx.authType.value = sbi_gen::AuthType_Nausf_UEAuthentication::EAP_AKA_PRIME;
                ctx.n5gAuthData = json(eap_payload_b64);
                ctx._links = json{{"eap-session",
                                   json{{"href",
                                         std::string(kApiRoot) + "/ue-authentications/" +
                                             auth_ctx_id + "/eap-session"}}}};
            } else {
                sbi_gen::Av5GHeAka av;
                try {
                    av = udm_result.authenticationVector->get<sbi_gen::Av5GHeAka>();
                } catch (const json::exception& e) {
                    return sbi_core::http2::problem_response(
                        500, "Internal Server Error", "UDM returned a malformed Av5GHeAka");
                }
                const auto rand = aka_crypto::from_hex<16>(av.rand);
                const auto autn = aka_crypto::from_hex<16>(av.autn);
                const auto xres_star = aka_crypto::from_hex<16>(av.xresStar);
                const auto kausf = aka_crypto::from_hex<32>(av.kausf);
                if (!rand || !autn || !xres_star || !kausf) {
                    return sbi_core::http2::problem_response(
                        500, "Internal Server Error", "UDM returned malformed hex in Av5GHeAka");
                }
                const auto hxres_star = aka_crypto::derive_hxres_star(*rand, *xres_star);

                ausf::AuthContext store_ctx{};
                store_ctx.supi = supi;
                store_ctx.serving_network_name = body->servingNetworkName;
                store_ctx.auth_type = "5G_AKA";
                store_ctx.xres_star = *xres_star;
                store_ctx.kausf_5g_aka = *kausf;
                auth_ctx_id = auth_contexts.create(std::move(store_ctx));

                sbi_gen::Av5gAka ausf_av{};
                ausf_av.rand = av.rand;
                ausf_av.hxresStar = aka_crypto::to_hex(hxres_star);
                ausf_av.autn = av.autn;

                ctx.authType.value = sbi_gen::AuthType_Nausf_UEAuthentication::V5G_AKA;
                ctx.n5gAuthData = json(ausf_av);
                ctx._links = json{{"5g-aka",
                                   json{{"href",
                                         std::string(kApiRoot) + "/ue-authentications/" +
                                             auth_ctx_id + "/5g-aka-confirmation"}}}};
            }
            ctx.servingNetworkName = body->servingNetworkName;

            ue_auth_counter->Add(1);
            json j = ctx;
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kApiRoot) + "/ue-authentications/" + auth_ctx_id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "PUT",
        std::string(kApiRoot) + "/ue-authentications/{authCtxId}/5g-aka-confirmation",
        [&verifier, &auth_contexts, &confirm_5g_aka_counter, &kausf_store](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::ConfirmationData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto auth_ctx_id = req.path_params.at("authCtxId");
            auto ctx = auth_contexts.get(auth_ctx_id);
            if (!ctx.has_value() || ctx->auth_type != "5G_AKA") {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No 5G-AKA authentication context " + auth_ctx_id);
            }
            const auto res_star = aka_crypto::from_hex<16>(body->resStar);
            if (!res_star) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", "resStar is not valid 32-hex-char data");
            }

            confirm_5g_aka_counter->Add(1);
            sbi_gen::ConfirmationDataResponse resp_data{};
            if (*res_star == ctx->xres_star) {
                resp_data.authResult.value =
                    sbi_gen::AuthResult_Nausf_UEAuthentication::AUTHENTICATION_SUCCESS;
                resp_data.supi = ctx->supi;
                resp_data.kseaf = aka_crypto::to_hex(
                    aka_crypto::derive_kseaf(ctx->kausf_5g_aka, ctx->serving_network_name));
                // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #104, ADR-0081): a fresh
                // KAUSF is now real, persistent, per-SUPI state -- TS 33.501 §6.14.2.3's own
                // "when the newly derived KAUSF is stored" trigger for CounterSoR
                // initialization/reset, needed by Nausf_SoRProtection (a later, separate call
                // from UDM, well after this exchange completes).
                kausf_store.store_fresh_kausf(ctx->supi, ctx->kausf_5g_aka);
            } else {
                resp_data.authResult.value =
                    sbi_gen::AuthResult_Nausf_UEAuthentication::AUTHENTICATION_FAILURE;
            }
            json j = resp_data;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "DELETE",
        std::string(kApiRoot) + "/ue-authentications/{authCtxId}/5g-aka-confirmation",
        [&verifier, &auth_contexts](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto auth_ctx_id = req.path_params.at("authCtxId");
            auto ctx = auth_contexts.get(auth_ctx_id);
            if (!ctx.has_value() || ctx->auth_type != "5G_AKA" ||
                !auth_contexts.remove(auth_ctx_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No 5G-AKA authentication context " + auth_ctx_id);
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/ue-authentications/{authCtxId}/eap-session",
        [&verifier, &auth_contexts, &eap_session_counter, &kausf_store](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::EapSession>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto auth_ctx_id = req.path_params.at("authCtxId");
            auto ctx = auth_contexts.get(auth_ctx_id);
            if (!ctx.has_value() || ctx->auth_type != "EAP_AKA_PRIME") {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No EAP-AKA' authentication context " + auth_ctx_id);
            }
            auto packet = aka_crypto::eap::base64_decode(body->eapPayload);
            if (!packet.has_value() || packet->size() < 2) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", "eapPayload is not valid base64 EAP data");
            }
            const uint8_t identifier = (*packet)[1];

            eap_session_counter->Add(1);
            sbi_gen::EapSession resp_data{};

            const bool mac_ok = aka_crypto::eap::verify_mac(*packet, ctx->k_aut);
            const auto parsed = aka_crypto::eap::parse_challenge_response(*packet);
            const bool res_ok =
                mac_ok && parsed.has_value() && parsed->res.size() == ctx->xres.size() &&
                std::equal(parsed->res.begin(), parsed->res.end(), ctx->xres.begin());

            if (res_ok) {
                const auto kseaf =
                    aka_crypto::derive_kseaf(ctx->kausf_eap, ctx->serving_network_name);
                resp_data.eapPayload =
                    aka_crypto::eap::base64_encode(aka_crypto::eap::build_success(identifier));
                resp_data.authResult = sbi_gen::AuthResult_Nausf_UEAuthentication{};
                resp_data.authResult->value =
                    sbi_gen::AuthResult_Nausf_UEAuthentication::AUTHENTICATION_SUCCESS;
                resp_data.supi = ctx->supi;
                resp_data.kSeaf = aka_crypto::to_hex(kseaf);
                resp_data.msk =
                    aka_crypto::to_hex(std::vector<uint8_t>(ctx->msk.begin(), ctx->msk.end()));
                // Same real trigger as the 5G-AKA confirmation handler above -- see its own
                // comment.
                kausf_store.store_fresh_kausf(ctx->supi, ctx->kausf_eap);
            } else {
                resp_data.eapPayload =
                    aka_crypto::eap::base64_encode(aka_crypto::eap::build_failure(identifier));
                resp_data.authResult = sbi_gen::AuthResult_Nausf_UEAuthentication{};
                resp_data.authResult->value =
                    sbi_gen::AuthResult_Nausf_UEAuthentication::AUTHENTICATION_FAILURE;
            }
            json j = resp_data;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "DELETE",
        std::string(kApiRoot) + "/ue-authentications/{authCtxId}/eap-session",
        [&verifier, &auth_contexts](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto auth_ctx_id = req.path_params.at("authCtxId");
            auto ctx = auth_contexts.get(auth_ctx_id);
            if (!ctx.has_value() || ctx->auth_type != "EAP_AKA_PRIME" ||
                !auth_contexts.remove(auth_ctx_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No EAP-AKA' authentication context " + auth_ctx_id);
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/ue-authentications/deregister",
        [&verifier, &auth_contexts, &deregister_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::DeregistrationInfo>(req, err);
            if (!body.has_value()) {
                return err;
            }
            if (!auth_contexts.remove_by_supi(body->supi)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No authentication context for supi " + body->supi);
            }
            deregister_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nausf_UEAuthentication: prose-authentications (ADR-0091, gap-closure task #104) ---
    //
    // Real, disclosed scope: only the "new CP-PRUK" first-time-relay-connection path (real
    // SUCI/SUPI in the request) is built -- the "returning UE with an existing 5gPrukId" path
    // (a direct 200 response from POST /prose-authentications, TS 33.503's own CP-PRUK-ID-based
    // reuse case) is NOT built, since it structurally needs a live PAnF (ProSe Anchor Function,
    // Npanf_ProseKey_get) lookup this project doesn't have -- PAnF is a whole separate NF
    // (CLAUDE.md's own Tier 2 scope, not built). For the same reason, CP-PRUK's own real
    // persistence/registration step (Npanf_ProseKey_Register) is skipped: CP-PRUK is derived and
    // consumed for KNR_ProSe within the SAME request (real crypto, not a fabricated shortcut --
    // the KDF outputs are exactly what the spec defines, only the cross-session PERSISTENCE of
    // CP-PRUK via PAnF is out of scope). /rg-authentications (5G-RG) remains deferred, same as
    // this file's own pre-existing disclosure.

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/prose-authentications",
        [&verifier,
         &udm_client,
         &udm_oauth,
         &udm_base,
         &prose_auth_contexts,
         &prose_authenticate_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::ProSeAuthenticationInfo>(req, err);
            if (!body.has_value()) {
                return err;
            }
            if (!body->supiOrSuci.has_value()) {
                // Real, disclosed scope boundary -- see this section's own header comment: the
                // n5gPrukId-based returning-UE path needs a live PAnF this project doesn't have.
                return sbi_core::http2::problem_response(
                    501,
                    "Not Implemented",
                    "5gPrukId-based ProSe re-authentication is not implemented (needs a live "
                    "PAnF/Npanf_ProseKey_get this project doesn't have) -- supply supiOrSuci for "
                    "a real first-time authentication instead");
            }
            const auto nonce1_bytes = aka_crypto::eap::base64_decode(body->nonce1);
            if (!nonce1_bytes.has_value()) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", "nonce1 is not valid base64");
            }

            auto token = udm_oauth.get_bearer_token();
            if (!token.has_value()) {
                return sbi_core::http2::problem_response(500,
                                                         "Internal Server Error",
                                                         "AUSF could not obtain a token for UDM: " +
                                                             token.error());
            }

            sbi_gen::ProSeAuthenticationInfoRequest udm_req{};
            udm_req.servingNetworkName = body->servingNetworkName;
            udm_req.relayServiceCode = body->relayServiceCode;

            sbi_core::http2::ClientRequest udm_http_req;
            udm_http_req.method = "POST";
            udm_http_req.url = udm_base + "/nudm-ueau/v1/" + *body->supiOrSuci +
                               "/prose-security-information/generate-av";
            udm_http_req.headers.emplace("content-type", "application/json");
            udm_http_req.headers.emplace("authorization", "Bearer " + *token);
            udm_http_req.body = json(udm_req).dump();

            auto udm_resp = udm_client.send(udm_http_req);
            if (!udm_resp.has_value()) {
                return sbi_core::http2::problem_response(
                    500, "Internal Server Error", "AUSF could not reach UDM: " + udm_resp.error());
            }
            if (udm_resp->status == 404) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "User does not exist in the HPLMN");
            }
            if (udm_resp->status != 200) {
                return sbi_core::http2::problem_response(
                    500,
                    "Internal Server Error",
                    "UDM GenerateProseAV returned unexpected status " +
                        std::to_string(udm_resp->status));
            }

            sbi_gen::ProSeAuthenticationInfoResult udm_result;
            try {
                udm_result =
                    json::parse(udm_resp->body).get<sbi_gen::ProSeAuthenticationInfoResult>();
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(
                    500,
                    "Internal Server Error",
                    "UDM returned a malformed ProSeAuthenticationInfoResult: " +
                        std::string(e.what()));
            }
            if (!udm_result.proseAuthenticationVectors.has_value() ||
                udm_result.proseAuthenticationVectors->empty()) {
                return sbi_core::http2::problem_response(
                    500, "Internal Server Error", "UDM returned no proseAuthenticationVectors");
            }
            const std::string supi = udm_result.supi.value_or(*body->supiOrSuci);
            const auto& av = (*udm_result.proseAuthenticationVectors)[0];

            const auto rand = aka_crypto::from_hex<16>(av.rand);
            const auto autn = aka_crypto::from_hex<16>(av.autn);
            const auto xres = aka_crypto::from_hex<8>(av.xres);
            const auto ck_prime = aka_crypto::from_hex<16>(av.ckPrime);
            const auto ik_prime = aka_crypto::from_hex<16>(av.ikPrime);
            if (!rand || !autn || !xres || !ck_prime || !ik_prime) {
                return sbi_core::http2::problem_response(
                    500, "Internal Server Error", "UDM returned malformed hex in AvEapAkaPrime");
            }

            const auto keys = aka_crypto::eap::derive_keys(*ck_prime, *ik_prime, supi);
            // KAUSF_P: real KAUSF (TS 33.501 Annex A.2, FC=0x6A), from a real EAP-AKA' run, per
            // TS 33.503's own clause 6.1.3.2 -- same real derivation as the normal
            // ue-authentications EAP-AKA' path above, just relabeled at the point of use (see
            // aka_crypto/kdf.hpp's own header comment).
            aka_crypto::Ak48 sqn_xor_ak{};
            std::copy(autn->begin(), autn->begin() + 6, sqn_xor_ak.begin());
            const auto kausf_p = aka_crypto::derive_kausf(
                *ck_prime, *ik_prime, body->servingNetworkName, sqn_xor_ak);

            const uint8_t identifier = (*rand)[0];
            const auto packet = aka_crypto::eap::build_challenge_request(
                identifier, *rand, *autn, body->servingNetworkName, keys.k_aut);
            const auto eap_payload_b64 = aka_crypto::eap::base64_encode(packet);

            ausf::ProSeAuthContext store_ctx{};
            store_ctx.supi = supi;
            store_ctx.serving_network_name = body->servingNetworkName;
            store_ctx.relay_service_code = body->relayServiceCode;
            store_ctx.nonce1 = *nonce1_bytes;
            store_ctx.k_aut = keys.k_aut;
            store_ctx.xres = *xres;
            store_ctx.kausf_p = kausf_p;
            const auto auth_ctx_id = prose_auth_contexts.create(std::move(store_ctx));

            sbi_gen::ProSeAuthenticationCtx ctx{};
            ctx.authType.value = sbi_gen::AuthType_Nausf_UEAuthentication::EAP_AKA_PRIME;
            ctx.proSeAuthData = eap_payload_b64;
            ctx._links = json{{"prose-auth",
                               json{{"href",
                                     std::string(kApiRoot) + "/prose-authentications/" +
                                         auth_ctx_id + "/prose-auth"}}}};

            prose_authenticate_counter->Add(1);
            json j = ctx;
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/3gppHal+json");
            resp.headers.emplace("location",
                                 std::string(kApiRoot) + "/prose-authentications/" + auth_ctx_id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/prose-authentications/{authCtxId}/prose-auth",
        [&verifier, &prose_auth_contexts, &prose_auth_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::ProSeEapSession>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto auth_ctx_id = req.path_params.at("authCtxId");
            auto ctx = prose_auth_contexts.get(auth_ctx_id);
            if (!ctx.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No ProSe authentication context " + auth_ctx_id);
            }
            auto packet = aka_crypto::eap::base64_decode(body->eapPayload);
            if (!packet.has_value() || packet->size() < 2) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", "eapPayload is not valid base64 EAP data");
            }
            const uint8_t identifier = (*packet)[1];

            prose_auth_counter->Add(1);
            sbi_gen::ProSeEapSession resp_data{};

            const bool mac_ok = aka_crypto::eap::verify_mac(*packet, ctx->k_aut);
            const auto parsed = aka_crypto::eap::parse_challenge_response(*packet);
            const bool res_ok =
                mac_ok && parsed.has_value() && parsed->res.size() == ctx->xres.size() &&
                std::equal(parsed->res.begin(), parsed->res.end(), ctx->xres.begin());

            if (res_ok) {
                // Real TS 33.503 Annex A.2/A.4 derivations (gap-closure task #104, ADR-0091) --
                // see aka_crypto/kdf.hpp's own header comment. Nonce_2: freshly generated here,
                // TS 33.503's own step 11 "AUSF...shall generate Nonce_2".
                const auto cp_pruk =
                    aka_crypto::derive_cp_pruk(ctx->kausf_p, ctx->supi, ctx->relay_service_code);
                const auto nonce2_bytes = aka_crypto::generate_rand();
                const std::vector<std::uint8_t> nonce2_vec(nonce2_bytes.begin(),
                                                           nonce2_bytes.end());
                const auto knr_prose =
                    aka_crypto::derive_knr_prose(cp_pruk, ctx->nonce1, nonce2_vec);

                resp_data.eapPayload =
                    aka_crypto::eap::base64_encode(aka_crypto::eap::build_success(identifier));
                resp_data.authResult = sbi_gen::AuthResult_Nausf_UEAuthentication{};
                resp_data.authResult->value =
                    sbi_gen::AuthResult_Nausf_UEAuthentication::AUTHENTICATION_SUCCESS;
                resp_data.knrProSe = aka_crypto::to_hex(knr_prose);
                resp_data.nonce2 = aka_crypto::eap::base64_encode(nonce2_vec);
            } else {
                resp_data.eapPayload =
                    aka_crypto::eap::base64_encode(aka_crypto::eap::build_failure(identifier));
                resp_data.authResult = sbi_gen::AuthResult_Nausf_UEAuthentication{};
                resp_data.authResult->value =
                    sbi_gen::AuthResult_Nausf_UEAuthentication::AUTHENTICATION_FAILURE;
            }
            json j = resp_data;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "DELETE",
        std::string(kApiRoot) + "/prose-authentications/{authCtxId}/prose-auth",
        [&verifier, &prose_auth_contexts](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto auth_ctx_id = req.path_params.at("authCtxId");
            if (!prose_auth_contexts.remove(auth_ctx_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No ProSe authentication context " + auth_ctx_id);
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nausf_SoRProtection (ADR-0081, gap-closure task #104) ---

    server.add_route(
        "POST",
        std::string(kSorProtectionApiRoot) + "/{supi}/ue-sor",
        [&verifier, &kausf_store, &sor_protection_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::SorInfo_Nausf_SoRProtection>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto supi = req.path_params.at("supi");

            auto sor_ctx = kausf_store.get(supi);
            if (!sor_ctx.has_value()) {
                return sbi_core::http2::problem_response(404,
                                                         "Not Found",
                                                         "No KAUSF on record for SUPI " + supi +
                                                             " (never authenticated, or this "
                                                             "AUSF instance restarted since)");
            }

            // Real, disclosed scope narrowing (see this file's own header): TS 33.501 clause
            // 6.14.2 NOTE 2 permits the AUSF to construct the SOR header itself (per TS 24.501
            // §9.11.3.51) when the requester didn't supply one -- that NAS-layer bit encoding is
            // a different spec section this build doesn't have in hand, so only the "received
            // from requester" branch is implemented; a request without sorHeader is rejected
            // rather than silently fabricating one.
            if (!body->sorHeader.has_value()) {
                return sbi_core::http2::problem_response(
                    400,
                    "Missing mandatory IE",
                    "This build requires the requester to supply sorHeader -- AUSF-side "
                    "construction of the SOR header (TS 24.501 9.11.3.51) is not implemented");
            }
            const auto sor_header_bytes = aka_crypto::eap::base64_decode(*body->sorHeader);
            if (!sor_header_bytes.has_value()) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", "sorHeader is not valid base64 data");
            }

            // P2 (Steering Info List, TS 33.501 Annex A.17): real only for the SecuredPacket
            // (opaque base64 bytes) form of steeringContainer, which needs no NAS-layer encoding
            // to use directly. The structured (array-of-SteeringInfo) form would need the same
            // TS 24.501 §9.11.3.51 encoding just disclosed as out of scope above -- real,
            // disclosed limitation, not silently wrong: this build omits P2 for that form rather
            // than guessing its wire encoding.
            std::optional<std::vector<uint8_t>> steering_info_list;
            if (body->steeringContainer.has_value()) {
                if (body->steeringContainer->is_string()) {
                    steering_info_list =
                        aka_crypto::eap::base64_decode(body->steeringContainer->get<std::string>());
                    if (!steering_info_list.has_value()) {
                        return sbi_core::http2::problem_response(
                            400,
                            "Bad Request",
                            "steeringContainer (SecuredPacket form) is not valid base64 data");
                    }
                } else {
                    spdlog::warn(
                        "ausf: SoR protection request for SUPI {} carries a structured "
                        "(array-of-SteeringInfo) steeringContainer -- P2 omitted from the "
                        "SoR-MAC-IAUSF computation (NAS-layer encoding, TS 24.501 §9.11.3.51, "
                        "not implemented; see this file's own header)",
                        supi);
                }
            }

            const auto counter = kausf_store.use_counter(supi);
            if (!counter.has_value()) {
                return sbi_core::http2::problem_response(
                    503,
                    "Service Unavailable",
                    "SoR protection service is suspended for SUPI " + supi +
                        " (CounterSoR wrap-around, TS 33.501 6.14.2.3 -- requires a fresh KAUSF)");
            }

            const auto mac_iausf = aka_crypto::derive_sor_mac_iausf(
                sor_ctx->kausf,
                *sor_header_bytes,
                *counter,
                steering_info_list.has_value() ? &*steering_info_list : nullptr);

            sbi_gen::SorSecurityInfo resp_data{};
            resp_data.sorMacIausf =
                aka_crypto::to_hex(std::vector<uint8_t>(mac_iausf.begin(), mac_iausf.end()));
            std::array<uint8_t, 2> counter_be{static_cast<uint8_t>((*counter >> 8) & 0xff),
                                              static_cast<uint8_t>(*counter & 0xff)};
            resp_data.counterSor = aka_crypto::to_hex(counter_be);
            if (body->ackInd) {
                // Real, disclosed scope: computed and returned per spec (the UDM needs it to
                // later verify the UE's own ack), but this build has no later real endpoint that
                // consumes/verifies a returned SoR-MAC-IUE against this cached value yet -- no
                // caller exists in this lab for that verification step, same "computed but not
                // yet consumed downstream" shape as other real-but-not-fully-wired values
                // elsewhere in this project.
                const auto xmac_iue = aka_crypto::derive_sor_mac_iue(sor_ctx->kausf, *counter);
                resp_data.sorXmacIue =
                    aka_crypto::to_hex(std::vector<uint8_t>(xmac_iue.begin(), xmac_iue.end()));
            }

            sor_protection_counter->Add(1);
            json j = resp_data;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    // --- Nausf_UPUProtection (ADR-0195, gap-closure per ADR-0193's audit) ---

    server.add_route(
        "POST",
        std::string(kUpuProtectionApiRoot) + "/{supi}/ue-upu",
        [&verifier, &kausf_store, &upu_protection_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            // Real codegen disambiguation: Nudm_SDM.yaml declares its own, distinct UpuInfo
            // schema, so this operation's own type is suffixed UpuInfo_Nausf_UPUProtection.
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::UpuInfo_Nausf_UPUProtection>(req, err);
            if (!body.has_value()) {
                return err;
            }
            // Real declared `minItems: 1` on upuDataList -- the codegen's structural typing alone
            // (a plain std::vector) doesn't enforce this JSON-Schema constraint, same class of gap
            // as LMF's own `ecgi`/`ncgi` mutual-exclusivity check (ADR-0191).
            if (body->upuDataList.empty()) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", "upuDataList must have at least one entry");
            }
            const auto supi = req.path_params.at("supi");

            auto sor_ctx = kausf_store.get(supi);
            if (!sor_ctx.has_value()) {
                return sbi_core::http2::problem_response(404,
                                                         "Not Found",
                                                         "No KAUSF on record for SUPI " + supi +
                                                             " (never authenticated, or this "
                                                             "AUSF instance restarted since)");
            }

            // Real, disclosed scope limit, same class as Nausf_SoRProtection's own (this file's
            // header): computing UPU-MAC-IAUSF (TS 33.501 Annex A.19) needs the real NAS-layer
            // encoding of "UE Parameters Update Data" per TS 24.501 §9.11.3.53A (starting from
            // octet 23) -- unlike SoR's own sorHeader/steeringContainer fields, this operation's
            // real YAML gives the requester no pre-encoded-bytes alternative to the structured
            // upuDataList, so there is no opaque-bytes shortcut available here the way SoR's own
            // SecuredPacket-form steeringContainer provided. That NAS-layer TLV encoder is not
            // implemented in this project (genuinely out of this session's spec material -- TS
            // 24.501 §9.11.3.53A is a different clause from the ones this project already has in
            // hand). The real KAUSF/CounterUPU state above is checked first so a caller gets a
            // meaningful distinction between "wrong AUSF instance" (404) and "this build cannot
            // compute the MAC yet" (501) -- CounterUPU is deliberately NOT consumed here (no MAC
            // is actually produced), matching the real spec intent that the counter is a freshness
            // input to an actual computation, not a request-attempt tally. upu_protection_counter
            // is captured for the real success path this operation doesn't have yet -- see this
            // comment's own disclosure above; it stays at zero until that real encoder exists.
            return sbi_core::http2::problem_response(
                501,
                "Not Implemented",
                "UPU-MAC-IAUSF computation requires TS 24.501 §9.11.3.53A NAS-layer encoding of "
                "UE Parameters Update Data, not implemented in this build");
        });

    std::thread(run_nrf_lifecycle, ausf_instance_id, nrf_base).detach();

    server.start();
    spdlog::info("ausf: listening on https://0.0.0.0:{} (TLS 1.3 + mTLS)", port);
    spdlog::info("ausf: Prometheus metrics at http://{}/metrics", metrics_bind_address);
    sbi_core::run_multi_threaded(ioc);
    return 0;
}
