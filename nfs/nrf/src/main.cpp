// nfs/nrf: the real NRF (NF Repository Function), TS 29.510 Nnrf_NFManagement +
// Nnrf_NFDiscovery + Nnrf_AccessToken + Nnrf_Bootstrapping (specs/5G_APIs-REL-19/, commit
// bca84b60a37773133bcae97e5c6c0d10a93b47b6). Replaces the Phase 0 nfs/stub-nrf throwaway.
// Phase 2's first NF, dependency root for every NF that follows (PROMPT.md order: NRF -> AMF ->
// SMF -> UDM -> UDR -> AUSF -> PCF).
//
// In scope (see the procedure list agreed before this NF was built):
//   RegisterNFInstance, GetNFInstance, UpdateNFInstance (real RFC 6902 patch application),
//   DeregisterNFInstance, GetNFInstances, CreateSubscription, UpdateSubscription,
//   RemoveSubscription (all NFManagement.yaml), SearchNFInstances (NFDiscovery.yaml),
//   real signed-JWT OAuth2 token issuance (AccessToken.yaml), BootstrappingInfoRequest
//   (Bootstrapping.yaml -- gap-closure, ADR-0193/ADR-0194).
// Out of scope, deferred not dropped, per ADR-0193's own project-wide audit
// (docs/CAPABILITY_GAP_ANALYSIS.md): NFManagement's /shared-data* (multi-NRF federation -- no
// second NRF instance exists in this lab), /scp-domain-routing-info* (stale disclosure -- SCP now
// exists, ADR-0186, but this wiring hasn't been revisited).
//
// UPDATE (ADR-0211, Tier-B gap-closure): OptionsNFInstances (NFManagement.yaml),
// RetrieveStoredSearch/RetrieveCompleteSearch (NFDiscovery.yaml), and RetrieveKeyRequest
// (AccessToken.yaml) added -- the 4 undisclosed missing operations ADR-0193's own audit found.
// Real, disclosed: RetrieveStoredSearch/RetrieveCompleteSearch return the same real cached
// nfInstances (this NRF has no partial-vs-complete-profile filtering to distinguish them);
// RetrieveKeyRequest returns this NRF's own real JWT public key (the same
// certs/nrf-jwt/public.pem every NF's own Verifier already trusts) only when the request's
// `issuer` matches this NRF's own instance id, honest `404` otherwise (no other real issuer
// exists in this lab to serve a key for).
//
// Disclosed simplifications (see docs/DECISIONS.md for the full ADRs):
//   - In-memory storage only, no persistence across restarts.
//   - SearchNFInstances filters on target-nf-type only, not the full query parameter set.
//   - Subscription notification fan-out ignores subscrCond (delivers to every active subscriber
//     on NF_REGISTERED/NF_DEREGISTERED, not just matching ones).
//   - Notification delivery is synchronous best-effort (log on failure, do not retry) --
//     consistent with ADR-0006's synchronous HTTP/2 client.
//   - Stored search results (searchId cache) have no TTL eviction, same class of simplification
//     as NfRegistry/SubscriptionRegistry's own in-memory-only storage.

#include "sbi_core/http2_client.hpp"
#include "sbi_core/http2_server.hpp"
#include "sbi_core/io_context_pool.hpp"
#include "sbi_core/jwt.hpp"
#include "sbi_core/logging.hpp"
#include "sbi_core/metrics.hpp"
#include "sbi_core/otel.hpp"
#include "sbi_core/problem_details.hpp"
#include "sbi_core/rate_limit.hpp"

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <fstream>
#include <functional>
#include <regex>
#include <sstream>
#include <thread>
#include <unordered_set>

// ADR-0193/ADR-0194: TS29510_Nnrf_Bootstrapping.yaml, the Tier-A gap that started the
// full-project YAML coverage audit -- see docs/CAPABILITY_GAP_ANALYSIS.md's own ADR-0193 section.
#include "TS29510_Nnrf_Bootstrapping.hpp"
#include "discovery_authz.hpp"
#include "registry.hpp"

// docs/DECISIONS.md ADR-0077 -- no hardcoded deployment literal in source.
#include "nf_config/nf_config.hpp"

namespace {

using nlohmann::json;

#ifndef CERTS_DIR
#error "CERTS_DIR must be defined by CMake (see nfs/nrf/CMakeLists.txt)"
#endif
#ifndef CONFIG_DIR
#error "CONFIG_DIR must be defined by CMake (see nfs/nrf/CMakeLists.txt)"
#endif

constexpr const char* kNfType = "NRF";

// NRF's own nfInstanceId, fixed rather than randomly generated per run (unlike every other NF's,
// which self-generate one via sbi_core::generate_uuid_v4() since nobody else needs to know it in
// advance). NRF is every other NF's OAuth2 issuer (AccessTokenClaims.iss); a
// sbi_core::jwt::Verifier must be constructed with the exact expected issuer id, which means any
// NF other than NRF itself has no way to know that id if it were randomly regenerated every NRF
// restart. Discovered as a real bootstrapping gap while wiring AMF's Verifier (previously latent
// because NRF is the only process that both issues and, until now, ever verified its own tokens,
// so it could just pass itself its own freshly-generated id -- see docs/DECISIONS.md ADR-0018.
constexpr const char* kNrfInstanceId = "5ba9a927-1d31-4c8e-8a10-000000000001";

sbi_core::http2::Response
problem_response(int status, const std::string& title, const std::string& detail) {
    auto pd = sbi_core::make_problem_details(status, title, detail);
    json j = pd;
    sbi_core::http2::Response r;
    r.status = status;
    r.headers.emplace("content-type", "application/problem+json");
    r.body = j.dump();
    return r;
}

// application/x-www-form-urlencoded parsing for POST /oauth2/token. Minimal: splits on '&'/'=',
// percent-decodes values. Good enough for the fixed field set AccessTokenReq actually needs here.
std::string url_decode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            const std::string hex = in.substr(i + 1, 2);
            out += static_cast<char>(std::stoi(hex, nullptr, 16));
            i += 2;
        } else if (in[i] == '+') {
            out += ' ';
        } else {
            out += in[i];
        }
    }
    return out;
}

