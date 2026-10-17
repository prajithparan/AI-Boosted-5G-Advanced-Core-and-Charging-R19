// nfs/aanf: AAnF -- AKMA Anchor Function, increment 1 (ADR-0495).
//
// Stage 2: TS 33.535 V19.0.0 (specs/3gpp/TS_33.535_j00.txt). Stage 3: TS 29.535 V19.5.0 and
// TS29535_Naanf_AKMA.yaml (specs/5G_APIs-REL-19, forge.3gpp.org REL-19, commit bca84b6; request /
// response bodies from TS29522_AKMA.yaml), DTOs generated into sbi_gen. API root
// {apiRoot}/naanf-akma/v1:
//
//   POST /register-anchorkey       Naanf_AKMA_AnchorKey_Register (TS 33.535 6.1 step 4-5): the
//                                  AUSF's A-KID + KAKMA for a SUPI; the latest replaces the older
//                                  (6.1 NOTE 1a)
//   POST /retrieve-applicationkey  Naanf_AKMA_ApplicationKey_Get / _AnonUser_Get (6.2.1, 6.2.2,
//                                  6.3): KAF = KDF(KAKMA, AF_ID) (Annex A.4), its expiry, and the
//                                  SUPI unless anonymous or not authorized
//   POST /remove-context           Naanf_AKMA_Context_Remove (6.6)
//
// AKMA contexts live in Valkey, shared by every replica.
//
// DISCLOSED, not hidden:
//   * Roaming check (6.2.1 steps 5-7, ADR-0497): with `deny_roaming_ues`, an immediate
//     ROAMING_STATUS report from the UDM; a roaming UE is refused ROAMING_AKMA_SERVICE_DENIED. The
//     subscription is removed after the report (no change notifications); no report -> provided.
//     GPSI (steps 3-4, ADR-0496): for the AFs in `gpsi_af_fqdns`, from the UDM's
//     id-translation-result, instead of the SUPI; not stored in the AKMA context (fetched per
//     request).
//   * No AKMA service-disable notification (6.8): notifUri is accepted and ignored.
//   * The AF authorization of 6.2.1 step 2 is the OAuth2 token (when present) plus the configured
//     `allowed_af_fqdns` list (empty = any AF).
//   * kAkma / kaf are hexadecimal strings: TS 29.535 / 29.522 type them "string" without an
//     encoding; hex follows the key fields of TS 29.509 (kseaf, kausf).

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

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "TS29522_AKMA.hpp"
#include "TS29535_Naanf_AKMA.hpp"
#include "aka_crypto/akma.hpp"
#include "aka_crypto/hex.hpp"
#include "akma_store.hpp"
#include "nf_config/nf_config.hpp"
#include "nf_config/redis.hpp"

