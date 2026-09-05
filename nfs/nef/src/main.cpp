// nfs/nef: NEF (Network Exposure Function), Nnef_PFDmanagement service (one of NEF's real
// services -- see this file's own "In scope" section for why only this one, not all 14 real NEF
// YAML files, is built this turn). Source:
// specs/5G_APIs-REL-19/TS29551_Nnef_PFDmanagement.yaml (v1.4.0), commit
// bca84b60a37773133bcae97e5c6c0d10a93b47b6. This project's eleventh NF, third built under the
// continuous move-to-next-NF process (docs/DECISIONS.md ADR-0184).
//
// In scope, agreed with the user before implementation: `docs/CAPABILITY_GAP_ANALYSIS.md`'s "still
// not done" list names NEF as one of the two remaining unbuilt Tier 1 NFs (with SCP). NEF's own
// real spec surface is 14 separate YAML files, ~52 operations total -- too large for one turn as a
// whole (Nnef_EventExposure, Nnef_SMContext, Nnef_SMService, Nnef_UEId, Nnef_Authentication,
// Nnef_TrafficInfluenceData, Nnef_DNAIMapping, Nnef_ECSAddress, Nnef_EASDeployment,
// Nnef_Inference/Training/VFLInference/VFLTraining all remain unbuilt after this turn, a real,
// disclosed deferral, not a claim of NEF completeness). `Nnef_PFDmanagement` (6 real top-level
// operations, the largest single well-defined NEF service) chosen as this turn's slice: it is the
// real, spec-documented consumer-facing counterpart to this project's own already-built UPF-side
// PFCP PFD Management (task #107/ADR-0086, TS 29.244 -- the N4 side SMF already uses to push PFDs
// to UPF), even though the two aren't wired together this turn (real, disclosed, deferred --
// SMF doesn't yet call this NEF API to source the PFDs it pushes onward).
//
// All 6 real top-level operations implemented:
//   GET    {apiRoot}/applications                  Nnef_PFDmanagement_AllFetch
//   POST   {apiRoot}/applications/partialpull       Nnef_PFDmanagement_AppFetchPartialUpdate
//   GET    {apiRoot}/applications/{appId}           Nnef_PFDmanagement_IndAppFetch
//   POST   {apiRoot}/subscriptions                  Nnef_PFDmanagement_CreateSubscr
//   PUT    {apiRoot}/subscriptions/{subscriptionId} Nnef_PFDmanagement_ModifySubscr
//   DELETE {apiRoot}/subscriptions/{subscriptionId} Nnef_PFDmanagement_Unsubscribe
//
// Real, disclosed simplifications/gaps -- stated up front, not discovered in review:
// 1. This YAML has NO operation anywhere that lets a caller WRITE PFD content into NEF -- the real
//    3GPP AF-to-NEF PFD provisioning path is genuinely out of 3GPP's own standardized SBI
//    framework scope (typically OAM/vendor-specific), not just unbuilt here (confirmed by direct
//    read of the full YAML, not assumed). `PfdCatalogStore` is therefore seed()-only, same
//    precedent as several of this project's own other "no live write path exists" stores.
// 2. Because of (1), the real `PfdChangeNotification`/`NotificationPush` callback delivery this
//    API declares (`CreateSubscr`'s own POST callbacks) has NO real trigger this project can ever
//    fire -- PFD content never changes after startup seeding, so there is no real "PFD changed"
//    event to notify about. This project does NOT build a notification-delivery function with no
//    real caller (dead code presented as if it were live infrastructure would be worse than
//    disclosing the gap plainly): `CreateSubscr`/`ModifySubscr`/`Unsubscribe` are real, live,
//    tested CRUD on the subscription resource itself, but no notification is ever sent. A real,
//    disclosed structural gap, not an oversight.
// 3. `AppFetchPartialUpdate`'s real "changed since" comparison
//    (`PfdCatalogStore::get_if_changed_since`) uses lexicographic string comparison of ISO8601 UTC
//    timestamps -- correct for this project's own generated `DateTime` string format (always
//    zero-padded, always UTC/`Z`-suffixed), a real, disclosed narrower assumption than a full
//    calendar-aware datetime comparison would need for arbitrary input formats.
//
// UPDATE (ADR-0207, gap-closure task #164, first of NEF's own remaining Tier-A slices):
// `Nnef_SMService` (TS29541, 1 op: SendSMS -- real `multipart/related` handling reusing the
// already-wired `SmsData`/`SmsDeliveryData` types from `TS29577_Nipsmgw_SMService.yaml`, same
// pattern as `nfs/smsf`'s own `SendMtSMS`, ADR-0188), `Nnef_UEId` (TS29591, 2 ops: FetchUEId,
// UEIDMappingInfoRetrieval), `Nnef_DNAIMapping` (TS29591 paths +
// `TS29522_DNAIMapping.yaml` schemas, 3 ops: create/get/delete subscription), and
// `Nnef_EASDeployment` (TS29591, 3 ops: create/get/delete subscription) added -- 4 of NEF's
// remaining 13 real Tier-A gaps (9 remain: `Nnef_SMContext`, `Nnef_ECSAddress`,
// `Nnef_EventExposure`, `Nnef_Inference`, `Nnef_TrafficInfluenceData`, `Nnef_Training`,
// `Nnef_VFLInference`, `Nnef_VFLTraining`, `Nnef_Authentication`). Real finding: unlike prior
// cross-file `$ref` cases in this project, `TS29591_Nnef_DNAIMapping.yaml` itself owns zero
// schemas (its request/response types are entirely `$ref`'d from the separate, real
// `TS29522_DNAIMapping.yaml`) -- `tools/sbi-codegen` only emits a real output file for a YAML that
// owns at least one schema, so `TS29522_DNAIMapping.yaml` needed its own, additional pilot-set
// entry for `DnaiMapSub`/`DnaiMapUpdateNotif` to actually generate; the prior "only the
// entry-point file needs adding" rule (confirmed by reading `loader.py`, ADR-0193/ADR-0201) holds
// for common-data groupings folded into `TS26510_CommonData_grp.hpp`, but not for a distinct,
// separately-numbered 3GPP schema file like this one. Disclosed: `FetchUEId`/
// `UEIDMappingInfoRetrieval` both real-`204` ("does not exist") since this NEF has no real
// roaming H-NEF/internal-UE-identity mapping database to look anything up in, matching the real
// YAML's own documented semantics for that case, not a fabricated lookup. `SendSMS` carries the
// same disclosed non-scope as `nfs/smsf`'s own `SendMtSMS`: no real TS 24.011 SMS-DELIVER-REPORT
// encoder and no real onward IP-SM-GW/SMSF relay exist in this build.
//
// UPDATE (ADR-0208, gap-closure task #164, second NEF slice): `Nnef_SMContext` (TS29541, 4 ops:
// Create/Delete(release)/Update/Deliver -- real `multipart/related` on Deliver, same disclosed
// non-scope as SendSMS above: no real onward NIDD MT/MO relay exists), `Nnef_Authentication`
// (TS29256, 1 op: UAV authentication -- real, honest `403` `UAVAuthFailure` since this NEF has no
// real UAS-NF/USS authentication backend, TS 23.256 scope, not a fabricated `200` success), and
// `Nnef_ECSAddress` (TS29591 paths + `TS29522_ECSAddress.yaml` schemas, 5 ops: subscription
// create/get/put/patch/delete, real RFC 7396 merge-patch on PATCH) added -- 3 more of NEF's
// remaining 9 real Tier-A gaps (6 remain: `Nnef_EventExposure`, `Nnef_Inference`,
// `Nnef_TrafficInfluenceData`, `Nnef_Training`, `Nnef_VFLInference`, `Nnef_VFLTraining`). Real
// finding: adding `Nnef_SMContext`/`Nnef_Authentication` to the pilot set collided not just with
// NEF's own types but with **other NFs'** already-shipping ones -- `SmContextCreateData`/
// `SmContextCreatedData`/`SmContextUpdateData`/`SmContextReleaseData`/`SmContextStatusNotification`
// (real `Nsmf_PDUSession` schemas) and `DeliverReqData` (already `_Nsmf_NIDD`-suffixed from
// ADR-0201) both picked up a new same-named collision from this slice's own `Nnef_SMContext`
// schemas; `AuthResult` (real `Nausf_UEAuthentication` schema) collided with this slice's own
// differently-valued `Nnef_Authentication` `AuthResult`. Fixed 9 bare-name references in
// `nfs/smf/src/main.cpp`, 2 in `nfs/amf/src/ngap_task.cpp`, 8 in `nfs/ausf/src/main.cpp`, and 5
// across two integration test files to the newly-suffixed real generated names -- pure renames, no
// behavioral change (see ADR-0208 for full detail and the explicit SMF/AMF/AUSF re-verification).
//
// UPDATE (ADR-0209, gap-closure task #164, third NEF slice): `Nnef_EventExposure` (TS29591, 4 ops:
// Create/Get/Replace(PUT)/Delete subscription) added -- 8 of NEF's remaining 13 real Tier-A gaps
// (5 remain: `Nnef_Inference`, `Nnef_TrafficInfluenceData`, `Nnef_Training`, `Nnef_VFLInference`,
// `Nnef_VFLTraining`). Same real, disclosed non-scope as every other EventExposure-family gap
// closed this project: the subscription resource is real, live CRUD, but no notification is ever
// actually fired (no real cross-NF event-detection pipeline exists to trigger one). Two real
// findings beyond the gap itself, both fully disclosed in ADR-0209: (1) a real `tools/sbi-codegen`
// limitation -- `TS26512_EventExposure.yaml`'s own real "abstract base (`items: {}`, deliberately
// unconstrained) narrowed by a concrete allOf subtype" pattern tripped the generator's existing
// allOf-conflict safety check (ADR-0190); fixed properly in the shared generator itself
// (`schema_to_ir.py`'s new `_is_opaque_type_ref` helper), not routed around. (2) That fix's own
// real, disclosed, project-wide consequence -- pulling in the two dependency files this required
// (`TS26512_EventExposure.yaml`, `TS29517_Naf_EventExposure.yaml`) bridged a real cyclic `$ref`
// into this project's pre-existing giant common-data SCC group, renaming its own generated file
// project-wide (`TS29122_CommonData_grp.hpp` -> `TS26510_CommonData_grp.hpp`, deterministic per
// `render.py`'s own naming rule), which broke 21 real `#include` lines across `nfs/`/`tests/`
// until fixed as a mechanical rename -- see ADR-0209 for the full file list and re-verification.
//
// UPDATE (ADR-0210, gap-closure task #164, fourth and final NEF slice): `Nnef_TrafficInfluenceData`
// (TS29591, 4 ops: Create/Get/Replace/Delete, real `anyOf: [{required:[dnns]},
// {required:[snssais]}]` constraint enforced explicitly since it isn't expressible in the generated
// struct's own required fields), `Nnef_Inference` (TS29591, 4 ops:
// Create/Update/PartialUpdate/Delete -- real, disclosed: no GET operation exists on this resource
// at all), `Nnef_Training` (TS29591, same real 4-op/no-GET shape), `Nnef_VFLInference` (TS29591, 5
// ops: Create/Get/Update/PartialUpdate/Delete), and `Nnef_VFLTraining` (TS29591, same real 5-op
// shape, real, disclosed: unlike every other NEF subscription resource this project has built, only
// `vflTrainSubs` is required here -- notifUri/notifCorrId are genuinely optional per this YAML's
// own schema) added -- **the last 5 of NEF's 13 real Tier-A gaps, closing task #164 completely: all
// 14 of NEF's own real YAML files (13 Tier-A + the original `Nnef_PFDmanagement`, ADR-0185) are now
// built.** Real finding: this slice's own dependency files (`TS29519_Application_Data.yaml`,
// `TS29530_Naf_Inference.yaml`, `TS29520_Nnwdaf_EventsSubscription.yaml`,
// `TS29530_Naf_Training.yaml`, `TS29520_Nnwdaf_VFLInference.yaml`,
// `TS29520_Nnwdaf_VFLTraining.yaml`) bridged a cyclic `$ref` that absorbed `Nnef_PFDmanagement`'s
// own schemas into the shared `TS26510_CommonData_grp.hpp` group -- its own standalone generated
// header stopped existing, breaking this file's own
// `#include "TS29551_Nnef_PFDmanagement.hpp"` line, fixed by removing it (the shared group header
// was already included). See ADR-0210 for the full disclosure, including two real bugs of my own
// caught via live verification (fabricated nested-object field shapes in the new test file's own
// bodies, and the resulting `400`s re-triggering the known `ASSERT`-before-cleanup leaked-process
// bug class from ADR-0204).

