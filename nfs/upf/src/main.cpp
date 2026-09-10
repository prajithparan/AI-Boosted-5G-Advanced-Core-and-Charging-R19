// nfs/upf: UPF (User Plane Function) -- Phase 3 Stages 1+3 (docs/DECISIONS.md ADR-0039/ADR-0042).
// PFCP/N4 server (TS 29.244), UPF's real primary protocol interface -- real 3GPP architecture has
// SMF talk to UPF over N4/PFCP for session control, never SBI, for that core exchange. UPF's SBI
// role has always included being a REGISTRATION CLIENT to NRF (real: NFType=UPF and
// NFProfile.upfInfo are genuine fields in TS26510_CommonData_grp.hpp's generated types, confirmed
// before writing this, not assumed) so SMF can discover it -- Stage 2 wires that discovery up for
// real.
//
// CORRECTION (ADR-0203, gap-closure task #162): this file's own header previously claimed "no
// Nupf_* API exists in the OpenAPI corpus" -- found, during ADR-0193's audit, to be false. Real
// R17+ UPF direct-exposure services DO exist in this project's own vendored R19 archive:
// TS29564_Nupf_EventExposure.yaml and TS29564_Nupf_GetUEPrivateIPaddrAndIdentifiers.yaml, both
// real, optional SBI services layered on top of UPF's still-primary N4/PFCP control interface (not
// a replacement for it -- TS 23.501's own architecture is unchanged). Both are now wired -- see the
// UPDATE below -- making this UPF the first in this project to run a real inbound SBI HTTP/2
// server alongside its PFCP one, each on its own thread/io_context.
//
// Implements: Heartbeat (§7.4.2), Association Setup (§7.4.4.1/§7.4.4.2), and Session
// Establishment (§7.5.2/§7.5.3, ADR-0042) -- specifically one uplink PDR/FAR pair per session
// (Source Interface=Access with a UP-allocated F-TEID, forwarding to Core), the minimal real
// slice TS 23.502's PDU Session Establishment needs. No downlink PDR/FAR: that needs the gNB's
// N3 GTP-U endpoint, which requires NGAP PDU Session Resource Setup (still not implemented, a
// disclosed gap predating this turn -- ADR-0038's own N2 SM info note).
//
// Deliberately deferred, not dropped: Session Modification/Deletion, Association Update/Release,
// Node Report, PFD Management, QER/URR (QoS enforcement/usage reporting) -- everything this build
// doesn't need yet. No packet forwarding datapath exists yet either (Stage 4, eBPF/XDP --
// ADR-0039); Session Establishment here allocates a real F-TEID and echoes it back, but no packet
// ever actually flows through it -- disclosed, not silently implied to work end-to-end.
//
// Disclosed simplification: this build never terminates -- no SIGINT/SIGTERM handling, matching
// every other NF in this project (none of them have graceful shutdown either).
//
// ADR-0050 Stage 2 update: a minimal per-TEID session map (TeidSessionStore below) now DOES exist
// -- SMF's real address and this session's CP F-SEID/URR ID, remembered only for as long as it
// takes to address a real, unsolicited Sx Session Report Request back to SMF once the datapath's
// real per-TEID byte counter (nfs/upf/bpf/gtpu_decap.bpf.c) crosses a provisioned Volume
// Threshold/Quota. Still in-memory only, still lost on restart -- no real Session
// Modification/Deletion exists yet to ever remove an entry either (a real, disclosed gap, not
// urgent while this project has no process-restart/session-teardown testing).
//
// UPDATE (ADR-0203, gap-closure task #162): UPF's first-ever real inbound SBI HTTP/2 server,
// TLS 1.3 + mTLS + OAuth2-verified like every other NF in this project, on its own dedicated
// thread/io_context (run_sbi_server, started from main() alongside run_nrf_lifecycle and
// run_pfcp_lifecycle -- three real, independent threads now, not two).
// - TS29564_Nupf_EventExposure.yaml (api root /nupf-ee/v1): all 3 operations, backed by a new
//   EventSubscriptionStore (event_subscription_store.hpp -- same real assign-id/store/remove
//   shape as nfs/smf/src's own ADR-0201 store, deliberately re-implemented rather than shared,
//   since NFs may not include each other's private headers). CreateSubscription real structural
//   validation of CreateEventSubscription's required subscription (itself requiring
//   eventList/eventNotifyUri/notifyCorrelationId/eventReportingMode/nfId), real 201+Location.
//   ModifySubscription real get/404 then real RFC 6902 JSON Patch
//   (nlohmann::json::patch()) against the stored subscription, real 200. DeleteSubscription real
//   get/404-then-remove/204. Disclosed: no real event notification delivery exists -- this UPF
//   has no real QoS-monitoring/usage-measurement/NAT-mapping data production pipeline behind any
//   of the real EventType values (QOS_MONITORING, USER_DATA_USAGE_MEASURES, ...), so nothing
//   would ever have real data to notify with even if delivery were wired up.
// - TS29564_Nupf_GetUEPrivateIPaddrAndIdentifiers.yaml (api root /nupf-gueip/v1):
//   SearchUeIpInfo, real query-parameter parsing (all optional per spec), then a real, honestly-
//   empty UeIpInfo{} 200 -- disclosed: this UPF's own real per-session state
//   (TeidSessionStore/SeidToTeidStore) tracks F-TEID/CP-SEID for Sx reporting, not any real
//   private/public UE IP address or NAT mapping (no real NAT44/CGN capability exists anywhere in
//   this build), so there is no real data to search.

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
#include <boost/asio/ip/udp.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>

#include "TS26510_CommonData_grp.hpp"
#include "TS29564_Nupf_GetUEPrivateIPaddrAndIdentifiers.hpp"
#include "datapath.hpp"
#include "event_subscription_store.hpp"
#include "pfcp_core/common_ies.hpp"
#include "pfcp_core/header.hpp"
#include "pfcp_core/ie.hpp"
#include "pfcp_core/pfd_ies.hpp"
#include "pfcp_core/session_ies.hpp"

// docs/DECISIONS.md ADR-0077 -- no hardcoded deployment literal in source.
#include "nf_config/nf_config.hpp"

#ifndef CERTS_DIR
#error "CERTS_DIR must be defined by CMake (see nfs/upf/CMakeLists.txt)"
#endif
#ifndef CONFIG_DIR
#error "CONFIG_DIR must be defined by CMake (see nfs/upf/CMakeLists.txt)"
#endif

namespace {

using nlohmann::json;

constexpr const char* kNfType = "UPF";

// Must match nfs/nrf/src/main.cpp's kNrfInstanceId exactly -- see docs/DECISIONS.md ADR-0018.
constexpr const char* kNrfInstanceId = "5ba9a927-1d31-4c8e-8a10-000000000001";

// TS29564_Nupf_EventExposure.yaml / TS29564_Nupf_GetUEPrivateIPaddrAndIdentifiers.yaml own real
// api roots (ADR-0203), confirmed via each YAML's own `servers:` block.
constexpr const char* kEeApiRoot = "/nupf-ee/v1";
constexpr const char* kGueipApiRoot = "/nupf-gueip/v1";

// Same real pattern every other NF's own local check_bearer uses (e.g. nfs/smf/src/main.cpp) --
// UPF never needed one before this ADR, since it had no inbound SBI server of its own.
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

// This project's only configured S-NSSAI/DNN combination throughout (simulators/ransim/config/
// gnb.yaml's sst=1/sd=1, SMF's own dnn="internet" default) -- reused here so UPF's advertised
// upfInfo genuinely matches what Stage 3's real N4 Session Establishment will ask for, not an
// arbitrary placeholder.
constexpr std::int64_t kSst = 1;
constexpr const char* kSd = "000001";
constexpr const char* kDnn = "internet";

// ADR-0050 Stage 2: what UPF needs to remember, per allocated uplink TEID, to address a real
// unsolicited Sx Session Report Request back to SMF once a Volume Threshold/Quota crossing fires.
// Populated by run_pfcp_lifecycle's main thread on Session Establishment; read (and its per-URR
// UR-SEQN counter advanced) from Datapath's own ring-buffer-polling thread whenever the usage
// report handler fires -- hence the mutex, unlike every other piece of session-establishment state
// in this file, which never leaves run_pfcp_lifecycle's single thread.
struct UrrSessionInfo {
    boost::asio::ip::udp::endpoint smf_endpoint;
    std::uint64_t cp_seid = 0;
    std::uint32_t urr_id = 0;
};

class TeidSessionStore {
public:
    void put(std::uint32_t teid, UrrSessionInfo info) {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_[teid] = info;
        next_ur_seqn_[teid] = 1; // TS 29.244 UR-SEQN: this project's own per-URR counter, not the
                                 // PFCP header's separate node-level Sequence Number (see
                                 // usage_report_handler's own comment on that distinction).
    }

    // Returns the session info plus the UR-SEQN to use for this report, advancing the counter for
    // next time. std::nullopt if no Create URR was ever provisioned for this TEID (e.g. the usage
    // report handler firing for a TEID this store never learned about -- shouldn't happen given
    // the datapath only counts TEIDs register_urr was called for, but checked rather than assumed).
    std::optional<std::pair<UrrSessionInfo, std::uint32_t>>
    get_and_advance_seqn(std::uint32_t teid) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = sessions_.find(teid);
        if (it == sessions_.end()) {
            return std::nullopt;
        }
        const std::uint32_t seqn = next_ur_seqn_[teid]++;
        return std::make_pair(it->second, seqn);
    }

    // ADR-0050 Stage 5: a pure, non-mutating read -- Session Modification Response needs this
    // session's real cp_seid (to address the response correctly, TS 29.244's addressing rule) but
    // must not advance the UR-SEQN counter as a side effect of that lookup.
    std::optional<UrrSessionInfo> get(std::uint32_t teid) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = sessions_.find(teid);
        if (it == sessions_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    // ADR-0071, gap-closure Tier 1d: real Session Deletion cleanup -- without this, a deleted
    // session's entry would live here for the rest of the process's lifetime (this project's own
    // disclosed "no restart/session-teardown testing yet" gap, from before Session Deletion had
    // any handler at all, no longer applies once this is called from one). No-op if the TEID isn't
    // present (already removed, or never had a URR).
    void remove(std::uint32_t teid) {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.erase(teid);
        next_ur_seqn_.erase(teid);
    }

private:
    std::mutex mutex_;
    std::unordered_map<std::uint32_t, UrrSessionInfo> sessions_;
    std::unordered_map<std::uint32_t, std::uint32_t> next_ur_seqn_;
};

// ADR-0050 Stage 5: resolves a real Session Modification Request's header SEID (this UPF's own
// F-SEID for the session, allocated at Establishment -- see SessionEstablishmentResult::up_seid)
// back to the TEID it corresponds to. Written and read on run_pfcp_lifecycle's single thread only
// (unlike TeidSessionStore, no datapath-thread access here) -- mutex-guarded anyway, for the same
// "don't rely on today's single-thread access staying true" reasoning already applied elsewhere in
// this file.
class SeidToTeidStore {
public:
    void put(std::uint64_t seid, std::uint32_t teid) {
        std::lock_guard<std::mutex> lock(mutex_);
        seid_to_teid_[seid] = teid;
    }