namespace {

using nlohmann::json;
namespace http2 = sbi_core::http2;

#ifndef CERTS_DIR
#error "CERTS_DIR must be defined by CMake (see nfs/aanf/CMakeLists.txt)"
#endif
#ifndef CONFIG_DIR
#error "CONFIG_DIR must be defined by CMake (see nfs/aanf/CMakeLists.txt)"
#endif

constexpr const char* kApiRoot = "/naanf-akma/v1";
// Must match nfs/nrf/src/main.cpp's kNrfInstanceId exactly -- see docs/DECISIONS.md ADR-0018.
constexpr const char* kNrfInstanceId = "5ba9a927-1d31-4c8e-8a10-000000000001";
constexpr const char* kSupiScope = "naanf-akma:applicationkeyget:supi-access";

http2::TlsConfig aanf_tls() {
    return http2::TlsConfig{
        .cert_path = CERTS_DIR "/aanf/cert.pem",
        .key_path = CERTS_DIR "/aanf/key.pem",
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

// The verified mTLS peer's certificate names `name` (CN or a dNSName SAN, case-insensitive). The
// lab PKI names each NF by its type (scripts/gen-lab-pki.sh) and each AF by its FQDN; the NRF
// issues tokens only against such a binding (ADR-0499 phase 2).
bool peer_is(const http2::Request& req, const std::string& name) {
    const auto same = [&](const std::string& v) {
        return v.size() == name.size() &&
               std::equal(v.begin(), v.end(), name.begin(), [](char a, char b) {
                   return std::tolower(static_cast<unsigned char>(a)) ==
                          std::tolower(static_cast<unsigned char>(b));
               });
    };
    return same(req.peer_cert_cn) || std::ranges::any_of(req.peer_cert_dns_names, same);
}

// A missing Authorization header is not itself a 401: the YAML's `security: [{}, ...]` (same
// reading as every other NF here, see nfs/nrf/src/main.cpp).
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

std::string rfc3339(std::chrono::system_clock::time_point t) {
    const std::time_t tt = std::chrono::system_clock::to_time_t(t);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// The FQDN part of a TS 29.522 AfId "<FQDN>.<10 hex digits>".
std::string af_fqdn(const std::string& af_id) {
    return af_id.substr(0, af_id.rfind('.'));
}

void run_nrf_lifecycle(const std::string& instance_id, const std::string& nrf_base) {
    http2::Client http_client(aanf_tls());
    // ADR-0499 phase 3: every client of this NF sends each API's token from here on; until the
    // NRF issues one (after NFRegister, TS 33.501 13.4.1.1.2) requests go without.
    sbi_core::install_default_token_source(
        http_client.tls(), nrf_base + "/oauth2/token", instance_id);
    constexpr int kHeartbeatSeconds = 30;
    const json profile{{"nfInstanceId", instance_id},
                       {"nfType", "AANF"},
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
            spdlog::info("aanf: registered with NRF (HTTP {})", r->status);
            // ADR-0499: a token refused before NFRegister is asked for again now.
            sbi_core::clear_default_token_backoff();
            break;
        }
        spdlog::warn("aanf: NRF registration attempt failed, retrying in 5s");
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
            spdlog::warn("aanf: NRF heartbeat failed");
        }
    }
}

} // namespace

int main() {
    sbi_core::init_logging("aanf");
    sbi_core::init_tracing("aanf");

    const auto config = nf_config::load("aanf", CONFIG_DIR);
    const auto port = nf_config::require<unsigned short>(config, "port", "AANF_PORT");
    const auto bind = nf_config::require<std::string>(config, "sbi_bind_address");
    const auto metrics_bind_address = nf_config::require<std::string>(
        config, "metrics_bind_address", "AANF_METRICS_BIND_ADDRESS");
    const auto nrf_base =
        nf_config::require<std::string>(config, "nrf_base_url", "AANF_NRF_BASE_URL");
    const auto redis_url = nf_config::require<std::string>(config, "redis_url", "AANF_REDIS_URL");
    const auto kaf_lifetime = std::chrono::seconds(
        nf_config::require<int>(config, "kaf_lifetime_seconds", "AANF_KAF_LIFETIME_SECONDS"));
    // Local policy for an AF calling without a token: whether the SUPI may be returned (6.2.1
    // step 8, "Whether to send SUPI or GPSI is determined by AAnF based on the local policy").
    const bool supi_without_token = config.value("supi_to_af_without_token", false);
    const auto allowed_afs = config.value("allowed_af_fqdns", std::vector<std::string>{});
    // ADR-0496, TS 33.535 6.2.1 steps 3-4 / 8: AFs that local policy gives the GPSI instead of the
    // SUPI; the GPSI comes from the UDM (Nudm_SDM id-translation-result).
    const auto gpsi_afs = config.value("gpsi_af_fqdns", std::vector<std::string>{});
    const auto udm_base = config.value("udm_base_url", std::string{});
    // ADR-0497, TS 33.535 6.2.1 steps 5-7 (feature RoamingRestriction): deny AKMA to a roaming UE.
    const bool deny_roaming = config.value("deny_roaming_ues", false);
    const auto self_base = config.value("self_base_url", std::string{});

    sbi_core::init_metrics(metrics_bind_address);
    auto meter = sbi_core::get_meter("aanf");
    auto registered = meter->CreateUInt64Counter("aanf_anchor_keys_registered_total",
                                                 "Naanf_AKMA_AnchorKey_Register requests stored");
    auto kafs = meter->CreateUInt64Counter("aanf_application_keys_total",
                                           "KAF delivered by Naanf_AKMA_ApplicationKey_Get");

    auto redis = nf_config::connect_redis_or_die(redis_url, "aanf");
    aanf::AkmaStore store(redis);
    http2::Client udm_client(aanf_tls());
    sbi_core::jwt::Verifier verifier(CERTS_DIR "/nrf-jwt/public.pem", kNrfInstanceId);
    const std::string instance_id = sbi_core::generate_uuid_v4();
    spdlog::info("aanf: starting, nfInstanceId={}", instance_id);

    boost::asio::io_context ioc;
    http2::Server server(ioc, bind, port, aanf_tls());
    // ADR-0499 phase 5 / REVIEW SEC-03: every route needs an access token for the AAnF (aud) and
    // scope naanf-akma unless `oauth2_required` is false; the UDM notification route is exempt.
    server.set_authorizer(std::make_shared<const sbi_core::http2::Authorizer>(
        sbi_core::http2::AuthorizerConfig{
            .required = nf_config::optional<bool>(config, "oauth2_required", "AANF_OAUTH2_REQUIRED")
                            .value_or(true),
            .nf_type = "AANF",
            .nf_instance_id = instance_id,
        },
        std::make_shared<sbi_core::jwt::Verifier>(CERTS_DIR "/nrf-jwt/public.pem",
                                                  kNrfInstanceId)));

    // ---- Naanf_AKMA_AnchorKey_Register (TS 29.535 5.2.2.2) -------------------------------------
    server.add_route(
        "POST", std::string(kApiRoot) + "/register-anchorkey", [&](const http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return problem(401, "Unauthorized", auth->error);
            }
            // SEC-03: only the AUSF registers anchor keys (TS 33.535 6.1). No TS 29.500 / 29.535
            // cause describes a wrong consumer, so none is given.
            if (!peer_is(req, "ausf")) {
                return problem(403, "Forbidden", "only the AUSF may register an AKMA anchor key");
            }
            sbi_gen::AkmaKeyInfo body;
            try {
                body = json::parse(req.body).get<sbi_gen::AkmaKeyInfo>();
            } catch (const std::exception& e) {
                return problem(400, "Bad Request", e.what(), "INVALID_MSG_FORMAT");
            }
            const auto kakma = aka_crypto::from_hex<32>(body.kAkma);
            if (!kakma || body.aKId.empty() || body.aKId.find('@') == std::string::npos) {
                return problem(400,
                               "Bad Request",
                               "kAkma must be 64 hexadecimal digits and aKId an NAI",
                               "MANDATORY_IE_INCORRECT");
            }
            if (!body.supi || body.gpsi) {
                // The YAML's oneOf allows GPSI; the AAnF keys contexts by SUPI (6.6), which the
                // AUSF always has.
                return problem(400,
                               "Bad Request",
                               "the AKMA context is registered by SUPI",
                               "MANDATORY_IE_MISSING");
            }
            store.put(aanf::AkmaContext{*body.supi, body.aKId, *kakma});
            registered->Add(1);
            spdlog::info("aanf: AKMA context for {} registered (A-KID {})", *body.supi, body.aKId);
            return http2::Response::json(200, json(body).dump());
        });