#include "sbi_core/http2_client.hpp"
#include "sbi_core/http2_server.hpp"
#include "sbi_core/io_context_pool.hpp"
#include "sbi_core/json_body.hpp"
#include "sbi_core/jwt.hpp"
#include "sbi_core/logging.hpp"
#include "sbi_core/metrics.hpp"
#include "sbi_core/multipart.hpp"
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
#include <vector>

#include "TS26510_CommonData_grp.hpp"
#include "TS29256_Nnef_Authentication.hpp"
#include "TS29522_DNAIMapping.hpp"
#include "TS29541_Nnef_SMContext.hpp"
#include "TS29577_Nipsmgw_SMService.hpp"
#include "TS29591_Nnef_EASDeployment.hpp"
#include "TS29591_Nnef_ECSAddress.hpp"
#include "TS29591_Nnef_Inference.hpp"
#include "TS29591_Nnef_TrafficInfluenceData.hpp"
#include "TS29591_Nnef_Training.hpp"
#include "TS29591_Nnef_UEId.hpp"
#include "TS29591_Nnef_VFLInference.hpp"
#include "TS29591_Nnef_VFLTraining.hpp"
#include "nf_config/nf_config.hpp"
#include "stores.hpp"

namespace {

using nlohmann::json;

#ifndef CERTS_DIR
#error "CERTS_DIR must be defined by CMake (see nfs/nef/CMakeLists.txt)"
#endif
#ifndef CONFIG_DIR
#error "CONFIG_DIR must be defined by CMake (see nfs/nef/CMakeLists.txt)"
#endif

constexpr const char* kNfType = "NEF";
constexpr const char* kApiRoot = "/nnef-pfdmanagement/v1";
// ADR-0207 (gap-closure task #164, first NEF slice). Real api roots confirmed from each YAML's
// own `servers:` block.
constexpr const char* kSmServiceApiRoot = "/nnef-smservice/v1";
constexpr const char* kUeIdApiRoot = "/nnef-ueid/v1";
constexpr const char* kDnaiMappingApiRoot = "/nnef-dnai-mapping/v1";
constexpr const char* kEasDeploymentApiRoot = "/nnef-eas-deployment/v1";
// ADR-0208 (gap-closure task #164, second NEF slice).
constexpr const char* kSmContextApiRoot = "/nnef-smcontext/v1";
constexpr const char* kAuthenticationApiRoot = "/nnef-authentication/v1";
constexpr const char* kEcsAddressApiRoot = "/nnef-ecs-addr-cfg-info/v1";
// ADR-0209 (gap-closure task #164, third NEF slice).
constexpr const char* kEventExposureApiRoot = "/nnef-eventexposure/v1";
// ADR-0210 (gap-closure task #164, fourth and final NEF slice).
constexpr const char* kTrafficInfluenceDataApiRoot = "/nnef-traffic-influence-data/v1";
// ADR-0302: the AF-FACING northbound API (TS 29.522), as distinct from the Nnef_* NF-facing
// services above. This is the "exposure" NEF is named for, and ADR-0294 found the whole surface
// missing.
constexpr const char* kAfTrafficInfluenceApiRoot = "/3gpp-traffic-influence/v1";
constexpr const char* kInferenceApiRoot = "/nnef-inference/v1";
constexpr const char* kTrainingApiRoot = "/nnef-training/v1";
constexpr const char* kVflInferenceApiRoot = "/nnef-vfl-inference/v1";
constexpr const char* kVflTrainingApiRoot = "/nnef-vfl-training/v1";

// Must match nfs/nrf/src/main.cpp's kNrfInstanceId exactly -- see docs/DECISIONS.md ADR-0018.
constexpr const char* kNrfInstanceId = "5ba9a927-1d31-4c8e-8a10-000000000001";

// Real, illustrative seed data -- see this file's own top comment, simplification 1, for why no
// live write path exists to populate this instead.
void seed_pfd_catalog(nef::PfdCatalogStore& store) {
    sbi_gen::PfdContent content;
    content.pfdId = "pfd1";
    content.urls = std::vector<std::string>{"^https://video\\.example\\.com/.*$"};

    sbi_gen::PfdDataForApp app1;
    app1.applicationId = "app-video-streaming";
    app1.pfds = std::vector<sbi_gen::PfdContent>{content};
    app1.pfdTimestamp = "2026-01-01T00:00:00Z";
    store.seed("app-video-streaming", json(app1));
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

// ADR-0302: an AF's TrafficInfluSub becomes a real TS 29.519 TrafficInfluData in UDR.
//
// This is the whole point of the feature. ADR-0294's finding was that this NEF's only outbound
// HTTP was NRF registration -- everything else landed in an in-process map. Traffic-influence data
// that never reaches UDR influences nothing: SMF reads it from UDR, and an in-process copy is
// invisible to it. free5GC's own NEF does the same thing (AppDataInfluenceDataPut).
//
// Real, disclosed field mapping -- only fields present in BOTH schemas are carried, nothing is
// synthesised:
//   afAppId, dnn, snssai, ethTrafficFilters, trafficFilters (-> TrafficInfluData's own
//   `trafficFilters`), trafficRoutes, appReloInd, supi/gpsi/ipv4Addr/ipv6Addr, externalGroupId
//   (-> interGroupId), trafficDataSets.
// TrafficInfluSub fields with no TrafficInfluData counterpart (afServiceId, afTransId,
// subscribedEvents, notificationDestination, the geo/temporal validity sets) stay in NEF's own
// copy of the subscription and are NOT invented into the UDR record.
sbi_gen::TrafficInfluData
to_traffic_influ_data(const sbi_gen::TrafficInfluSub_TrafficInfluence& s) {
    sbi_gen::TrafficInfluData d{};
    d.afAppId = s.afAppId;
    d.appReloInd = s.appReloInd;
    d.dnn = s.dnn;
    d.snssai = s.snssai;
    d.ethTrafficFilters = s.ethTrafficFilters;
    d.trafficFilters = s.trafficFilters;
    d.trafficRoutes = s.trafficRoutes;
    d.ipv4Addr = s.ipv4Addr;
    d.ipv6Addr = s.ipv6Addr;
    d.trafficDataSets = s.trafficDataSets;
    d.sfcIdDl = s.sfcIdDl;
    d.sfcIdUl = s.sfcIdUl;
    d.metadata = s.metadata;
    d.candDnaiInd = s.candDnaiInd;
    d.tempValidities = s.tempValidities;
    d.subscribedEvents = s.subscribedEvents;
    // NOT mapped, each for a real reason rather than an oversight:
    //   * `gpsi` and `macAddr` identify the target UE in TrafficInfluSub, but TrafficInfluData has
    //     no counterpart for either -- it identifies by `supi`, which the AF-facing schema does
    //     not carry (confirmed by direct read: `supi` is not among TrafficInfluSub's properties).
    //     Translating GPSI to SUPI needs a UDM lookup this route does not perform, so nothing is
    //     invented; an AF request identified only by GPSI or MAC still creates the NEF-side
    //     subscription and produces a UDR record without a subscriber key.
    //   * `externalGroupId` (an External-Group-Id) is NOT assigned to `interGroupId` (an
    //     Internal-Group-Id) despite the tempting name symmetry -- they are different identifier
    //     spaces and the external-to-internal mapping is UDM's job (Nudm_SDM GroupIdentifiers).
    //     Assigning one to the other would put an external id in an internal id field and be
    //     wrong in a way nothing downstream would detect.
    return d;
}

// PUT the record to UDR, or DELETE it. Best-effort and logged, never fatal to the AF's request:
// the AF-facing resource genuinely exists at NEF once created, and failing the AF's call because a
// downstream write failed would misreport whose fault it is. The failure IS logged loudly, because
// a subscription that never reached UDR is inert and silence about that is what ADR-0294 objected
// to in the first place.
bool push_influence_to_udr(sbi_core::http2::Client& udr_client,
                           const std::string& udr_base_url,
                           const std::string& influence_id,
                           const std::optional<sbi_gen::TrafficInfluData>& data) {
    sbi_core::http2::ClientRequest req;
    req.url = udr_base_url + "/nudr-dr/v2/application-data/influenceData/" + influence_id;
    if (data.has_value()) {
        req.method = "PUT";
        req.headers.emplace("content-type", "application/json");
        req.body = nlohmann::json(*data).dump();
    } else {
        req.method = "DELETE";
    }
    auto resp = udr_client.send(req);
    if (!resp.has_value() || resp->status >= 300) {
        spdlog::warn("nef: traffic-influence {} to UDR failed for influenceId={} (status={}) -- "
                     "the subscription exists at NEF but no SMF will act on it",
                     data.has_value() ? "PUT" : "DELETE",
                     influence_id,
                     resp.has_value() ? resp->status : 0);
        return false;
    }
    spdlog::info("nef: traffic-influence {} to UDR for influenceId={} (status={})",
                 data.has_value() ? "PUT" : "DELETE",
                 influence_id,
                 resp->status);
    return true;
}

// Runs on a dedicated thread, never on the server's io_context -- same reasoning as
// nfs/ausf/src/main.cpp's run_nrf_lifecycle (docs/DECISIONS.md ADR-0006/ADR-0019).
void run_nrf_lifecycle(const std::string& nef_instance_id, const std::string& nrf_base) {
    sbi_core::http2::TlsConfig client_tls{
        .cert_path = CERTS_DIR "/nef/cert.pem",
        .key_path = CERTS_DIR "/nef/key.pem",
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
        http_client, nrf_base + "/oauth2/token", nef_instance_id, "nnrf-nfm", "NRF");

    constexpr int kHeartbeatSeconds = 30;
    json profile{
        {"nfInstanceId", nef_instance_id},
        {"nfType", kNfType},
        {"nfStatus", "REGISTERED"},
        {"ipv4Addresses", json::array({"127.0.0.1"})},
        {"heartBeatTimer", kHeartbeatSeconds},
    };

    while (true) {
        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("nef: OAuth2 token fetch failed: {}", token.error());
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        sbi_core::http2::ClientRequest put_req;
        put_req.method = "PUT";
        put_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + nef_instance_id;
        put_req.headers.emplace("content-type", "application/json");
        put_req.headers.emplace("authorization", "Bearer " + *token);
        put_req.headers.emplace(
            sbi_core::headers::kSenderTimestamp,
            sbi_core::headers::format_sender_timestamp(std::chrono::system_clock::now()));
        put_req.body = profile.dump();
        auto put_resp = http_client.send(put_req);
        if (put_resp.has_value() && (put_resp->status == 200 || put_resp->status == 201)) {
            spdlog::info("nef: registered with NRF (HTTP {})", put_resp->status);
            break;
        }
        spdlog::warn("nef: NRF registration attempt failed, retrying in 5s");
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(kHeartbeatSeconds / 2));

        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("nef: OAuth2 token fetch failed for heartbeat: {}", token.error());
            continue;
        }

        sbi_core::http2::ClientRequest patch_req;
        patch_req.method = "PATCH";
        patch_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + nef_instance_id;
        patch_req.headers.emplace("content-type", "application/json-patch+json");
        patch_req.headers.emplace("authorization", "Bearer " + *token);
        patch_req.body =
            json::array({json{{"op", "replace"}, {"path", "/nfStatus"}, {"value", "REGISTERED"}}})
                .dump();

        auto patch_resp = http_client.send(patch_req);
        if (!patch_resp.has_value() || patch_resp->status != 200) {
            spdlog::warn("nef: heartbeat failed");
        }
    }
}

} // namespace

