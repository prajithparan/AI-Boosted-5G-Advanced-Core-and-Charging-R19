// nfs/nssf: NSSF (Network Slice Selection Function), Nnssf_NSSelection + Nnssf_NSSAIAvailability
// services. Source: specs/5G_APIs-REL-19/TS29531_Nnssf_NSSelection.yaml (v2.4.0) and
// specs/5G_APIs-REL-19/TS29531_Nnssf_NSSAIAvailability.yaml (v1.4.0), commit
// bca84b60a37773133bcae97e5c6c0d10a93b47b6. This project's ninth NF -- the first Tier 1 NF built
// after the original NRF -> AMF -> SMF -> UDM -> UDR -> AUSF -> PCF order (docs/DECISIONS.md
// kickoff decisions); user-directed turn to move off UDR's gap-closure backlog (task #106,
// exhausted -- see ADR-0182) onto a whole new, previously-unbuilt NF, chosen from
// docs/CAPABILITY_GAP_ANALYSIS.md's own "still not done" list (nssf/nef/scp/bsf).
//
// In scope, agreed with the user before implementation -- all 8 real operations across both
// services (kNsSelectionApiRoot = /nnssf-nsselection/v2, kNssaiAvailApiRoot =
// /nnssf-nssaiavailability/v1, both defined below):
//   GET    {nsSelection}/network-slice-information            NSSelectionGet
//   PUT    {nssaiAvail}/nssai-availability/{nfId}              NSSAIAvailabilityPut
//   PATCH  {nssaiAvail}/nssai-availability/{nfId}              NSSAIAvailabilityPatch
//   DELETE {nssaiAvail}/nssai-availability/{nfId}              NSSAIAvailabilityDelete
//   POST   {nssaiAvail}/nssai-availability/subscriptions       NSSAIAvailabilityPost
//   DELETE {nssaiAvail}/nssai-availability/subscriptions/{id}  NSSAIAvailabilityUnsubscribe
//   PATCH  {nssaiAvail}/nssai-availability/subscriptions/{id}  NSSAIAvailabilitySubModifyPatch
//   OPTIONS {nssaiAvail}/nssai-availability                    NSSAIAvailabilityOptions
// Plus the real `nssaiAvailabilityNotification` callback this project sends OUT to each
// subscription's own `nfNssaiAvailabilityUri` (POST's own real callback declaration in the YAML).
//
// Real, disclosed simplifications -- stated up front, not discovered in review:
// 1. NSSelectionGet's own real slice-selection DECISION (subscriber entitlement checks against
//    UDM, network-slice-instance load/NRF discovery, NSAG-to-TA mapping) is out of scope for this
//    turn. This implementation seeds a fixed "network-supported NSSAI" catalog at startup
//    (seed_catalog() below -- there is no real write route for this in either YAML; a real
//    deployment's NSSF gets this from local configuration, which this project models as a
//    startup seed, same precedent as nfs/udr's seed()-only stores) and splits whatever S-NSSAI
//    list the caller asks about into allowed/rejected by catalog membership.
// 2. `AllowedNssai.accessType` is a REQUIRED field in the response schema, but NSSelectionGet's
//    own real query parameters (confirmed by direct read of TS29531_Nnssf_NSSelection.yaml) never
//    actually convey which access type the caller is asking about. This project always fills it
//    with "3GPP_ACCESS" -- a real, disclosed default, not an invented field.
// 3. NSSAIAvailabilityPut/Patch's own real "authorization" of submitted per-TA S-NSSAI data
//    (`authorize_availability` below) echoes every submitted `SupportedNssaiAvailabilityData` back
//    as authorized unchanged -- no real restriction/rejection logic (e.g. `restrictedSnssaiList`)
//    is modeled, since this project has no roaming-partner/PLMN-restriction data source to check
//    against yet.
// 4. `nssaiAvailabilityNotification` delivery only fires from NSSAIAvailabilityPut/Patch (a real
//    change to what's authorized). NSSAIAvailabilityDelete does NOT trigger it: the real
//    `NssfEventNotification` schema has no clean field for "this AMF's whole per-TA availability
//    was withdrawn" (its `unavailableNsiList`/`altNssai` fields are NSI/S-NSSAI-replacement
//    shaped, not "an NF instance deregistered") -- a real, disclosed gap rather than an invented
//    mapping onto fields that don't mean that.
// Delivery itself is synchronous, best-effort, non-blocking-to-the-caller -- same precedent as
// nfs/udr's own onDataChange webhook delivery (docs/DECISIONS.md ADR-0171); see that ADR for the
// full reasoning this reuses.

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

