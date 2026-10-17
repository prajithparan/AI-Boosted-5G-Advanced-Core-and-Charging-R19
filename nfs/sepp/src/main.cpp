// nfs/sepp: SEPP -- Security Edge Protection Proxy, increment 1 (ADR-0491): N32-c and N32-f with
// TLS security.
//
// Stage 3: TS 29.573 V19.7.0 (specs/3gpp/TS_29.573_j70.txt) and TS29573_N32_Handshake.yaml
// (specs/5G_APIs-REL-19, forge.3gpp.org REL-19, commit bca84b6; info.version 1.4.0, DTOs generated
// into sbi_gen). Routing: TS 29.500 V19.7.0 6.1.4.3. FQDNs: TS 23.003 28.2 / 28.5.
//
//   N32-c  {apiRoot}/n32c-handshake/v1/exchange-capability  5.2.2 Security Capability Negotiation
//                                                          (TLS; NONE tears the context down)
//          {apiRoot}/n32c-handshake/v1/exchange-params     5.2.3 PRINS parameter exchange
//                                                          (cipher suites, protection policy)
//          {apiRoot}/n32c-handshake/v1/n32f-terminate      5.2.4 (PRINS N32-f contexts)
//          {apiRoot}/n32c-handshake/v1/n32f-error          5.2.5 (logged, 204)
//   N32-f  any other path on the N32 port                  5.3.3 forwarding with TLS security
//   NF side any path on the SBI port                       6.1.4.3.3: an NF in this PLMN sends the
//                                                          request with 3gpp-Sbi-Target-apiRoot, or
//                                                          6.1.4.3.2: to <label>.<this SEPP's FQDN>
//           {apiRoot}/nsepp-telescopic/v1/mapping          5.4.2 / 5.4.3 (ADR-0492)
//
// Two servers: the SBI port faces the NFs of this PLMN, the N32 port faces partner SEPPs. Both are
// TLS 1.3 + mTLS (the N32-c "end to end TLS" of 5.2.1 is the N32 port's TLS).
//
// DISCLOSED, not hidden:
//   * PRINS (ADR-0493): selectable in 5.2.2 by `security_capabilities`; 5.2.3 cipher suites with
//     the N32-f master key exported from the TLS connection carrying the request, and the
//     protection policy exchange; 5.2.4 termination. JOSE-protected forwarding (5.3.2) is NOT built
//     yet: traffic towards a PRINS peer gets 504. No RI security information, no security profiles.
//   * N32-f always carries 3gpp-Sbi-Target-apiRoot (both SEPPs indicate support -- this one
//     always does); forwarding to a peer that does not support the header (the target URI itself
//     on N32-f) is not built. Telescopic FQDNs (ADR-0492): served; only the Location header of a
//     response is rewritten into telescopic form, not URIs inside bodies (NF profiles, callbacks),
//     and an unknown label is not looked up at other SEPPs of this PLMN (single SEPP here).
//   * The receiving SEPP reaches an NF of its own PLMN through `local_nf_routes` in config (the
//     target authority -> a reachable apiRoot): it stands in for the DNS resolution of the
//     3gppnetwork.org FQDN, which this lab does not run.
//   * Roaming intermediaries (5.5), N32-f keepalive PINGs (5.3.3.5), TLSCOR OPTIONS (5.3.3.2.2),
//     SNDN32F (senderN32fFqdn / port), topology hiding and message filtering are not built.
//   * The SEPP does not validate the NFs' OAuth2 tokens (they are for the target producer).
//   * NRF: registered as nfType SEPP (fqdn, plmnList) with heartbeat; no SeppInfo (ports / N32
//     purposes) is advertised, and NFs do not discover the SEPP through the NRF here.

#include "sbi_core/http2_client.hpp"
#include "sbi_core/http2_server.hpp"
#include "sbi_core/io_context_pool.hpp"
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

#include <atomic>
#include <chrono>
#include <functional>
#include <limits>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "TS29573_JOSEProtectedMessageForwarding.hpp"
#include "TS29573_SeppTelescopicFqdnMapping.hpp"
#include "n32.hpp"
#include "nf_config/nf_config.hpp"
#include "prins_codec.hpp"

namespace {

using nlohmann::json;
namespace http2 = sbi_core::http2;

#ifndef CERTS_DIR
#error "CERTS_DIR must be defined by CMake (see nfs/sepp/CMakeLists.txt)"
#endif
#ifndef CONFIG_DIR
#error "CONFIG_DIR must be defined by CMake (see nfs/sepp/CMakeLists.txt)"
#endif

constexpr const char* kHandshakeRoot = "/n32c-handshake/v1";
constexpr const char* kTargetApiRootHeader = "3gpp-sbi-target-apiroot";
constexpr const char* kHandshakeIdHeader = "3gpp-sbi-n32-handshake-id";
constexpr const char* kPurposeHeader = "3gpp-sbi-interplmn-purpose";

http2::TlsConfig sepp_tls() {
    return http2::TlsConfig{
        .cert_path = CERTS_DIR "/sepp/cert.pem",
        .key_path = CERTS_DIR "/sepp/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
}

// TS 29.500 5.2.7: ProblemDetails with the application error cause; the Server header names this
// SEPP as the originator (TS 29.500 6.10.10 / table 5.2.2.2-2: "SEPP-<SEPP FQDN>").
http2::Response problem(int status,
                        const std::string& title,
                        const std::string& detail,
                        const std::optional<std::string>& cause,
                        const std::string& own_fqdn) {
    http2::Response r;
    r.status = status;
    r.headers.emplace("content-type", "application/problem+json");
    r.headers.emplace("server", "SEPP-" + own_fqdn);
    r.body = json(sbi_core::make_problem_details(status, title, detail, cause)).dump();
    return r;
}

std::optional<std::string> header(const http2::Request& req, const char* name) {
    const auto it = req.headers.find(name);
    return it == req.headers.end() ? std::nullopt : std::optional{it->second};
}

std::string random_handshake_id() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(rng()));
    return buf;
}

// Headers that describe one hop, not the message: never relayed.
bool hop_by_hop(const std::string& name) {
    return name.empty() || name.front() == ':' || name == "host" || name == "content-length" ||
           name == "connection" || name == "keep-alive" || name == "te" ||
           name == "transfer-encoding" || name == "upgrade" || name == "proxy-connection";
}