    std::optional<std::uint32_t> get(std::uint64_t seid) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = seid_to_teid_.find(seid);
        if (it == seid_to_teid_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    // ADR-0071: same real Session Deletion cleanup rationale as TeidSessionStore::remove above.
    void remove(std::uint64_t seid) {
        std::lock_guard<std::mutex> lock(mutex_);
        seid_to_teid_.erase(seid);
    }

private:
    std::mutex mutex_;
    std::unordered_map<std::uint64_t, std::uint32_t> seid_to_teid_;
};

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #107 part 2, ADR-0086): provisioning-plane
// storage for PFDs pushed by a real Sx PFD Management Request (TS 29.244 §7.4.3), keyed by
// Application ID. Real, disclosed gap: this project's UPF has no Application Detection Filter
// (ADF) traffic-classification engine yet, so nothing on the data-plane side actually reads from
// this store today -- it exists so PFDs can be received, replaced, and deleted correctly per the
// real spec semantics (see build_pfd_management_response_ies's own comment), a real, separate,
// much larger gap than this message pair alone. Only ever touched by run_pfcp_lifecycle's own
// single thread today; mutex-guarded anyway, same "don't rely on today's single-thread access
// staying true" reasoning as SeidToTeidStore above.
class PfdStore {
public:
    void put(const std::string& application_id, std::vector<pfcp_core::PfdContents> pfds) {
        std::lock_guard<std::mutex> lock(mutex_);
        pfds_by_app_id_[application_id] = std::move(pfds);
    }

    void remove(const std::string& application_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        pfds_by_app_id_.erase(application_id);
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        pfds_by_app_id_.clear();
    }

    std::size_t application_count() {
        std::lock_guard<std::mutex> lock(mutex_);
        return pfds_by_app_id_.size();
    }

private:
    std::mutex mutex_;
    std::unordered_map<std::string, std::vector<pfcp_core::PfdContents>> pfds_by_app_id_;
};

// ADR-0050 Stage 2: a small, dedicated UDP socket the usage report handler uses (from Datapath's
// own polling thread) to fire-and-forget a Session Report Request to SMF. Deliberately separate
// from run_pfcp_lifecycle's own receive socket -- that one is only ever touched by the main
// thread; giving the datapath thread its own socket avoids needing to reason about concurrent
// send/receive on one boost::asio::ip::udp::socket from two threads at once.
class ReportSender {
public:
    ReportSender() : socket_(ioc_, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), 0)) {}

    void send(const boost::asio::ip::udp::endpoint& target,
              const std::vector<std::uint8_t>& bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        boost::system::error_code ec;
        socket_.send_to(boost::asio::buffer(bytes), target, 0, ec);
        if (ec) {
            spdlog::warn("upf: failed to send unsolicited PFCP message to {}: {}",
                         target.address().to_string(),
                         ec.message());
        }
    }

private:
    boost::asio::io_context ioc_;
    boost::asio::ip::udp::socket socket_;
    std::mutex mutex_;
};

// Same pattern as every other NF's run_nrf_lifecycle (docs/DECISIONS.md ADR-0006/ADR-0019), with
// one real difference: UPF has no HTTP2 server of its own to advertise (see file header) -- this
// is purely an outbound SBI client role.
void run_nrf_lifecycle(const std::string& upf_instance_id, const std::string& nrf_base) {
    sbi_core::http2::TlsConfig client_tls{
        .cert_path = CERTS_DIR "/upf/cert.pem",
        .key_path = CERTS_DIR "/upf/key.pem",
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
        http_client, nrf_base + "/oauth2/token", upf_instance_id, "nnrf-nfm", "NRF");

    constexpr int kHeartbeatSeconds = 30;

    sbi_gen::ExtSnssai snssai{};
    snssai.sst = kSst;
    snssai.sd = kSd;
    sbi_gen::DnnUpfInfoItem dnn_info{};
    dnn_info.dnn = kDnn;
    sbi_gen::SnssaiUpfInfoItem snssai_upf_info{};
    snssai_upf_info.sNssai = snssai;
    snssai_upf_info.dnnUpfInfoList = std::vector<sbi_gen::DnnUpfInfoItem>{dnn_info};
    sbi_gen::UpfInfo upf_info{};
    upf_info.sNssaiUpfInfoList = std::vector<sbi_gen::SnssaiUpfInfoItem>{snssai_upf_info};

    json profile{
        {"nfInstanceId", upf_instance_id},
        {"nfType", kNfType},
        {"nfStatus", "REGISTERED"},
        {"ipv4Addresses", json::array({"127.0.0.1"})},
        {"heartBeatTimer", kHeartbeatSeconds},
        {"upfInfo", json(upf_info)},
    };

    while (true) {
        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("upf: OAuth2 token fetch failed: {}", token.error());
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        sbi_core::http2::ClientRequest put_req;
        put_req.method = "PUT";
        put_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + upf_instance_id;
        put_req.headers.emplace("content-type", "application/json");
        put_req.headers.emplace("authorization", "Bearer " + *token);
        put_req.headers.emplace(
            sbi_core::headers::kSenderTimestamp,
            sbi_core::headers::format_sender_timestamp(std::chrono::system_clock::now()));
        put_req.body = profile.dump();

        auto put_resp = http_client.send(put_req);
        if (put_resp.has_value() && (put_resp->status == 200 || put_resp->status == 201)) {
            spdlog::info("upf: registered with NRF (HTTP {})", put_resp->status);
            break;
        }
        spdlog::warn("upf: NRF registration attempt failed, retrying in 5s");
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(kHeartbeatSeconds / 2));

        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("upf: OAuth2 token fetch failed for heartbeat: {}", token.error());
            continue;
        }

        sbi_core::http2::ClientRequest patch_req;
        patch_req.method = "PATCH";
        patch_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + upf_instance_id;
        patch_req.headers.emplace("content-type", "application/json-patch+json");
        patch_req.headers.emplace("authorization", "Bearer " + *token);
        patch_req.body =
            json::array({json{{"op", "replace"}, {"path", "/nfStatus"}, {"value", "REGISTERED"}}})
                .dump();

        auto patch_resp = http_client.send(patch_req);
        if (!patch_resp.has_value() || patch_resp->status != 200) {
            spdlog::warn("upf: heartbeat failed");
        }
    }
}