#include "TS26510_CommonData_grp.hpp"
#include "nf_config/nf_config.hpp"
#include "stores.hpp"

namespace {

using nlohmann::json;

#ifndef CERTS_DIR
#error "CERTS_DIR must be defined by CMake (see nfs/nssf/CMakeLists.txt)"
#endif
#ifndef CONFIG_DIR
#error "CONFIG_DIR must be defined by CMake (see nfs/nssf/CMakeLists.txt)"
#endif

constexpr const char* kNfType = "NSSF";
constexpr const char* kNsSelectionApiRoot = "/nnssf-nsselection/v2";
constexpr const char* kNssaiAvailApiRoot = "/nnssf-nssaiavailability/v1";

// Must match nfs/nrf/src/main.cpp's kNrfInstanceId exactly -- see docs/DECISIONS.md ADR-0018.
constexpr const char* kNrfInstanceId = "5ba9a927-1d31-4c8e-8a10-000000000001";

// Real, standardized SST values (TS 23.501 Table 5.15.2.2-1: 1 = eMBB, 2 = URLLC) -- not
// fabricated. See this file's own top comment, simplification 1, for why this is a fixed startup
// seed rather than a real per-deployment slice catalog.
std::vector<sbi_gen::Snssai> seed_catalog() {
    sbi_gen::Snssai embb;
    embb.sst = 1;
    sbi_gen::Snssai urllc;
    urllc.sst = 2;
    return {embb, urllc};
}

// Same pattern as every other NF's check_bearer -- see nfs/nrf/src/main.cpp's comment for why a
// missing Authorization header is not itself a 401 (bootstrap security alternative:
// `security: [{}, oAuth2ClientCredentials:[...]]` in both YAML files).
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

// A query parameter declared `content: application/json` in the YAML (rather than a plain
// `schema:`) carries a JSON-encoded value in the query string -- `req.query_params` is already
// percent-decoded (libs/sbi-core/src/http2_server.cpp's parse_query_string), so this just needs
// nlohmann::json::parse + get<T>() on the raw string value.
template <typename T>
std::optional<T> get_json_query_param(const sbi_core::http2::Request& req,
                                      const std::string& name) {
    auto it = req.query_params.find(name);
    if (it == req.query_params.end()) {
        return std::nullopt;
    }
    try {
        return nlohmann::json::parse(it->second).get<T>();
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

bool snssai_equal(const sbi_gen::Snssai& a, const sbi_gen::Snssai& b) {
    return a.sst == b.sst && a.sd.value_or("") == b.sd.value_or("");
}

bool tai_equal(const sbi_gen::Tai_CommonData& a, const sbi_gen::Tai_CommonData& b) {
    return a.plmnId.mcc == b.plmnId.mcc && a.plmnId.mnc == b.plmnId.mnc && a.tac == b.tac;
}

// See this file's own top comment, simplification 1-2.
sbi_gen::AuthorizedNetworkSliceInfo
decide_slice_selection(const std::vector<sbi_gen::Snssai>& catalog,
                       const std::optional<std::vector<sbi_gen::Snssai>>& requested) {
    sbi_gen::AuthorizedNetworkSliceInfo result;
    const std::vector<sbi_gen::Snssai>& candidates = requested.has_value() ? *requested : catalog;

    std::vector<sbi_gen::AllowedSnssai> allowed;
    std::vector<sbi_gen::Snssai> rejected;
    for (const auto& candidate : candidates) {
        const bool in_catalog = std::any_of(catalog.begin(), catalog.end(), [&](const auto& c) {
            return snssai_equal(c, candidate);
        });
        if (in_catalog) {
            sbi_gen::AllowedSnssai a;
            a.allowedSnssai = candidate;
            allowed.push_back(std::move(a));
        } else {
            rejected.push_back(candidate);
        }
    }

    if (!allowed.empty()) {
        sbi_gen::AllowedNssai an;
        an.allowedSnssaiList = std::move(allowed);
        an.accessType.value = sbi_gen::AccessType::V3GPP_ACCESS;
        result.allowedNssaiList = std::vector<sbi_gen::AllowedNssai>{std::move(an)};
    }
    if (!rejected.empty()) {
        result.rejectedNssaiInPlmn = std::move(rejected);
    }
    return result;
}

// See this file's own top comment, simplification 3.
sbi_gen::AuthorizedNssaiAvailabilityInfo
authorize_availability(const sbi_gen::NssaiAvailabilityInfo& info) {
    sbi_gen::AuthorizedNssaiAvailabilityInfo out;
    out.authorizedNssaiAvailabilityData.reserve(info.supportedNssaiAvailabilityData.size());
    for (const auto& supported : info.supportedNssaiAvailabilityData) {
        sbi_gen::AuthorizedNssaiAvailabilityData a;
        a.tai = supported.tai;
        a.supportedSnssaiList = supported.supportedSnssaiList;
        a.taiList = supported.taiList;
        a.taiRangeList = supported.taiRangeList;
        a.nsagInfos = supported.nsagInfos;
        out.authorizedNssaiAvailabilityData.push_back(std::move(a));
    }
    return out;
}

// Real `nssaiAvailabilityNotification` delivery (the callback declared in
// TS29531_Nnssf_NSSAIAvailability.yaml's own POST /nssai-availability/subscriptions operation:
// `{$request.body#/nfNssaiAvailabilityUri}`). See this file's own top comment, simplification 4,
// for scope (PUT/PATCH only) and the synchronous/best-effort delivery precedent this reuses from
// nfs/udr's onDataChange (ADR-0171). Matching: a subscription with no taiList, or an empty one, is
// real per the schema (`taiList` is genuinely optional) and is treated as "subscribed to every
// TA"; otherwise only subscriptions whose own taiList contains at least one of the TAs this update
// touched are notified.
void deliver_nssai_availability_notification(
    sbi_core::http2::Client& notify_client,
    nssf::NssaiAvailabilitySubscriptionStore& subs,
    const sbi_gen::AuthorizedNssaiAvailabilityInfo& authorized) {
    std::vector<sbi_gen::Tai_CommonData> touched_tais;
    touched_tais.reserve(authorized.authorizedNssaiAvailabilityData.size());
    for (const auto& data : authorized.authorizedNssaiAvailabilityData) {
        touched_tais.push_back(data.tai);
    }

    for (const auto& [sub_id, sub_json] : subs.list_all()) {
        sbi_gen::NssfEventSubscriptionCreateData sub;
        try {
            sub = sub_json.get<sbi_gen::NssfEventSubscriptionCreateData>();
        } catch (const nlohmann::json::exception&) {
            continue;
        }

        bool matched = !sub.taiList.has_value() || sub.taiList->empty();
        if (!matched) {
            for (const auto& sub_tai : *sub.taiList) {
                if (std::any_of(touched_tais.begin(), touched_tais.end(), [&](const auto& t) {
                        return tai_equal(t, sub_tai);
                    })) {
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) {
            continue;
        }

        sbi_gen::NssfEventNotification notif;
        notif.subscriptionId = sub_id;
        notif.authorizedNssaiAvailabilityData = authorized.authorizedNssaiAvailabilityData;

        sbi_core::http2::ClientRequest req;
        req.method = "POST";
        req.url = sub.nfNssaiAvailabilityUri;
        req.headers.emplace("content-type", "application/json");
        req.body = json(notif).dump();
        if (auto resp = notify_client.send(req); !resp.has_value() || resp->status != 204) {
            spdlog::warn("nssf: nssaiAvailabilityNotification delivery to {} failed or non-204 "
                         "(subscriptionId={})",
                         req.url,
                         sub_id);
        }
    }
}

// Runs on a dedicated thread, never on the server's io_context -- same reasoning as
// nfs/ausf/src/main.cpp's run_nrf_lifecycle (docs/DECISIONS.md ADR-0006/ADR-0019).
void run_nrf_lifecycle(const std::string& nssf_instance_id, const std::string& nrf_base) {
    sbi_core::http2::TlsConfig client_tls{
        .cert_path = CERTS_DIR "/nssf/cert.pem",
        .key_path = CERTS_DIR "/nssf/key.pem",
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
        http_client, nrf_base + "/oauth2/token", nssf_instance_id, "nnrf-nfm", "NRF");

    constexpr int kHeartbeatSeconds = 30;
    json profile{
        {"nfInstanceId", nssf_instance_id},
        {"nfType", kNfType},
        {"nfStatus", "REGISTERED"},
        {"ipv4Addresses", json::array({"127.0.0.1"})},
        {"heartBeatTimer", kHeartbeatSeconds},
    };

    while (true) {
        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("nssf: OAuth2 token fetch failed: {}", token.error());
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        sbi_core::http2::ClientRequest put_req;
        put_req.method = "PUT";
        put_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + nssf_instance_id;
        put_req.headers.emplace("content-type", "application/json");
        put_req.headers.emplace("authorization", "Bearer " + *token);
        put_req.headers.emplace(
            sbi_core::headers::kSenderTimestamp,
            sbi_core::headers::format_sender_timestamp(std::chrono::system_clock::now()));
        put_req.body = profile.dump();
        auto put_resp = http_client.send(put_req);
        if (put_resp.has_value() && (put_resp->status == 200 || put_resp->status == 201)) {
            spdlog::info("nssf: registered with NRF (HTTP {})", put_resp->status);
            break;
        }
        spdlog::warn("nssf: NRF registration attempt failed, retrying in 5s");
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(kHeartbeatSeconds / 2));

        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("nssf: OAuth2 token fetch failed for heartbeat: {}", token.error());
            continue;
        }

        sbi_core::http2::ClientRequest patch_req;
        patch_req.method = "PATCH";
        patch_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + nssf_instance_id;
        patch_req.headers.emplace("content-type", "application/json-patch+json");
        patch_req.headers.emplace("authorization", "Bearer " + *token);
        patch_req.body =
            json::array({json{{"op", "replace"}, {"path", "/nfStatus"}, {"value", "REGISTERED"}}})
                .dump();

        auto patch_resp = http_client.send(patch_req);
        if (!patch_resp.has_value() || patch_resp->status != 200) {
            spdlog::warn("nssf: heartbeat failed");
        }
    }
}

} // namespace

int main() {
    sbi_core::init_logging("nssf");
    sbi_core::init_tracing("nssf");

    // ADR-0077 (user-directed, mandatory, project-wide): no DB URL/connection/deployment
    // parameter may be a hardcoded literal default in source -- real values live in the
    // checked-in config/nssf.json, with an env var override per key still available.
    const auto config = nf_config::load("nssf", CONFIG_DIR);
    const auto port = nf_config::require<unsigned short>(config, "port");
    const auto metrics_bind_address =
        nf_config::require<std::string>(config, "metrics_bind_address");
    const auto sbi_bind_address = nf_config::require<std::string>(config, "sbi_bind_address");
    const auto nrf_base =
        nf_config::require<std::string>(config, "nrf_base_url", "NSSF_NRF_BASE_URL");

    sbi_core::init_metrics(metrics_bind_address);

    const std::string nssf_instance_id = sbi_core::generate_uuid_v4();
    spdlog::info("nssf: starting, nfInstanceId={}", nssf_instance_id);

    sbi_core::http2::TlsConfig server_tls{
        .cert_path = CERTS_DIR "/nssf/cert.pem",
        .key_path = CERTS_DIR "/nssf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };

    sbi_core::jwt::Verifier verifier(CERTS_DIR "/nrf-jwt/public.pem", kNrfInstanceId);

    // NSSF's own client identity for delivering nssaiAvailabilityNotification callbacks -- separate
    // sbi_core::http2::Client from run_nrf_lifecycle's (which runs on its own thread; this one is
    // only ever touched from route handlers, which all run on ioc's single thread -- see
    // http2_server.hpp).
    sbi_core::http2::TlsConfig notify_client_tls{
        .cert_path = CERTS_DIR "/nssf/cert.pem",
        .key_path = CERTS_DIR "/nssf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client notify_client(std::move(notify_client_tls));

    nssf::NssaiAvailabilityStore availability_store;
    nssf::NssaiAvailabilitySubscriptionStore subscription_store;
    const std::vector<sbi_gen::Snssai> nssai_catalog = seed_catalog();

    auto meter = sbi_core::get_meter("nssf");
    auto ns_selection_counter =
        meter->CreateUInt64Counter("nssf_ns_selection_total", "Total NSSelectionGet calls");
    auto avail_put_counter = meter->CreateUInt64Counter("nssf_nssai_availability_put_total",
                                                        "Total NSSAIAvailabilityPut calls");
    auto avail_patch_counter = meter->CreateUInt64Counter("nssf_nssai_availability_patch_total",
                                                          "Total NSSAIAvailabilityPatch calls");
    auto avail_delete_counter = meter->CreateUInt64Counter("nssf_nssai_availability_delete_total",
                                                           "Total NSSAIAvailabilityDelete calls");
    auto sub_create_counter = meter->CreateUInt64Counter("nssf_nssai_availability_sub_create_total",
                                                         "Total NSSAIAvailabilityPost calls");
    auto sub_delete_counter = meter->CreateUInt64Counter(
        "nssf_nssai_availability_unsubscribe_total", "Total NSSAIAvailabilityUnsubscribe calls");
    auto sub_patch_counter = meter->CreateUInt64Counter(
        "nssf_nssai_availability_sub_patch_total", "Total NSSAIAvailabilitySubModifyPatch calls");

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

    // --- Nnssf_NSSelection: network-slice-information ---

    server.add_route(
        "GET",
        std::string(kNsSelectionApiRoot) + "/network-slice-information",
        [&verifier, &nssai_catalog, &ns_selection_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            if (req.query_params.find("nf-type") == req.query_params.end() ||
                req.query_params.find("nf-id") == req.query_params.end()) {
                return sbi_core::http2::problem_response(
                    400, "Missing mandatory query parameter", "nf-type and nf-id are required");
            }
            ns_selection_counter->Add(1);

            std::optional<std::vector<sbi_gen::Snssai>> requested;
            if (auto reg = get_json_query_param<sbi_gen::SliceInfoForRegistration>(
                    req, "slice-info-request-for-registration");
                reg.has_value()) {
                if (reg->requestedNssai.has_value()) {
                    requested = *reg->requestedNssai;
                } else if (reg->subscribedNssai.has_value()) {
                    std::vector<sbi_gen::Snssai> v;
                    v.reserve(reg->subscribedNssai->size());
                    for (const auto& s : *reg->subscribedNssai) {
                        v.push_back(s.subscribedSnssai);
                    }
                    requested = std::move(v);
                }
            } else if (auto pdu = get_json_query_param<sbi_gen::SliceInfoForPDUSession>(
                           req, "slice-info-request-for-pdu-session");
                       pdu.has_value()) {
                requested = std::vector<sbi_gen::Snssai>{pdu->sNssai};
            } else if (auto cu = get_json_query_param<sbi_gen::SliceInfoForUEConfigurationUpdate>(
                           req, "slice-info-request-for-ue-cu");
                       cu.has_value()) {
                if (cu->requestedNssai.has_value()) {
                    requested = *cu->requestedNssai;
                } else if (cu->subscribedNssai.has_value()) {
                    std::vector<sbi_gen::Snssai> v;
                    v.reserve(cu->subscribedNssai->size());
                    for (const auto& s : *cu->subscribedNssai) {
                        v.push_back(s.subscribedSnssai);
                    }
                    requested = std::move(v);
                }
            } else if (auto pdn = get_json_query_param<std::vector<sbi_gen::Snssai>>(
                           req, "slice-info-request-for-pdn-connection");
                       pdn.has_value()) {
                requested = *pdn;
            } else if (auto other = get_json_query_param<std::vector<sbi_gen::Snssai>>(
                           req, "slice-info-request-for-other-purpose");
                       other.has_value()) {
                requested = *other;
            }

            auto result = decide_slice_selection(nssai_catalog, requested);
            return sbi_core::http2::Response::json(200, json(result).dump());
        });

    // --- Nnssf_NSSAIAvailability: nssai-availability/subscriptions (registered before the
    // {nfId}-parametrized routes below; no real segment-count+method collision exists between
    // them here, but literal-before-wildcard stays the project's own defensive convention -- see
    // docs/DECISIONS.md ADR-0168/ADR-0169). ---

    server.add_route(
        "POST",
        std::string(kNssaiAvailApiRoot) + "/nssai-availability/subscriptions",
        [&verifier, &subscription_store, &sub_create_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::NssfEventSubscriptionCreateData>(
                req, err);
            if (!body.has_value()) {
                return err;
            }

            sub_create_counter->Add(1);
            const auto id = subscription_store.create(json(*body));

            sbi_gen::NssfEventSubscriptionCreatedData created;
            created.subscriptionId = id;
            created.expiry = body->expiry;

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kNssaiAvailApiRoot) +
                                     "/nssai-availability/subscriptions/" + id);
            resp.body = json(created).dump();
            return resp;
        });

    server.add_route(
        "DELETE",
        std::string(kNssaiAvailApiRoot) + "/nssai-availability/subscriptions/{subscriptionId}",
        [&verifier, &subscription_store, &sub_delete_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            if (!subscription_store.remove(id)) {
                return sbi_core::http2::problem_response(404, "Not Found", "No subscription " + id);
            }
            sub_delete_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "PATCH",
        std::string(kNssaiAvailApiRoot) + "/nssai-availability/subscriptions/{subscriptionId}",
        [&verifier, &subscription_store, &sub_patch_counter](const sbi_core::http2::Request& req) {
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
            auto patched = subscription_store.patch(id, patch_ops);
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(404, "Not Found", "No subscription " + id);
            }
            sub_patch_counter->Add(1);

            sbi_gen::NssfEventSubscriptionCreatedData created;
            created.subscriptionId = id;
            try {
                created.expiry = patched->get<sbi_gen::NssfEventSubscriptionCreateData>().expiry;
            } catch (const json::exception&) {
                // Real, disclosed: a patch that produces a document no longer matching
                // NssfEventSubscriptionCreateData's own shape still succeeded as a raw RFC 6902
                // patch (same precedent as nfs/nrf's NfRegistry::apply_patch) -- the response is
                // just missing the optional expiry echo in that case, not an error.
            }
            return sbi_core::http2::Response::json(200, json(created).dump());
        });

    // --- Nnssf_NSSAIAvailability: nssai-availability/{nfId} ---

    server.add_route(
        "PUT",
        std::string(kNssaiAvailApiRoot) + "/nssai-availability/{nfId}",
        [&verifier, &availability_store, &subscription_store, &notify_client, &avail_put_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto nf_id = req.path_params.at("nfId");
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::NssaiAvailabilityInfo>(req, err);
            if (!body.has_value()) {
                return err;
            }

            avail_put_counter->Add(1);
            availability_store.put(nf_id, json(*body));

            auto authorized = authorize_availability(*body);
            deliver_nssai_availability_notification(notify_client, subscription_store, authorized);
            return sbi_core::http2::Response::json(200, json(authorized).dump());
        });

    server.add_route(
        "PATCH",
        std::string(kNssaiAvailApiRoot) + "/nssai-availability/{nfId}",
        [&verifier, &availability_store, &subscription_store, &notify_client, &avail_patch_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto nf_id = req.path_params.at("nfId");
            json patch_ops;
            try {
                patch_ops = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            auto patched = availability_store.patch(nf_id, patch_ops);
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No NSSAI availability data for nfId " + nf_id);
            }
            avail_patch_counter->Add(1);

            sbi_gen::NssaiAvailabilityInfo info;
            try {
                info = patched->get<sbi_gen::NssaiAvailabilityInfo>();
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(
                    400, "Patch produced an invalid NssaiAvailabilityInfo", e.what());
            }
            auto authorized = authorize_availability(info);
            deliver_nssai_availability_notification(notify_client, subscription_store, authorized);
            return sbi_core::http2::Response::json(200, json(authorized).dump());
        });

    server.add_route(
        "DELETE",
        std::string(kNssaiAvailApiRoot) + "/nssai-availability/{nfId}",
        [&verifier, &availability_store, &avail_delete_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto nf_id = req.path_params.at("nfId");
            if (!availability_store.remove(nf_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No NSSAI availability data for nfId " + nf_id);
            }
            avail_delete_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nnssf_NSSAIAvailability: bare nssai-availability (capability discovery only) ---

    server.add_route(
        "OPTIONS",
        std::string(kNssaiAvailApiRoot) + "/nssai-availability",
        [&verifier](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("accept-encoding", "identity");
            return resp;
        });

    std::thread(run_nrf_lifecycle, nssf_instance_id, nrf_base).detach();

    server.start();
    spdlog::info("nssf: listening on https://{}:{} (TLS 1.3 + mTLS)", sbi_bind_address, port);
    spdlog::info("nssf: Prometheus metrics at http://{}/metrics", metrics_bind_address);
    sbi_core::run_multi_threaded(ioc);
    return 0;
}