std::string lower(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// Relays `req` to `url`, dropping `drop` headers, adding `extra`; the Via header gets this SEPP
// appended on the request and on the response (TS 29.500 6.10.10.3).
http2::Response relay(http2::Client& client,
                      const http2::Request& req,
                      const std::string& url,
                      const std::vector<std::string>& drop,
                      const std::vector<std::pair<std::string, std::string>>& extra,
                      const std::string& own_fqdn) {
    const std::string me = "2.0 SEPP-" + own_fqdn;
    http2::ClientRequest out;
    out.method = req.method;
    out.url = url;
    out.body = req.body;
    std::string via;
    for (const auto& [name, value] : req.headers) {
        const std::string n = lower(name);
        if (hop_by_hop(n) || std::ranges::find(drop, n) != drop.end()) {
            continue;
        }
        if (n == "via") {
            via = via.empty() ? value : via + ", " + value;
            continue;
        }
        out.headers.emplace(n, value);
    }
    for (const auto& [name, value] : extra) {
        out.headers.emplace(name, value);
    }
    out.headers.emplace("via", via.empty() ? me : via + ", " + me);

    auto r = client.send(out);
    if (!r) {
        return problem(504,
                       "Gateway Timeout",
                       "next hop not reachable: " + r.error(),
                       "TARGET_NF_NOT_REACHABLE",
                       own_fqdn);
    }
    http2::Response resp;
    resp.status = static_cast<int>(r->status);
    std::string rvia;
    for (const auto& [name, value] : r->headers) {
        const std::string n = lower(name);
        if (hop_by_hop(n)) {
            continue;
        }
        if (n == "via") {
            rvia = rvia.empty() ? value : rvia + ", " + value;
            continue;
        }
        resp.headers.emplace(n, value);
    }
    resp.headers.emplace("via", rvia.empty() ? me : rvia + ", " + me);
    resp.body = std::move(r->body);
    return resp;
}

// "https://h:p/prefix" -> {"h:p", "/prefix"}.
std::pair<std::string, std::string> split_api_root(const std::string& api_root) {
    std::string s = api_root;
    if (const auto scheme = s.find("://"); scheme != std::string::npos) {
        s = s.substr(scheme + 3);
    }
    const auto slash = s.find('/');
    if (slash == std::string::npos) {
        return {lower(s), ""};
    }
    std::string prefix = s.substr(slash);
    while (!prefix.empty() && prefix.back() == '/') {
        prefix.pop_back();
    }
    return {lower(s.substr(0, slash)), prefix};
}

// NRF registration + heartbeat (TS 29.510 NFRegister / NFUpdate), same shape as every NF here
// (e.g. nfs/eir run_nrf_lifecycle); on its own thread, never on the server's io_context.
void run_nrf_lifecycle(const std::string& instance_id,
                       const std::string& nrf_base,
                       const std::string& fqdn,
                       const std::string& mcc,
                       const std::string& mnc,
                       const std::atomic<bool>& running) {
    http2::Client http_client(sepp_tls());
    // ADR-0499 phase 3: tokens on this client only -- a relay must not put its own token on
    // the requests it forwards.
    sbi_core::TokenSource nrf_tokens(http_client, nrf_base + "/oauth2/token", instance_id);
    sbi_core::attach_tokens(http_client, nrf_tokens);
    constexpr int kHeartbeatSeconds = 30;
    const json profile{
        {"nfInstanceId", instance_id},
        {"nfType", "SEPP"},
        {"nfStatus", "REGISTERED"},
        {"fqdn", fqdn},
        {"plmnList", json::array({json{{"mcc", mcc}, {"mnc", mnc}}})},
        {"ipv4Addresses", json::array({"127.0.0.1"})},
        {"heartBeatTimer", kHeartbeatSeconds},
    };
    const auto pause = [&](int seconds) {
        for (int i = 0; i < seconds * 10 && running.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return running.load();
    };
    bool registered = false;
    while (!registered && running.load()) {
        http2::ClientRequest put;
        put.method = "PUT";
        put.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + instance_id;
        put.headers.emplace("content-type", "application/json");
        put.headers.emplace(
            sbi_core::headers::kSenderTimestamp,
            sbi_core::headers::format_sender_timestamp(std::chrono::system_clock::now()));
        put.body = profile.dump();
        const auto r = http_client.send(put);
        registered = r && (r->status == 200 || r->status == 201);
        if (registered) {
            spdlog::info("sepp: registered with NRF (HTTP {})", r->status);
            // ADR-0499: a token refused before NFRegister is asked for again now.
            nrf_tokens.clear_backoff();
            break;
        }
        spdlog::warn("sepp: NRF registration attempt failed, retrying in 5s");
        pause(5);
    }
    while (pause(kHeartbeatSeconds / 2)) {
        http2::ClientRequest patch;
        patch.method = "PATCH";
        patch.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + instance_id;
        patch.headers.emplace("content-type", "application/json-patch+json");
        patch.body =
            json::array({json{{"op", "replace"}, {"path", "/nfStatus"}, {"value", "REGISTERED"}}})
                .dump();
        if (const auto r = http_client.send(patch); !r || r->status != 200) {
            spdlog::warn("sepp: NRF heartbeat failed");
        }
    }
}

// The consumer PLMN in a Bearer token's claims, if the token carries one (TS 29.510 6.3.5.2.4
// AccessTokenClaims.consumerPlmnId). The token's signature is the producer's to verify; this only
// reads the claim for the N32-f PLMN check (TS 29.573 5.3.2.1 step 6).
std::optional<std::pair<std::string, std::string>> token_consumer_plmn(const std::string& auth) {
    constexpr std::string_view kBearer = "Bearer ";
    if (auth.compare(0, kBearer.size(), kBearer) != 0) {
        return std::nullopt;
    }
    const auto parts = auth.substr(kBearer.size());
    const auto a = parts.find('.');
    const auto b = parts.find('.', a == std::string::npos ? a : a + 1);
    if (a == std::string::npos || b == std::string::npos) {
        return std::nullopt;
    }
    const auto raw = sepp::base64url_decode(parts.substr(a + 1, b - a - 1));
    if (!raw) {
        return std::nullopt;
    }
    const json claims = json::parse(raw->begin(), raw->end(), nullptr, false);
    if (!claims.is_object() || !claims.contains("consumerPlmnId") ||
        !claims.at("consumerPlmnId").is_object()) {
        return std::nullopt;
    }
    const auto& plmn = claims.at("consumerPlmnId");
    return std::pair{plmn.value("mcc", std::string{}), plmn.value("mnc", std::string{})};
}

std::string hex64(std::uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

} // namespace

int main() {
    sbi_core::init_logging("sepp");
    sbi_core::init_tracing("sepp");

    const auto config = nf_config::load("sepp", CONFIG_DIR);
    const auto port = nf_config::require<unsigned short>(config, "port", "SEPP_PORT");
    const auto n32_port = nf_config::require<unsigned short>(config, "n32_port", "SEPP_N32_PORT");
    const auto bind = nf_config::require<std::string>(config, "sbi_bind_address");
    const auto metrics_bind_address = nf_config::require<std::string>(
        config, "metrics_bind_address", "SEPP_METRICS_BIND_ADDRESS");
    const auto retry_seconds = config.value("handshake_retry_seconds", 2);
    const auto nrf_base =
        nf_config::require<std::string>(config, "nrf_base_url", "SEPP_NRF_BASE_URL");

    sepp::NegotiationPolicy policy;
    policy.own_fqdn = lower(nf_config::require<std::string>(config, "fqdn", "SEPP_FQDN"));
    policy.own_mcc = nf_config::require<std::string>(config, "mcc", "SEPP_MCC");
    policy.own_mnc = nf_config::require<std::string>(config, "mnc", "SEPP_MNC");
    policy.allowed_purposes = config.at("allowed_purposes").get<std::vector<std::string>>();
    if (config.contains("security_capabilities")) {
        policy.security_capabilities =
            config.at("security_capabilities").get<std::vector<std::string>>();
    }
    if (config.contains("protection_policy")) {
        policy.protection_policy = config.at("protection_policy").get<sbi_gen::ProtectionPolicy>();
    }
    for (const auto& p : config.at("peers")) {
        policy.peers.push_back(sepp::PeerConfig{lower(p.at("fqdn").get<std::string>()),
                                                p.at("mcc").get<std::string>(),
                                                p.at("mnc").get<std::string>(),
                                                p.at("n32_api_root").get<std::string>(),
                                                p.value("initiate", false)});
    }
    // Target authority ("host:port", lower case) -> apiRoot this SEPP can reach (see header).
    std::map<std::string, std::string> local_nf_routes;
    if (config.contains("local_nf_routes")) {
        for (const auto& [authority, api_root] : config.at("local_nf_routes").items()) {
            local_nf_routes.emplace(lower(authority), api_root.get<std::string>());
        }
    }
    const std::string own_domain = sepp::home_network_domain(policy.own_mcc, policy.own_mnc);

    sbi_core::init_metrics(metrics_bind_address);
    auto meter = sbi_core::get_meter("sepp");
    auto handshakes = meter->CreateUInt64Counter(
        "sepp_n32c_handshakes_total", "N32-c security capability negotiations completed");
    auto forwarded = meter->CreateUInt64Counter("sepp_n32f_forwarded_total",
                                                "Requests relayed over N32-f (both directions)");
    auto refused = meter->CreateUInt64Counter("sepp_n32f_refused_total",
                                              "N32-f requests refused (no context, bad target)");

    spdlog::info(
        "sepp: {} serving PLMN {} ({} peer(s))", policy.own_fqdn, own_domain, policy.peers.size());

    sepp::ContextStore contexts;
    sepp::TelescopicTable telescopic;
    http2::Client client(sepp_tls());

    boost::asio::io_context ioc;
    http2::Server sbi(ioc, bind, port, sepp_tls());
    http2::Server n32(ioc, bind, n32_port, sepp_tls());
    // ADR-0493: every N32-c connection exports the N32-f master key candidate (TS 33.501
    // 13.2.4.4.1); exchange-params uses the one of the connection it arrived on.
    n32.set_tls_exporters({{sepp::kN32MasterExporterLabel, sepp::kN32MasterKeyLength}});
    sepp::PrinsStore prins;
    // TS 33.501 13.2.4.4.1: "A different sequence counter shall be maintained for each IV salt".
    std::mutex seq_mutex;
    std::map<std::string, std::uint32_t> seqs;
    // The 32-bit SEQ must never wrap under one key (a repeated nonce, AES-GCM's fatal case): at
    // exhaustion the context refuses to send until it is renegotiated (review fix, ADR-0494).
    const auto next_seq = [&](const std::string& context_id,
                              sepp::KeyUse use) -> std::optional<std::uint32_t> {
        const std::lock_guard<std::mutex> lock(seq_mutex);
        auto& seq = seqs[context_id + "/" + std::to_string(static_cast<int>(use))];
        if (seq == std::numeric_limits<std::uint32_t>::max()) {
            return std::nullopt;
        }
        return seq++;
    };
    std::atomic<std::uint64_t> message_counter{1};
    // TS 29.573 6.1.5.3.2 "sender ... shall uniquely identify the SEPP that is sending the
    // request": the claimed FQDN must be one the mTLS client certificate names (review fix --
    // without it a partner could claim another partner's identity and obtain or tear down that
    // partner's N32 context).
    const auto sender_authenticated = [](const http2::Request& req, const std::string& sender) {
        const std::string want = lower(sender);
        return std::ranges::any_of(req.peer_cert_dns_names,
                                   [&](const std::string& n) { return lower(n) == want; });
    };

    // ---- N32-c (TS 29.573 6.1) ----------------------------------------------------------------
    n32.add_route(
        "POST",
        std::string(kHandshakeRoot) + "/exchange-capability",
        [&](const http2::Request& req) {
            sbi_gen::SecNegotiateReqData body;
            try {
                body = json::parse(req.body).get<sbi_gen::SecNegotiateReqData>();
            } catch (const std::exception& e) {
                return problem(400, "Bad Request", e.what(), "INVALID_MSG_FORMAT", policy.own_fqdn);
            }
            if (!sender_authenticated(req, body.sender)) {
                return problem(403,
                               "Forbidden",
                               "sender " + body.sender +
                                   " is not the SEPP the client certificate authenticates",
                               "NEGOTIATION_NOT_ALLOWED",
                               policy.own_fqdn);
            }
            const auto d = sepp::negotiate(policy, body, contexts.outgoing(lower(body.sender)));
            if (!d.response) {
                spdlog::warn("sepp: exchange-capability from {} refused: {} ({})",
                             body.sender,
                             d.cause.value_or(""),
                             d.detail);
                return problem(d.status,
                               d.status == 409   ? "Conflict"
                               : d.status == 400 ? "Bad Request"
                                                 : "Forbidden",
                               d.detail,
                               d.cause,
                               policy.own_fqdn);
            }
            if (d.teardown) {
                contexts.erase(lower(body.sender));
                spdlog::info("sepp: N32 context with {} torn down by the peer", body.sender);
            } else if (d.context) {
                contexts.put(*d.context);
                handshakes->Add(1);
                spdlog::info("sepp: N32 context with {} (TLS, handshake id {})",
                             d.context->peer_fqdn,
                             d.context->handshake_id.value_or("-"));
            }
            return http2::Response::json(200, json(*d.response).dump());
        });
    // 5.2.3 Parameter Exchange (PRINS only; ADR-0493).
    n32.add_route(
        "POST", std::string(kHandshakeRoot) + "/exchange-params", [&](const http2::Request& req) {
            sbi_gen::SecParamExchReqData body;
            try {
                body = json::parse(req.body).get<sbi_gen::SecParamExchReqData>();
            } catch (const std::exception& e) {
                return problem(400, "Bad Request", e.what(), "INVALID_MSG_FORMAT", policy.own_fqdn);
            }
            // The peer: the sender IE, else the one N32-c context this certificate names.
            std::optional<sepp::Context> capability;
            if (body.sender) {
                if (!sender_authenticated(req, *body.sender)) {
                    return problem(403,
                                   "Forbidden",
                                   "sender " + *body.sender +
                                       " is not the SEPP the client certificate authenticates",
                                   "NEGOTIATION_NOT_ALLOWED",
                                   policy.own_fqdn);
                }
                capability = contexts.by_peer(lower(*body.sender));
            } else {
                capability = sepp::correlate(contexts.all(),
                                             req.peer_cert_dns_names,
                                             header(req, "via").value_or(""),
                                             std::nullopt,
                                             std::nullopt);
            }
            std::optional<std::vector<std::uint8_t>> master;
            if (const auto it = req.tls_exports.find(sepp::kN32MasterExporterLabel);
                it != req.tls_exports.end()) {
                master = it->second;
            }
            const auto peer_fqdn = capability ? capability->peer_fqdn : std::string{};
            const auto d = sepp::exchange_params(policy,
                                                 capability,
                                                 prins.by_peer(peer_fqdn),
                                                 body,
                                                 master,
                                                 prins.outgoing(peer_fqdn));
            if (!d.response) {
                spdlog::warn(
                    "sepp: exchange-params refused: {} ({})", d.cause.value_or(""), d.detail);
                return problem(d.status,
                               d.status == 400   ? "Bad Request"
                               : d.status == 409 ? "Conflict"
                               : d.status == 500 ? "Internal Server Error"
                                                 : "Forbidden",
                               d.detail,
                               d.cause,
                               policy.own_fqdn);
            }
            if (d.context) {
                prins.put(*d.context);
                spdlog::info("sepp: PRINS N32-f context with {}: local {} peer {} {} master {}",
                             d.context->peer_fqdn,
                             d.context->local_id,
                             d.context->peer_id,
                             d.context->jwe_cipher,
                             sepp::key_fingerprint(d.context->master_key));
            }
            return http2::Response::json(200, json(*d.response).dump());
        });
    n32.add_route(
        "POST", std::string(kHandshakeRoot) + "/n32f-terminate", [&](const http2::Request& req) {
            sbi_gen::N32fContextInfo body;
            try {
                body = json::parse(req.body).get<sbi_gen::N32fContextInfo>();
            } catch (const std::exception& e) {
                return problem(400, "Bad Request", e.what(), "INVALID_MSG_FORMAT", policy.own_fqdn);
            }
            // 5.2.4: the request names the context this SEPP issued; the answer carries the
            // initiating SEPP's id this SEPP stored.
            const auto ctx = prins.by_local_id(body.n32fContextId);
            if (!ctx) {
                return problem(404,
                               "Not Found",
                               "no N32-f context " + body.n32fContextId,
                               "CONTEXT_NOT_FOUND",
                               policy.own_fqdn);
            }
            prins.erase_local_id(ctx->local_id);
            contexts.erase(ctx->peer_fqdn);
            spdlog::info("sepp: PRINS N32-f context {} with {} terminated by the peer",
                         ctx->local_id,
                         ctx->peer_fqdn);
            sbi_gen::N32fContextInfo out;
            out.n32fContextId = ctx->peer_id;
            return http2::Response::json(200, json(out).dump());
        });
    n32.add_route(
        "POST", std::string(kHandshakeRoot) + "/n32f-error", [&](const http2::Request& req) {
            sbi_gen::N32fErrorInfo body;
            try {
                body = json::parse(req.body).get<sbi_gen::N32fErrorInfo>();
            } catch (const std::exception& e) {
                return problem(400, "Bad Request", e.what(), "INVALID_MSG_FORMAT", policy.own_fqdn);
            }
            // 5.2.5 step 2a: log that the N32-f message was not processed by the peer.
            spdlog::warn("sepp: peer reports N32-f message {} not processed: {}",
                         body.n32fMessageId,
                         body.n32fErrorType.value);
            http2::Response r;
            r.status = 204;
            return r;
        });

    // ---- N32-f with PRINS, receiving SEPP (TS 29.573 5.3.2, TS 33.501 13.2.4.7; ADR-0494) -------
    // 5.2.5: the error goes to the peer over N32-c, best effort, off the request's thread.
    std::atomic<int> reports_in_flight{0};
    const auto report_n32f_error =
        [&](const sepp::PrinsContext& pc, const std::string& message_id, const char* error_type) {
            const auto cap = contexts.by_peer(pc.peer_fqdn);
            if (!cap) {
                return;
            }
            sbi_gen::N32fErrorInfo info;
            info.n32fMessageId = message_id;
            info.n32fErrorType.value = error_type;
            info.n32fContextId = pc.peer_id;
            http2::ClientRequest er;
            er.method = "POST";
            er.url = cap->n32_api_root + kHandshakeRoot + "/n32f-error";
            er.headers.emplace("content-type", "application/json");
            er.body = json(info).dump();
            // At most a few in flight: a peer flooding bad messages must not spawn unbounded
            // threads (review fix); the excess reports are dropped and logged.
            if (reports_in_flight.fetch_add(1) >= 4) {
                reports_in_flight.fetch_sub(1);
                spdlog::warn("sepp: N32-f error report for {} dropped (too many in flight)",
                             message_id);
                return;
            }
            std::thread([er = std::move(er), &reports_in_flight] {
                http2::Client c(sepp_tls());
                (void)c.send(er);
                reports_in_flight.fetch_sub(1);
            }).detach();
        };
    n32.add_route("POST", "/n32f-forward/v1/n32f-process", [&](const http2::Request& req) {
        sbi_gen::N32fReformattedReqMsg body;
        try {
            body = json::parse(req.body).get<sbi_gen::N32fReformattedReqMsg>();
        } catch (const std::exception& e) {
            return problem(400, "Bad Request", e.what(), "INVALID_MSG_FORMAT", policy.own_fqdn);
        }
        const auto meta = sepp::peek_metadata(body.reformattedData);
        if (!meta) {
            return problem(400,
                           "Bad Request",
                           "no metaData in the reformatted message",
                           "MANDATORY_IE_INCORRECT",
                           policy.own_fqdn);
        }
        const auto pc = prins.by_local_id(meta->n32fContextId);
        if (!pc) {
            refused->Add(1);
            return problem(403,
                           "Forbidden",
                           "n32fContextId " + meta->n32fContextId + " is unknown",
                           "CONTEXT_NOT_FOUND",
                           policy.own_fqdn);
        }
        if (body.modificationsBlock) {
            // No roaming intermediary is configured, so no RI modification can be verified.
            refused->Add(1);
            return problem(403,
                           "Forbidden",
                           "modificationsBlock present but no roaming intermediary is authorized",
                           "UNSPECIFIED",
                           policy.own_fqdn);
        }
        const auto keys = sepp::derive_keys(
            pc->master_key, pc->local_id, sepp::key_use(!pc->initiator, false), pc->jwe_cipher);
        const auto opened = sepp::open(
            body.reformattedData, pc->jwe_cipher, keys, false, [&](const sepp::HttpMessage& m) {
                return sepp::select(
                    pc->selected_policy, m.method, m.path.substr(0, m.path.find('?')), false);
            });
        if (!opened) {
            refused->Add(1);
            if (opened.error().error == sepp::OpenError::PolicyMismatch) {
                return problem(
                    403, "Forbidden", opened.error().detail, "POLICY_MISMATCH", policy.own_fqdn);
            }
            report_n32f_error(*pc,
                              meta->messageId,
                              opened.error().error == sepp::OpenError::IntegrityCheckFailed
                                  ? "INTEGRITY_CHECK_FAILED"
                                  : "MESSAGE_RECONSTRUCTION_FAILED");
            return problem(403, "Forbidden", opened.error().detail, "UNSPECIFIED", policy.own_fqdn);
        }
        const sepp::HttpMessage& in = *opened;

        // 5.3.2.1 step 6: the consumer PLMN in a Bearer token must be the context's remote PLMN.
        const auto peer_cfg = std::ranges::find_if(
            policy.peers, [&](const sepp::PeerConfig& x) { return x.fqdn == pc->peer_fqdn; });
        for (const auto& [name, value] : in.headers) {
            if (name != "authorization" || peer_cfg == policy.peers.end()) {
                continue;
            }
            const auto plmn = token_consumer_plmn(value);
            if (plmn && sepp::home_network_domain(plmn->first, plmn->second) !=
                            sepp::home_network_domain(peer_cfg->mcc, peer_cfg->mnc)) {
                refused->Add(1);
                return problem(403,
                               "Forbidden",
                               "the Bearer token's consumer PLMN is not the N32-f context's",
                               "PLMNID_MISMATCH",
                               policy.own_fqdn);
            }
        }

        // The NF of this PLMN: requestLine.authority, through local_nf_routes as for TLS.
        sepp::HttpMessage out;
        const std::string authority = lower(in.authority);
        if (sepp::network_domain_of_host(sepp::host_of(authority)) != own_domain) {
            out.status = 400;
            out.headers = {{"content-type", "application/problem+json"},
                           {"server", "SEPP-" + policy.own_fqdn}};
            out.body = json(sbi_core::make_problem_details(400,
                                                           "Bad Request",
                                                           "authority " + authority +
                                                               " is not in " + own_domain,
                                                           std::string("MANDATORY_IE_INCORRECT")))
                           .dump();
        } else {
            auto route = local_nf_routes.find(authority);
            if (route == local_nf_routes.end() && authority.find(':') == std::string::npos) {
                route = local_nf_routes.find(authority + ":443");
            }
            std::optional<http2::ClientResponse> nf_rsp;
            if (route != local_nf_routes.end()) {
                http2::ClientRequest fwd;
                fwd.method = in.method;
                fwd.url = route->second + in.path;
                for (const auto& [name, value] : in.headers) {
                    if (!hop_by_hop(name) && name != "via") {
                        fwd.headers.emplace(name, value);
                    }
                }
                fwd.headers.emplace("via", "2.0 SEPP-" + policy.own_fqdn);
                fwd.body = in.body;
                if (auto r = client.send(fwd)) {
                    nf_rsp = std::move(*r);
                }
            }
            if (!nf_rsp) {
                out.status = 504;
                out.headers = {{"content-type", "application/problem+json"},
                               {"server", "SEPP-" + policy.own_fqdn}};
                out.body =
                    json(sbi_core::make_problem_details(504,
                                                        "Gateway Timeout",
                                                        "no route to " + authority,
                                                        std::string("TARGET_NF_NOT_REACHABLE")))
                        .dump();
            } else {
                out.status = static_cast<int>(nf_rsp->status);
                for (const auto& [name, value] : nf_rsp->headers) {
                    const std::string n = lower(name);
                    if (!hop_by_hop(n)) {
                        out.headers.emplace_back(n, value);
                    }
                }
                out.headers.emplace_back("via", "2.0 SEPP-" + policy.own_fqdn);
                out.body = std::move(nf_rsp->body);
            }
        }
        forwarded->Add(1);
        const auto use = sepp::key_use(pc->initiator, true);
        const auto rsp_keys = sepp::derive_keys(pc->master_key, pc->peer_id, use, pc->jwe_cipher);
        const auto sel = sepp::select(
            pc->selected_policy, in.method, in.path.substr(0, in.path.find('?')), true);
        const auto rsp_seq_opt = next_seq(pc->local_id, use);
        if (!rsp_seq_opt) {
            return problem(503,
                           "Service Unavailable",
                           "N32-f sequence space of this context exhausted; renegotiate",
                           "NF_SERVICE_CONGESTION",
                           policy.own_fqdn);
        }
        const std::uint32_t rsp_seq = *rsp_seq_opt;
        auto jwe = sepp::protect(
            out, sel, pc->jwe_cipher, rsp_keys, rsp_seq, pc->peer_id, meta->messageId);
        if (!jwe) {
            return problem(
                500, "Internal Server Error", jwe.error(), "SYSTEM_FAILURE", policy.own_fqdn);
        }
        sbi_gen::N32fReformattedRspMsg rsp;
        rsp.reformattedData = std::move(*jwe);
        return http2::Response::json(200, json(rsp).dump());
    });

    // ---- N32-f, receiving SEPP (5.3.3, TS 29.500 6.1.4.3.4) ----------------------------------
    n32.add_route("*", "/**", [&](const http2::Request& req) {
        const auto via = header(req, "via").value_or("");
        std::vector<sepp::Context> tls_contexts;
        for (auto& c : contexts.all()) {
            if (c.security == "TLS") {
                tls_contexts.push_back(std::move(c));
            }
        }
        // Plain forwarding exists only for TLS security; PRINS uses the JOSE API (5.3.2).
        const auto ctx = sepp::correlate(tls_contexts,
                                         req.peer_cert_dns_names,
                                         via,
                                         header(req, kHandshakeIdHeader),
                                         header(req, kPurposeHeader));
        if (!ctx) {
            refused->Add(1);
            return problem(403,
                           "Forbidden",
                           "N32-f request over TLS with no related N32-c context",
                           "CONTEXT_NOT_FOUND",
                           policy.own_fqdn);
        }
        const auto target = header(req, kTargetApiRootHeader);
        if (!target) {
            refused->Add(1);
            return problem(400,
                           "Bad Request",
                           "3gpp-Sbi-Target-apiRoot is required on N32-f",
                           "MANDATORY_IE_MISSING",
                           policy.own_fqdn);
        }
        if (sepp::network_domain_of_host(sepp::host_of(*target)) != own_domain) {
            refused->Add(1);
            return problem(400,
                           "Bad Request",
                           "3gpp-Sbi-Target-apiRoot " + *target + " is not in PLMN " + own_domain,
                           "MANDATORY_IE_INCORRECT",
                           policy.own_fqdn);
        }
        const auto [authority, prefix] = split_api_root(*target);
        // A target apiRoot without a port is the https default (e.g. one rebuilt from a telescopic
        // FQDN, which carries no port): "host" or "host:443" in config.
        auto route = local_nf_routes.find(authority);
        if (route == local_nf_routes.end() && authority.find(':') == std::string::npos) {
            route = local_nf_routes.find(authority + ":443");
        }
        if (route == local_nf_routes.end()) {
            refused->Add(1);
            return problem(504,
                           "Gateway Timeout",
                           "no route to " + authority + " in this PLMN",
                           "TARGET_NF_NOT_REACHABLE",
                           policy.own_fqdn);
        }
        forwarded->Add(1);
        // 5.3.3.3: the handshake id is removed by the receiving SEPP; the target apiRoot is
        // consumed here.
        return relay(client,
                     req,
                     route->second + prefix + req.path,
                     {kTargetApiRootHeader, kHandshakeIdHeader},
                     {},
                     policy.own_fqdn);
    });

    // ---- Nsepp_Telescopic_FQDN_Mapping (TS 29.573 5.4, ADR-0492) ------------------------------
    sbi.add_route("GET", "/nsepp-telescopic/v1/mapping", [&](const http2::Request& req) {
        const auto fqdn = req.query_params.find("foreign-fqdn");
        const auto label = req.query_params.find("telescopic-label");
        const bool has_fqdn = fqdn != req.query_params.end();
        const bool has_label = label != req.query_params.end();
        if (has_fqdn == has_label) {
            return problem(400,
                           "Bad Request",
                           "exactly one of foreign-fqdn and telescopic-label is required",
                           "INVALID_QUERY_PARAM",
                           policy.own_fqdn);
        }
        sbi_gen::TelescopicMapping mapping;
        if (has_fqdn) {
            // 5.4.2: only an FQDN of another PLMN has a telescopic form.
            const auto domain = sepp::network_domain_of_host(fqdn->second);
            if (!domain || *domain == own_domain) {
                return problem(400,
                               "Bad Request",
                               "foreign-fqdn " + fqdn->second + " is not an FQDN of another PLMN",
                               "INVALID_QUERY_PARAM",
                               policy.own_fqdn);
            }
            mapping.telescopicLabel = telescopic.label_for(fqdn->second);
            mapping.seppDomain = policy.own_fqdn;
        } else {
            const auto foreign = telescopic.foreign_for(label->second);
            if (!foreign) {
                return problem(404,
                               "Not Found",
                               "telescopic label " + label->second + " is not known to this SEPP",
                               "RESOURCE_URI_STRUCTURE_NOT_FOUND",
                               policy.own_fqdn);
            }
            mapping.foreignFqdn = *foreign;
        }
        return http2::Response::json(200, json(mapping).dump());
    });

    // ---- NF side, sending SEPP (TS 29.500 6.1.4.3.2 / 6.1.4.3.3 / 6.1.4.3.4) ------------------
    sbi.add_route("*", "/**", [&](const http2::Request& req) {
        // A telescopic FQDN in the request URI wins over a 3gpp-Sbi-Target-apiRoot header
        // (6.1.4.3.3 NOTE 1: the header is then ignored).
        std::optional<std::string> target;
        bool via_telescopic = false;
        if (const auto tl =
                sepp::telescopic_label_of_host(sepp::host_of(req.authority), policy.own_fqdn)) {
            const auto foreign = telescopic.foreign_for(*tl);
            if (!foreign) {
                return problem(404,
                               "Not Found",
                               "telescopic label " + *tl + " is not known to this SEPP",
                               "RESOURCE_URI_STRUCTURE_NOT_FOUND",
                               policy.own_fqdn);
            }
            target = "https://" + *foreign;
            via_telescopic = true;
        } else {
            target = header(req, kTargetApiRootHeader);
        }
        if (!target) {
            return problem(400,
                           "Bad Request",
                           "neither a telescopic FQDN nor 3gpp-Sbi-Target-apiRoot names the target",
                           "MANDATORY_IE_MISSING",
                           policy.own_fqdn);
        }
        const auto domain = sepp::network_domain_of_host(sepp::host_of(*target));
        if (!domain || *domain == own_domain) {
            return problem(400,
                           "Bad Request",
                           "3gpp-Sbi-Target-apiRoot " + *target + " is not in a remote PLMN",
                           "MANDATORY_IE_INCORRECT",
                           policy.own_fqdn);
        }
        const auto ctx = contexts.by_domain(*domain);
        if (ctx && ctx->security != "TLS") {
            // PRINS (TS 29.573 5.3.2.3 / 5.3.2.4, TS 33.501 13.2.4.8 steps 1-3; ADR-0494).
            const auto pc = prins.by_peer(ctx->peer_fqdn);
            if (!pc) {
                return problem(504,
                               "Gateway Timeout",
                               "no PRINS N32-f context with " + *domain,
                               "TARGET_NF_NOT_REACHABLE",
                               policy.own_fqdn);
            }
            const auto [authority, prefix] = split_api_root(*target);
            sepp::HttpMessage m;
            m.method = req.method;
            m.authority = authority;
            m.path = prefix + req.path;
            for (const auto& [name, value] : req.headers) {
                const std::string n = lower(name);
                if (!hop_by_hop(n) && n != "via" && n != kTargetApiRootHeader &&
                    n != kHandshakeIdHeader) {
                    m.headers.emplace_back(n, value);
                }
            }
            m.body = req.body;
            const std::string api_path = m.path.substr(0, m.path.find('?'));
            const auto use = sepp::key_use(pc->initiator, false);
            const auto keys = sepp::derive_keys(pc->master_key, pc->peer_id, use, pc->jwe_cipher);
            const std::string message_id = hex64(message_counter++);
            const auto req_seq_opt = next_seq(pc->local_id, use);
            if (!req_seq_opt) {
                return problem(503,
                               "Service Unavailable",
                               "N32-f sequence space of this context exhausted; renegotiate",
                               "NF_SERVICE_CONGESTION",
                               policy.own_fqdn);
            }
            const std::uint32_t req_seq = *req_seq_opt;
            auto jwe = sepp::protect(m,
                                     sepp::select(pc->selected_policy, m.method, api_path, false),
                                     pc->jwe_cipher,
                                     keys,
                                     req_seq,
                                     pc->peer_id,
                                     message_id);
            if (!jwe) {
                return problem(
                    500, "Internal Server Error", jwe.error(), "SYSTEM_FAILURE", policy.own_fqdn);
            }
            sbi_gen::N32fReformattedReqMsg msg;
            msg.reformattedData = std::move(*jwe);
            http2::ClientRequest out;
            out.method = "POST";
            out.url = ctx->n32_api_root + "/n32f-forward/v1/n32f-process";
            out.headers.emplace("content-type", "application/json");
            out.headers.emplace("via", "2.0 SEPP-" + policy.own_fqdn);
            if (const auto prio = header(req, "3gpp-sbi-message-priority")) {
                out.headers.emplace("3gpp-sbi-message-priority", *prio); // 5.3.2.4
            }
            out.body = json(msg).dump();
            forwarded->Add(1);
            const auto r = client.send(out);
            if (!r) {
                return problem(504,
                               "Gateway Timeout",
                               "peer SEPP not reachable: " + r.error(),
                               "TARGET_NF_NOT_REACHABLE",
                               policy.own_fqdn);
            }
            if (r->status != 200) {
                // An error of the peer SEPP itself (CONTEXT_NOT_FOUND, POLICY_MISMATCH, ...).
                http2::Response e;
                e.status = static_cast<int>(r->status);
                for (const auto& [name, value] : r->headers) {
                    if (!hop_by_hop(lower(name))) {
                        e.headers.emplace(lower(name), value);
                    }
                }
                e.headers.emplace("via", "2.0 SEPP-" + policy.own_fqdn);
                e.body = r->body;
                return e;
            }
            sbi_gen::N32fReformattedRspMsg rsp;
            try {
                rsp = json::parse(r->body).get<sbi_gen::N32fReformattedRspMsg>();
            } catch (const std::exception& ex) {
                return problem(
                    502, "Bad Gateway", ex.what(), "INBOUND_SERVER_ERROR", policy.own_fqdn);
            }
            const auto rmeta = sepp::peek_metadata(rsp.reformattedData);
            if (!rmeta || lower(rmeta->n32fContextId) != pc->local_id ||
                rmeta->messageId != message_id) {
                return problem(502,
                               "Bad Gateway",
                               "the protected response names another context or message",
                               "INBOUND_SERVER_ERROR",
                               policy.own_fqdn);
            }
            const auto rkeys = sepp::derive_keys(
                pc->master_key, pc->local_id, sepp::key_use(!pc->initiator, true), pc->jwe_cipher);
            const auto opened = sepp::open(
                rsp.reformattedData, pc->jwe_cipher, rkeys, true, [&](const sepp::HttpMessage&) {
                    return sepp::select(pc->selected_policy, m.method, api_path, true);
                });
            if (!opened) {
                report_n32f_error(*pc,
                                  message_id,
                                  opened.error().error == sepp::OpenError::IntegrityCheckFailed
                                      ? "INTEGRITY_CHECK_FAILED"
                                  : opened.error().error == sepp::OpenError::PolicyMismatch
                                      ? "POLICY_MISMATCH"
                                      : "MESSAGE_RECONSTRUCTION_FAILED");
                return problem(502,
                               "Bad Gateway",
                               opened.error().detail,
                               "INBOUND_SERVER_ERROR",
                               policy.own_fqdn);
            }
            http2::Response back;
            back.status = opened->status;
            std::string via_chain;
            for (const auto& [name, value] : opened->headers) {
                if (name == "via") {
                    via_chain = via_chain.empty() ? value : via_chain + ", " + value;
                } else {
                    back.headers.emplace(name, value);
                }
            }
            const std::string me = "2.0 SEPP-" + policy.own_fqdn;
            back.headers.emplace("via", via_chain.empty() ? me : via_chain + ", " + me);
            back.body = opened->body;
            return back;
        }
        if (!ctx) {
            return problem(504,
                           "Gateway Timeout",
                           "no N32 context with PLMN " + *domain,
                           "TARGET_NF_NOT_REACHABLE",
                           policy.own_fqdn);
        }
        if (!ctx->peer_target_api_root) {
            return problem(504,
                           "Gateway Timeout",
                           "the SEPP of " + *domain +
                               " does not support 3gpp-Sbi-Target-apiRoot (not built, ADR-0491)",
                           "TARGET_NF_NOT_REACHABLE",
                           policy.own_fqdn);
        }
        std::vector<std::pair<std::string, std::string>> extra;
        if (ctx->handshake_id) {
            extra.emplace_back(kHandshakeIdHeader, *ctx->handshake_id);
        }
        std::vector<std::string> drop;
        if (via_telescopic) {
            // 6.1.4.3.4 second case: the sending SEPP inserts the header, set to the target
            // apiRoot derived from the telescopic FQDN (any header the NF sent is ignored).
            drop.emplace_back(kTargetApiRootHeader);
            extra.emplace_back(kTargetApiRootHeader, *target);
        }
        forwarded->Add(1);
        // Both SEPPs support the header: HTTPS to the peer SEPP's apiRoot.
        auto api_root = ctx->n32_api_root;
        while (!api_root.empty() && api_root.back() == '/') {
            api_root.pop_back();
        }
        auto resp = relay(client, req, api_root + req.path, drop, extra, policy.own_fqdn);
        if (via_telescopic) {
            // 6.1.4.3.2: a link relation URI of the other PLMN goes back to the NF in telescopic
            // form (here: the Location header; URIs inside bodies are not rewritten, ADR-0492).
            const auto loc = resp.headers.find("location");
            if (loc != resp.headers.end()) {
                const std::string host = sepp::host_of(loc->second);
                if (sepp::network_domain_of_host(host) &&
                    *sepp::network_domain_of_host(host) != own_domain) {
                    // Case-insensitive (FQDNs are), and no rewrite without a match (review fix).
                    const auto at = lower(loc->second).find(host);
                    if (at != std::string::npos) {
                        std::string rewritten = loc->second;
                        rewritten.replace(
                            at, host.size(), telescopic.label_for(host) + "." + policy.own_fqdn);
                        loc->second = rewritten;
                    }
                }
            }
        }
        return resp;
    });

    // ---- initiating SEPP: 5.2.2 towards every peer configured with initiate -------------------
    std::atomic<bool> running{true};
    const std::string instance_id = sbi_core::generate_uuid_v4();
    std::thread nrf_thread(run_nrf_lifecycle,
                           instance_id,
                           nrf_base,
                           policy.own_fqdn,
                           policy.own_mcc,
                           policy.own_mnc,
                           std::cref(running));
    // 5.2.3 as initiating SEPP: cipher suites (exporting the master key from the connection the
    // request travels on), then the protection policy exchange. The responder must already hold the
    // PRINS capability context, so the capability context here is stored only after this succeeds.
    const auto run_param_exchange = [&](http2::Client& c, const sepp::PeerConfig& peer) -> bool {
        const std::string url = peer.n32_api_root + kHandshakeRoot + "/exchange-params";
        sbi_gen::SecParamExchReqData ciphers;
        ciphers.n32fContextId = sepp::random_n32f_context_id();
        ciphers.jweCipherSuiteList = policy.jwe_cipher_suites;
        ciphers.jwsCipherSuiteList = policy.jws_cipher_suites;
        ciphers.sender = policy.own_fqdn;
        http2::ClientRequest req;
        req.method = "POST";
        req.url = url;
        req.headers.emplace("content-type", "application/json");
        req.body = json(ciphers).dump();
        req.tls_exports = {{sepp::kN32MasterExporterLabel, sepp::kN32MasterKeyLength}};
        prins.set_outgoing(peer.fqdn, true);
        const auto r = c.send(req);
        prins.set_outgoing(peer.fqdn, false);
        if (!r || r->status != 200) {
            spdlog::warn("sepp: exchange-params (cipher suites) to {} failed ({})",
                         peer.fqdn,
                         r ? std::to_string(r->status) + " " + r->body : r.error());
            return false;
        }
        const auto master = r->tls_exports.find(sepp::kN32MasterExporterLabel);
        if (master == r->tls_exports.end()) {
            spdlog::error("sepp: no TLS exporter output for the N32-c connection to {}", peer.fqdn);
            return false;
        }
        sepp::PrinsContext ctx;
        try {
            const auto rsp = json::parse(r->body).get<sbi_gen::SecParamExchRspData>();
            if (!rsp.selectedJweCipherSuite) {
                spdlog::error("sepp: {} selected no JWE cipher suite", peer.fqdn);
                return false;
            }
            ctx.peer_fqdn = peer.fqdn;
            ctx.local_id = ciphers.n32fContextId;
            ctx.peer_id = lower(rsp.n32fContextId);
            ctx.initiator = true;
            ctx.jwe_cipher = *rsp.selectedJweCipherSuite;
            ctx.jws_cipher = rsp.selectedJwsCipherSuite;
            ctx.master_key = master->second;
        } catch (const std::exception& e) {
            spdlog::error("sepp: bad exchange-params response from {}: {}", peer.fqdn, e.what());
            return false;
        }
        // 5.2.3.3 protection policy exchange, on the N32-f context just created.
        sbi_gen::SecParamExchReqData pol;
        pol.n32fContextId = ctx.local_id;
        pol.protectionPolicyInfo = policy.protection_policy;
        pol.sender = policy.own_fqdn;
        http2::ClientRequest preq;
        preq.method = "POST";
        preq.url = url;
        preq.headers.emplace("content-type", "application/json");
        preq.body = json(pol).dump();
        if (const auto pr = c.send(preq); pr && pr->status == 200) {
            try {
                const auto prsp = json::parse(pr->body).get<sbi_gen::SecParamExchRspData>();
                ctx.selected_policy = prsp.selProtectionPolicyInfo;
                ctx.peer_policy = prsp.selProtectionPolicyInfo;
            } catch (const std::exception& e) {
                spdlog::warn(
                    "sepp: bad protection policy response from {}: {}", peer.fqdn, e.what());
            }
        } else {
            spdlog::warn(
                "sepp: protection policy exchange with {} failed ({}); continuing with the "
                "cipher suite only",
                peer.fqdn,
                pr ? std::to_string(pr->status) : pr.error());
        }
        prins.put(ctx);
        spdlog::info("sepp: PRINS N32-f context with {}: local {} peer {} {} master {}",
                     ctx.peer_fqdn,
                     ctx.local_id,
                     ctx.peer_id,
                     ctx.jwe_cipher,
                     sepp::key_fingerprint(ctx.master_key));
        return true;
    };

    std::thread initiator([&] {
        http2::Client c(sepp_tls());
        while (running.load()) {
            for (const auto& peer : policy.peers) {
                if (!peer.initiate || contexts.by_peer(peer.fqdn)) {
                    continue;
                }
                const auto id = random_handshake_id();
                http2::ClientRequest req;
                req.method = "POST";
                req.url = peer.n32_api_root + kHandshakeRoot + "/exchange-capability";
                req.headers.emplace("content-type", "application/json");
                req.body = json(sepp::make_request(policy, peer, id, false)).dump();
                contexts.set_outgoing(peer.fqdn, true);
                const auto r = c.send(req);
                contexts.set_outgoing(peer.fqdn, false);
                if (!r || r->status != 200) {
                    spdlog::warn("sepp: exchange-capability to {} failed ({})",
                                 peer.fqdn,
                                 r ? std::to_string(r->status) + " " + r->body : r.error());
                    continue;
                }
                try {
                    const auto rsp = json::parse(r->body).get<sbi_gen::SecNegotiateRspData>();
                    if (const auto ctx = sepp::context_from_response(peer, rsp, id)) {
                        if (ctx->security == "PRINS" && !run_param_exchange(c, peer)) {
                            continue; // retried on the next round, capability included
                        }
                        contexts.put(*ctx);
                        handshakes->Add(1);
                        spdlog::info("sepp: N32 context with {} ({}, handshake id {})",
                                     ctx->peer_fqdn,
                                     ctx->security,
                                     ctx->handshake_id.value_or("-"));
                    } else {
                        spdlog::error("sepp: {} selected {} -- no context",
                                      peer.fqdn,
                                      rsp.selectedSecCapability.value);
                    }
                } catch (const std::exception& e) {
                    spdlog::error(
                        "sepp: bad exchange-capability response from {}: {}", peer.fqdn, e.what());
                }
            }
            for (int i = 0; i < retry_seconds * 10 && running.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    });

    // On shutdown, tear every N32-f TLS connection down (5.2.2: capability "NONE").
    sbi_core::on_shutdown_signal([&] {
        running = false;
        http2::Client c(sepp_tls());
        for (const auto& ctx : contexts.all()) {
            const auto peer = std::ranges::find_if(
                policy.peers, [&](const sepp::PeerConfig& p) { return p.fqdn == ctx.peer_fqdn; });
            if (peer == policy.peers.end()) {
                continue;
            }
            http2::ClientRequest req;
            req.method = "POST";
            req.headers.emplace("content-type", "application/json");
            if (const auto p = prins.by_peer(ctx.peer_fqdn); ctx.security == "PRINS" && p) {
                // 5.2.4: name the context the peer issued.
                sbi_gen::N32fContextInfo info;
                info.n32fContextId = p->peer_id;
                req.url = ctx.n32_api_root + kHandshakeRoot + "/n32f-terminate";
                req.body = json(info).dump();
            } else {
                req.url = ctx.n32_api_root + kHandshakeRoot + "/exchange-capability";
                req.body = json(sepp::make_request(policy, *peer, ctx.handshake_id, true)).dump();
            }
            (void)c.send(req);
        }
    });

    sbi.start();
    n32.start();
    spdlog::info("sepp: NF side https://{}:{}, N32 https://{}:{} (TLS 1.3 + mTLS)",
                 bind,
                 port,
                 bind,
                 n32_port);
    sbi_core::run_multi_threaded(ioc);
    running = false;
    initiator.join();
    nrf_thread.join();
    return 0;
}