std::unordered_map<std::string, std::string> parse_form_urlencoded(const std::string& body) {
    std::unordered_map<std::string, std::string> fields;
    std::stringstream ss(body);
    std::string pair;
    while (std::getline(ss, pair, '&')) {
        const auto eq = pair.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        fields[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
    }
    return fields;
}

// Returns nullopt if no Authorization header is present at all -- RegisterNFInstance/
// GetNFInstance/etc. all declare `security: [{}, oAuth2ClientCredentials:[...]]` in the YAML,
// i.e. an anonymous alternative is explicitly schema-permitted (bootstrap: an NF may not have a
// token yet before its first registration). If a header IS present, it must verify -- callers
// that want to reject anonymous access entirely can check has_value() themselves.
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

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #102, ADR-0079): real NFProfile semantic
// validation. Before this, RegisterNFInstance only checked that nfInstanceId/nfType/nfStatus
// KEYS were present (below), never that their VALUES were well-formed -- a malformed nfType or
// an out-of-range heartBeatTimer was silently accepted. Every constraint here is grounded
// directly in the real OpenAPI YAML, cited per field, not invented:
// - nfInstanceId: format=uuid, "shall be a UUID version 4" (TS29571_CommonData.yaml).
// - heartBeatTimer: type integer, minimum 1 (TS29510_Nnrf_NFManagement.yaml) -- no maximum is
//   declared in the spec, so none is enforced here.
// - nfType: TS29571_CommonData.yaml's NFType is an open anyOf[enum,string] (any string parses
//   without throwing, for wire round-tripping -- see sbi_gen::NFType's own comment) -- but NRF,
//   as the registry owning the canonical NF catalog, should still reject values outside the real
//   known set at registration time, the same real validation free5GC's own NRF applies. The set
//   below is the exact real enum list the spec (and this project's own codegen, sbi_gen::NFType)
//   already derived from the YAML, not independently invented.
// - nfStatus / nfServices[].nfServiceStatus: real 4-value enum (REGISTERED/SUSPENDED/
//   UNDISCOVERABLE/CANARY_RELEASE), both TS29510_Nnrf_NFManagement.yaml.
// - nfServices[].scheme: real {http,https} UriScheme enum (TS29571_CommonData.yaml).
// - nfServices[].ipEndPoints[].transport: TCP only -- NOT the more general TS29571_CommonData
//   TransportProtocol (which also allows UDP); THIS API's own local TransportProtocol schema
//   (TS29510_Nnrf_NFManagement.yaml) narrows it to TCP only, confirmed by reading the YAML
//   directly, not assumed from the more general type.
// - nfServices[].ipEndPoints[].port: minimum 0, maximum 65535 (TS29510_Nnrf_NFManagement.yaml).
// - ipv4Addresses[] / nfServices[].ipEndPoints[].ipv4Address: real dotted-decimal regex, copied
//   verbatim from TS29571_CommonData.yaml's Ipv4Addr pattern.
// - ipv6Addresses[] / nfServices[].ipEndPoints[].ipv6Address: real colon-hex regex pair, copied
//   verbatim from TS29571_CommonData.yaml's Ipv6Addr pattern.
// Returns nullopt if valid, else a human-readable reason for the first violation found.
const std::unordered_set<std::string>& known_nf_types() {
    static const std::unordered_set<std::string> types = {
        "NRF",      "UDM",        "AMF",    "SMF",      "AUSF",      "NEF",      "PCF",    "SMSF",
        "NSSF",     "UDR",        "LMF",    "GMLC",     "5G_EIR",    "SEPP",     "UPF",    "N3IWF",
        "AF",       "UDSF",       "BSF",    "CHF",      "NWDAF",     "PCSCF",    "CBCF",   "HSS",
        "UCMF",     "SOR_AF",     "SPAF",   "MME",      "SCSAS",     "SCEF",     "SCP",    "NSSAAF",
        "ICSCF",    "SCSCF",      "DRA",    "IMS_AS",   "AANF",      "5G_DDNMF", "NSACF",  "MFAF",
        "EASDF",    "DCCF",       "MB_SMF", "TSCTSF",   "ADRF",      "GBA_BSF",  "CEF",    "MB_UPF",
        "NSWOF",    "PKMF",       "MNPF",   "SMS_GMSC", "SMS_IWMSC", "MBSF",     "MBSTF",  "PANF",
        "IP_SM_GW", "SMS_ROUTER", "DCSF",   "MRF",      "MRFP",      "MF",       "SLPKMF", "RH",
        "EIF",      "AIOTF",      "ADM",
    };
    return types;
}

const std::unordered_set<std::string>& known_status_values() {
    static const std::unordered_set<std::string> statuses = {
        "REGISTERED", "SUSPENDED", "UNDISCOVERABLE", "CANARY_RELEASE"};
    return statuses;
}

bool is_uuid_v4(const std::string& s) {
    static const std::regex kPattern(
        "^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$", std::regex::icase);
    return std::regex_match(s, kPattern);
}

bool is_ipv4(const std::string& s) {
    static const std::regex kPattern(
        "^(([0-9]|[1-9][0-9]|1[0-9][0-9]|2[0-4][0-9]|25[0-5])\\.){3}([0-9]|[1-9][0-9]|1[0-9][0-9]"
        "|2[0-4][0-9]|25[0-5])$");
    return std::regex_match(s, kPattern);
}

bool is_ipv6(const std::string& s) {
    static const std::regex kPattern1(
        "^((:|(0?|([1-9a-f][0-9a-f]{0,3}))):)((0?|([1-9a-f][0-9a-f]{0,3})):){0,6}(:|(0?|([1-9a-f]"
        "[0-9a-f]{0,3})))$",
        std::regex::icase);
    static const std::regex kPattern2(
        "^((([^:]+:){7}([^:]+))|((([^:]+:)*[^:]+)?::(([^:]+:)*[^:]+)?))$", std::regex::icase);
    return std::regex_match(s, kPattern1) && std::regex_match(s, kPattern2);
}

std::optional<std::string> validate_ip_endpoint(const json& ep) {
    if (ep.contains("ipv4Address") && ep.at("ipv4Address").is_string() &&
        !is_ipv4(ep.at("ipv4Address").get<std::string>())) {
        return "ipEndPoints[].ipv4Address is not a valid dotted-decimal IPv4 address";
    }
    if (ep.contains("ipv6Address") && ep.at("ipv6Address").is_string() &&
        !is_ipv6(ep.at("ipv6Address").get<std::string>())) {
        return "ipEndPoints[].ipv6Address is not a valid IPv6 address";
    }
    if (ep.contains("transport") && ep.at("transport").is_string() &&
        ep.at("transport").get<std::string>() != "TCP") {
        return "ipEndPoints[].transport must be TCP (this API's own TransportProtocol schema, "
               "unlike the general one, allows only TCP)";
    }
    if (ep.contains("port") && ep.at("port").is_number()) {
        const auto port = ep.at("port").get<std::int64_t>();
        if (port < 0 || port > 65535) {
            return "ipEndPoints[].port must be in [0, 65535]";
        }
    }
    return std::nullopt;
}

std::optional<std::string> validate_nf_profile(const json& profile) {
    if (profile.contains("nfInstanceId") && profile.at("nfInstanceId").is_string() &&
        !is_uuid_v4(profile.at("nfInstanceId").get<std::string>())) {
        return "nfInstanceId must be a UUID v4";
    }
    if (profile.contains("nfType") && profile.at("nfType").is_string() &&
        !known_nf_types().contains(profile.at("nfType").get<std::string>())) {
        return "nfType '" + profile.at("nfType").get<std::string>() +
               "' is not a recognized NFType";
    }
    if (profile.contains("nfStatus") && profile.at("nfStatus").is_string() &&
        !known_status_values().contains(profile.at("nfStatus").get<std::string>())) {
        return "nfStatus '" + profile.at("nfStatus").get<std::string>() +
               "' is not one of REGISTERED/SUSPENDED/UNDISCOVERABLE/CANARY_RELEASE";
    }
    if (profile.contains("heartBeatTimer") && profile.at("heartBeatTimer").is_number()) {
        if (profile.at("heartBeatTimer").get<std::int64_t>() < 1) {
            return "heartBeatTimer must be >= 1";
        }
    }
    if (profile.contains("ipv4Addresses") && profile.at("ipv4Addresses").is_array()) {
        for (const auto& addr : profile.at("ipv4Addresses")) {
            if (addr.is_string() && !is_ipv4(addr.get<std::string>())) {
                return "ipv4Addresses contains an invalid IPv4 address";
            }
        }
    }
    if (profile.contains("ipv6Addresses") && profile.at("ipv6Addresses").is_array()) {
        for (const auto& addr : profile.at("ipv6Addresses")) {
            if (addr.is_string() && !is_ipv6(addr.get<std::string>())) {
                return "ipv6Addresses contains an invalid IPv6 address";
            }
        }
    }
    if (profile.contains("nfServices") && profile.at("nfServices").is_array()) {
        for (const auto& svc : profile.at("nfServices")) {
            if (svc.contains("scheme") && svc.at("scheme").is_string()) {
                const auto scheme = svc.at("scheme").get<std::string>();
                if (scheme != "http" && scheme != "https") {
                    return "nfServices[].scheme must be http or https";
                }
            }
            if (svc.contains("nfServiceStatus") && svc.at("nfServiceStatus").is_string() &&
                !known_status_values().contains(svc.at("nfServiceStatus").get<std::string>())) {
                return "nfServices[].nfServiceStatus '" +
                       svc.at("nfServiceStatus").get<std::string>() +
                       "' is not one of REGISTERED/SUSPENDED/UNDISCOVERABLE/CANARY_RELEASE";
            }
            if (svc.contains("ipEndPoints") && svc.at("ipEndPoints").is_array()) {
                for (const auto& ep : svc.at("ipEndPoints")) {
                    if (auto err = validate_ip_endpoint(ep); err.has_value()) {
                        return err;
                    }
                }
            }
        }
    }
    return std::nullopt;
}

} // namespace