    // ---- Naanf_AKMA_ApplicationKey_Get / _AnonUser_Get (TS 29.535 5.2.2.3) ---------------------
    server.add_route(
        "POST", std::string(kApiRoot) + "/retrieve-applicationkey", [&](const http2::Request& req) {
            const auto auth = check_bearer(req, verifier);
            if (auth && !auth->valid) {
                return problem(401, "Unauthorized", auth->error);
            }
            sbi_gen::AkmaAfKeyRequest body;
            try {
                body = json::parse(req.body).get<sbi_gen::AkmaAfKeyRequest>();
            } catch (const std::exception& e) {
                return problem(400, "Bad Request", e.what(), "INVALID_MSG_FORMAT");
            }
            const auto af_id = aka_crypto::af_id_octets(body.afId);
            if (!af_id) {
                return problem(400,
                               "Bad Request",
                               "afId must be \"<FQDN>.<Ua* security protocol id: 10 hex digits>\"",
                               "MANDATORY_IE_INCORRECT");
            }
            // SEC-03: the afId is the caller's own -- an AF's certificate names its FQDN -- unless
            // the caller is the NEF, which verified the AF_ID of the external AF it relays for
            // (TS 33.535 6.3 step 2).
            if (!peer_is(req, "nef") && !peer_is(req, af_fqdn(body.afId))) {
                return problem(403,
                               "Forbidden",
                               "afId " + af_fqdn(body.afId) +
                                   " is not the calling AF's (its certificate does not name it)");
            }
            if (!allowed_afs.empty() &&
                std::ranges::find(allowed_afs, af_fqdn(body.afId)) == allowed_afs.end()) {
                return problem(403, "Forbidden", "AF " + af_fqdn(body.afId) + " is not authorized");
            }
            const auto ctx = store.by_akid(body.aKId);
            if (!ctx) {
                return problem(
                    403, "Forbidden", "no KAKMA for A-KID " + body.aKId, "K_AKMA_NOT_PRESENT");
            }
            // Steps 5-7: the UE's roaming status from the UDM (Nudm_EventExposure, ROAMING_STATUS
            // with an immediate report). This UDM sends no later change notifications, so the
            // subscription is removed straight after the report is read (ADR-0497).
            if (deny_roaming && !udm_base.empty()) {
                http2::ClientRequest sub;
                sub.method = "POST";
                sub.url = udm_base + "/nudm-ee/v1/" + ctx->supi + "/ee-subscriptions";
                sub.headers.emplace("content-type", "application/json");
                sub.body =
                    json{{"callbackReference", self_base + "/aanf-ee-notify"},
                         {"monitoringConfigurations",
                          json{{"1",
                                json{{"eventType", "ROAMING_STATUS"}, {"immediateFlag", true}}}}}}
                        .dump();
                const auto r = udm_client.send(sub);
                std::optional<bool> roaming;
                if (r && r->status == 201) {
                    const auto created = json::parse(r->body, nullptr, false);
                    if (created.is_object() && created.contains("eventReports")) {
                        for (const auto& rep : created.at("eventReports")) {
                            if (rep.value("eventType", "") == "ROAMING_STATUS" &&
                                rep.contains("report")) {
                                roaming = rep.at("report").value("roaming", false);
                            }
                        }
                    }
                    if (const auto loc = r->headers.find("location"); loc != r->headers.end()) {
                        http2::ClientRequest del;
                        del.method = "DELETE";
                        del.url = udm_base + loc->second;
                        (void)udm_client.send(del);
                    }
                }
                if (roaming.value_or(false)) {
                    return problem(403,
                                   "Forbidden",
                                   "AKMA is not provided to a roaming UE (local policy)",
                                   "ROAMING_AKMA_SERVICE_DENIED");
                }
                if (!roaming) {
                    spdlog::warn("aanf: no roaming status for {} from the UDM; AKMA provided",
                                 ctx->supi);
                }
            }
            const auto kaf = aka_crypto::derive_kaf(ctx->kakma, *af_id);
            sbi_gen::AkmaAfKeyData out;
            out.kaf = aka_crypto::to_hex(kaf);
            out.expiry = rfc3339(std::chrono::system_clock::now() + kaf_lifetime);
            const bool anonymous = body.anonInd.value_or(false);
            const bool supi_allowed =
                auth ? auth->scope.find(kSupiScope) != std::string::npos : supi_without_token;
            const bool wants_gpsi =
                std::ranges::find(gpsi_afs, af_fqdn(body.afId)) != gpsi_afs.end();
            if (!anonymous && wants_gpsi && !udm_base.empty()) {
                http2::ClientRequest q;
                q.method = "GET";
                q.url = udm_base + "/nudm-sdm/v2/" + ctx->supi + "/id-translation-result";
                if (const auto r = udm_client.send(q); r && r->status == 200) {
                    const auto t = json::parse(r->body, nullptr, false);
                    if (t.is_object() && t.contains("gpsi") && t.at("gpsi").is_string()) {
                        out.gpsi = t.at("gpsi").get<std::string>();
                    }
                }
                if (!out.gpsi) {
                    return problem(500,
                                   "Internal Server Error",
                                   "no GPSI could be fetched from the UDM for this UE",
                                   "UNSPECIFIED_NF_FAILURE");
                }
            } else if (!anonymous && supi_allowed) {
                out.supi = ctx->supi;
            }
            kafs->Add(1);
            return http2::Response::json(200, json(out).dump());
        });