// ADR-0203: UPF's first-ever real inbound SBI HTTP/2 server (TS29564_Nupf_EventExposure.yaml +
// TS29564_Nupf_GetUEPrivateIPaddrAndIdentifiers.yaml). Runs on its own dedicated thread/
// io_context, same "never on another subsystem's thread" reasoning as run_nrf_lifecycle above and
// AMF's own NGAP thread (ADR-0030/ADR-0031) -- blocks forever via its own ioc.run(), never
// returns.
void run_sbi_server(unsigned short port,
                    upf::EventSubscriptionStore& event_subs,
                    sbi_core::TpsLimitConfig tps_limit) {
    sbi_core::http2::TlsConfig server_tls{
        .cert_path = CERTS_DIR "/upf/cert.pem",
        .key_path = CERTS_DIR "/upf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::jwt::Verifier verifier(CERTS_DIR "/nrf-jwt/public.pem", kNrfInstanceId);

    auto meter = sbi_core::get_meter("upf");
    auto ee_create_counter = meter->CreateUInt64Counter(
        "upf_ee_create_subscription_total", "Total Nupf_EventExposure CreateSubscription calls");
    auto ee_modify_counter = meter->CreateUInt64Counter(
        "upf_ee_modify_subscription_total", "Total Nupf_EventExposure ModifySubscription calls");
    auto ee_delete_counter = meter->CreateUInt64Counter(
        "upf_ee_delete_subscription_total", "Total Nupf_EventExposure DeleteSubscription calls");
    auto gueip_search_counter = meter->CreateUInt64Counter(
        "upf_gueip_search_ue_ip_info_total",
        "Total Nupf_GetUEPrivateIPaddrAndIdentifiers SearchUeIpInfo calls");

    boost::asio::io_context ioc;
    // 0.0.0.0: same Docker-reachability reasoning as every other NF's own bind (ADR-0014).
    sbi_core::http2::Server server(ioc, "0.0.0.0", port, server_tls);

    // P15 / P4.12 (ADR-0280): optional TPS ceiling from this NF's own config (`max_tps`,
    // `tps_burst`), overridable per deployment via SBI_MAX_TPS. Absent means unlimited, so this
    // changes nothing until an operator opts in.
    if (tps_limit.enabled()) {
        server.set_tps_limit(tps_limit.sustained_tps, tps_limit.burst);
        spdlog::info("TPS ceiling active: {} req/s sustained, burst {}",
                     tps_limit.sustained_tps,
                     tps_limit.burst > 0.0 ? tps_limit.burst : tps_limit.sustained_tps);
    }

    // ---- TS29564_Nupf_EventExposure.yaml ----

    server.add_route(
        "POST",
        std::string(kEeApiRoot) + "/ee-subscriptions",
        [&verifier, &event_subs, &ee_create_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::CreateEventSubscription>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto id = event_subs.create(body->subscription);
            ee_create_counter->Add(1);
            sbi_gen::CreatedEventSubscription resp_data;
            resp_data.subscription = body->subscription;
            resp_data.subscriptionId = id;
            json j = resp_data;
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location", std::string(kEeApiRoot) + "/ee-subscriptions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "PATCH",
        std::string(kEeApiRoot) + "/ee-subscriptions/{subscriptionId}",
        [&verifier, &event_subs, &ee_modify_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto sub_id = req.path_params.at("subscriptionId");
            auto existing = event_subs.get(sub_id);
            if (!existing.has_value()) {
                return sbi_core::http2::problem_response(404, "Not Found", "No such subscription");
            }
            json patch_doc;
            try {
                patch_doc = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            json patched;
            try {
                patched = existing->patch(patch_doc);
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", std::string("Invalid JSON Patch: ") + e.what());
            }
            sbi_gen::UpfEventSubscription updated;
            try {
                updated = patched.get<sbi_gen::UpfEventSubscription>();
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", std::string("Patched document invalid: ") + e.what());
            }
            event_subs.update(sub_id, patched);
            ee_modify_counter->Add(1);
            json j = updated;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "DELETE",
        std::string(kEeApiRoot) + "/ee-subscriptions/{subscriptionId}",
        [&verifier, &event_subs, &ee_delete_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto sub_id = req.path_params.at("subscriptionId");
            if (!event_subs.get(sub_id).has_value()) {
                return sbi_core::http2::problem_response(404, "Not Found", "No such subscription");
            }
            event_subs.remove(sub_id);
            ee_delete_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // ---- TS29564_Nupf_GetUEPrivateIPaddrAndIdentifiers.yaml ----

    server.add_route(
        "GET",
        std::string(kGueipApiRoot) + "/ue-ip-info",
        [&verifier, &gueip_search_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            gueip_search_counter->Add(1);
            // Disclosed simplification: honestly-empty UeIpInfo -- this UPF's own real per-
            // session state (TeidSessionStore/SeidToTeidStore) tracks F-TEID/CP-SEID for Sx
            // reporting, not any real private/public UE IP address or NAT mapping (no real
            // NAT44/CGN capability exists anywhere in this build).
            sbi_gen::UeIpInfo resp_data;
            json j = resp_data;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.start();
    spdlog::info("upf: SBI server listening on https://0.0.0.0:{} (TLS 1.3 + mTLS)", port);
    sbi_core::run_multi_threaded(ioc); // blocks forever
}

// Builds a Heartbeat Response or Association Setup Response's IE region for the given sequence
// number, dispatched by run_pfcp_lifecycle below.
std::vector<std::uint8_t> build_heartbeat_response_ies(std::time_t start_time) {
    std::vector<std::uint8_t> ies;
    pfcp_core::encode_ie(ies,
                         static_cast<std::uint16_t>(pfcp_core::IeType::RecoveryTimeStamp),
                         pfcp_core::encode_recovery_time_stamp(start_time));
    return ies;
}

// TS 29.244 Table 8.2.25-1, feature octet 5 bit 5 (FTUP): "F-TEID allocation / release in the UP
// function is supported by the UP function" -- true now that Stage 3 (ADR-0042) actually allocates
// F-TEIDs on CH request, so this must say so honestly rather than staying the all-zero
// "no optional features" bitmask Stage 1 originally sent.
std::vector<std::uint8_t> encode_up_function_features_ftup_only() {
    constexpr std::uint8_t kFtupBit = 0x10; // octet 5, bit 5
    return {kFtupBit, 0x00};
}

std::vector<std::uint8_t>
build_association_setup_response_ies(std::time_t start_time,
                                     std::array<std::uint8_t, 4> node_ipv4) {
    std::vector<std::uint8_t> ies;
    pfcp_core::encode_ie(ies,
                         static_cast<std::uint16_t>(pfcp_core::IeType::NodeId),
                         pfcp_core::encode_node_id_ipv4(node_ipv4));
    pfcp_core::encode_ie(ies,
                         static_cast<std::uint16_t>(pfcp_core::IeType::Cause),
                         pfcp_core::encode_cause(pfcp_core::Cause::RequestAccepted));
    pfcp_core::encode_ie(ies,
                         static_cast<std::uint16_t>(pfcp_core::IeType::RecoveryTimeStamp),
                         pfcp_core::encode_recovery_time_stamp(start_time));
    pfcp_core::encode_ie(ies,
                         static_cast<std::uint16_t>(pfcp_core::IeType::UpFunctionFeatures),
                         encode_up_function_features_ftup_only());
    return ies;
}

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #107, ADR-0084): real TS 29.244 §7.4.4.3/
// §7.4.4.4 Sx Association Update Request/Response -- real IE table confirmed directly against
// the vendored spec text (specs/PFCP/29244-e30.pdf), same source ADR-0039 already established
// for every other PFCP message in this file. Real, disclosed scope: the request's own optional
// "Sx Association Release Request"/"Graceful Release Period"/"User Plane IP Resource Information"
// IEs (used when the UP function itself is the one requesting changes via this message) are not
// decoded -- this build only handles the CP-initiated "update my own Node ID's advertised
// features" direction, the one real scenario this lab's single-CP-per-UP topology actually needs.
// Real, disclosed limitation: UP/CP Function Features are unconditionally accepted/echoed back
// (RequestAccepted) -- this lab has no real per-feature negotiation state machine, same
// disclosed simplification AssociationSetupResponse's own encode_up_function_features_ftup_only
// already carries.
std::vector<std::uint8_t>
build_association_update_response_ies(std::array<std::uint8_t, 4> node_ipv4) {
    std::vector<std::uint8_t> ies;
    pfcp_core::encode_ie(ies,
                         static_cast<std::uint16_t>(pfcp_core::IeType::NodeId),
                         pfcp_core::encode_node_id_ipv4(node_ipv4));
    pfcp_core::encode_ie(ies,
                         static_cast<std::uint16_t>(pfcp_core::IeType::Cause),
                         pfcp_core::encode_cause(pfcp_core::Cause::RequestAccepted));
    pfcp_core::encode_ie(ies,
                         static_cast<std::uint16_t>(pfcp_core::IeType::UpFunctionFeatures),
                         encode_up_function_features_ftup_only());
    return ies;
}

// Real TS 29.244 §7.4.4.5/§7.4.4.6 Sx Association Release Request/Response -- real IE table
// confirmed against the same vendored spec text. Real, disclosed scope: a real Association
// Release conceptually tears down ALL Sx sessions associated with the releasing peer (TS 29.244
// §7.4.4 general text) -- this build accepts the release and logs it, but does NOT bulk-delete
// this UPF's own session state for the requesting peer (this lab's own single-CP-per-UP scope
// means Session Set Deletion, TS 29.244 §7.4.6, is the real, precise, FQ-CSID-scoped mechanism
// spec'd for exactly this cleanup -- a separate, still-open gap, not fabricated here as a side
// effect of this message instead).
std::vector<std::uint8_t>
build_association_release_response_ies(std::array<std::uint8_t, 4> node_ipv4) {
    std::vector<std::uint8_t> ies;
    pfcp_core::encode_ie(ies,
                         static_cast<std::uint16_t>(pfcp_core::IeType::NodeId),
                         pfcp_core::encode_node_id_ipv4(node_ipv4));
    pfcp_core::encode_ie(ies,
                         static_cast<std::uint16_t>(pfcp_core::IeType::Cause),
                         pfcp_core::encode_cause(pfcp_core::Cause::RequestAccepted));
    return ies;
}

// Returns every top-level IE of `type` in `ies`, in wire order -- find_ie only returns the first
// match, which every message this project decoded before this one was content with (at most one
// real occurrence of any IE it cared about); PFD Management's own "Application ID's PFDs" and
// "PFD" IEs are real, spec-permitted repeated groups ("Several IEs with the same IE type may be
// present..."), so this project's first genuine need for a multi-match lookup.
std::vector<const pfcp_core::Ie*> find_all_ies(const std::vector<pfcp_core::Ie>& ies,
                                               std::uint16_t type) {
    std::vector<const pfcp_core::Ie*> matches;
    for (const auto& ie : ies) {
        if (ie.type == type) {
            matches.push_back(&ie);
        }
    }
    return matches;
}

// Real TS 29.244 §7.4.3.1/§7.4.3.2 Sx PFD Management Request/Response -- real IE tables confirmed
// directly against the vendored spec text. Real, replace-not-merge semantics per Table 7.4.3.1-1/
// 7.4.3.1-2's own condition text, applied literally: if the top-level "Application ID's PFDs" IE
// is absent from the whole message, delete every PFD stored for every Application ID; for each
// "Application ID's PFDs" group that IS present, if its own "PFD" child is absent, delete every
// PFD stored for just that Application ID; otherwise, the PFD Contents carried in this message
// become that Application ID's complete new set (not an incremental add). Real, disclosed scope
// narrowing matching this session's own AssociationUpdate/Release precedent: always responds
// Cause=RequestAccepted, no Offending IE support -- a malformed request is logged and the offending
// group is simply skipped rather than rejecting the whole message, since this project has no other
// real error case to distinguish it from (no admission-control/quota reason PFD provisioning would
// ever genuinely fail in this lab).
std::vector<std::uint8_t>
build_pfd_management_response_ies(const std::vector<std::uint8_t>& request_ies,
                                  PfdStore& pfd_store) {
    const auto ies = pfcp_core::decode_ies(request_ies);
    const auto app_pfds_ies =
        ies.has_value()
            ? find_all_ies(*ies, static_cast<std::uint16_t>(pfcp_core::IeType::ApplicationIdsPfds))
            : std::vector<const pfcp_core::Ie*>{};

    if (app_pfds_ies.empty()) {
        pfd_store.clear();
        spdlog::info("upf: PFD Management Request carried no Application ID's PFDs -- cleared "
                     "all provisioned PFDs");
    } else {
        for (const auto* app_pfds_ie : app_pfds_ies) {
            const auto group = pfcp_core::decode_ies(app_pfds_ie->value);
            if (!group.has_value()) {
                spdlog::warn("upf: PFD Management Request has a malformed Application ID's PFDs "
                             "group, skipping it");
                continue;
            }
            const auto* app_id_ie = pfcp_core::find_ie(
                *group, static_cast<std::uint16_t>(pfcp_core::IeType::ApplicationId));
            if (app_id_ie == nullptr) {
                spdlog::warn("upf: PFD Management Request has an Application ID's PFDs group "
                             "missing the mandatory Application ID, skipping it");
                continue;
            }
            const auto application_id = pfcp_core::decode_application_id(app_id_ie->value);

            const auto pfd_context_ies =
                find_all_ies(*group, static_cast<std::uint16_t>(pfcp_core::IeType::PfdContext));
            if (pfd_context_ies.empty()) {
                pfd_store.remove(application_id);
                spdlog::info("upf: PFD Management Request deleted all PFDs for Application ID {}",
                             application_id);
                continue;
            }

            std::vector<pfcp_core::PfdContents> pfds;
            for (const auto* pfd_context_ie : pfd_context_ies) {
                const auto pfd_context = pfcp_core::decode_ies(pfd_context_ie->value);
                if (!pfd_context.has_value()) {
                    continue;
                }
                for (const auto& pfd_ie : *pfd_context) {
                    if (pfd_ie.type != static_cast<std::uint16_t>(pfcp_core::IeType::PfdContents)) {
                        continue;
                    }
                    auto contents = pfcp_core::decode_pfd_contents(pfd_ie.value);
                    if (contents.has_value()) {
                        pfds.push_back(std::move(*contents));
                    }
                }
            }
            pfd_store.put(application_id, pfds);
            spdlog::info("upf: PFD Management Request provisioned {} PFD(s) for Application ID {}",
                         pfds.size(),
                         application_id);
        }
    }

    std::vector<std::uint8_t> resp_ies;
    pfcp_core::encode_ie(resp_ies,
                         static_cast<std::uint16_t>(pfcp_core::IeType::Cause),
                         pfcp_core::encode_cause(pfcp_core::Cause::RequestAccepted));
    return resp_ies;
}

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #107, ADR-0087): real TS 29.244 §7.4.5.1 Sx
// Node Report Request, reporting a User Plane Path Failure Report toward one remote GTP-U peer.
// This is UP-function-initiated (the spec's own §7.4.5.1.1: "sent... by the UP function"), so
// unlike every earlier PFCP builder in this file, it produces a REQUEST this UPF would send, not a
// response to one it received -- same real direction as this file's own SessionReportRequest
// (built inline in the datapath's usage_report_handler below) and Association Setup Request (SMF
// side, nfs/smf/src/main.cpp).
//
// Real, disclosed gap: this project's UPF has no live GTP-U path-failure DETECTION (the eBPF/XDP
// datapath, ADR-0043, decapsulates/forwards; it does not monitor peer reachability) -- so nothing
// in this codebase calls this function yet. It exists, byte-correct and unit-tested, so a real
// detector can call it once one exists, matching this project's own established precedent of
// building a real send-side function ahead of its live trigger (ReportSender's own
// SessionReportRequest machinery, ADR-0050 Stage 2, was built the same way before Stage 5 gave it
// a real caller).
// [[maybe_unused]]: real, disclosed above -- no live caller exists yet (no path-failure detector),
// same reasoning applied here as everywhere else in this codebase to a genuinely unused-for-now
// but real, tested, spec-correct function.
[[maybe_unused]] std::vector<std::uint8_t>
build_node_report_request_ies(std::array<std::uint8_t, 4> node_ipv4,
                              std::array<std::uint8_t, 4> failed_remote_peer_ipv4) {
    pfcp_core::NodeReportType report_type;
    report_type.user_plane_path_failure_report = true;

    std::vector<std::uint8_t> failure_report_ies;
    pfcp_core::encode_ie(failure_report_ies,
                         static_cast<std::uint16_t>(pfcp_core::IeType::RemoteGtpuPeer),
                         pfcp_core::encode_remote_gtpu_peer_ipv4(failed_remote_peer_ipv4));

    std::vector<std::uint8_t> ies;
    pfcp_core::encode_ie(ies,
                         static_cast<std::uint16_t>(pfcp_core::IeType::NodeId),
                         pfcp_core::encode_node_id_ipv4(node_ipv4));
    pfcp_core::encode_ie(ies,
                         static_cast<std::uint16_t>(pfcp_core::IeType::NodeReportType),
                         pfcp_core::encode_node_report_type(report_type));
    pfcp_core::encode_ie(ies,
                         static_cast<std::uint16_t>(pfcp_core::IeType::UserPlanePathFailureReport),
                         failure_report_ies);
    return ies;
}

struct SessionEstablishmentResult {
    std::vector<std::uint8_t> ies;
    // The header SEID for this response: TS 29.244's addressing rule ("the sending entity uses
    // the SEID value provided by the corresponding receiving entity") means UPF's response header
    // must carry the value the CP F-SEID IE in the request said to use -- not UPF's own new SEID.
    std::uint64_t response_header_seid = 0;
    // ADR-0071 (gap-closure Tier 1d): set whenever this session allocated a real uplink F-TEID,
    // regardless of whether a URR was also provisioned -- run_pfcp_lifecycle needs this
    // unconditionally to register SeidToTeidStore, since ANY session with an allocated F-TEID must
    // be reachable by a later real Session Modification/Deletion (Update/Remove QER, Update/Remove
    // BAR, Session Deletion), not just ones with a URR. Real bug fixed this turn: earlier code
    // (ADR-0050 Stage 5) only ever set/used allocated_teid_with_urr, so a QER-only session (no
    // charging URR -- an entirely ordinary real case) could establish successfully but then could
    // never be found by SEID for any later Modification/Deletion, a genuine conformance gap this
    // field closes. allocated_teid_with_urr is kept as a SEPARATE field (set only when a URR was
    // ALSO provisioned) since TeidSessionStore's own real purpose -- addressing an unsolicited
    // Session Report Request back to SMF -- genuinely only applies to URR-bearing sessions.
    std::optional<std::uint32_t> allocated_teid;
    std::optional<std::uint32_t> allocated_teid_with_urr;
    std::optional<std::uint32_t> urr_id;
    // ADR-0050 Stage 5: this UPF's own newly-generated F-SEID for the session -- exposed so
    // run_pfcp_lifecycle can register it in SeidToTeidStore (a later real Session Modification
    // Request addresses this session using exactly this value, per the same addressing rule this
    // struct's own response_header_seid comment already documents).
    std::uint64_t up_seid = 0;
};

// Session Establishment (TS 29.244 §7.5.2/§7.5.3, ADR-0042). Decodes the CP F-SEID and the first
// Create PDR/Create FAR pair, allocates a real local F-TEID (if the PDI's F-TEID requested CH)
// and a real UP F-SEID, and builds the response. `next_seid`/`next_teid` are simple counters --
// safe as plain (non-atomic) locals since this function only ever runs on run_pfcp_lifecycle's
// single thread.
std::optional<SessionEstablishmentResult>
build_session_establishment_response_ies(const std::vector<std::uint8_t>& request_ies,
                                         std::array<std::uint8_t, 4> node_ipv4,
                                         std::uint64_t& next_seid,
                                         std::uint32_t& next_teid,
                                         upf::Datapath* datapath) {
    const auto ies = pfcp_core::decode_ies(request_ies);
    if (!ies.has_value()) {
        return std::nullopt;
    }
    const auto* cp_f_seid_ie =
        pfcp_core::find_ie(*ies, static_cast<std::uint16_t>(pfcp_core::IeType::FSeid));
    const auto* create_pdr_ie =
        pfcp_core::find_ie(*ies, static_cast<std::uint16_t>(pfcp_core::IeType::CreatePdr));
    if (cp_f_seid_ie == nullptr || create_pdr_ie == nullptr) {
        spdlog::warn(
            "upf: Session Establishment Request missing mandatory CP F-SEID or Create PDR");
        return std::nullopt;
    }
    const auto cp_f_seid = pfcp_core::decode_f_seid_ipv4(cp_f_seid_ie->value);
    if (!cp_f_seid.has_value()) {
        spdlog::warn("upf: Session Establishment Request has a malformed CP F-SEID");
        return std::nullopt;
    }

    const auto pdr_ies = pfcp_core::decode_ies(create_pdr_ie->value);
    const auto* pdr_id_ie =
        pdr_ies.has_value()
            ? pfcp_core::find_ie(*pdr_ies, static_cast<std::uint16_t>(pfcp_core::IeType::PdrId))
            : nullptr;
    const auto* pdi_ie =
        pdr_ies.has_value()
            ? pfcp_core::find_ie(*pdr_ies, static_cast<std::uint16_t>(pfcp_core::IeType::Pdi))
            : nullptr;
    const auto pdr_id =
        pdr_id_ie != nullptr ? pfcp_core::decode_pdr_id(pdr_id_ie->value) : std::nullopt;

    // ADR-0050 Stage 2: the real Create URR IE (TS 29.244 §7.5.2.4), if SMF's Stage 1 provisioned
    // one from CHF's actual grant (ADR-0048's Nchf_ConvergedCharging_Create) -- top-level sibling
    // of Create PDR/Create FAR, not nested inside either. Only the fields Annex C.2.1.1's volume-
    // based flow uses are read; URR ID is read from the wire rather than assumed to always be 1,
    // since nothing about UPF's own logic depends on SMF's specific numbering choice.
    const auto* create_urr_ie =
        pfcp_core::find_ie(*ies, static_cast<std::uint16_t>(pfcp_core::IeType::CreateUrr));
    std::optional<std::uint32_t> urr_id;
    std::optional<std::uint64_t> urr_volume_threshold;
    std::optional<std::uint64_t> urr_volume_quota;
    if (create_urr_ie != nullptr) {
        const auto urr_ies = pfcp_core::decode_ies(create_urr_ie->value);
        if (urr_ies.has_value()) {
            const auto* urr_id_ie =
                pfcp_core::find_ie(*urr_ies, static_cast<std::uint16_t>(pfcp_core::IeType::UrrId));
            const auto* threshold_ie = pfcp_core::find_ie(
                *urr_ies, static_cast<std::uint16_t>(pfcp_core::IeType::VolumeThreshold));
            const auto* quota_ie = pfcp_core::find_ie(
                *urr_ies, static_cast<std::uint16_t>(pfcp_core::IeType::VolumeQuota));
            if (urr_id_ie != nullptr) {
                urr_id = pfcp_core::decode_urr_id(urr_id_ie->value);
            }
            if (threshold_ie != nullptr) {
                urr_volume_threshold = pfcp_core::decode_volume_total(threshold_ie->value);
            }
            if (quota_ie != nullptr) {
                urr_volume_quota = pfcp_core::decode_volume_total(quota_ie->value);
            }
        }
        if (!urr_id.has_value() || !urr_volume_threshold.has_value() ||
            !urr_volume_quota.has_value()) {
            spdlog::warn("upf: Session Establishment Request carried a malformed Create URR, "
                         "ignoring usage tracking for this session");
        }
    }

    // ADR-0071, gap-closure Tier 1d: the real Create QER IE (TS 29.244 Table 7.5.2.5-1), top-level
    // sibling of Create PDR/Create FAR/Create URR, same as the Create URR handling above. Real,
    // disclosed simplification matching Create URR's own established scope: only the first Create
    // QER is applied (this build's per-TEID datapath map holds one QER slot, same "one per
    // session" narrowing already applied to URR). Gate Status is real Mandatory in Create QER; a
    // request missing it is treated as malformed and this session gets no QoS enforcement, logged
    // rather than silently accepted. Maximum Bitrate is real Conditional -- absent means no real
    // MBR enforcement (register_qer's own header comment already documents mbr_ul_kbps=0 as that
    // exact case).
    const auto* create_qer_ie =
        pfcp_core::find_ie(*ies, static_cast<std::uint16_t>(pfcp_core::IeType::CreateQer));
    std::optional<pfcp_core::GateStatus> qer_gate_status;
    std::uint32_t qer_mbr_ul_kbps = 0;
    if (create_qer_ie != nullptr) {
        const auto qer_ies = pfcp_core::decode_ies(create_qer_ie->value);
        if (qer_ies.has_value()) {
            const auto* gate_status_ie = pfcp_core::find_ie(
                *qer_ies, static_cast<std::uint16_t>(pfcp_core::IeType::GateStatus));
            const auto* mbr_ie =
                pfcp_core::find_ie(*qer_ies, static_cast<std::uint16_t>(pfcp_core::IeType::Mbr));
            if (gate_status_ie != nullptr) {
                qer_gate_status = pfcp_core::decode_gate_status(gate_status_ie->value);
            }
            if (mbr_ie != nullptr) {
                if (const auto mbr = pfcp_core::decode_mbr(mbr_ie->value); mbr.has_value()) {
                    qer_mbr_ul_kbps = static_cast<std::uint32_t>(
                        std::min<std::uint64_t>(mbr->ul_kbps, 0xFFFFFFFFULL));
                }
            }
        }
        if (!qer_gate_status.has_value()) {
            spdlog::warn("upf: Session Establishment Request carried a malformed/incomplete "
                         "Create QER (missing Mandatory Gate Status), ignoring QoS enforcement "
                         "for this session");
        }
    }

    // ADR-0071: real Create BAR IE (TS 29.244 Table 7.5.2.6-1), parsed and acknowledged only --
    // this project has no downlink datapath (see this file's own header comment: no downlink
    // PDR/FAR exists, since that needs NGAP PDU Session Resource Setup, still not implemented), so
    // a BAR's real purpose (buffering downlink data while paging/notifying the UE) cannot actually
    // be enforced here. Disclosed, deliberate scope: PFCP-level parse/log only, no BAR state is
    // stored or applied to any datapath.
    const auto* create_bar_ie =
        pfcp_core::find_ie(*ies, static_cast<std::uint16_t>(pfcp_core::IeType::CreateBar));
    if (create_bar_ie != nullptr) {
        const auto bar_ies = pfcp_core::decode_ies(create_bar_ie->value);
        const auto* bar_id_ie =
            bar_ies.has_value()
                ? pfcp_core::find_ie(*bar_ies, static_cast<std::uint16_t>(pfcp_core::IeType::BarId))
                : nullptr;
        const auto bar_id =
            bar_id_ie != nullptr ? pfcp_core::decode_bar_id(bar_id_ie->value) : std::nullopt;
        if (bar_id.has_value()) {
            spdlog::info("upf: Create BAR {} acknowledged (PFCP-level only -- no downlink "
                         "datapath exists to enforce buffering, see ADR-0071)",
                         *bar_id);
        } else {
            spdlog::warn("upf: Session Establishment Request carried a malformed Create BAR "
                         "(missing Mandatory BAR ID)");
        }
    }

    SessionEstablishmentResult result;

    std::vector<std::uint8_t> ies_out;
    pfcp_core::encode_ie(ies_out,
                         static_cast<std::uint16_t>(pfcp_core::IeType::NodeId),
                         pfcp_core::encode_node_id_ipv4(node_ipv4));
    pfcp_core::encode_ie(ies_out,
                         static_cast<std::uint16_t>(pfcp_core::IeType::Cause),
                         pfcp_core::encode_cause(pfcp_core::Cause::RequestAccepted));

    pfcp_core::FSeid up_f_seid;
    up_f_seid.seid = next_seid++;
    up_f_seid.ipv4 = node_ipv4;
    result.up_seid = up_f_seid.seid;
    pfcp_core::encode_ie(ies_out,
                         static_cast<std::uint16_t>(pfcp_core::IeType::FSeid),
                         pfcp_core::encode_f_seid_ipv4(up_f_seid));

    if (pdr_id.has_value() && pdi_ie != nullptr) {
        const auto pdi_ies = pfcp_core::decode_ies(pdi_ie->value);
        const auto* f_teid_ie =
            pdi_ies.has_value()
                ? pfcp_core::find_ie(*pdi_ies, static_cast<std::uint16_t>(pfcp_core::IeType::FTeid))
                : nullptr;
        if (f_teid_ie != nullptr && pfcp_core::decode_f_teid_is_choose_request(f_teid_ie->value)) {
            const std::uint32_t allocated_teid = next_teid++;
            std::vector<std::uint8_t> created_pdr;
            pfcp_core::encode_ie(created_pdr,
                                 static_cast<std::uint16_t>(pfcp_core::IeType::PdrId),
                                 pfcp_core::encode_pdr_id(*pdr_id));
            pfcp_core::encode_ie(
                created_pdr,
                static_cast<std::uint16_t>(pfcp_core::IeType::FTeid),
                pfcp_core::encode_f_teid_allocated_ipv4(allocated_teid, node_ipv4));
            pfcp_core::encode_ie(
                ies_out, static_cast<std::uint16_t>(pfcp_core::IeType::CreatedPdr), created_pdr);
            spdlog::info("upf: allocated F-TEID {:#x} for PDR ID {}", allocated_teid, *pdr_id);
            // ADR-0043: registers the TEID with the real XDP program so it actually recognizes
            // and decapsulates uplink traffic for this PDR -- a no-op (logged, not fatal) if no
            // datapath was created (e.g. missing privileges, see datapath.hpp's own comment).
            if (datapath != nullptr) {
                datapath->register_teid(allocated_teid);
                if (urr_id.has_value() && urr_volume_threshold.has_value() &&
                    urr_volume_quota.has_value()) {
                    datapath->register_urr(
                        allocated_teid, *urr_volume_threshold, *urr_volume_quota);
                    spdlog::info(
                        "upf: registered URR {} for TEID {:#x}: threshold={} quota={} octets",
                        *urr_id,
                        allocated_teid,
                        *urr_volume_threshold,
                        *urr_volume_quota);
                }
                if (qer_gate_status.has_value()) {
                    datapath->register_qer(allocated_teid,
                                           qer_gate_status->ul_closed,
                                           qer_gate_status->dl_closed,
                                           qer_mbr_ul_kbps);
                    spdlog::info("upf: registered QER for TEID {:#x}: ul_gate={} dl_gate={} "
                                 "mbr_ul_kbps={}",
                                 allocated_teid,
                                 qer_gate_status->ul_closed ? "CLOSED" : "OPEN",
                                 qer_gate_status->dl_closed ? "CLOSED" : "OPEN",
                                 qer_mbr_ul_kbps);
                }
            }
            // ADR-0071: unconditional -- see this field's own header comment on the real bug this
            // fixes (SEID resolution must work for every session with an allocated F-TEID).
            result.allocated_teid = allocated_teid;
            if (urr_id.has_value() && urr_volume_threshold.has_value() &&
                urr_volume_quota.has_value()) {
                result.allocated_teid_with_urr = allocated_teid;
                result.urr_id = urr_id;
            }
        }
    }

    result.ies = std::move(ies_out);
    result.response_header_seid = cp_f_seid->seid;
    return result;
}

struct SessionModificationResult {
    std::vector<std::uint8_t> ies;
    std::uint64_t response_header_seid = 0;
};

// Real Sx Session Modification (TS 29.244 §7.5.4/§7.5.5, ADR-0050 Stage 5) -- this build's only
// supported modification is a real Update URR (grouped IE type=13, TS 29.244 Table 7.5.4.4-1)
// pushing a re-authorized Volume Threshold/Volume Quota for an already-created URR. `request_seid`
// is the incoming header's own SEID -- this UPF's own F-SEID for the session (the value the CP
// addressed the request TO, per the addressing rule SessionEstablishmentResult's own comment
// documents), not what the response should echo back.
std::optional<SessionModificationResult>
build_session_modification_response_ies(const std::vector<std::uint8_t>& request_ies,
                                        std::uint64_t request_seid,
                                        SeidToTeidStore& seid_to_teid_store,
                                        TeidSessionStore& teid_session_store,
                                        upf::Datapath* datapath) {
    SessionModificationResult result;
    // Real spec addressing rule: the response echoes the CP's own SEID for this session -- only
    // known via the session's already-stored UrrSessionInfo below. Falls back to request_seid
    // (technically the wrong direction, same disclosed simplification ADR-0050 Stage 0 already
    // carries for Session Report Response) only if that lookup fails.
    result.response_header_seid = request_seid;

    const auto teid = seid_to_teid_store.get(request_seid);
    if (!teid.has_value()) {
        spdlog::warn("upf: Session Modification Request references unknown SEID {:#x}",
                     request_seid);
        pfcp_core::encode_ie(result.ies,
                             static_cast<std::uint16_t>(pfcp_core::IeType::Cause),
                             pfcp_core::encode_cause(pfcp_core::Cause::RequestRejected));
        return result;
    }
    if (const auto session_info = teid_session_store.get(*teid); session_info.has_value()) {
        result.response_header_seid = session_info->cp_seid;
    }

    const auto ies = pfcp_core::decode_ies(request_ies);

    // ADR-0071, gap-closure Tier 1d: real Session Modification can carry any combination of
    // Update URR / Update QER / Remove QER / Update BAR / Remove BAR as top-level sibling IEs (TS
    // 29.244 Table 7.5.4.1-1 -- all Conditional/Optional, none Mandatory). This project's earlier
    // Stage 5 build only ever handled Update URR and returned as soon as that one IE was checked;
    // extended here (rather than kept as separate early-returns) so a single real request touching
    // more than one of these actually gets all of them applied, not just the first. `failed` tracks
    // whether ANY requested modification could not be applied -- the whole response is Rejected if
    // so (this build has no per-IE Failed Rule ID reporting, a real, disclosed simplification: TS
    // 29.244 does support partial success via Failed Rule ID, not implemented here).
    bool failed = false;

    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #101, ADR-0092): real Create PDR/Create
    // FAR within a Session Modification Request (TS 29.244 Table 7.5.4.1-1 -- both real,
    // Conditional, legal here just as in Session Establishment, not just at session creation).
    // SMF's own PATH_SWITCH_REQ handling uses this to provision this project's first-ever real
    // downlink PDR/FAR (with a real Outer Header Creation pointing GTP-U at the new gNB). Real,
    // disclosed scope, same class as Update BAR/Remove BAR's own established "PFCP-level only"
    // disclosure a few lines below: this project's real eBPF/XDP datapath (see
    // datapath->register_teid's own real, disclosed uplink-decapsulation-only scope, Phase 3) has
    // no real GTP-U encapsulation path at all -- a downlink FAR's own Outer Header Creation is
    // decoded and logged for real (not silently ignored, unlike before this ADR), acknowledged at
    // the real PFCP control-plane level, but does NOT yet cause real outgoing packets to actually
    // be encapsulated toward the gNB. Before this ADR, a Create PDR/Create FAR arriving here was
    // silently ignored while the response still unconditionally claimed Cause=RequestAccepted -- a
    // real, now-fixed false-positive-success bug, not merely an added feature.
    const auto* mod_create_pdr_ie =
        ies.has_value()
            ? pfcp_core::find_ie(*ies, static_cast<std::uint16_t>(pfcp_core::IeType::CreatePdr))
            : nullptr;
    const auto* mod_create_far_ie =
        ies.has_value()
            ? pfcp_core::find_ie(*ies, static_cast<std::uint16_t>(pfcp_core::IeType::CreateFar))
            : nullptr;
    if (mod_create_pdr_ie != nullptr || mod_create_far_ie != nullptr) {
        std::optional<std::uint32_t> new_pdr_id;
        std::optional<std::uint32_t> new_far_id_from_pdr;
        if (mod_create_pdr_ie != nullptr) {
            const auto new_pdr_ies = pfcp_core::decode_ies(mod_create_pdr_ie->value);
            const auto* pdr_id_ie =
                new_pdr_ies.has_value()
                    ? pfcp_core::find_ie(*new_pdr_ies,
                                         static_cast<std::uint16_t>(pfcp_core::IeType::PdrId))
                    : nullptr;
            const auto* far_id_ie =
                new_pdr_ies.has_value()
                    ? pfcp_core::find_ie(*new_pdr_ies,
                                         static_cast<std::uint16_t>(pfcp_core::IeType::FarId))
                    : nullptr;
            new_pdr_id =
                pdr_id_ie != nullptr ? pfcp_core::decode_pdr_id(pdr_id_ie->value) : std::nullopt;
            new_far_id_from_pdr =
                far_id_ie != nullptr ? pfcp_core::decode_far_id(far_id_ie->value) : std::nullopt;
        }
        std::optional<pfcp_core::OuterHeaderCreationGtpuIpv4> new_ohc;
        std::optional<std::uint32_t> new_far_id;
        if (mod_create_far_ie != nullptr) {
            const auto new_far_ies = pfcp_core::decode_ies(mod_create_far_ie->value);
            const auto* far_id_ie =
                new_far_ies.has_value()
                    ? pfcp_core::find_ie(*new_far_ies,
                                         static_cast<std::uint16_t>(pfcp_core::IeType::FarId))
                    : nullptr;
            new_far_id =
                far_id_ie != nullptr ? pfcp_core::decode_far_id(far_id_ie->value) : std::nullopt;
            const auto* fwd_params_ie =
                new_far_ies.has_value()
                    ? pfcp_core::find_ie(
                          *new_far_ies,
                          static_cast<std::uint16_t>(pfcp_core::IeType::ForwardingParameters))
                    : nullptr;
            if (fwd_params_ie != nullptr) {
                const auto fwd_params_ies = pfcp_core::decode_ies(fwd_params_ie->value);
                const auto* ohc_ie =
                    fwd_params_ies.has_value()
                        ? pfcp_core::find_ie(
                              *fwd_params_ies,
                              static_cast<std::uint16_t>(pfcp_core::IeType::OuterHeaderCreation))
                        : nullptr;
                if (ohc_ie != nullptr) {
                    new_ohc = pfcp_core::decode_outer_header_creation_gtpu_ipv4(ohc_ie->value);
                }
            }
        }
        if (!new_pdr_id.has_value() || !new_far_id_from_pdr.has_value() ||
            !new_far_id.has_value() || !new_ohc.has_value()) {
            spdlog::warn("upf: Session Modification Request's Create PDR/Create FAR for TEID "
                         "{:#x} is malformed or missing Outer Header Creation, ignoring",
                         *teid);
            failed = true;
        } else {
            spdlog::info(
                "upf: real Create PDR {} / Create FAR {} for TEID {:#x} -- downlink Outer Header "
                "Creation decoded (GTP-U/UDP/IPv4, peer TEID={:#x}, peer IPv4={}.{}.{}.{}) and "
                "acknowledged at the PFCP control-plane level; NOT yet wired into the real "
                "eBPF/XDP datapath (no downlink GTP-U encapsulation path exists there, a real, "
                "disclosed gap -- see this branch's own comment)",
                *new_pdr_id,
                *new_far_id,
                *teid,
                new_ohc->teid,
                new_ohc->ipv4[0],
                new_ohc->ipv4[1],
                new_ohc->ipv4[2],
                new_ohc->ipv4[3]);
        }
    }

    const auto* update_urr_ie =
        ies.has_value()
            ? pfcp_core::find_ie(*ies, static_cast<std::uint16_t>(pfcp_core::IeType::UpdateUrr))
            : nullptr;
    if (update_urr_ie != nullptr) {
        const auto update_urr_ies = pfcp_core::decode_ies(update_urr_ie->value);
        const auto* threshold_ie =
            update_urr_ies.has_value()
                ? pfcp_core::find_ie(*update_urr_ies,
                                     static_cast<std::uint16_t>(pfcp_core::IeType::VolumeThreshold))
                : nullptr;
        const auto* quota_ie =
            update_urr_ies.has_value()
                ? pfcp_core::find_ie(*update_urr_ies,
                                     static_cast<std::uint16_t>(pfcp_core::IeType::VolumeQuota))
                : nullptr;
        const auto new_threshold = threshold_ie != nullptr
                                       ? pfcp_core::decode_volume_total(threshold_ie->value)
                                       : std::nullopt;
        const auto new_quota =
            quota_ie != nullptr ? pfcp_core::decode_volume_total(quota_ie->value) : std::nullopt;
        if (!new_threshold.has_value() || !new_quota.has_value() || datapath == nullptr ||
            !datapath->update_urr_thresholds(*teid, *new_threshold, *new_quota)) {
            spdlog::warn("upf: failed to apply Update URR for TEID {:#x}", *teid);
            failed = true;
        } else {
            spdlog::info("upf: applied Update URR for TEID {:#x}: threshold={} quota={} octets",
                         *teid,
                         *new_threshold,
                         *new_quota);
        }
    }

    // ADR-0071: real Update QER (TS 29.244 Table 7.5.4.5-1) -- QER ID is Mandatory but, matching
    // this build's own "one QER per TEID" scope already established at Session Establishment, the
    // actual value on the wire is not cross-checked against a stored QER ID; only its presence is
    // required, same disclosed narrowing as URR ID's own handling throughout this file. Gate
    // Status/Maximum Bitrate are real Conditional -- passed through as std::optional so
    // Datapath::update_qer can do a real read-modify-write (see its own header comment for why a
    // naive full-overwrite would be a correctness bug here).
    const auto* update_qer_ie =
        ies.has_value()
            ? pfcp_core::find_ie(*ies, static_cast<std::uint16_t>(pfcp_core::IeType::UpdateQer))
            : nullptr;
    if (update_qer_ie != nullptr) {
        const auto update_qer_ies = pfcp_core::decode_ies(update_qer_ie->value);
        const auto* qer_id_ie =
            update_qer_ies.has_value()
                ? pfcp_core::find_ie(*update_qer_ies,
                                     static_cast<std::uint16_t>(pfcp_core::IeType::QerId))
                : nullptr;
        const auto* gate_status_ie =
            update_qer_ies.has_value()
                ? pfcp_core::find_ie(*update_qer_ies,
                                     static_cast<std::uint16_t>(pfcp_core::IeType::GateStatus))
                : nullptr;
        const auto* mbr_ie =
            update_qer_ies.has_value()
                ? pfcp_core::find_ie(*update_qer_ies,
                                     static_cast<std::uint16_t>(pfcp_core::IeType::Mbr))
                : nullptr;
        std::optional<bool> new_ul_gate_closed;
        std::optional<bool> new_dl_gate_closed;
        if (gate_status_ie != nullptr) {
            if (const auto gate = pfcp_core::decode_gate_status(gate_status_ie->value);
                gate.has_value()) {
                new_ul_gate_closed = gate->ul_closed;
                new_dl_gate_closed = gate->dl_closed;
            }
        }
        std::optional<std::uint32_t> new_mbr_ul_kbps;
        if (mbr_ie != nullptr) {
            if (const auto mbr = pfcp_core::decode_mbr(mbr_ie->value); mbr.has_value()) {
                new_mbr_ul_kbps = static_cast<std::uint32_t>(
                    std::min<std::uint64_t>(mbr->ul_kbps, 0xFFFFFFFFULL));
            }
        }
        if (qer_id_ie == nullptr) {
            spdlog::warn("upf: Update QER for TEID {:#x} has no QER ID -- rejected", *teid);
            failed = true;
        } else if (datapath == nullptr) {
            // ADR-0328: no eBPF/XDP datapath on this host (the startup warning above says so).
            // Establishment already treats that as a degrade to control-plane-only operation --
            // it installs the session and skips register_qer without failing -- so rejecting a
            // MODIFICATION for the same environmental reason was incoherent: UPF would accept a
            // session it then refused to modify. The request is well-formed and the control plane
            // accepts it; what is missing is a datapath to apply it to, which is not a property of
            // the request.
            //
            // Logged at warning, not info, because the CP peer will read this acceptance as "the
            // rate changed" and on this host it did not.
            spdlog::warn("upf: accepted Update QER for TEID {:#x} but NO datapath exists to "
                         "enforce it -- control plane only, traffic is NOT rate-limited",
                         *teid);
        } else if (!datapath->update_qer(
                       *teid, new_ul_gate_closed, new_dl_gate_closed, new_mbr_ul_kbps)) {
            spdlog::warn("upf: failed to apply Update QER for TEID {:#x} -- no QER registered for "
                         "it, or the BPF map update failed",
                         *teid);
            failed = true;
        } else {
            spdlog::info("upf: applied Update QER for TEID {:#x}", *teid);
        }
    }

    // ADR-0328: real Create QER in a Session MODIFICATION (TS 29.244 Table 7.5.4.2-1 lists Create
    // QER there, not only in Establishment).
    //
    // This was missing, and the way it was missing is the point: an unhandled Create QER fell
    // through every branch above without setting `failed`, so UPF answered RequestAccepted and the
    // CP saw a successful modification while nothing had been registered on the datapath. SMF's
    // ADR-0328 enforcement path needs exactly this message -- it sends Create QER when a session
    // was established without one (PCF's original decision carried no parsable AMBR) -- so without
    // this branch SMF would have counted a subscriber as throttled who was still running at full
    // rate. An accepted request that does nothing is worse than a rejected one.
    const auto* create_qer_ie =
        ies.has_value()
            ? pfcp_core::find_ie(*ies, static_cast<std::uint16_t>(pfcp_core::IeType::CreateQer))
            : nullptr;
    if (create_qer_ie != nullptr) {
        const auto create_qer_ies = pfcp_core::decode_ies(create_qer_ie->value);
        const auto* qer_id_ie =
            create_qer_ies.has_value()
                ? pfcp_core::find_ie(*create_qer_ies,
                                     static_cast<std::uint16_t>(pfcp_core::IeType::QerId))
                : nullptr;
        const auto* gate_status_ie =
            create_qer_ies.has_value()
                ? pfcp_core::find_ie(*create_qer_ies,
                                     static_cast<std::uint16_t>(pfcp_core::IeType::GateStatus))
                : nullptr;
        const auto* mbr_ie =
            create_qer_ies.has_value()
                ? pfcp_core::find_ie(*create_qer_ies,
                                     static_cast<std::uint16_t>(pfcp_core::IeType::Mbr))
                : nullptr;
        // Gate Status is Mandatory in a Create QER, same as the Establishment path requires.
        const auto gate = gate_status_ie != nullptr
                              ? pfcp_core::decode_gate_status(gate_status_ie->value)
                              : std::nullopt;
        std::uint32_t new_mbr_ul_kbps = 0;
        if (mbr_ie != nullptr) {
            if (const auto mbr = pfcp_core::decode_mbr(mbr_ie->value); mbr.has_value()) {
                new_mbr_ul_kbps = static_cast<std::uint32_t>(
                    std::min<std::uint64_t>(mbr->ul_kbps, 0xFFFFFFFFULL));
            }
        }
        if (qer_id_ie == nullptr || !gate.has_value()) {
            spdlog::warn("upf: Create QER for TEID {:#x} is missing its Mandatory QER ID or Gate "
                         "Status -- rejected",
                         *teid);
            failed = true;
        } else if (datapath == nullptr) {
            // Same reasoning as Update QER above: well-formed request, no datapath to apply it to.
            spdlog::warn("upf: accepted Create QER for TEID {:#x} but NO datapath exists to "
                         "enforce it -- control plane only, traffic is NOT rate-limited",
                         *teid);
        } else {
            datapath->register_qer(*teid, gate->ul_closed, gate->dl_closed, new_mbr_ul_kbps);
            spdlog::info("upf: applied Create QER for TEID {:#x}: ul_gate={} dl_gate={} "
                         "mbr_ul_kbps={}",
                         *teid,
                         gate->ul_closed ? "CLOSED" : "OPEN",
                         gate->dl_closed ? "CLOSED" : "OPEN",
                         new_mbr_ul_kbps);
        }
    }

    // ADR-0071: real Remove QER (TS 29.244 Table 7.5.4.9-1). Idempotent by design: if no QER is
    // currently registered (already removed, or never created), the real intent -- "no QER active
    // for this TEID" -- is already true, so this does NOT set `failed`, only logs.
    const auto* remove_qer_ie =
        ies.has_value()
            ? pfcp_core::find_ie(*ies, static_cast<std::uint16_t>(pfcp_core::IeType::RemoveQer))
            : nullptr;
    if (remove_qer_ie != nullptr) {
        if (datapath != nullptr && datapath->remove_qer(*teid)) {
            spdlog::info("upf: removed QER for TEID {:#x}", *teid);
        } else {
            spdlog::warn("upf: Remove QER for TEID {:#x} found no QER to remove", *teid);
        }
    }

    // ADR-0071: real Update BAR / Remove BAR (TS 29.244 Table 7.5.4.11-1/7.5.4.12-1) -- same
    // disclosed "parse/log only, no downlink datapath to apply it to" scope as Create BAR's own
    // handling in build_session_establishment_response_ies. Always a no-op success: there is no
    // stored BAR state this build could fail to find.
    const auto* update_bar_ie =
        ies.has_value()
            ? pfcp_core::find_ie(*ies, static_cast<std::uint16_t>(pfcp_core::IeType::UpdateBar))
            : nullptr;
    if (update_bar_ie != nullptr) {
        spdlog::info("upf: Update BAR for TEID {:#x} acknowledged (PFCP-level only, see ADR-0071)",
                     *teid);
    }
    const auto* remove_bar_ie =
        ies.has_value()
            ? pfcp_core::find_ie(*ies, static_cast<std::uint16_t>(pfcp_core::IeType::RemoveBar))
            : nullptr;
    if (remove_bar_ie != nullptr) {
        spdlog::info("upf: Remove BAR for TEID {:#x} acknowledged (PFCP-level only, see ADR-0071)",
                     *teid);
    }

    pfcp_core::encode_ie(result.ies,
                         static_cast<std::uint16_t>(pfcp_core::IeType::Cause),
                         pfcp_core::encode_cause(failed ? pfcp_core::Cause::RequestRejected
                                                        : pfcp_core::Cause::RequestAccepted));
    return result;
}

struct SessionDeletionResult {
    std::vector<std::uint8_t> ies;
    std::uint64_t response_header_seid = 0;
};

// ADR-0071, gap-closure Tier 1d: real Sx Session Deletion (TS 29.244 §7.5.6/§7.5.7) -- the one
// PFCP message type this build had no handler for at all before this turn (previously fell into
// run_pfcp_lifecycle's catch-all "no handler yet, ignoring" branch, never responding).
// `request_seid` is the incoming header's own SEID, same UPF-addressed-by-CP meaning as
// build_session_modification_response_ies's own parameter of the same name.
std::optional<SessionDeletionResult>
build_session_deletion_response_ies(std::uint64_t request_seid,
                                    SeidToTeidStore& seid_to_teid_store,
                                    TeidSessionStore& teid_session_store,
                                    upf::Datapath* datapath) {
    SessionDeletionResult result;
    result.response_header_seid = request_seid;

    const auto teid = seid_to_teid_store.get(request_seid);
    if (!teid.has_value()) {
        // Real spec Table 8.2.1-1 (Cause): "Session context not found... if the F-SEID included in
        // a Sx Session Modification/Deletion Request message is unknown" -- the exact real case
        // here, not the generic RequestRejected earlier PFCP work in this file used before this
        // Cause value existed.
        spdlog::warn("upf: Session Deletion Request references unknown SEID {:#x}", request_seid);
        pfcp_core::encode_ie(result.ies,
                             static_cast<std::uint16_t>(pfcp_core::IeType::Cause),
                             pfcp_core::encode_cause(pfcp_core::Cause::SessionContextNotFound));
        return result;
    }

    // Real spec addressing rule (same as Session Modification's own use of this pattern): the
    // response echoes the CP's own SEID for this session, known from the already-stored
    // UrrSessionInfo if this session ever provisioned a URR.
    const auto session_info = teid_session_store.get(*teid);
    if (session_info.has_value()) {
        result.response_header_seid = session_info->cp_seid;
    }

    // ADR-0071: real cumulative usage, if any URR was provisioned for this session -- this also
    // tears down ALL per-TEID datapath state (teid_map/urr_map/qer_map), the real, full session
    // teardown TS 29.244 §7.5.6 requires at Sx session termination. A missing/absent datapath
    // (e.g. no eBPF privileges, see datapath.hpp's own comment) just means no real total to
    // report -- PFCP-level deletion still succeeds, same "control-plane works with or without a
    // datapath" pattern this file already follows elsewhere.
    const std::optional<std::uint64_t> total_octets =
        datapath != nullptr ? datapath->remove_teid(*teid) : std::nullopt;

    if (total_octets.has_value() && session_info.has_value()) {
        const auto report_info = teid_session_store.get_and_advance_seqn(*teid);
        // TS 29.244 Table 7.5.7.2-1's own narrower field set (URR ID, UR-SEQN, Usage Report
        // Trigger, Volume Measurement) -- see ie.hpp's own UsageReportSessionDeletion comment for
        // why this is real IE type 79, not the 80 used in the unsolicited Session Report path
        // above.
        std::vector<std::uint8_t> usage_report;
        pfcp_core::encode_ie(usage_report,
                             static_cast<std::uint16_t>(pfcp_core::IeType::UrrId),
                             pfcp_core::encode_urr_id(session_info->urr_id));
        pfcp_core::encode_ie(
            usage_report,
            static_cast<std::uint16_t>(pfcp_core::IeType::UrSeqn),
            pfcp_core::encode_ur_seqn(report_info.has_value() ? report_info->second : 0));
        pfcp_core::encode_ie(usage_report,
                             static_cast<std::uint16_t>(pfcp_core::IeType::UsageReportTrigger),
                             pfcp_core::encode_usage_report_trigger_termr());
        pfcp_core::encode_ie(usage_report,
                             static_cast<std::uint16_t>(pfcp_core::IeType::VolumeMeasurement),
                             pfcp_core::encode_volume_total(*total_octets));
        pfcp_core::encode_ie(
            result.ies,
            static_cast<std::uint16_t>(pfcp_core::IeType::UsageReportSessionDeletion),
            usage_report);
        spdlog::info("upf: Session Deletion for TEID {:#x} reporting final usage: {} octets",
                     *teid,
                     *total_octets);
    }

    teid_session_store.remove(*teid);
    seid_to_teid_store.remove(request_seid);

    pfcp_core::encode_ie(result.ies,
                         static_cast<std::uint16_t>(pfcp_core::IeType::Cause),
                         pfcp_core::encode_cause(pfcp_core::Cause::RequestAccepted));
    spdlog::info("upf: Sx Session deleted for TEID {:#x}", *teid);
    return result;
}

// Runs on the main thread (blocking UDP I/O, same "blocking transport gets its own thread"
// discipline ADR-0006/ADR-0030 already established -- here it's simply the only thread, since
// UPF has no HTTP2 server to share time with). Never returns.
void run_pfcp_lifecycle(std::time_t start_time,
                        upf::Datapath* datapath,
                        TeidSessionStore& teid_session_store,
                        SeidToTeidStore& seid_to_teid_store) {
    boost::asio::io_context ioc;
    boost::asio::ip::udp::socket socket(
        ioc, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), pfcp_core::kPfcpPort));
    spdlog::info("upf: listening for PFCP/N4 (UDP) on 0.0.0.0:{}", pfcp_core::kPfcpPort);

    constexpr std::array<std::uint8_t, 4> kNodeIpv4{127, 0, 0, 1}; // this lab's loopback-only scope
    std::uint64_t next_seid = 1;
    std::uint32_t next_teid = 1;
    PfdStore pfd_store;

    std::vector<std::uint8_t> recv_buf(2048);
    while (true) {
        boost::asio::ip::udp::endpoint sender;
        boost::system::error_code ec;
        const std::size_t n = socket.receive_from(boost::asio::buffer(recv_buf), sender, 0, ec);
        if (ec) {
            spdlog::warn("upf: PFCP receive failed: {}", ec.message());
            continue;
        }
        const std::vector<std::uint8_t> msg(recv_buf.begin(),
                                            recv_buf.begin() + static_cast<std::ptrdiff_t>(n));

        std::size_t offset = 0;
        std::uint16_t ies_length = 0;
        const auto header = pfcp_core::decode_header(msg, offset, ies_length);
        if (!header.has_value()) {
            spdlog::warn("upf: failed to decode PFCP header from {}, ignoring",
                         sender.address().to_string());
            continue;
        }
        if (offset + ies_length > msg.size()) {
            spdlog::warn("upf: PFCP message length field overruns the datagram, ignoring");
            continue;
        }
        const std::vector<std::uint8_t> ie_bytes(
            msg.begin() + static_cast<std::ptrdiff_t>(offset),
            msg.begin() + static_cast<std::ptrdiff_t>(offset + ies_length));

        pfcp_core::Header resp_header;
        resp_header.has_seid = false;
        resp_header.sequence_number = header->sequence_number;
        std::vector<std::uint8_t> resp_ies;

        if (header->message_type == pfcp_core::MessageType::HeartbeatRequest) {
            resp_header.message_type = pfcp_core::MessageType::HeartbeatResponse;
            resp_ies = build_heartbeat_response_ies(start_time);
            spdlog::info("upf: replying to Heartbeat Request from {}",
                         sender.address().to_string());
        } else if (header->message_type == pfcp_core::MessageType::AssociationSetupRequest) {
            resp_header.message_type = pfcp_core::MessageType::AssociationSetupResponse;
            resp_ies = build_association_setup_response_ies(start_time, kNodeIpv4);
            spdlog::info("upf: Sx Association Setup accepted from {}",
                         sender.address().to_string());
        } else if (header->message_type == pfcp_core::MessageType::SessionEstablishmentRequest) {
            const auto result = build_session_establishment_response_ies(
                ie_bytes, kNodeIpv4, next_seid, next_teid, datapath);
            if (!result.has_value()) {
                spdlog::warn("upf: malformed Session Establishment Request from {}, ignoring",
                             sender.address().to_string());
                continue;
            }
            resp_header.message_type = pfcp_core::MessageType::SessionEstablishmentResponse;
            resp_header.has_seid = true;
            resp_header.seid = result->response_header_seid;
            resp_ies = result->ies;
            spdlog::info("upf: Sx Session established from {}", sender.address().to_string());
            // ADR-0050 Stage 2: `sender` here is SMF's real, persistent PFCP peer endpoint (its
            // PfcpPeer, ADR-0050 Stage 0, sends every request -- including this one -- from the
            // same bound socket it also listens on), so it can be reused verbatim as the address
            // to send a real, unsolicited Sx Session Report Request back to later.
            if (result->allocated_teid_with_urr.has_value() && result->urr_id.has_value()) {
                UrrSessionInfo info;
                info.smf_endpoint = sender;
                info.cp_seid = result->response_header_seid;
                info.urr_id = *result->urr_id;
                teid_session_store.put(*result->allocated_teid_with_urr, info);
            }
            // ADR-0071: unconditional -- see SessionEstablishmentResult::allocated_teid's own
            // header comment for the real bug this fixes (a QER-only session with no URR used to
            // be unreachable by SEID for any later Modification/Deletion at all).
            if (result->allocated_teid.has_value()) {
                seid_to_teid_store.put(result->up_seid, *result->allocated_teid);
            }
        } else if (header->message_type == pfcp_core::MessageType::SessionModificationRequest) {
            const auto result = build_session_modification_response_ies(
                ie_bytes, header->seid, seid_to_teid_store, teid_session_store, datapath);
            if (!result.has_value()) {
                spdlog::warn("upf: malformed Session Modification Request from {}, ignoring",
                             sender.address().to_string());
                continue;
            }
            resp_header.message_type = pfcp_core::MessageType::SessionModificationResponse;
            resp_header.has_seid = true;
            resp_header.seid = result->response_header_seid;
            resp_ies = result->ies;
            spdlog::info("upf: Sx Session Modification processed from {}",
                         sender.address().to_string());
        } else if (header->message_type == pfcp_core::MessageType::SessionDeletionRequest) {
            const auto result = build_session_deletion_response_ies(
                header->seid, seid_to_teid_store, teid_session_store, datapath);
            if (!result.has_value()) {
                spdlog::warn("upf: malformed Session Deletion Request from {}, ignoring",
                             sender.address().to_string());
                continue;
            }
            resp_header.message_type = pfcp_core::MessageType::SessionDeletionResponse;
            resp_header.has_seid = true;
            resp_header.seid = result->response_header_seid;
            resp_ies = result->ies;
            spdlog::info("upf: Sx Session Deletion processed from {}",
                         sender.address().to_string());
        } else if (header->message_type == pfcp_core::MessageType::AssociationUpdateRequest) {
            resp_header.message_type = pfcp_core::MessageType::AssociationUpdateResponse;
            resp_ies = build_association_update_response_ies(kNodeIpv4);
            spdlog::info("upf: Sx Association Update accepted from {}",
                         sender.address().to_string());
        } else if (header->message_type == pfcp_core::MessageType::AssociationReleaseRequest) {
            resp_header.message_type = pfcp_core::MessageType::AssociationReleaseResponse;
            resp_ies = build_association_release_response_ies(kNodeIpv4);
            spdlog::info("upf: Sx Association Release accepted from {} (real, disclosed scope: "
                         "this UPF's own session state for the peer is NOT bulk-deleted as a side "
                         "effect -- see this file's own comment on Session Set Deletion)",
                         sender.address().to_string());
        } else if (header->message_type == pfcp_core::MessageType::PfdManagementRequest) {
            resp_header.message_type = pfcp_core::MessageType::PfdManagementResponse;
            resp_ies = build_pfd_management_response_ies(ie_bytes, pfd_store);
            spdlog::info("upf: Sx PFD Management processed from {}, {} Application ID(s) now "
                         "provisioned",
                         sender.address().to_string(),
                         pfd_store.application_count());
        } else {
            spdlog::warn("upf: received PFCP message type {} with no handler yet, ignoring",
                         static_cast<int>(header->message_type));
            continue;
        }

        auto resp_bytes =
            pfcp_core::encode_header(resp_header, static_cast<std::uint16_t>(resp_ies.size()));
        resp_bytes.insert(resp_bytes.end(), resp_ies.begin(), resp_ies.end());
        socket.send_to(boost::asio::buffer(resp_bytes), sender, 0, ec);
        if (ec) {
            spdlog::warn("upf: PFCP send failed: {}", ec.message());
        }
    }
}

} // namespace

int main() {
    const auto config = nf_config::load("upf", CONFIG_DIR);
    const auto metrics_bind_address =
        nf_config::require<std::string>(config, "metrics_bind_address");
    const auto nrf_base_url =
        nf_config::require<std::string>(config, "nrf_base_url", "UPF_NRF_BASE_URL");
    // ADR-0203: UPF's first-ever real inbound SBI server port.
    const auto sbi_port = nf_config::require<unsigned short>(config, "port");

    sbi_core::init_logging("upf");
    sbi_core::init_tracing("upf");
    sbi_core::init_metrics(metrics_bind_address);

    const std::string upf_instance_id = sbi_core::generate_uuid_v4();
    spdlog::info("upf: starting, nfInstanceId={}", upf_instance_id);
    spdlog::info("upf: Prometheus metrics at http://{}/metrics", metrics_bind_address);

    const std::time_t start_time = std::time(nullptr);

    // ADR-0203: outlives run_sbi_server's own thread (the process's entire lifetime, same as
    // teid_session_store below).
    upf::EventSubscriptionStore event_subs;

    // ADR-0050 Stage 2: outlive `datapath` below (declared here, before it, so both are still
    // alive for as long as the datapath's ring-buffer-polling thread might invoke the usage
    // report handler -- which, since this process never terminates, is the process's entire
    // lifetime) and are shared between run_pfcp_lifecycle's main thread (populates
    // teid_session_store) and the datapath's own thread (reads it, sends via report_sender).
    TeidSessionStore teid_session_store;
    // ADR-0050 Stage 5: only ever touched by run_pfcp_lifecycle's own single thread (Establishment
    // writes, Modification reads), declared here alongside teid_session_store for the same
    // "outlives everything that could touch it" reasoning, not because it's actually shared with
    // the datapath thread the way teid_session_store is.
    SeidToTeidStore seid_to_teid_store;
    ReportSender report_sender;
    // Real PFCP header Sequence Number (TS 29.244 §7.2.2.1) for messages UPF itself originates --
    // a real, node-level counter, deliberately NOT the same value as UR-SEQN (TeidSessionStore's
    // per-URR counter): the two are different real fields with different scopes/lifetimes in the
    // spec, and conflating them was considered and rejected while writing this.
    std::atomic<std::uint32_t> next_pfcp_sequence_number{1};

    auto usage_report_handler = [&teid_session_store, &report_sender, &next_pfcp_sequence_number](
                                    std::uint32_t teid,
                                    std::uint64_t total_octets,
                                    bool quota_exhausted) {
        const auto info = teid_session_store.get_and_advance_seqn(teid);
        if (!info.has_value()) {
            spdlog::warn("upf: usage report fired for TEID {:#x} with no known session, dropping",
                         teid);
            return;
        }
        const auto& [session, ur_seqn] = *info;

        // TS 29.244 §7.5.8.3 Usage Report (within Session Report Request): URR ID, UR-SEQN,
        // Usage Report Trigger, Volume Measurement -- the fields Annex C.2.1.1's volume-based
        // flow needs; this project models no others (see session_ies.hpp's own file-header
        // disclosure of that scope).
        std::vector<std::uint8_t> usage_report;
        pfcp_core::encode_ie(usage_report,
                             static_cast<std::uint16_t>(pfcp_core::IeType::UrrId),
                             pfcp_core::encode_urr_id(session.urr_id));
        pfcp_core::encode_ie(usage_report,
                             static_cast<std::uint16_t>(pfcp_core::IeType::UrSeqn),
                             pfcp_core::encode_ur_seqn(ur_seqn));
        pfcp_core::encode_ie(usage_report,
                             static_cast<std::uint16_t>(pfcp_core::IeType::UsageReportTrigger),
                             quota_exhausted ? pfcp_core::encode_usage_report_trigger_volqu()
                                             : pfcp_core::encode_usage_report_trigger_volth());
        pfcp_core::encode_ie(usage_report,
                             static_cast<std::uint16_t>(pfcp_core::IeType::VolumeMeasurement),
                             pfcp_core::encode_volume_total(total_octets));

        std::vector<std::uint8_t> ies;
        pfcp_core::encode_ie(ies,
                             static_cast<std::uint16_t>(pfcp_core::IeType::ReportType),
                             pfcp_core::encode_report_type_usage_report());
        pfcp_core::encode_ie(
            ies, static_cast<std::uint16_t>(pfcp_core::IeType::UsageReport), usage_report);

        pfcp_core::Header header;
        header.message_type = pfcp_core::MessageType::SessionReportRequest;
        // TS 29.244's addressing rule this file already relies on elsewhere (see
        // SessionEstablishmentResult's own comment): the sending entity uses the SEID value
        // provided by the corresponding receiving entity -- here, SMF's own CP F-SEID from this
        // session's Establishment Request.
        header.has_seid = true;
        header.seid = session.cp_seid;
        header.sequence_number = next_pfcp_sequence_number.fetch_add(1);

        auto bytes = pfcp_core::encode_header(header, static_cast<std::uint16_t>(ies.size()));
        bytes.insert(bytes.end(), ies.begin(), ies.end());
        report_sender.send(session.smf_endpoint, bytes);
        spdlog::info("upf: sent Sx Session Report Request to {} for TEID {:#x}: total={} octets, "
                     "trigger={}",
                     session.smf_endpoint.address().to_string(),
                     teid,
                     total_octets,
                     quota_exhausted ? "VOLQU" : "VOLTH");
    };

    // ADR-0043: real eBPF/XDP GTP-U decapsulation datapath. Failure is disclosed and non-fatal --
    // PFCP control-plane signalling (Stages 1-3) works identically with or without it (see
    // datapath.hpp's own comment for why, and what privileges a real datapath needs).
    auto datapath = upf::Datapath::create(usage_report_handler);
    if (!datapath.has_value()) {
        spdlog::warn("upf: eBPF/XDP datapath not started (see preceding error) -- PFCP "
                     "control-plane signalling still works, but no uplink packet will actually "
                     "be decapsulated/forwarded");
    }

    std::thread(run_nrf_lifecycle, upf_instance_id, nrf_base_url).detach();
    std::thread(run_sbi_server, sbi_port, std::ref(event_subs), sbi_core::read_tps_limit(config))
        .detach();
    run_pfcp_lifecycle(start_time,
                       datapath.has_value() ? &*datapath : nullptr,
                       teid_session_store,
                       seid_to_teid_store); // blocks forever
    return 0;
}