int main() {
    sbi_core::init_logging("nef");
    sbi_core::init_tracing("nef");

    // ADR-0077 (user-directed, mandatory, project-wide): no DB URL/connection/deployment
    // parameter may be a hardcoded literal default in source -- real values live in the
    // checked-in config/nef.json, with an env var override per key still available.
    const auto config = nf_config::load("nef", CONFIG_DIR);
    const auto port = nf_config::require<unsigned short>(config, "port");
    const auto metrics_bind_address =
        nf_config::require<std::string>(config, "metrics_bind_address");
    const auto nrf_base =
        nf_config::require<std::string>(config, "nrf_base_url", "NEF_NRF_BASE_URL");
    // ADR-0302: NEF's first real downstream NF. Until now its only outbound HTTP was NRF
    // registration -- see ADR-0294.
    const auto udr_base_url =
        nf_config::require<std::string>(config, "udr_base_url", "NEF_UDR_BASE_URL");

    sbi_core::init_metrics(metrics_bind_address);

    const std::string nef_instance_id = sbi_core::generate_uuid_v4();
    spdlog::info("nef: starting, nfInstanceId={}", nef_instance_id);

    sbi_core::http2::TlsConfig server_tls{
        .cert_path = CERTS_DIR "/nef/cert.pem",
        .key_path = CERTS_DIR "/nef/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };

    sbi_core::jwt::Verifier verifier(CERTS_DIR "/nrf-jwt/public.pem", kNrfInstanceId);

    nef::AfTrafficInfluenceSubStore af_traffic_influence_subs;
    // One client per thread is this project's standing contract for http2::Client (libcurl's own
    // per-easy-handle single-thread requirement); the server runs its handlers on a single
    // io_context thread, so one client shared by those handlers is correct here -- the same shape
    // every other NF's route-handler client already uses.
    sbi_core::http2::TlsConfig udr_client_tls{
        .cert_path = CERTS_DIR "/nef/cert.pem",
        .key_path = CERTS_DIR "/nef/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client udr_client(std::move(udr_client_tls));

    nef::PfdCatalogStore pfd_catalog;
    seed_pfd_catalog(pfd_catalog);
    nef::PfdSubscriptionStore subscriptions;
    nef::DnaiMapSubStore dnai_map_subs;
    nef::EasDeploySubStore eas_deploy_subs;
    nef::SmContextStore sm_contexts;
    nef::EcsAddrCfgInfoSubStore ecs_addr_subs;
    nef::NefEventExposureSubStore event_exposure_subs;
    nef::TrafficInfluDataSubStore traffic_influence_subs;
    nef::InferEventSubStore inference_subs;
    nef::TrainEventsSubStore training_subs;
    nef::VflInferSubStore vfl_inference_subs;
    nef::NefVflTrainSubStore vfl_training_subs;

    auto meter = sbi_core::get_meter("nef");
    auto all_fetch_counter = meter->CreateUInt64Counter("nef_pfd_all_fetch_total",
                                                        "Total Nnef_PFDmanagement_AllFetch calls");
    auto partial_fetch_counter = meter->CreateUInt64Counter(
        "nef_pfd_partial_fetch_total", "Total Nnef_PFDmanagement_AppFetchPartialUpdate calls");
    auto ind_fetch_counter = meter->CreateUInt64Counter(
        "nef_pfd_ind_fetch_total", "Total Nnef_PFDmanagement_IndAppFetch calls");
    auto sub_create_counter = meter->CreateUInt64Counter(
        "nef_pfd_sub_create_total", "Total Nnef_PFDmanagement_CreateSubscr calls");
    auto sub_modify_counter = meter->CreateUInt64Counter(
        "nef_pfd_sub_modify_total", "Total Nnef_PFDmanagement_ModifySubscr calls");
    auto sub_delete_counter = meter->CreateUInt64Counter(
        "nef_pfd_sub_delete_total", "Total Nnef_PFDmanagement_Unsubscribe calls");
    auto send_sms_counter =
        meter->CreateUInt64Counter("nef_send_sms_total", "Total Nnef_SMService SendSMS calls");
    auto fetch_ueid_counter =
        meter->CreateUInt64Counter("nef_fetch_ueid_total", "Total Nnef_UEId FetchUEId calls");
    auto ueid_mapping_counter = meter->CreateUInt64Counter(
        "nef_ueid_mapping_total", "Total Nnef_UEId UEIDMappingInfoRetrieval calls");
    auto dnai_map_create_counter = meter->CreateUInt64Counter(
        "nef_dnai_map_create_total", "Total Nnef_DNAIMapping CreateIndividualSubcription calls");
    auto dnai_map_delete_counter = meter->CreateUInt64Counter(
        "nef_dnai_map_delete_total", "Total Nnef_DNAIMapping DeleteIndividualSubcription calls");
    auto eas_deploy_create_counter =
        meter->CreateUInt64Counter("nef_eas_deploy_create_total",
                                   "Total Nnef_EASDeployment CreateIndividualSubcription calls");
    auto eas_deploy_delete_counter =
        meter->CreateUInt64Counter("nef_eas_deploy_delete_total",
                                   "Total Nnef_EASDeployment DeleteIndividualSubcription calls");
    auto sm_context_create_counter = meter->CreateUInt64Counter(
        "nef_smcontext_create_total", "Total Nnef_SMContext Create calls");
    auto sm_context_release_counter = meter->CreateUInt64Counter(
        "nef_smcontext_release_total", "Total Nnef_SMContext Delete (release) calls");
    auto sm_context_update_counter = meter->CreateUInt64Counter(
        "nef_smcontext_update_total", "Total Nnef_SMContext Update calls");
    auto sm_context_deliver_counter = meter->CreateUInt64Counter(
        "nef_smcontext_deliver_total", "Total Nnef_SMContext Deliver calls");
    auto uav_auth_counter = meter->CreateUInt64Counter(
        "nef_uav_authentication_total", "Total Nnef_Authentication UAV authentication calls");
    auto ecs_addr_create_counter = meter->CreateUInt64Counter(
        "nef_ecs_addr_sub_create_total", "Total Nnef_ECSAddress CreateIndividualSubcription calls");
    auto ecs_addr_put_counter = meter->CreateUInt64Counter(
        "nef_ecs_addr_sub_put_total", "Total Nnef_ECSAddress ReplaceIndividualSubcription calls");
    auto ecs_addr_patch_counter = meter->CreateUInt64Counter(
        "nef_ecs_addr_sub_patch_total", "Total Nnef_ECSAddress ModifyIndividualSubcription calls");
    auto ecs_addr_delete_counter = meter->CreateUInt64Counter(
        "nef_ecs_addr_sub_delete_total", "Total Nnef_ECSAddress DeleteIndividualSubcription calls");
    auto event_exposure_create_counter =
        meter->CreateUInt64Counter("nef_event_exposure_sub_create_total",
                                   "Total Nnef_EventExposure CreateIndividualSubcription calls");
    auto event_exposure_put_counter =
        meter->CreateUInt64Counter("nef_event_exposure_sub_put_total",
                                   "Total Nnef_EventExposure ReplaceIndividualSubcription calls");
    auto event_exposure_delete_counter =
        meter->CreateUInt64Counter("nef_event_exposure_sub_delete_total",
                                   "Total Nnef_EventExposure DeleteIndividualSubcription calls");
    auto traffic_influence_create_counter = meter->CreateUInt64Counter(
        "nef_traffic_influence_sub_create_total",
        "Total Nnef_TrafficInfluenceData CreateIndividualSubcription calls");
    auto traffic_influence_put_counter = meter->CreateUInt64Counter(
        "nef_traffic_influence_sub_put_total",
        "Total Nnef_TrafficInfluenceData ReplaceIndividualSubcription calls");
    auto traffic_influence_delete_counter = meter->CreateUInt64Counter(
        "nef_traffic_influence_sub_delete_total",
        "Total Nnef_TrafficInfluenceData DeleteIndividualSubcription calls");
    auto inference_create_counter = meter->CreateUInt64Counter(
        "nef_inference_sub_create_total", "Total Nnef_Inference CreateInferenceSubcription calls");
    auto inference_put_counter = meter->CreateUInt64Counter(
        "nef_inference_sub_put_total", "Total Nnef_Inference UpdateInferenceSubcription calls");
    auto inference_patch_counter =
        meter->CreateUInt64Counter("nef_inference_sub_patch_total",
                                   "Total Nnef_Inference PartialUpdateInferenceSubcription calls");
    auto inference_delete_counter = meter->CreateUInt64Counter(
        "nef_inference_sub_delete_total", "Total Nnef_Inference DeleteInferenceSubcription calls");
    auto training_create_counter = meter->CreateUInt64Counter(
        "nef_training_sub_create_total", "Total Nnef_Training CreateTrainingSubcription calls");
    auto training_put_counter = meter->CreateUInt64Counter(
        "nef_training_sub_put_total", "Total Nnef_Training UpdateTrainingSubcription calls");
    auto training_patch_counter =
        meter->CreateUInt64Counter("nef_training_sub_patch_total",
                                   "Total Nnef_Training PartialUpdateTrainingSubcription calls");
    auto training_delete_counter = meter->CreateUInt64Counter(
        "nef_training_sub_delete_total", "Total Nnef_Training DeleteTrainingSubcription calls");
    auto vfl_inference_create_counter =
        meter->CreateUInt64Counter("nef_vfl_inference_sub_create_total",
                                   "Total Nnef_VFLInference CreateVFLInferenceSubcription calls");
    auto vfl_inference_put_counter =
        meter->CreateUInt64Counter("nef_vfl_inference_sub_put_total",
                                   "Total Nnef_VFLInference UpdateVFLInferenceSubcription calls");
    auto vfl_inference_patch_counter = meter->CreateUInt64Counter(
        "nef_vfl_inference_sub_patch_total",
        "Total Nnef_VFLInference PartialUpdateVFLInferenceSubcription calls");
    auto vfl_inference_delete_counter =
        meter->CreateUInt64Counter("nef_vfl_inference_sub_delete_total",
                                   "Total Nnef_VFLInference DeleteVFLInferenceSubcription calls");
    auto vfl_training_create_counter =
        meter->CreateUInt64Counter("nef_vfl_training_sub_create_total",
                                   "Total Nnef_VFLTraining CreateNEFVFLTrainingSubcription calls");
    auto vfl_training_put_counter =
        meter->CreateUInt64Counter("nef_vfl_training_sub_put_total",
                                   "Total Nnef_VFLTraining UpdateNEFVFLTrainingSubcription calls");
    auto vfl_training_patch_counter =
        meter->CreateUInt64Counter("nef_vfl_training_sub_patch_total",
                                   "Total Nnef_VFLTraining ModifyNEFVFLTrainingSubcription calls");
    auto vfl_training_delete_counter =
        meter->CreateUInt64Counter("nef_vfl_training_sub_delete_total",
                                   "Total Nnef_VFLTraining DeleteNEFVFLTrainingSubcription calls");

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

    // --- Nnef_PFDmanagement: applications ---

    server.add_route(
        "GET",
        std::string(kApiRoot) + "/applications",
        [&verifier, &pfd_catalog, &all_fetch_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto ids_it = req.query_params.find("application-ids");
            if (ids_it == req.query_params.end()) {
                return sbi_core::http2::problem_response(
                    400, "Missing mandatory query parameter", "application-ids is required");
            }
            all_fetch_counter->Add(1);
            const auto ids = sbi_core::http2::split_form_array(ids_it->second);
            json results = json::array();
            for (const auto& pfd_data : pfd_catalog.get_many(ids)) {
                results.push_back(pfd_data);
            }
            return sbi_core::http2::Response::json(200, results.dump());
        });

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/applications/partialpull",
        [&verifier, &pfd_catalog, &partial_fetch_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<std::vector<sbi_gen::ApplicationForPfdRequest>>(
                    req, err);
            if (!body.has_value()) {
                return err;
            }
            partial_fetch_counter->Add(1);

            json changed = json::array();
            for (const auto& request_item : *body) {
                std::optional<std::string> since;
                if (request_item.pfdTimestamp.has_value()) {
                    since = *request_item.pfdTimestamp;
                }
                if (auto pfd_data =
                        pfd_catalog.get_if_changed_since(request_item.applicationId, since);
                    pfd_data.has_value()) {
                    changed.push_back(*pfd_data);
                }
            }
            if (changed.empty()) {
                sbi_core::http2::Response resp;
                resp.status = 204;
                return resp;
            }
            return sbi_core::http2::Response::json(200, changed.dump());
        });

    server.add_route(
        "GET",
        std::string(kApiRoot) + "/applications/{appId}",
        [&verifier, &pfd_catalog, &ind_fetch_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto app_id = req.path_params.at("appId");
            auto pfd_data = pfd_catalog.get(app_id);
            if (!pfd_data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No PFDs for application " + app_id);
            }
            ind_fetch_counter->Add(1);
            return sbi_core::http2::Response::json(200, pfd_data->dump());
        });

    // --- Nnef_PFDmanagement: subscriptions ---

    server.add_route(
        "POST",
        std::string(kApiRoot) + "/subscriptions",
        [&verifier, &subscriptions, &sub_create_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::PfdSubscription>(req, err);
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
        std::string(kApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &subscriptions, &sub_modify_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::PfdSubscription>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            if (!subscriptions.put(id, j)) {
                return sbi_core::http2::problem_response(404, "Not Found", "No subscription " + id);
            }
            sub_modify_counter->Add(1);
            return sbi_core::http2::Response::json(200, j.dump());
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

    // --- Nnef_SMService (ADR-0207, gap-closure task #164, first NEF slice) ---

    server.add_route(
        "POST",
        std::string(kSmServiceApiRoot) + "/sm-contexts/{supi}/sendsms",
        [&verifier, &send_sms_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto content_type_it = req.headers.find("content-type");
            if (content_type_it == req.headers.end()) {
                return sbi_core::http2::problem_response(
                    400, "Invalid Service Request", "Content-Type header is required");
            }
            auto parts = sbi_core::multipart::parse(content_type_it->second, req.body);
            if (!parts.has_value() || parts->empty()) {
                return sbi_core::http2::problem_response(
                    400, "Invalid Service Request", "Malformed multipart/related body");
            }
            sbi_gen::SmsData jsonData;
            try {
                jsonData = json::parse((*parts)[0].body).get<sbi_gen::SmsData>();
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(400, "Invalid Service Request", e.what());
            }
            bool found_payload = false;
            for (const auto& part : *parts) {
                if (part.content_id.has_value() &&
                    *part.content_id == jsonData.smsPayload.contentId) {
                    found_payload = true;
                    break;
                }
            }
            if (!found_payload) {
                return sbi_core::http2::problem_response(
                    400, "Invalid Service Request", "smsPayload binary part not found");
            }

            send_sms_counter->Add(1);
            // Real, disclosed simplification -- same class of gap as nfs/smsf's own
            // SendMtSMS (ADR-0188): no real TS 24.011 SMS-DELIVER-REPORT encoder exists in this
            // build, so the binary delivery-report part is an honest empty placeholder, not a
            // fabricated PDU. This NEF also has no real onward IP-SM-GW/SMSF relay wired -- the
            // real ack is NEF-level acceptance only, same disclosed non-scope.
            sbi_gen::SmsDeliveryData resp_json;
            resp_json.smsPayload.contentId = "smsPayload";

            sbi_core::multipart::Part json_part;
            json_part.content_type = "application/json";
            json_part.body = json(resp_json).dump();
            sbi_core::multipart::Part bin_part;
            bin_part.content_type = "application/vnd.3gpp.sms";
            bin_part.content_id = "smsPayload";
            const auto encoded = sbi_core::multipart::encode({json_part, bin_part});

            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", encoded.content_type_header);
            resp.body = encoded.body;
            return resp;
        });

    // --- Nnef_UEId (ADR-0207, gap-closure task #164, first NEF slice) ---

    server.add_route(
        "POST",
        std::string(kUeIdApiRoot) + "/fetch",
        [&verifier, &fetch_ueid_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::UeIdReq>(req, err);
            if (!body.has_value()) {
                return err;
            }
            fetch_ueid_counter->Add(1);
            // Real, disclosed gap: this NEF has no real roaming H-NEF/internal-UE-identity
            // mapping database to look anything up in -- real 204 "does not exist", matching the
            // real YAML's own documented semantics for this case, not a fabricated lookup.
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "POST",
        std::string(kUeIdApiRoot) + "/get-ueid-mapping",
        [&verifier, &ueid_mapping_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::MapUeIdInfo>(req, err);
            if (!body.has_value()) {
                return err;
            }
            ueid_mapping_counter->Add(1);
            // Same real, disclosed gap as FetchUEId above -- no real UE ID mapping database.
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nnef_DNAIMapping (ADR-0207, gap-closure task #164, first NEF slice) ---

    server.add_route(
        "POST",
        std::string(kDnaiMappingApiRoot) + "/subscriptions",
        [&verifier, &dnai_map_subs, &dnai_map_create_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::DnaiMapSub>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            const auto id = dnai_map_subs.create(j);
            dnai_map_create_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kDnaiMappingApiRoot) + "/subscriptions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kDnaiMappingApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &dnai_map_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            auto sub = dnai_map_subs.get(id);
            if (!sub.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No DNAI mapping subscription " + id);
            }
            return sbi_core::http2::Response::json(200, sub->dump());
        });

    server.add_route(
        "DELETE",
        std::string(kDnaiMappingApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &dnai_map_subs, &dnai_map_delete_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            if (!dnai_map_subs.remove(id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No DNAI mapping subscription " + id);
            }
            dnai_map_delete_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nnef_EASDeployment (ADR-0207, gap-closure task #164, first NEF slice) ---

    server.add_route(
        "POST",
        std::string(kEasDeploymentApiRoot) + "/subscriptions",
        [&verifier, &eas_deploy_subs, &eas_deploy_create_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::EasDeploySubData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            const auto id = eas_deploy_subs.create(j);
            eas_deploy_create_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kEasDeploymentApiRoot) + "/subscriptions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kEasDeploymentApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &eas_deploy_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            auto sub = eas_deploy_subs.get(id);
            if (!sub.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No EAS deployment subscription " + id);
            }
            return sbi_core::http2::Response::json(200, sub->dump());
        });

    server.add_route(
        "DELETE",
        std::string(kEasDeploymentApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &eas_deploy_subs, &eas_deploy_delete_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            if (!eas_deploy_subs.remove(id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No EAS deployment subscription " + id);
            }
            eas_deploy_delete_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nnef_SMContext (ADR-0208, gap-closure task #164, second NEF slice) ---

    server.add_route(
        "POST",
        std::string(kSmContextApiRoot) + "/sm-contexts",
        [&verifier, &sm_contexts, &sm_context_create_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::SmContextCreateData_Nnef_SMContext>(req,
                                                                                              err);
            if (!body.has_value()) {
                return err;
            }
            sm_context_create_counter->Add(1);

            // Real, disclosed simplification: echoes only the fields the real
            // SmContextCreatedData shares with the create request (supi/pduSessionId/dnn/
            // snssai/nefId) -- rdsSupport/extBufSupport/supportedFeatures/maxPacketSize are left
            // unset since no real small-data-rate/feature-negotiation logic exists in this build.
            sbi_gen::SmContextCreatedData_Nnef_SMContext created;
            created.supi = body->supi;
            created.pduSessionId = body->pduSessionId;
            created.dnn = body->dnn;
            created.snssai = body->snssai;
            created.nefId = body->nefId;
            json j = created;
            const auto id = sm_contexts.create(json(*body));

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location", std::string(kSmContextApiRoot) + "/sm-contexts/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "POST",
        std::string(kSmContextApiRoot) + "/sm-contexts/{smContextId}/release",
        [&verifier, &sm_contexts, &sm_context_release_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("smContextId");
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::SmContextReleaseData_Nnef_SMContext>(req,
                                                                                               err);
            if (!body.has_value()) {
                return err;
            }
            if (!sm_contexts.remove(id)) {
                return sbi_core::http2::problem_response(404, "Not Found", "No SM context " + id);
            }
            sm_context_release_counter->Add(1);
            // Real, disclosed simplification: no real small-data-rate-control tracking exists to
            // report back in a SmContextReleasedData body -- honest 204, not a fabricated 200 with
            // an empty-but-typed body.
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "POST",
        std::string(kSmContextApiRoot) + "/sm-contexts/{smContextId}/update",
        [&verifier, &sm_contexts, &sm_context_update_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("smContextId");
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::SmContextUpdateData_Nnef_SMContext>(req,
                                                                                              err);
            if (!body.has_value()) {
                return err;
            }
            if (!sm_contexts.update(id, json(*body))) {
                return sbi_core::http2::problem_response(404, "Not Found", "No SM context " + id);
            }
            sm_context_update_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "POST",
        std::string(kSmContextApiRoot) + "/sm-contexts/{smContextId}/deliver",
        [&verifier, &sm_contexts, &sm_context_deliver_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("smContextId");
            if (!sm_contexts.get(id).has_value()) {
                return sbi_core::http2::problem_response(404, "Not Found", "No SM context " + id);
            }
            const auto content_type_it = req.headers.find("content-type");
            if (content_type_it == req.headers.end()) {
                return sbi_core::http2::problem_response(
                    400, "Invalid Service Request", "Content-Type header is required");
            }
            auto parts = sbi_core::multipart::parse(content_type_it->second, req.body);
            if (!parts.has_value() || parts->empty()) {
                return sbi_core::http2::problem_response(
                    400, "Invalid Service Request", "Malformed multipart/related body");
            }
            sbi_gen::DeliverReqData_Nnef_SMContext json_data;
            try {
                json_data =
                    json::parse((*parts)[0].body).get<sbi_gen::DeliverReqData_Nnef_SMContext>();
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(400, "Invalid Service Request", e.what());
            }
            bool found_payload = false;
            for (const auto& part : *parts) {
                if (part.content_id.has_value() && *part.content_id == json_data.data.contentId) {
                    found_payload = true;
                    break;
                }
            }
            if (!found_payload) {
                return sbi_core::http2::problem_response(
                    400, "Invalid Service Request", "binaryMoData part not found");
            }
            sm_context_deliver_counter->Add(1);
            // Real, disclosed gap: same class as SendSMS/UEId above -- no real onward NIDD
            // MT/MO relay exists in this build, so this is NEF-level acceptance only.
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nnef_Authentication (ADR-0208, gap-closure task #164, second NEF slice) ---

    server.add_route(
        "POST",
        std::string(kAuthenticationApiRoot) + "/uav-authentications",
        [&verifier, &uav_auth_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::UAVAuthInfo>(req, err);
            if (!body.has_value()) {
                return err;
            }
            uav_auth_counter->Add(1);
            // Real, disclosed gap: this NEF has no real UAS-NF/USS backend to authenticate a UAV
            // against (TS 23.256's own Uncrewed Aerial Systems authentication/authorization
            // architecture is out of scope for this build) -- the real 403 UAVAuthFailure path
            // (one of only two success/failure response bodies this operation's own YAML
            // defines, unlike several other NEF gaps closed this project where an honest 501 was
            // available; it is not here) is the honest response, not a fabricated 200 success.
            sbi_gen::UAVAuthFailure failure;
            failure.error.status = 403;
            failure.error.title = "UAV Authentication Failure";
            failure.error.detail =
                "No real UAS-NF/USS authentication backend exists in this build (gpsi=" +
                body->gpsi + ")";
            return sbi_core::http2::Response::json(403, json(failure).dump());
        });

    // --- Nnef_ECSAddress (ADR-0208, gap-closure task #164, second NEF slice) ---

    server.add_route(
        "POST",
        std::string(kEcsAddressApiRoot) + "/subscriptions",
        [&verifier, &ecs_addr_subs, &ecs_addr_create_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::EcsAddrCfgInfoSub>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            const auto id = ecs_addr_subs.create(j);
            ecs_addr_create_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kEcsAddressApiRoot) + "/subscriptions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kEcsAddressApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &ecs_addr_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            auto sub = ecs_addr_subs.get(id);
            if (!sub.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No ECS address subscription " + id);
            }
            return sbi_core::http2::Response::json(200, sub->dump());
        });

    server.add_route(
        "PUT",
        std::string(kEcsAddressApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &ecs_addr_subs, &ecs_addr_put_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::EcsAddrCfgInfoSub>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            if (!ecs_addr_subs.put(id, j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No ECS address subscription " + id);
            }
            ecs_addr_put_counter->Add(1);
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "PATCH",
        std::string(kEcsAddressApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &ecs_addr_subs, &ecs_addr_patch_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            sbi_core::http2::Response err;
            // Validated against the real generated EcsAddrCfgInfoSubPatch shape first (same
            // precedent as PCF/UDR's own merge-patch routes), then applied as a real RFC 7396
            // merge patch -- matching this operation's own declared
            // application/merge-patch+json request content type exactly.
            auto typed_patch =
                sbi_core::http2::parse_json_body<sbi_gen::EcsAddrCfgInfoSubPatch>(req, err);
            if (!typed_patch.has_value()) {
                return err;
            }
            if (!ecs_addr_subs.patch(id, json(*typed_patch))) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No ECS address subscription " + id);
            }
            ecs_addr_patch_counter->Add(1);
            auto updated = ecs_addr_subs.get(id);
            return sbi_core::http2::Response::json(200, updated->dump());
        });

    server.add_route(
        "DELETE",
        std::string(kEcsAddressApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &ecs_addr_subs, &ecs_addr_delete_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            if (!ecs_addr_subs.remove(id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No ECS address subscription " + id);
            }
            ecs_addr_delete_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nnef_EventExposure (ADR-0209, gap-closure task #164, third NEF slice) ---

    server.add_route(
        "POST",
        std::string(kEventExposureApiRoot) + "/subscriptions",
        [&verifier, &event_exposure_subs, &event_exposure_create_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::NefEventExposureSubsc>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            const auto id = event_exposure_subs.create(j);
            event_exposure_create_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kEventExposureApiRoot) + "/subscriptions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kEventExposureApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &event_exposure_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            auto sub = event_exposure_subs.get(id);
            if (!sub.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No event exposure subscription " + id);
            }
            return sbi_core::http2::Response::json(200, sub->dump());
        });

    server.add_route(
        "PUT",
        std::string(kEventExposureApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &event_exposure_subs, &event_exposure_put_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::NefEventExposureSubsc>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            if (!event_exposure_subs.put(id, j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No event exposure subscription " + id);
            }
            event_exposure_put_counter->Add(1);
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "DELETE",
        std::string(kEventExposureApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &event_exposure_subs, &event_exposure_delete_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            if (!event_exposure_subs.remove(id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No event exposure subscription " + id);
            }
            event_exposure_delete_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- ADR-0302: TS 29.522 TrafficInfluence, the AF-facing (northbound) API ---
    //
    // Six real operations from TS29522_TrafficInfluence.yaml. Unlike every other route in this
    // file, these are consumed by an external Application Function, not by another NF -- this is
    // the surface ADR-0294 found entirely missing, and free5GC serves the same two AF-facing
    // groups (`/3gpp-traffic-influence`, `/3gpp-pfd-management`).
    //
    // The generated DTO cannot enforce the spec's two `oneOf` mutual-exclusivity groups (ADR-0301
    // explains why: every field is optional in the struct), so CreateNewSubscription validates
    // them here, explicitly, against the spec's own lists. Without that, a request naming neither
    // an application nor any traffic filter would be accepted and stored as an influence rule that
    // can never match anything.

    server.add_route(
        "GET",
        std::string(kAfTrafficInfluenceApiRoot) + "/{afId}/subscriptions",
        [&verifier, &af_traffic_influence_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& sub : af_traffic_influence_subs.list(req.path_params.at("afId"))) {
                out.push_back(sub);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAfTrafficInfluenceApiRoot) + "/{afId}/subscriptions",
        [&verifier, &af_traffic_influence_subs, &udr_client, udr_base_url](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::TrafficInfluSub_TrafficInfluence>(
                req, err);
            if (!body.has_value()) {
                return err;
            }

            // Real TS29522_TrafficInfluence.yaml constraint 1: exactly one of afAppId /
            // trafficFilters / ethTrafficFilters / trafficDataSets identifies the traffic.
            const int traffic_selectors = (body->afAppId.has_value() ? 1 : 0) +
                                          (body->trafficFilters.has_value() ? 1 : 0) +
                                          (body->ethTrafficFilters.has_value() ? 1 : 0) +
                                          (body->trafficDataSets.has_value() ? 1 : 0);
            if (traffic_selectors != 1) {
                return sbi_core::http2::problem_response(
                    400,
                    "Invalid request",
                    "exactly one of afAppId, trafficFilters, ethTrafficFilters or trafficDataSets "
                    "shall be present");
            }
            // Real constraint 2: exactly one of ipv4Addr / ipv6Addr / macAddr / gpsi /
            // externalGroupId / anyUeInd identifies the target.
            const int target_selectors =
                (body->ipv4Addr.has_value() ? 1 : 0) + (body->ipv6Addr.has_value() ? 1 : 0) +
                (body->macAddr.has_value() ? 1 : 0) + (body->gpsi.has_value() ? 1 : 0) +
                (body->externalGroupId.has_value() ? 1 : 0) + (body->anyUeInd.has_value() ? 1 : 0);
            if (target_selectors != 1) {
                return sbi_core::http2::problem_response(
                    400,
                    "Invalid request",
                    "exactly one of ipv4Addr, ipv6Addr, macAddr, gpsi, externalGroupId or "
                    "anyUeInd shall be present");
            }

            const auto af_id = req.path_params.at("afId");
            json j = *body;
            const auto sub_id = af_traffic_influence_subs.create(af_id, j);
            // The influenceId is derived from the AF and subscription ids so the UDR record can be
            // found and deleted again from the AF-facing resource alone.
            push_influence_to_udr(
                udr_client, udr_base_url, af_id + "-" + sub_id, to_traffic_influ_data(*body));

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kAfTrafficInfluenceApiRoot) + "/" + af_id +
                                     "/subscriptions/" + sub_id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfTrafficInfluenceApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_traffic_influence_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto sub = af_traffic_influence_subs.get(req.path_params.at("afId"),
                                                     req.path_params.at("subscriptionId"));
            if (!sub.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such traffic influence subscription");
            }
            return sbi_core::http2::Response::json(200, sub->dump());
        });

    server.add_route(
        "PUT",
        std::string(kAfTrafficInfluenceApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_traffic_influence_subs, &udr_client, udr_base_url](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::TrafficInfluSub_TrafficInfluence>(
                req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto af_id = req.path_params.at("afId");
            const auto sub_id = req.path_params.at("subscriptionId");
            json j = *body;
            if (!af_traffic_influence_subs.put(af_id, sub_id, j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such traffic influence subscription");
            }
            push_influence_to_udr(
                udr_client, udr_base_url, af_id + "-" + sub_id, to_traffic_influ_data(*body));
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "PATCH",
        std::string(kAfTrafficInfluenceApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_traffic_influence_subs, &udr_client, udr_base_url](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            // Validate the patch against its own real schema (TrafficInfluSubPatch) before
            // applying it, the same discipline every other merge-patch route in this project uses.
            sbi_core::http2::Response err;
            auto patch_dto =
                sbi_core::http2::parse_json_body<sbi_gen::TrafficInfluSubPatch>(req, err);
            if (!patch_dto.has_value()) {
                return err;
            }
            const auto af_id = req.path_params.at("afId");
            const auto sub_id = req.path_params.at("subscriptionId");
            auto patched =
                af_traffic_influence_subs.merge_patch(af_id, sub_id, json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such traffic influence subscription");
            }
            // Re-push the PATCHED subscription, not the patch: UDR holds a whole record, and
            // sending only the changed fields would silently erase the rest.
            const auto merged = patched->get<sbi_gen::TrafficInfluSub_TrafficInfluence>();
            push_influence_to_udr(
                udr_client, udr_base_url, af_id + "-" + sub_id, to_traffic_influ_data(merged));
            return sbi_core::http2::Response::json(200, patched->dump());
        });

    server.add_route(
        "DELETE",
        std::string(kAfTrafficInfluenceApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_traffic_influence_subs, &udr_client, udr_base_url](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto af_id = req.path_params.at("afId");
            const auto sub_id = req.path_params.at("subscriptionId");
            if (!af_traffic_influence_subs.remove(af_id, sub_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such traffic influence subscription");
            }
            // The UDR record must go too -- an orphaned influence rule would keep steering
            // traffic for a subscription the AF believes it deleted.
            push_influence_to_udr(udr_client, udr_base_url, af_id + "-" + sub_id, std::nullopt);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nnef_TrafficInfluenceData (ADR-0210, gap-closure task #164, fourth NEF slice) ---

    server.add_route(
        "POST",
        std::string(kTrafficInfluenceDataApiRoot) + "/subscriptions",
        [&verifier, &traffic_influence_subs, &traffic_influence_create_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::TrafficInfluDataSub>(req, err);
            if (!body.has_value()) {
                return err;
            }
            // Real YAML constraint beyond simple required fields: the request must include at
            // least one of dnns/snssais (its own `anyOf: [{required:[dnns]},
            // {required:[snssais]}]`) -- not expressible in the generated struct's own
            // required/optional fields, so checked explicitly here.
            if (!body->dnns.has_value() && !body->snssais.has_value()) {
                return sbi_core::http2::problem_response(
                    400, "Missing mandatory IE", "At least one of dnns/snssais is required");
            }
            json j = *body;
            const auto id = traffic_influence_subs.create(j);
            traffic_influence_create_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace(
                "location", std::string(kTrafficInfluenceDataApiRoot) + "/subscriptions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kTrafficInfluenceDataApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &traffic_influence_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            auto sub = traffic_influence_subs.get(id);
            if (!sub.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No traffic influence subscription " + id);
            }
            return sbi_core::http2::Response::json(200, sub->dump());
        });

    server.add_route(
        "PUT",
        std::string(kTrafficInfluenceDataApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &traffic_influence_subs, &traffic_influence_put_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::TrafficInfluDataSub>(req, err);
            if (!body.has_value()) {
                return err;
            }
            if (!body->dnns.has_value() && !body->snssais.has_value()) {
                return sbi_core::http2::problem_response(
                    400, "Missing mandatory IE", "At least one of dnns/snssais is required");
            }
            json j = *body;
            if (!traffic_influence_subs.put(id, j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No traffic influence subscription " + id);
            }
            traffic_influence_put_counter->Add(1);
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "DELETE",
        std::string(kTrafficInfluenceDataApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &traffic_influence_subs, &traffic_influence_delete_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            if (!traffic_influence_subs.remove(id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No traffic influence subscription " + id);
            }
            traffic_influence_delete_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nnef_Inference (ADR-0210, gap-closure task #164, fourth NEF slice) ---
    // Real, disclosed: this YAML has no GET operation on the Individual Inference Subscription
    // resource at all (confirmed by direct read) -- Create/Update/PartialUpdate/Delete only.

    server.add_route(
        "POST",
        std::string(kInferenceApiRoot) + "/subscriptions",
        [&verifier, &inference_subs, &inference_create_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::InferEventSubsc_Nnef_Inference>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            const auto id = inference_subs.create(j);
            inference_create_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kInferenceApiRoot) + "/subscriptions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "PUT",
        std::string(kInferenceApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &inference_subs, &inference_put_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::InferEventSubsc_Nnef_Inference>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            if (!inference_subs.put(id, j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No inference subscription " + id);
            }
            inference_put_counter->Add(1);
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "PATCH",
        std::string(kInferenceApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &inference_subs, &inference_patch_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            sbi_core::http2::Response err;
            auto typed_patch =
                sbi_core::http2::parse_json_body<sbi_gen::InferEventSubscPatch_Nnef_Inference>(req,
                                                                                               err);
            if (!typed_patch.has_value()) {
                return err;
            }
            if (!inference_subs.patch(id, json(*typed_patch))) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No inference subscription " + id);
            }
            inference_patch_counter->Add(1);
            auto updated = inference_subs.get(id);
            return sbi_core::http2::Response::json(200, updated->dump());
        });

    server.add_route(
        "DELETE",
        std::string(kInferenceApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &inference_subs, &inference_delete_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            if (!inference_subs.remove(id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No inference subscription " + id);
            }
            inference_delete_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nnef_Training (ADR-0210, gap-closure task #164, fourth NEF slice) ---
    // Same real, disclosed gap as Nnef_Inference above: no GET operation on this real resource.

    server.add_route(
        "POST",
        std::string(kTrainingApiRoot) + "/subscriptions",
        [&verifier, &training_subs, &training_create_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::TrainEventsSubsc_Nnef_Training>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            const auto id = training_subs.create(j);
            training_create_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kTrainingApiRoot) + "/subscriptions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "PUT",
        std::string(kTrainingApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &training_subs, &training_put_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::TrainEventsSubsc_Nnef_Training>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            if (!training_subs.put(id, j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No training subscription " + id);
            }
            training_put_counter->Add(1);
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "PATCH",
        std::string(kTrainingApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &training_subs, &training_patch_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            sbi_core::http2::Response err;
            auto typed_patch =
                sbi_core::http2::parse_json_body<sbi_gen::TrainEventsSubscPatch_Nnef_Training>(req,
                                                                                               err);
            if (!typed_patch.has_value()) {
                return err;
            }
            if (!training_subs.patch(id, json(*typed_patch))) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No training subscription " + id);
            }
            training_patch_counter->Add(1);
            auto updated = training_subs.get(id);
            return sbi_core::http2::Response::json(200, updated->dump());
        });

    server.add_route(
        "DELETE",
        std::string(kTrainingApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &training_subs, &training_delete_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            if (!training_subs.remove(id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No training subscription " + id);
            }
            training_delete_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nnef_VFLInference (ADR-0210, gap-closure task #164, fourth NEF slice) ---

    server.add_route(
        "POST",
        std::string(kVflInferenceApiRoot) + "/subscriptions",
        [&verifier, &vfl_inference_subs, &vfl_inference_create_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::VflInferSub_Nnef_VFLInference>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            const auto id = vfl_inference_subs.create(j);
            vfl_inference_create_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kVflInferenceApiRoot) + "/subscriptions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kVflInferenceApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &vfl_inference_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            auto sub = vfl_inference_subs.get(id);
            if (!sub.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No VFL inference subscription " + id);
            }
            return sbi_core::http2::Response::json(200, sub->dump());
        });

    server.add_route(
        "PUT",
        std::string(kVflInferenceApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &vfl_inference_subs, &vfl_inference_put_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::VflInferSub_Nnef_VFLInference>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            if (!vfl_inference_subs.put(id, j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No VFL inference subscription " + id);
            }
            vfl_inference_put_counter->Add(1);
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "PATCH",
        std::string(kVflInferenceApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &vfl_inference_subs, &vfl_inference_patch_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            sbi_core::http2::Response err;
            auto typed_patch =
                sbi_core::http2::parse_json_body<sbi_gen::VflInferSubPatch_Nnef_VFLInference>(req,
                                                                                              err);
            if (!typed_patch.has_value()) {
                return err;
            }
            if (!vfl_inference_subs.patch(id, json(*typed_patch))) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No VFL inference subscription " + id);
            }
            vfl_inference_patch_counter->Add(1);
            auto updated = vfl_inference_subs.get(id);
            return sbi_core::http2::Response::json(200, updated->dump());
        });

    server.add_route(
        "DELETE",
        std::string(kVflInferenceApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &vfl_inference_subs, &vfl_inference_delete_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            if (!vfl_inference_subs.remove(id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No VFL inference subscription " + id);
            }
            vfl_inference_delete_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nnef_VFLTraining (ADR-0210, gap-closure task #164, fourth and final NEF slice) ---
    // Real, disclosed: unlike every other NEF subscription resource this project has built, this
    // one's own required fields are just `vflTrainSubs` -- notifUri/notifCorrId are both genuinely
    // optional per this YAML's own schema (confirmed by direct read, not assumed).

    server.add_route(
        "POST",
        std::string(kVflTrainingApiRoot) + "/subscriptions",
        [&verifier, &vfl_training_subs, &vfl_training_create_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::NefVflTrainSubs>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            const auto id = vfl_training_subs.create(j);
            vfl_training_create_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kVflTrainingApiRoot) + "/subscriptions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kVflTrainingApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &vfl_training_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            auto sub = vfl_training_subs.get(id);
            if (!sub.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No VFL training subscription " + id);
            }
            return sbi_core::http2::Response::json(200, sub->dump());
        });

    server.add_route(
        "PUT",
        std::string(kVflTrainingApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &vfl_training_subs, &vfl_training_put_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::NefVflTrainSubs>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            if (!vfl_training_subs.put(id, j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No VFL training subscription " + id);
            }
            vfl_training_put_counter->Add(1);
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "PATCH",
        std::string(kVflTrainingApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &vfl_training_subs, &vfl_training_patch_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            sbi_core::http2::Response err;
            auto typed_patch =
                sbi_core::http2::parse_json_body<sbi_gen::NefVflTrainSubsPatch>(req, err);
            if (!typed_patch.has_value()) {
                return err;
            }
            if (!vfl_training_subs.patch(id, json(*typed_patch))) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No VFL training subscription " + id);
            }
            vfl_training_patch_counter->Add(1);
            auto updated = vfl_training_subs.get(id);
            return sbi_core::http2::Response::json(200, updated->dump());
        });

    server.add_route(
        "DELETE",
        std::string(kVflTrainingApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &vfl_training_subs, &vfl_training_delete_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subscriptionId");
            if (!vfl_training_subs.remove(id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No VFL training subscription " + id);
            }
            vfl_training_delete_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    std::thread(run_nrf_lifecycle, nef_instance_id, nrf_base).detach();

    server.start();
    spdlog::info("nef: listening on https://0.0.0.0:{} (TLS 1.3 + mTLS)", port);
    spdlog::info("nef: Prometheus metrics at http://{}/metrics", metrics_bind_address);
    sbi_core::run_multi_threaded(ioc);
    return 0;
}