    // ---- Naanf_AKMA_Context_Remove (TS 29.535 5.2.2.4) -----------------------------------------
    server.add_route(
        "POST", std::string(kApiRoot) + "/remove-context", [&](const http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth && !auth->valid) {
                return problem(401, "Unauthorized", auth->error);
            }
            if (!peer_is(req, "ausf")) { // SEC-03, as for register-anchorkey
                return problem(403, "Forbidden", "only the AUSF may remove an AKMA context");
            }
            sbi_gen::CtxRemove body;
            try {
                body = json::parse(req.body).get<sbi_gen::CtxRemove>();
            } catch (const std::exception& e) {
                return problem(400, "Bad Request", e.what(), "INVALID_MSG_FORMAT");
            }
            if (!body.supi) {
                return problem(400, "Bad Request", "supi is required", "MANDATORY_IE_MISSING");
            }
            if (!store.remove_supi(*body.supi)) {
                return problem(404,
                               "Not Found",
                               "no AKMA context for " + *body.supi,
                               "AKMA_CONTEXT_NOT_FOUND");
            }
            http2::Response r;
            r.status = 204;
            return r;
        });

    // The callbackReference given to the UDM's ROAMING_STATUS subscription (ADR-0497). The UDM
    // sends no later notifications today and the AAnF unsubscribes right after the immediate
    // report; served so a notification that does arrive is accepted, not 404.
    server.add_route(
        "POST",
        "/aanf-ee-notify",
        [](const http2::Request&) {
            http2::Response r;
            r.status = 204;
            return r;
        },
        // TS 33.501 13.4.1.0: OAuth 2.0 does not apply to the notification operation (ADR-0499).
        http2::RouteAuth::None);

    std::thread(run_nrf_lifecycle, instance_id, nrf_base).detach();
    server.start();
    spdlog::info("aanf: listening on https://{}:{} (TLS 1.3 + mTLS)", bind, port);
    sbi_core::run_multi_threaded(ioc);
    return 0;
}