int main() {
    const auto config = nf_config::load("nrf", CONFIG_DIR);
    const auto port = nf_config::require<unsigned short>(config, "port");
    const auto metrics_bind_address =
        nf_config::require<std::string>(config, "metrics_bind_address");
    const auto sbi_bind_address = nf_config::require<std::string>(config, "sbi_bind_address");

    sbi_core::init_logging("nrf");
    sbi_core::init_tracing("nrf");
    sbi_core::init_metrics(metrics_bind_address);

    const std::string certs_dir = CERTS_DIR;
    const sbi_core::http2::TlsConfig server_tls{
        .cert_path = certs_dir + "/nrf/cert.pem",
        .key_path = certs_dir + "/nrf/key.pem",
        .ca_path = certs_dir + "/ca/ca.crt",
    };
    const sbi_core::http2::TlsConfig client_tls = server_tls; // NRF is also a client for
                                                              // notification delivery.

    const std::string nrf_instance_id = kNrfInstanceId;
    spdlog::info("nrf: starting, nfInstanceId={}", nrf_instance_id);

    sbi_core::jwt::Issuer issuer(certs_dir + "/nrf-jwt/private.pem", nrf_instance_id);
    sbi_core::jwt::Verifier verifier(certs_dir + "/nrf-jwt/public.pem", nrf_instance_id);

    auto nf_registry = std::make_shared<nrf::NfRegistry>();
    auto sub_registry = std::make_shared<nrf::SubscriptionRegistry>();
    auto stored_searches = std::make_shared<nrf::StoredSearchStore>();
    auto notify_client = std::make_shared<sbi_core::http2::Client>(client_tls);

    auto meter = sbi_core::get_meter("nrf");
    auto registrations_counter = meter->CreateUInt64Counter(
        "nrf_registrations_total", "Total successful RegisterNFInstance calls");
    auto deregistrations_counter = meter->CreateUInt64Counter(
        "nrf_deregistrations_total", "Total successful DeregisterNFInstance calls");
    auto heartbeats_counter = meter->CreateUInt64Counter("nrf_heartbeats_total",
                                                         "Total successful UpdateNFInstance calls");
    auto tokens_counter =
        meter->CreateUInt64Counter("nrf_tokens_issued_total", "Total OAuth2 access tokens issued");

    // ADR-0262: the real TS 28.552 clause 5.10.1 / 5.10.3 measurements. Names are the spec's own,
    // cited in help text; metric names keep this project's `nrf_*_total` exporter namespace.
    //
    // The registration failure split is the spec's, not a status-code convenience: §5.10.1.3 is
    // failure due to ENCODING ERROR OF THE NF PROFILE (per TS 29.510) and §5.10.1.4 is failure due
    // to NRF INTERNAL ERROR. This handler's 400s are all profile-encoding failures (malformed
    // JSON, missing mandatory IE, invalid NFProfile), which is why they map to the first.
    auto nfs_reg_req_counter = meter->CreateUInt64Counter(
        "nrf_nfs_reg_req_total",
        "TS 28.552 5.10.1.1 NFS.RegReq -- NF service registration requests");
    auto nfs_reg_succ_counter = meter->CreateUInt64Counter(
        "nrf_nfs_reg_succ_total",
        "TS 28.552 5.10.1.2 NFS.RegSucc -- successful NF service registrations");
    auto nfs_reg_fail_encode_counter = meter->CreateUInt64Counter(
        "nrf_nfs_reg_fail_encode_err_total",
        "TS 28.552 5.10.1.3 NFS.RegFailEncodeErr -- failed registrations "
        "due to encoding error of the NF profile");
    // TS 28.552 5.10.1.4 (NFS.RegFailNrfErr) and 5.10.3.5 (NFS.DiscFailNrfErr) are
    // deliberately NOT exposed: neither handler has any internal-error path -- every
    // rejection either can produce is a profile-encoding or input-parameter failure.
    // Exporting a permanently-zero series would read as "measured, and none occurred",
    // which is a stronger claim than this build can make. Absent and explained beats
    // present and misleading.
    auto nfs_disc_req_counter = meter->CreateUInt64Counter(
        "nrf_nfs_disc_req_total", "TS 28.552 5.10.3.1 NFS.DiscReq -- NF discovery requests");
    auto nfs_disc_succ_counter = meter->CreateUInt64Counter(
        "nrf_nfs_disc_succ_total", "TS 28.552 5.10.3.2 NFS.DiscSucc -- successful NF discoveries");
    auto nfs_disc_fail_unauth_counter =
        meter->CreateUInt64Counter("nrf_nfs_disc_fail_unauth_total",
                                   "TS 28.552 5.10.3.3 NFS.DiscFailUnauth -- discoveries failed "
                                   "because the requester is unauthorized");
    auto nfs_disc_fail_input_counter =
        meter->CreateUInt64Counter("nrf_nfs_disc_fail_input_err_total",
                                   "TS 28.552 5.10.3.4 NFS.DiscFailInputErr -- discoveries failed "
                                   "due to an input parameter error");
    auto registered_gauge = meter->CreateInt64ObservableGauge(
        "nrf_registered_nf_count", "Number of NF instances currently registered");
    registered_gauge->AddCallback(
        [](opentelemetry::metrics::ObserverResult observer_result, void* state) {
            auto* registry = static_cast<nrf::NfRegistry*>(state);
            const auto count = static_cast<std::int64_t>(registry->list_all().size());
            if (auto obs = opentelemetry::nostd::get_if<opentelemetry::nostd::shared_ptr<
                    opentelemetry::metrics::ObserverResultT<std::int64_t>>>(&observer_result)) {
                (*obs)->Observe(count);
            }
        },
        nf_registry.get());

    auto notify =
        [notify_client, sub_registry, nrf_instance_id](const std::string& event,
                                                       const std::string& nf_instance_id,
                                                       const std::optional<json>& profile) {
            json body{
                {"event", event},
                {"nfInstanceUri", "/nnrf-nfm/v1/nf-instances/" + nf_instance_id},
            };
            if (profile.has_value()) {
                body["nfProfile"] = *profile;
            }
            const std::string body_str = body.dump();
            for (const auto& uri : sub_registry->all_notification_uris()) {
                sbi_core::http2::ClientRequest req;
                req.method = "POST";
                req.url = uri;
                req.headers.emplace("content-type", "application/json");
                req.body = body_str;
                auto resp = notify_client->send(req);
                if (!resp.has_value()) {
                    spdlog::warn("nrf: notification delivery to {} failed: {}", uri, resp.error());
                }
            }
        };

    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #102, ADR-0079): real heartbeat-expiry
    // sweep, open5GS's own real per-NF `t_no_heartbeat` mechanism as the model (not a byte-for-
    // byte port -- this project's own periodic-sweep design rather than a per-NF timer object).
    // Interval/margin are this lab's own reasonable, disclosed choice -- open5GS's own specific
    // numeric margin isn't published/known, not claimed to match it. Only NFs that themselves
    // supplied heartBeatTimer are ever swept (see NfRegistry::sweep_expired's own comment).
    constexpr std::chrono::seconds kHeartbeatSweepInterval{5};
    constexpr std::chrono::seconds kHeartbeatExpiryMargin{5};
    std::thread([nf_registry, notify, kHeartbeatSweepInterval, kHeartbeatExpiryMargin]() {
        while (true) {
            std::this_thread::sleep_for(kHeartbeatSweepInterval);
            for (const auto& id : nf_registry->sweep_expired(kHeartbeatExpiryMargin)) {
                spdlog::warn("nrf: NF instance {} missed its heartBeatTimer -- deregistering", id);
                notify("NF_DEREGISTERED", id, std::nullopt);
            }
        }
    }).detach();

    boost::asio::io_context ioc;
    // 0.0.0.0, not 127.0.0.1: found via actual Docker verification, not assumed -- a
    // loopback-only bind is unreachable from outside the container even with `docker run -p`,
    // since Docker's port mapping targets the container's external interface, not its loopback.
    // See docs/DECISIONS.md ADR-0014. Still reachable at 127.0.0.1 for anything running on the
    // same host/network namespace (hello-nf's local dev usage is unaffected).
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

    // Nnrf_Bootstrapping (TS29510_Nnrf_Bootstrapping.yaml v1.3.0) -- gap-closure per ADR-0193/
    // ADR-0194 (docs/CAPABILITY_GAP_ANALYSIS.md's ADR-0193 audit section). Real path exactly as
    // the vendored YAML declares it: `servers: url: '{nrfApiRoot}'` has no service-name/version
    // prefix (unlike NFManagement's `{apiRoot}/nnrf-nfm/v1`), and `paths:` declares bare
    // `/bootstrapping` -- Bootstrapping is meant to be reachable before an NF knows anything about
    // this NRF's other service paths, so no prefix is invented here.
    server.add_route("GET", "/bootstrapping", [](const sbi_core::http2::Request& /*req*/) {
        sbi_gen::BootstrappingInfo info;
        info.status = sbi_gen::Status{sbi_gen::Status::OPERATIVE};
        info.nrfInstanceId = std::string(kNrfInstanceId);
        // Real, disclosed: every route above (check_bearer) validates a bearer token only if
        // one is present -- the YAML's own `security: [{}, oAuth2ClientCredentials]` on those
        // operations explicitly permits the anonymous alternative, so this NRF does not
        // actually require OAuth2 today. oauth2Required is honestly false for the exact three
        // real services this NRF implements, not a placeholder value. Map keys per this
        // field's own YAML description ("e.g. nnrf-nfm or nnrf-disc").
        info.oauth2Required = json{
            {"nnrf-nfm", false},
            {"nnrf-disc", false},
            {"nnrf-oauth2", false},
        };
        // Real, disclosed gap: TS 29.510 clause 6.4.6.3.3 (the real link-relation vocabulary
        // for this required map) is stage-3 prose this project's vendored material doesn't
        // include -- only the OpenAPI schema, which places no enum on the map's own keys.
        // "self" is the one universally-defensible HAL relation (RFC 8288), used to satisfy
        // the schema's real `minProperties: 1` structural requirement -- not a claim to the
        // full real TS 29.510 relation set.
        info._links = json{
            {"self", json{{"href", "/bootstrapping"}}},
        };
        // nrfFeatures/nrfSetId deliberately omitted: both optional per the YAML, and no real
        // supported-features bitmask tracking or NRF Set concept exists in this project --
        // populating either would be fabricated data, not a real simplification.

        const json body = info;
        const std::string body_str = body.dump();
        sbi_core::http2::Response resp;
        resp.status = 200;
        // Real spec content type -- application/3gppHal+json, not application/json.
        resp.headers.emplace("content-type", "application/3gppHal+json");
        resp.headers.emplace("cache-control", "max-age=60");
        // Real, disclosed: the YAML declares an `If-None-Match` request parameter but never
        // declares a 304 response for this operation, so no conditional-request short-circuit
        // is implemented here -- a real content-derived ETag is still emitted so a client's
        // own cache stays correct on its next request.
        resp.headers.emplace("etag",
                             "\"" + std::to_string(std::hash<std::string>{}(body_str)) + "\"");
        resp.body = body_str;
        return resp;
    });

    server.add_route(
        "POST", "/oauth2/token", [&issuer, &tokens_counter](const sbi_core::http2::Request& req) {
            auto fields = parse_form_urlencoded(req.body);
            if (fields.count("grant_type") == 0 || fields["grant_type"] != "client_credentials") {
                return problem_response(400,
                                        "Unsupported grant_type",
                                        "Only grant_type=client_credentials is supported");
            }
            if (fields.count("nfInstanceId") == 0 || fields.count("scope") == 0) {
                return problem_response(
                    400, "Missing mandatory IE", "AccessTokenReq requires nfInstanceId and scope");
            }
            const std::string target_nf_type =
                fields.count("targetNfType") != 0 ? fields["targetNfType"] : kNfType;

            auto issued = issuer.issue(fields["nfInstanceId"],
                                       target_nf_type,
                                       fields["scope"],
                                       std::chrono::seconds(3600));
            tokens_counter->Add(1);

            json resp{
                {"access_token", issued.token},
                {"token_type", issued.token_type},
                {"expires_in", issued.expires_in.count()},
                {"scope", fields["scope"]},
            };
            return sbi_core::http2::Response::json(200, resp.dump());
        });

    // Gap-closure: RetrieveKeyRequest (TS29510_Nnrf_AccessToken.yaml). Real, disclosed: NRF only
    // knows its own real signing key -- an `issuer` other than this NRF's own instance id gets an
    // honest 404, not a fabricated key. `rawPubKey` is the real base64 SubjectPublicKeyInfo body
    // of `certs/nrf-jwt/public.pem` (the exact same key every NF's own sbi_core::jwt::Verifier
    // already loads to validate NRF-issued tokens), extracted by stripping the real PEM
    // "-----BEGIN/END PUBLIC KEY-----" delimiter lines -- not re-derived or fabricated key
    // material.
    server.add_route("POST",
                     "/oauth2/retrieve-key",
                     [&certs_dir, nrf_instance_id](const sbi_core::http2::Request& req) {
                         json body;
                         try {
                             body = json::parse(req.body);
                         } catch (const json::parse_error& e) {
                             return problem_response(400, "Malformed JSON", e.what());
                         }
                         if (!body.contains("issuer") || !body.contains("headerParameters")) {
                             return problem_response(
                                 400,
                                 "Missing mandatory IE",
                                 "RetrieveKeyReq requires issuer and headerParameters");
                         }
                         const auto requested_issuer = body.at("issuer").get<std::string>();
                         if (requested_issuer != nrf_instance_id) {
                             return problem_response(
                                 404, "Not Found", "No key known for issuer " + requested_issuer);
                         }
                         std::ifstream key_file(certs_dir + "/nrf-jwt/public.pem");
                         std::string raw_pub_key;
                         std::string line;
                         while (std::getline(key_file, line)) {
                             if (line.find("-----") == std::string::npos) {
                                 raw_pub_key += line;
                             }
                         }
                         json resp{{"rawPubKey", raw_pub_key}};
                         return sbi_core::http2::Response::json(200, resp.dump());
                     });

    server.add_route(
        "PUT",
        "/nnrf-nfm/v1/nf-instances/{nfInstanceID}",
        [&verifier,
         nf_registry,
         &notify,
         &registrations_counter,
         &nfs_reg_req_counter,
         &nfs_reg_succ_counter,
         &nfs_reg_fail_encode_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            // TS 28.552 5.10.1.1(c): counted on RECEIPT of the NFRegister request. An
            // unauthenticated caller is not an NFRegister request from an NF, so 401 is counted as
            // neither a request nor a failure -- the same reading ADR-0262 applies in SMF.
            nfs_reg_req_counter->Add(1);
            const auto path_id = req.path_params.at("nfInstanceID");
            json profile;
            try {
                profile = json::parse(req.body);
            } catch (const json::parse_error& e) {
                nfs_reg_fail_encode_counter->Add(1);
                return problem_response(400, "Malformed JSON", e.what());
            }
            for (const char* required : {"nfInstanceId", "nfType", "nfStatus"}) {
                if (!profile.contains(required)) {
                    nfs_reg_fail_encode_counter->Add(1);
                    return problem_response(400,
                                            "Missing mandatory IE",
                                            std::string("NFProfile missing '") + required + "'");
                }
            }
            if (auto err = validate_nf_profile(profile); err.has_value()) {
                nfs_reg_fail_encode_counter->Add(1);
                return problem_response(400, "Invalid NFProfile", *err);
            }
            const bool is_new = nf_registry->put(path_id, profile);
            spdlog::info("nrf: {} NF instance {} (type={}, status={})",
                         is_new ? "registered new" : "replaced",
                         path_id,
                         profile.value("nfType", "?"),
                         profile.value("nfStatus", "?"));
            registrations_counter->Add(1);
            // 5.10.1.2(c): on transmission of a response indicating SUCCESS. Both 201 (new) and
            // 200 (profile replaced) are successful registrations.
            nfs_reg_succ_counter->Add(1);
            notify(is_new ? "NF_REGISTERED" : "NF_PROFILE_CHANGED",
                   path_id,
                   std::make_optional(profile));

            sbi_core::http2::Response resp;
            resp.status = is_new ? 201 : 200;
            resp.headers.emplace("content-type", "application/json");
            if (is_new) {
                resp.headers.emplace("location", "/nnrf-nfm/v1/nf-instances/" + path_id);
            }
            resp.body = profile.dump();
            return resp;
        });

    server.add_route(
        "GET",
        "/nnrf-nfm/v1/nf-instances/{nfInstanceID}",
        [&verifier, nf_registry](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            const auto path_id = req.path_params.at("nfInstanceID");
            auto profile = nf_registry->get(path_id);
            if (!profile.has_value()) {
                return problem_response(404, "Not Found", "No NF instance with id " + path_id);
            }
            return sbi_core::http2::Response::json(200, profile->dump());
        });

    server.add_route("GET",
                     "/nnrf-nfm/v1/nf-instances",
                     [&verifier, nf_registry](const sbi_core::http2::Request& req) {
                         if (auto auth = check_bearer(req, verifier);
                             auth.has_value() && !auth->valid) {
                             return problem_response(401, "Unauthorized", auth->error);
                         }
                         json arr = json::array();
                         for (const auto& profile : nf_registry->list_all()) {
                             arr.push_back(profile);
                         }
                         return sbi_core::http2::Response::json(200, arr.dump());
                     });

    server.add_route(
        "PATCH",
        "/nnrf-nfm/v1/nf-instances/{nfInstanceID}",
        [&verifier, nf_registry, &notify, &heartbeats_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            const auto path_id = req.path_params.at("nfInstanceID");
            json patch_ops;
            try {
                patch_ops = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return problem_response(400, "Malformed JSON", e.what());
            }
            std::optional<json> patched;
            try {
                patched = nf_registry->apply_patch(path_id, patch_ops);
            } catch (const json::exception& e) {
                return problem_response(400, "Invalid JSON Patch", e.what());
            }
            if (!patched.has_value()) {
                return problem_response(404, "Not Found", "No NF instance with id " + path_id);
            }
            spdlog::info("nrf: heartbeat/update for {}", path_id);
            heartbeats_counter->Add(1);
            nf_registry->touch_heartbeat(path_id);
            notify("NF_PROFILE_CHANGED", path_id, patched);
            return sbi_core::http2::Response::json(200, patched->dump());
        });

    server.add_route(
        "DELETE",
        "/nnrf-nfm/v1/nf-instances/{nfInstanceID}",
        [&verifier, nf_registry, &notify, &deregistrations_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            const auto path_id = req.path_params.at("nfInstanceID");
            const bool existed = nf_registry->remove(path_id);
            spdlog::info(
                "nrf: deregister {} ({})", path_id, existed ? "removed" : "was not registered");
            if (existed) {
                deregistrations_counter->Add(1);
                notify("NF_DEREGISTERED", path_id, std::nullopt);
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // NFDiscovery.yaml's SearchNFInstances -- distinct base path (/nnrf-disc/v1) from the
    // NFManagement routes above (/nnrf-nfm/v1), both served by this same NRF process.
    server.add_route(
        "GET",
        "/nnrf-disc/v1/nf-instances",
        [&verifier,
         nf_registry,
         stored_searches,
         &nfs_disc_req_counter,
         &nfs_disc_succ_counter,
         &nfs_disc_fail_unauth_counter,
         &nfs_disc_fail_input_counter](const sbi_core::http2::Request& req) {
            // TS 28.552 5.10.3.1(c): counted on receipt of the NFDiscover request. Unlike
            // registration, discovery HAS a defined unauthorized-failure measurement
            // (5.10.3.3 NFS.DiscFailUnauth), so a rejected token is counted here as both a
            // request and an unauthorized failure rather than dropped.
            nfs_disc_req_counter->Add(1);
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                nfs_disc_fail_unauth_counter->Add(1);
                return problem_response(401, "Unauthorized", auth->error);
            }
            // Query string parsing: http2::Request doesn't split query params out (Phase 0's
            // router never needed to), so parse target-nf-type directly out of req.path here.
            const auto qpos = req.path.find('?');
            std::string target_type;
            if (qpos != std::string::npos) {
                auto qs = parse_form_urlencoded(req.path.substr(qpos + 1));
                if (auto it = qs.find("target-nf-type"); it != qs.end()) {
                    target_type = it->second;
                }
            }
            if (target_type.empty()) {
                nfs_disc_fail_input_counter->Add(1);
                return problem_response(
                    400, "Missing mandatory query parameter", "target-nf-type is required");
            }
            // TS 33.501 13.3.1.3 / TS 29.510 6.2.3.2.3.1: requester info for the allowed* checks
            // (ADR-0477). requester-plmn-list / requester-snssais are JSON-encoded arrays.
            nrf::DiscoveryRequester requester;
            if (qpos != std::string::npos) {
                auto qs = parse_form_urlencoded(req.path.substr(qpos + 1));
                if (auto it = qs.find("requester-nf-type"); it != qs.end()) {
                    requester.nf_type = it->second;
                }
                for (const auto& [name, dst] :
                     {std::pair<const char*, json*>{"requester-plmn-list", &requester.plmns},
                      std::pair<const char*, json*>{"requester-snssais", &requester.snssais}}) {
                    if (auto it = qs.find(name); it != qs.end()) {
                        *dst = json::parse(it->second, nullptr, false);
                        if (!dst->is_array()) {
                            nfs_disc_fail_input_counter->Add(1);
                            return problem_response(400,
                                                    "Invalid query parameter",
                                                    std::string(name) + " must be a JSON array");
                        }
                    }
                }
            }
            json instances = json::array();
            for (const auto& profile : nf_registry->search_by_type(target_type)) {
                if (nrf::profile_discoverable_by(profile, requester)) {
                    instances.push_back(profile);
                }
            }
            // Gap-closure: real searchId, cached so a later RetrieveStoredSearch/
            // RetrieveCompleteSearch (below) can re-fetch this same result.
            const auto search_id = stored_searches->put(instances);
            json result{
                {"validityPeriod", 60},
                {"nfInstances", instances},
                {"searchId", search_id},
            };
            nfs_disc_succ_counter->Add(1);
            return sbi_core::http2::Response::json(200, result.dump());
        });

    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md's Tier-B NRF audit): RetrieveStoredSearch /
    // RetrieveCompleteSearch. Real, disclosed: this NRF has no partial-vs-complete-profile
    // filtering, so both real operations return the same real nfInstances SearchNFInstances
    // already cached for this searchId -- not a fabricated distinction.
    server.add_route(
        "GET",
        "/nnrf-disc/v1/searches/{searchId}",
        [&verifier, stored_searches](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            const auto search_id = req.path_params.at("searchId");
            auto instances = stored_searches->get(search_id);
            if (!instances.has_value()) {
                return problem_response(404, "Not Found", "No stored search " + search_id);
            }
            json result{{"nfInstances", *instances}};
            return sbi_core::http2::Response::json(200, result.dump());
        });

    server.add_route(
        "GET",
        "/nnrf-disc/v1/searches/{searchId}/complete",
        [&verifier, stored_searches](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            const auto search_id = req.path_params.at("searchId");
            auto instances = stored_searches->get(search_id);
            if (!instances.has_value()) {
                return problem_response(404, "Not Found", "No stored search " + search_id);
            }
            json result{{"nfInstances", *instances}};
            return sbi_core::http2::Response::json(200, result.dump());
        });

    // Gap-closure: OptionsNFInstances (TS29510_Nnrf_NFManagement.yaml). Real capability-discovery
    // response, same real pattern already established for NSSF's own OPTIONS route (ADR-0183):
    // no `supportedFeatures` bitmask is implemented for NRF, so an honest 200 with just the real
    // `Accept-Encoding` header (no body) is returned rather than fabricating a features list.
    server.add_route(
        "OPTIONS", "/nnrf-nfm/v1/nf-instances", [&verifier](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("accept-encoding", "identity");
            return resp;
        });

    server.add_route(
        "POST",
        "/nnrf-nfm/v1/subscriptions",
        [&verifier, sub_registry](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            json sub_data;
            try {
                sub_data = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return problem_response(400, "Malformed JSON", e.what());
            }
            if (!sub_data.contains("nfStatusNotificationUri")) {
                return problem_response(400,
                                        "Missing mandatory IE",
                                        "SubscriptionData requires nfStatusNotificationUri");
            }
            auto created = sub_registry->create(sub_data);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 "/nnrf-nfm/v1/subscriptions/" +
                                     created.at("subscriptionId").get<std::string>());
            resp.body = created.dump();
            return resp;
        });

    server.add_route(
        "PATCH",
        "/nnrf-nfm/v1/subscriptions/{subscriptionID}",
        [&verifier, sub_registry](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return problem_response(401, "Unauthorized", auth->error);
            }
            const auto sub_id = req.path_params.at("subscriptionID");
            json sub_data;
            try {
                sub_data = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return problem_response(400, "Malformed JSON", e.what());
            }
            auto updated = sub_registry->update(sub_id, sub_data);
            if (!updated.has_value()) {
                return problem_response(404, "Not Found", "No subscription with id " + sub_id);
            }
            return sbi_core::http2::Response::json(200, updated->dump());
        });

    server.add_route("DELETE",
                     "/nnrf-nfm/v1/subscriptions/{subscriptionID}",
                     [&verifier, sub_registry](const sbi_core::http2::Request& req) {
                         if (auto auth = check_bearer(req, verifier);
                             auth.has_value() && !auth->valid) {
                             return problem_response(401, "Unauthorized", auth->error);
                         }
                         const auto sub_id = req.path_params.at("subscriptionID");
                         sub_registry->remove(sub_id);
                         sbi_core::http2::Response resp;
                         resp.status = 204;
                         return resp;
                     });

    server.start();
    spdlog::info("nrf: listening on https://{}:{} (TLS 1.3 + mTLS)", sbi_bind_address, port);
    spdlog::info("nrf: Prometheus metrics at http://{}/metrics", metrics_bind_address);
    sbi_core::run_multi_threaded(ioc);
    return 0;
}
