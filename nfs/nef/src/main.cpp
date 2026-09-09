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
#include "TS29122_AsSessionWithQoS.hpp"          // ADR-0315
#include "TS29122_ChargeableParty.hpp"           // ADR-0326
#include "TS29122_DeviceTriggering.hpp"          // ADR-0324
#include "TS29122_MonitoringEvent.hpp"           // ADR-0316
#include "TS29122_NpConfiguration.hpp"           // ADR-0324
#include "TS29122_RacsParameterProvisioning.hpp" // ADR-0324
#include "TS29122_ReportingNetworkStatus.hpp"    // ADR-0324
#include "TS29256_Nnef_Authentication.hpp"
#include "TS29522_5GLANParameterProvision.hpp"  // ADR-0323
#include "TS29522_ACSParameterProvision.hpp"    // ADR-0322
#include "TS29522_ASTI.hpp"                     // ADR-0324
#include "TS29522_AddressingParamProvision.hpp" // ADR-0323
#include "TS29522_AnalyticsExposure.hpp"        // ADR-0324
#include "TS29522_ApplyingBdtPolicy.hpp"        // ADR-0326
#include "TS29522_CagInfoParamProvision.hpp"    // ADR-0323
#include "TS29522_DNAIMapping.hpp"
#include "TS29522_EcsAddressProvision.hpp"          // ADR-0326
#include "TS29522_GroupParametersProvisioning.hpp"  // ADR-0323
#include "TS29522_IPTVConfiguration.hpp"            // ADR-0322
#include "TS29522_ImsEventExposure.hpp"             // ADR-0324
#include "TS29522_ImsParamProvision.hpp"            // ADR-0326
#include "TS29522_ImsSessionManagement.hpp"         // ADR-0326
#include "TS29522_LpiParameterProvision.hpp"        // ADR-0322
#include "TS29522_MBSGroupMsgDelivery.hpp"          // ADR-0326
#include "TS29522_MemberUESelectionAssistance.hpp"  // ADR-0326
#include "TS29522_PDTQPolicyNegotiation.hpp"        // ADR-0326
#include "TS29522_RSLPPIParametersProvisioning.hpp" // ADR-0326
#include "TS29522_SliceParamProvision.hpp"          // ADR-0323
#include "TS29522_UEId.hpp"                         // ADR-0319
#include "TS29522_VFLInference.hpp"                 // ADR-0326
#include "TS29522_VFLTraining.hpp"                  // ADR-0326
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
// ADR-0315: the AF-facing QoS request API (TS 29.122), second of the 57 AF-facing services.
constexpr const char* kAfAsSessionWithQosApiRoot = "/3gpp-as-session-with-qos/v1";
// ADR-0316: the AF-facing event-monitoring API (TS 29.122), third of the 57.
constexpr const char* kAfMonitoringEventApiRoot = "/3gpp-monitoring-event/v1";
// ADR-0319: the AF-facing UE identifier API (TS 29.522), fourth of the 57.
constexpr const char* kAfUeIdApiRoot = "/3gpp-ueid/v1";
// ADR-0321: the AF-facing service-parameter provisioning API (TS 29.522), fifth of the 57.
constexpr const char* kAfServiceParamApiRoot = "/3gpp-service-parameter/v1";
// ADR-0322: three more AF-facing provisioning services (TS 29.522), sixth through eighth of 57.
constexpr const char* kAfIptvApiRoot = "/3gpp-iptvconfiguration/v1";
constexpr const char* kAfLpiApiRoot = "/3gpp-lpi-pp/v1";
constexpr const char* kAfAcsApiRoot = "/3gpp-acs-pp/v1";
// ADR-0323: the four `/pp`-shaped parameter-provisioning services (TS 29.522), ninth through
// twelfth of the 57.
// ADR-0323: the `/pp` services have no {afId} in their paths, so their documents share one
// namespace rather than being invented into per-AF buckets the spec does not define.
constexpr const char* kPpNamespace = "pp";
constexpr const char* kAfCagPpApiRoot = "/3gpp-caginfo-pp/v1";
constexpr const char* kAfAddrPpApiRoot = "/3gpp-addr-pp/v1";
constexpr const char* kAfSlicePpApiRoot = "/3gpp-slice-pp/v1";
constexpr const char* kAfGroupPpApiRoot = "/3gpp-grp-pp/v1";
// ADR-0324: eight more AF-facing services (TS 29.122 and TS 29.522), thirteenth
// through twentieth of the 57.
constexpr const char* kAfDeviceTriggerApiRoot = "/3gpp-device-triggering/v1";
constexpr const char* kAfNpConfigApiRoot = "/3gpp-network-parameter-configuration/v1";
constexpr const char* kAfRacsApiRoot = "/3gpp-racs-pp/v1";
constexpr const char* kAfNetStatusApiRoot = "/3gpp-net-stat-report/v1";
constexpr const char* kAfBdtApiRoot = "/3gpp-bdt/v1";
constexpr const char* kAfAnalyticsApiRoot = "/3gpp-analyticsexposure/v1";
constexpr const char* kAfAstiApiRoot = "/3gpp-asti/v1";
// ADR-0326: fifteen more AF services. Every root below is the YAML's servers[0].url,
// checked by the api_root_conformance ctest added in ADR-0325.
constexpr const char* kAfChargeablePartyApiRoot =
    "/3gpp-chargeable-party/v1";                               // TS29122_ChargeableParty
constexpr const char* kAf5glanPpApiRoot = "/3gpp-5glan-pp/v1"; // TS29522_5GLANParameterProvision
constexpr const char* kAfAmInfluenceApiRoot = "/3gpp-am-influence/v1"; // TS29522_AMInfluence
constexpr const char* kAfApplyBdtApiRoot =
    "/3gpp-applying-bdt-policy/v1";                                    // TS29522_ApplyingBdtPolicy
constexpr const char* kAfDnaiMappingApiRoot = "/3gpp-dnai-mapping/v1"; // TS29522_DNAIMapping
constexpr const char* kAfEcsAddrProvApiRoot =
    "/3gpp-ecs-address-provision/v1";                      // TS29522_EcsAddressProvision
constexpr const char* kAfImsPpApiRoot = "/3gpp-ims-pp/v1"; // TS29522_ImsParamProvision
constexpr const char* kAfImsSmApiRoot = "/3gpp-ims-sm/v1"; // TS29522_ImsSessionManagement
constexpr const char* kAfMbsGroupMsgApiRoot =
    "/3gpp-mbs-group-msg/v1"; // TS29522_MBSGroupMsgDelivery
constexpr const char* kAfMsEventApiRoot = "/3gpp-ms-event-exposure/v1"; // TS29522_MSEventExposure
constexpr const char* kAfMusaApiRoot = "/3gpp-musa/v1"; // TS29522_MemberUESelectionAssistance
constexpr const char* kAfPdtqApiRoot =
    "/3gpp-pdtq-policy-negotiation/v1"; // TS29522_PDTQPolicyNegotiation
constexpr const char* kAfRslppiPpApiRoot =
    "/3gpp-rslppi-pp/v1"; // TS29522_RSLPPIParametersProvisioning
constexpr const char* kAfVflInferApiRoot = "/3gpp-vfl-inference/v1"; // TS29522_VFLInference
constexpr const char* kAfVflTrainApiRoot = "/3gpp-vfl-training/v1";  // TS29522_VFLTraining
constexpr const char* kAfImsEventApiRoot = "/3gpp-ims-ee/v1";
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

// ADR-0315: an AF's QoS request becomes a real PCF application session.
//
// This is the AsSessionWithQoS analogue of ADR-0302's traffic-influence brokering, and the same
// principle decides it: a NEF that records an AF's QoS request and tells nobody has authorised
// nothing. PCF is what actually installs QoS, via Npcf_PolicyAuthorization app-sessions -- an
// endpoint this project already implements.
//
// Real, disclosed mapping. Only fields present in BOTH schemas are carried:
//   dnn        -> AppSessionContextReqData.dnn
//   notifUri   -> REQUIRED by the PCF schema; sourced from this NEF's own callback URI, because
//                 the AF's own notificationDestination is where NEF sends events, not where PCF
//                 should send them. Pointing PCF at the AF directly would bypass the exposure
//                 function entirely, which is the one thing NEF exists to prevent.
//   suppFeat   -> REQUIRED; carried from the AF's supportedFeatures when present, else "0",
//                 which is the real "no optional features" value rather than an invented one.
//
// NOT mapped, each for a stated reason rather than an oversight:
//   * `flowInfo`/`ethFlowInfo` describe the AF's traffic filters and PCF expects them inside
//     `medComponents` -> `medSubComps`, a nested structure whose construction has real semantics
//     (media type, flow direction) this slice does not attempt. Sending a session with no media
//     component is honest: PCF authorises the session, not specific flows.
//   * `qosReference` names an operator-configured QoS profile. PCF carries it per media
//     component, so it goes where medComponents would -- deferred with them, together, rather
//     than half-wired.
//   * `gpsi`/`extGroupId` identify the target; `AppSessionContextReqData` identifies by UE
//     address, and translating one to the other needs a UDM lookup this route does not perform.
std::optional<std::string>
create_pcf_app_session(sbi_core::http2::Client& pcf_client,
                       const std::string& pcf_base_url,
                       const std::string& nef_callback_uri,
                       const sbi_gen::AsSessionWithQoSSubscription& sub) {
    sbi_gen::AppSessionContextReqData req{};
    req.notifUri = nef_callback_uri;
    req.suppFeat = sub.supportedFeatures.value_or("0");
    req.dnn = sub.dnn;

    sbi_gen::AppSessionContext context{};
    context.ascReqData = req;

    sbi_core::http2::ClientRequest http_req;
    http_req.method = "POST";
    http_req.url = pcf_base_url + "/npcf-policyauthorization/v1/app-sessions";
    http_req.headers.emplace("content-type", "application/json");
    http_req.body = nlohmann::json(context).dump();
    auto resp = pcf_client.send(http_req);
    if (!resp.has_value() || resp->status >= 300) {
        spdlog::warn("nef: PCF app-session creation failed (status={}) -- the AF's QoS request is "
                     "recorded at NEF but NOTHING has authorised it",
                     resp.has_value() ? resp->status : 0);
        return std::nullopt;
    }
    // The app-session id comes back in Location; it is what DELETE needs later.
    if (const auto location = resp->headers.find("location"); location != resp->headers.end()) {
        const auto slash = location->second.rfind('/');
        if (slash != std::string::npos) {
            const auto id = location->second.substr(slash + 1);
            spdlog::info("nef: PCF app-session {} created for an AF QoS request", id);
            return id;
        }
    }
    spdlog::warn("nef: PCF accepted the app-session but returned no Location -- it cannot be "
                 "deleted later, so it is treated as not created");
    return std::nullopt;
}

void delete_pcf_app_session(sbi_core::http2::Client& pcf_client,
                            const std::string& pcf_base_url,
                            const std::string& app_session_id) {
    sbi_core::http2::ClientRequest req;
    req.method = "POST"; // real Npcf_PolicyAuthorization: delete is a POST to .../delete
    req.url =
        pcf_base_url + "/npcf-policyauthorization/v1/app-sessions/" + app_session_id + "/delete";
    req.headers.emplace("content-type", "application/json");
    req.body = "{}";
    auto resp = pcf_client.send(req);
    if (!resp.has_value() || resp->status >= 300) {
        spdlog::warn("nef: PCF app-session {} could not be deleted (status={}) -- the QoS "
                     "authorisation may outlive the AF subscription that created it",
                     app_session_id,
                     resp.has_value() ? resp->status : 0);
    }
}

// ADR-0316: the AF's MonitoringType -> the Nudm_EE EventType UDM actually understands.
//
// These are two DIFFERENT enums for overlapping concepts, and the differences are the whole
// reason this needs a function rather than a cast:
//
//   * `CHANGE_OF_IMSI_IMEI_ASSOCIATION` (TS 29.122, EPC naming) is
//     `CHANGE_OF_SUPI_PEI_ASSOCIATION` in Nudm_EE (5GC naming). Same event, different era's
//     vocabulary.
//   * `UE_REACHABILITY` is ONE value for the AF but splits into `UE_REACHABILITY_FOR_DATA` and
//     `UE_REACHABILITY_FOR_SMS` in Nudm_EE. The AF schema carries the discriminator itself --
//     `reachabilityType` (SMS|DATA) -- so this is resolved from the request rather than guessed.
//     An AF that asks for reachability without saying which gets DATA, which is what the field's
//     own absence conventionally means for a data-oriented exposure API; that default is stated
//     here rather than buried.
//   * Everything else is name-identical and mapped 1:1.
//
// A MonitoringType with no Nudm_EE counterpart returns nullopt and the subscription is REJECTED
// rather than silently created with a wrong event type -- a monitoring subscription that reports
// the wrong event is worse than one that was refused.
std::optional<std::string>
monitoring_type_to_ee_event(const sbi_gen::MonitoringEventSubscription& sub) {
    const auto& type = sub.monitoringType.value;
    if (type == sbi_gen::MonitoringType::UE_REACHABILITY) {
        const bool sms = sub.reachabilityType.has_value() &&
                         sub.reachabilityType->value == sbi_gen::ReachabilityType::SMS;
        return sms ? sbi_gen::EventType_Nudm_EE::UE_REACHABILITY_FOR_SMS
                   : sbi_gen::EventType_Nudm_EE::UE_REACHABILITY_FOR_DATA;
    }
    if (type == sbi_gen::MonitoringType::CHANGE_OF_IMSI_IMEI_ASSOCIATION) {
        return sbi_gen::EventType_Nudm_EE::CHANGE_OF_SUPI_PEI_ASSOCIATION;
    }
    if (type == sbi_gen::MonitoringType::LOSS_OF_CONNECTIVITY ||
        type == sbi_gen::MonitoringType::LOCATION_REPORTING ||
        type == sbi_gen::MonitoringType::ROAMING_STATUS ||
        type == sbi_gen::MonitoringType::COMMUNICATION_FAILURE ||
        type == sbi_gen::MonitoringType::AVAILABILITY_AFTER_DDN_FAILURE) {
        return type; // name-identical in both enums
    }
    return std::nullopt;
}

// ADR-0316: which UE this subscription monitors, in the form UDM's Nudm_EE path expects.
//
// The AF identifies a UE by EXTERNAL identifier (`externalId`, an NAI) or `msisdn`; Nudm_EE's
// `{ueIdentity}` takes a GPSI. `externalId` IS a GPSI in its `extid-` form and an MSISDN is a GPSI
// in its `msisdn-` form, so this is a real formatting rule rather than an invented mapping.
//
// A group subscription (`externalGroupId`) is NOT translated: Nudm_EE subscribes per UE, and
// expanding a group needs a membership lookup this route does not perform. Such a request is
// stored at NEF and reported as not brokered, rather than silently monitoring nobody.
std::optional<std::string> af_target_to_gpsi(const sbi_gen::MonitoringEventSubscription& sub) {
    if (sub.externalId.has_value() && !sub.externalId->empty()) {
        return "extid-" + *sub.externalId;
    }
    if (sub.msisdn.has_value() && !sub.msisdn->empty()) {
        return "msisdn-" + *sub.msisdn;
    }
    return std::nullopt;
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
    // ADR-0315: NEF's second real downstream NF -- PCF, for AF QoS authorisation.
    const auto pcf_base_url =
        nf_config::require<std::string>(config, "pcf_base_url", "NEF_PCF_BASE_URL");
    // ADR-0315: this NEF's own externally-reachable base, so PCF is told to notify NEF rather than
    // the AF directly. Present in config/nef.json since the file was written; read here for the
    // first time, because nothing before now needed to tell another NF where to find this one.
    const auto self_base_url =
        nf_config::require<std::string>(config, "self_base_url", "NEF_SELF_BASE_URL");
    // ADR-0316: UDM, for Nudm_EE event-monitoring subscriptions.
    const auto udm_base_url =
        nf_config::require<std::string>(config, "udm_base_url", "NEF_UDM_BASE_URL");

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
    nef::AfQosSubStore af_qos_subs;
    nef::AfMonitoringSubStore af_monitoring_subs;
    nef::AfUeIdMappingStore af_ueid_mappings;
    nef::AfServiceParamSubStore af_service_param_subs;
    nef::AfDocumentStore af_iptv_configs;
    nef::AfDocumentStore af_lpi_provisionings;
    nef::AfDocumentStore af_acs_subscriptions;
    nef::AfDocumentStore af_cag_pp;
    nef::AfDocumentStore af_addr_pp;
    nef::AfDocumentStore af_slice_pp;
    nef::AfDocumentStore af_group_pp;
    nef::AfDocumentStore af_device_trigger;
    nef::AfDocumentStore af_np_config;
    nef::AfDocumentStore af_racs;
    nef::AfDocumentStore af_net_status;
    nef::AfDocumentStore af_bdt;
    nef::AfDocumentStore af_analytics;
    nef::AfDocumentStore af_asti;
    nef::AfDocumentStore af_ims_event;
    nef::AfDocumentStore af_chargeable_party;
    nef::AfDocumentStore af_5glan_pp;
    nef::AfDocumentStore af_am_influence;
    nef::AfDocumentStore af_apply_bdt;
    nef::AfDocumentStore af_dnai_mapping;
    nef::AfDocumentStore af_ecs_addr_prov;
    nef::AfDocumentStore af_ims_pp;
    nef::AfDocumentStore af_ims_sm;
    nef::AfDocumentStore af_mbs_group_msg;
    nef::AfDocumentStore af_ms_event;
    nef::AfDocumentStore af_musa;
    nef::AfDocumentStore af_pdtq;
    nef::AfDocumentStore af_rslppi_pp;
    nef::AfDocumentStore af_vfl_infer;
    nef::AfDocumentStore af_vfl_train;
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
    sbi_core::http2::TlsConfig pcf_client_tls{
        .cert_path = CERTS_DIR "/nef/cert.pem",
        .key_path = CERTS_DIR "/nef/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client pcf_client(std::move(pcf_client_tls));
    // ADR-0317: notifications go OUT to Application Functions, which are not NFs in this
    // project's PKI. Its own client, so an AF's TLS behaviour cannot disturb the NF-facing ones.
    sbi_core::http2::TlsConfig af_client_tls{
        .cert_path = CERTS_DIR "/nef/cert.pem",
        .key_path = CERTS_DIR "/nef/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client af_client(std::move(af_client_tls));

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
            auto body = sbi_core::http2::parse_json_body<sbi_gen::UeIdReq_Nnef_UEId>(req, err);
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

    // --- ADR-0324: eight more AF-facing services (TS 29.122 + TS 29.522) ---
    //
    // DeviceTriggering, NpConfiguration, RacsParameterProvisioning, ReportingNetworkStatus,
    // ResourceManagementOfBdt, AnalyticsExposure, ASTI and ImsEventExposure -- 46 operations.
    //
    // The method set per service is taken from each YAML rather than assumed uniform: two of the
    // eight (AnalyticsExposure, ASTI) define NO PATCH operation, so none is routed for them. A
    // sixth route added "for consistency" would answer a method the spec does not define.
    //
    // Downstream: NONE of these eight. Each belongs somewhere real -- AnalyticsExposure to NWDAF
    // (not built, Phase 5), ResourceManagementOfBdt to PCF's BDT policy, ReportingNetworkStatus to
    // NWDAF congestion analytics, DeviceTriggering to an SMS/T8 delivery path this project does
    // not have -- and wiring each is per-service work against an NF that in several cases does not
    // exist here yet. They are accepted, validated against their real generated DTOs, and stored.

    server.add_route(
        "GET",
        std::string(kAfDeviceTriggerApiRoot) + "/{scsAsId}/transactions",
        [&verifier, &af_device_trigger](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& d : af_device_trigger.list(req.path_params.at("scsAsId"))) {
                out.push_back(d);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAfDeviceTriggerApiRoot) + "/{scsAsId}/transactions",
        [&verifier, &af_device_trigger](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::DeviceTriggering>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto af_id = req.path_params.at("scsAsId");
            json j = *body;
            const auto id = af_device_trigger.create(af_id, j);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kAfDeviceTriggerApiRoot) + "/" + af_id +
                                     "/transactions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfDeviceTriggerApiRoot) + "/{scsAsId}/transactions/{transactionId}",
        [&verifier, &af_device_trigger](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto d = af_device_trigger.get(req.path_params.at("scsAsId"),
                                           req.path_params.at("transactionId"));
            if (!d.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such device triggering transaction");
            }
            return sbi_core::http2::Response::json(200, d->dump());
        });

    server.add_route(
        "PUT",
        std::string(kAfDeviceTriggerApiRoot) + "/{scsAsId}/transactions/{transactionId}",
        [&verifier, &af_device_trigger](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::DeviceTriggering>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            if (!af_device_trigger.put(
                    req.path_params.at("scsAsId"), req.path_params.at("transactionId"), j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such device triggering transaction");
            }
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "PATCH",
        std::string(kAfDeviceTriggerApiRoot) + "/{scsAsId}/transactions/{transactionId}",
        [&verifier, &af_device_trigger](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            if (!sbi_core::http2::parse_json_body<sbi_gen::DeviceTriggeringPatch>(req, err)
                     .has_value()) {
                return err;
            }
            auto patched = af_device_trigger.merge_patch(req.path_params.at("scsAsId"),
                                                         req.path_params.at("transactionId"),
                                                         json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such device triggering transaction");
            }
            return sbi_core::http2::Response::json(200, patched->dump());
        });

    server.add_route(
        "DELETE",
        std::string(kAfDeviceTriggerApiRoot) + "/{scsAsId}/transactions/{transactionId}",
        [&verifier, &af_device_trigger](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            if (!af_device_trigger.remove(req.path_params.at("scsAsId"),
                                          req.path_params.at("transactionId"))) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such device triggering transaction");
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfNpConfigApiRoot) + "/{scsAsId}/configurations",
        [&verifier, &af_np_config](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& d : af_np_config.list(req.path_params.at("scsAsId"))) {
                out.push_back(d);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAfNpConfigApiRoot) + "/{scsAsId}/configurations",
        [&verifier, &af_np_config](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::NpConfiguration>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto af_id = req.path_params.at("scsAsId");
            json j = *body;
            const auto id = af_np_config.create(af_id, j);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kAfNpConfigApiRoot) + "/" + af_id +
                                     "/configurations/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfNpConfigApiRoot) + "/{scsAsId}/configurations/{configurationId}",
        [&verifier, &af_np_config](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto d = af_np_config.get(req.path_params.at("scsAsId"),
                                      req.path_params.at("configurationId"));
            if (!d.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such network parameter configuration");
            }
            return sbi_core::http2::Response::json(200, d->dump());
        });

    server.add_route(
        "PUT",
        std::string(kAfNpConfigApiRoot) + "/{scsAsId}/configurations/{configurationId}",
        [&verifier, &af_np_config](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::NpConfiguration>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            if (!af_np_config.put(
                    req.path_params.at("scsAsId"), req.path_params.at("configurationId"), j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such network parameter configuration");
            }
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "PATCH",
        std::string(kAfNpConfigApiRoot) + "/{scsAsId}/configurations/{configurationId}",
        [&verifier, &af_np_config](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            if (!sbi_core::http2::parse_json_body<sbi_gen::NpConfigurationPatch>(req, err)
                     .has_value()) {
                return err;
            }
            auto patched = af_np_config.merge_patch(req.path_params.at("scsAsId"),
                                                    req.path_params.at("configurationId"),
                                                    json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such network parameter configuration");
            }
            return sbi_core::http2::Response::json(200, patched->dump());
        });

    server.add_route(
        "DELETE",
        std::string(kAfNpConfigApiRoot) + "/{scsAsId}/configurations/{configurationId}",
        [&verifier, &af_np_config](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            if (!af_np_config.remove(req.path_params.at("scsAsId"),
                                     req.path_params.at("configurationId"))) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such network parameter configuration");
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfRacsApiRoot) + "/{scsAsId}/provisionings",
        [&verifier, &af_racs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& d : af_racs.list(req.path_params.at("scsAsId"))) {
                out.push_back(d);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAfRacsApiRoot) + "/{scsAsId}/provisionings",
        [&verifier, &af_racs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::RacsProvisioningData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto af_id = req.path_params.at("scsAsId");
            json j = *body;
            const auto id = af_racs.create(af_id, j);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace(
                "location", std::string(kAfRacsApiRoot) + "/" + af_id + "/provisionings/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfRacsApiRoot) + "/{scsAsId}/provisionings/{provisioningId}",
        [&verifier, &af_racs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto d =
                af_racs.get(req.path_params.at("scsAsId"), req.path_params.at("provisioningId"));
            if (!d.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such RACS provisioning");
            }
            return sbi_core::http2::Response::json(200, d->dump());
        });

    server.add_route(
        "PUT",
        std::string(kAfRacsApiRoot) + "/{scsAsId}/provisionings/{provisioningId}",
        [&verifier, &af_racs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::RacsProvisioningData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            if (!af_racs.put(
                    req.path_params.at("scsAsId"), req.path_params.at("provisioningId"), j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such RACS provisioning");
            }
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "PATCH",
        std::string(kAfRacsApiRoot) + "/{scsAsId}/provisionings/{provisioningId}",
        [&verifier, &af_racs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            if (!sbi_core::http2::parse_json_body<sbi_gen::RacsProvisioningDataPatch>(req, err)
                     .has_value()) {
                return err;
            }
            auto patched = af_racs.merge_patch(req.path_params.at("scsAsId"),
                                               req.path_params.at("provisioningId"),
                                               json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such RACS provisioning");
            }
            return sbi_core::http2::Response::json(200, patched->dump());
        });

    server.add_route(
        "DELETE",
        std::string(kAfRacsApiRoot) + "/{scsAsId}/provisionings/{provisioningId}",
        [&verifier, &af_racs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            if (!af_racs.remove(req.path_params.at("scsAsId"),
                                req.path_params.at("provisioningId"))) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such RACS provisioning");
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfNetStatusApiRoot) + "/{scsAsId}/subscriptions",
        [&verifier, &af_net_status](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& d : af_net_status.list(req.path_params.at("scsAsId"))) {
                out.push_back(d);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAfNetStatusApiRoot) + "/{scsAsId}/subscriptions",
        [&verifier, &af_net_status](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::NetworkStatusReportingSubscription>(req,
                                                                                              err);
            if (!body.has_value()) {
                return err;
            }
            const auto af_id = req.path_params.at("scsAsId");
            json j = *body;
            const auto id = af_net_status.create(af_id, j);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kAfNetStatusApiRoot) + "/" + af_id +
                                     "/subscriptions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfNetStatusApiRoot) + "/{scsAsId}/subscriptions/{subscriptionId}",
        [&verifier, &af_net_status](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto d = af_net_status.get(req.path_params.at("scsAsId"),
                                       req.path_params.at("subscriptionId"));
            if (!d.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such network status subscription");
            }
            return sbi_core::http2::Response::json(200, d->dump());
        });

    server.add_route(
        "PUT",
        std::string(kAfNetStatusApiRoot) + "/{scsAsId}/subscriptions/{subscriptionId}",
        [&verifier, &af_net_status](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::NetworkStatusReportingSubscription>(req,
                                                                                              err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            if (!af_net_status.put(
                    req.path_params.at("scsAsId"), req.path_params.at("subscriptionId"), j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such network status subscription");
            }
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "PATCH",
        std::string(kAfNetStatusApiRoot) + "/{scsAsId}/subscriptions/{subscriptionId}",
        [&verifier, &af_net_status](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            if (!sbi_core::http2::parse_json_body<sbi_gen::NetStatusRepSubsPatch>(req, err)
                     .has_value()) {
                return err;
            }
            auto patched = af_net_status.merge_patch(req.path_params.at("scsAsId"),
                                                     req.path_params.at("subscriptionId"),
                                                     json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such network status subscription");
            }
            return sbi_core::http2::Response::json(200, patched->dump());
        });

    server.add_route(
        "DELETE",
        std::string(kAfNetStatusApiRoot) + "/{scsAsId}/subscriptions/{subscriptionId}",
        [&verifier, &af_net_status](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            if (!af_net_status.remove(req.path_params.at("scsAsId"),
                                      req.path_params.at("subscriptionId"))) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such network status subscription");
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfBdtApiRoot) + "/{scsAsId}/subscriptions",
        [&verifier, &af_bdt](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& d : af_bdt.list(req.path_params.at("scsAsId"))) {
                out.push_back(d);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAfBdtApiRoot) + "/{scsAsId}/subscriptions",
        [&verifier, &af_bdt](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::Bdt>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto af_id = req.path_params.at("scsAsId");
            json j = *body;
            const auto id = af_bdt.create(af_id, j);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kAfBdtApiRoot) + "/" + af_id + "/subscriptions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfBdtApiRoot) + "/{scsAsId}/subscriptions/{subscriptionId}",
        [&verifier, &af_bdt](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto d =
                af_bdt.get(req.path_params.at("scsAsId"), req.path_params.at("subscriptionId"));
            if (!d.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such BDT subscription");
            }
            return sbi_core::http2::Response::json(200, d->dump());
        });

    server.add_route(
        "PUT",
        std::string(kAfBdtApiRoot) + "/{scsAsId}/subscriptions/{subscriptionId}",
        [&verifier, &af_bdt](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::Bdt>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            if (!af_bdt.put(
                    req.path_params.at("scsAsId"), req.path_params.at("subscriptionId"), j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such BDT subscription");
            }
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "PATCH",
        std::string(kAfBdtApiRoot) + "/{scsAsId}/subscriptions/{subscriptionId}",
        [&verifier, &af_bdt](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            if (!sbi_core::http2::parse_json_body<sbi_gen::BdtPatch>(req, err).has_value()) {
                return err;
            }
            auto patched = af_bdt.merge_patch(req.path_params.at("scsAsId"),
                                              req.path_params.at("subscriptionId"),
                                              json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such BDT subscription");
            }
            return sbi_core::http2::Response::json(200, patched->dump());
        });

    server.add_route(
        "DELETE",
        std::string(kAfBdtApiRoot) + "/{scsAsId}/subscriptions/{subscriptionId}",
        [&verifier, &af_bdt](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            if (!af_bdt.remove(req.path_params.at("scsAsId"),
                               req.path_params.at("subscriptionId"))) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such BDT subscription");
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- ADR-0326: fifteen AF services generated from verified spec data ---

    // TS29122_ChargeableParty -- /3gpp-chargeable-party/v1, root and method set read from the YAML
    // (ADR-0326).
    server.add_route(
        "GET",
        std::string(kAfChargeablePartyApiRoot) + "/{scsAsId}/transactions",
        [&verifier, &af_chargeable_party](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& doc : af_chargeable_party.list(req.path_params.at("scsAsId"))) {
                out.push_back(doc);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAfChargeablePartyApiRoot) + "/{scsAsId}/transactions",
        [&verifier, &af_chargeable_party](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::ChargeableParty>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            const auto id = af_chargeable_party.create(req.path_params.at("scsAsId"), j);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kAfChargeablePartyApiRoot) + "/" +
                                     req.path_params.at("scsAsId") + "/transactions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfChargeablePartyApiRoot) + "/{scsAsId}/transactions/{transactionId}",
        [&verifier, &af_chargeable_party](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto doc = af_chargeable_party.get(req.path_params.at("scsAsId"),
                                               req.path_params.at("transactionId"));
            if (!doc.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such chargeable party transaction");
            }
            return sbi_core::http2::Response::json(200, doc->dump());
        });
    server.add_route(
        "PATCH",
        std::string(kAfChargeablePartyApiRoot) + "/{scsAsId}/transactions/{transactionId}",
        [&verifier, &af_chargeable_party](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            if (!sbi_core::http2::parse_json_body<sbi_gen::ChargeablePartyPatch>(req, err)
                     .has_value()) {
                return err;
            }
            auto patched = af_chargeable_party.merge_patch(req.path_params.at("scsAsId"),
                                                           req.path_params.at("transactionId"),
                                                           json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such chargeable party transaction");
            }
            return sbi_core::http2::Response::json(200, patched->dump());
        });
    server.add_route(
        "DELETE",
        std::string(kAfChargeablePartyApiRoot) + "/{scsAsId}/transactions/{transactionId}",
        [&verifier, &af_chargeable_party](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            if (!af_chargeable_party.remove(req.path_params.at("scsAsId"),
                                            req.path_params.at("transactionId"))) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such chargeable party transaction");
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });
    // TS29522_5GLANParameterProvision -- /3gpp-5glan-pp/v1, root and method set read from the YAML
    // (ADR-0326).
    server.add_route(
        "GET",
        std::string(kAf5glanPpApiRoot) + "/{afId}/subscriptions",
        [&verifier, &af_5glan_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& doc : af_5glan_pp.list(req.path_params.at("afId"))) {
                out.push_back(doc);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAf5glanPpApiRoot) + "/{afId}/subscriptions",
        [&verifier, &af_5glan_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::N5GLanParametersProvision>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            const auto id = af_5glan_pp.create(req.path_params.at("afId"), j);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kAf5glanPpApiRoot) + "/" + req.path_params.at("afId") +
                                     "/subscriptions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAf5glanPpApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_5glan_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto doc =
                af_5glan_pp.get(req.path_params.at("afId"), req.path_params.at("subscriptionId"));
            if (!doc.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such 5GLAN parameter provisioning");
            }
            return sbi_core::http2::Response::json(200, doc->dump());
        });
    server.add_route(
        "PUT",
        std::string(kAf5glanPpApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_5glan_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::N5GLanParametersProvision>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            if (!af_5glan_pp.put(
                    req.path_params.at("afId"), req.path_params.at("subscriptionId"), j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such 5GLAN parameter provisioning");
            }
            return sbi_core::http2::Response::json(200, j.dump());
        });
    server.add_route(
        "PATCH",
        std::string(kAf5glanPpApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_5glan_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            if (!sbi_core::http2::parse_json_body<sbi_gen::N5GLanParametersProvisionPatch>(req, err)
                     .has_value()) {
                return err;
            }
            auto patched = af_5glan_pp.merge_patch(req.path_params.at("afId"),
                                                   req.path_params.at("subscriptionId"),
                                                   json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such 5GLAN parameter provisioning");
            }
            return sbi_core::http2::Response::json(200, patched->dump());
        });
    server.add_route(
        "DELETE",
        std::string(kAf5glanPpApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_5glan_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            if (!af_5glan_pp.remove(req.path_params.at("afId"),
                                    req.path_params.at("subscriptionId"))) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such 5GLAN parameter provisioning");
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });
    // TS29522_AMInfluence -- /3gpp-am-influence/v1, root and method set read from the YAML
    // (ADR-0326).
    server.add_route(
        "GET",
        std::string(kAfAmInfluenceApiRoot) + "/{afId}/subscriptions",
        [&verifier, &af_am_influence](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& doc : af_am_influence.list(req.path_params.at("afId"))) {
                out.push_back(doc);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAfAmInfluenceApiRoot) + "/{afId}/subscriptions",
        [&verifier, &af_am_influence](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::AmInfluSub>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            const auto id = af_am_influence.create(req.path_params.at("afId"), j);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kAfAmInfluenceApiRoot) + "/" +
                                     req.path_params.at("afId") + "/subscriptions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfAmInfluenceApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_am_influence](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto doc = af_am_influence.get(req.path_params.at("afId"),
                                           req.path_params.at("subscriptionId"));
            if (!doc.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such AM influence subscription");
            }
            return sbi_core::http2::Response::json(200, doc->dump());
        });
    server.add_route(
        "PUT",
        std::string(kAfAmInfluenceApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_am_influence](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::AmInfluSub>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            if (!af_am_influence.put(
                    req.path_params.at("afId"), req.path_params.at("subscriptionId"), j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such AM influence subscription");
            }
            return sbi_core::http2::Response::json(200, j.dump());
        });
    server.add_route(
        "PATCH",
        std::string(kAfAmInfluenceApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_am_influence](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            if (!sbi_core::http2::parse_json_body<sbi_gen::AmInfluSubPatch>(req, err).has_value()) {
                return err;
            }
            auto patched = af_am_influence.merge_patch(req.path_params.at("afId"),
                                                       req.path_params.at("subscriptionId"),
                                                       json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such AM influence subscription");
            }
            return sbi_core::http2::Response::json(200, patched->dump());
        });
    server.add_route(
        "DELETE",
        std::string(kAfAmInfluenceApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_am_influence](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            if (!af_am_influence.remove(req.path_params.at("afId"),
                                        req.path_params.at("subscriptionId"))) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such AM influence subscription");
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });
    // TS29522_ApplyingBdtPolicy -- /3gpp-applying-bdt-policy/v1, root and method set read from the
    // YAML (ADR-0326).
    server.add_route(
        "GET",
        std::string(kAfApplyBdtApiRoot) + "/{afId}/subscriptions",
        [&verifier, &af_apply_bdt](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& doc : af_apply_bdt.list(req.path_params.at("afId"))) {
                out.push_back(doc);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAfApplyBdtApiRoot) + "/{afId}/subscriptions",
        [&verifier, &af_apply_bdt](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::AppliedBdtPolicy>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            const auto id = af_apply_bdt.create(req.path_params.at("afId"), j);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kAfApplyBdtApiRoot) + "/" +
                                     req.path_params.at("afId") + "/subscriptions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfApplyBdtApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_apply_bdt](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto doc =
                af_apply_bdt.get(req.path_params.at("afId"), req.path_params.at("subscriptionId"));
            if (!doc.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such applied BDT policy");
            }
            return sbi_core::http2::Response::json(200, doc->dump());
        });
    server.add_route(
        "PATCH",
        std::string(kAfApplyBdtApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_apply_bdt](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            if (!sbi_core::http2::parse_json_body<sbi_gen::AppliedBdtPolicyPatch>(req, err)
                     .has_value()) {
                return err;
            }
            auto patched = af_apply_bdt.merge_patch(req.path_params.at("afId"),
                                                    req.path_params.at("subscriptionId"),
                                                    json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such applied BDT policy");
            }
            return sbi_core::http2::Response::json(200, patched->dump());
        });
    server.add_route(
        "DELETE",
        std::string(kAfApplyBdtApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_apply_bdt](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            if (!af_apply_bdt.remove(req.path_params.at("afId"),
                                     req.path_params.at("subscriptionId"))) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such applied BDT policy");
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });
    // TS29522_DNAIMapping -- /3gpp-dnai-mapping/v1, root and method set read from the YAML
    // (ADR-0326).
    server.add_route(
        "GET",
        std::string(kAfDnaiMappingApiRoot) + "/{afId}/subscriptions",
        [&verifier, &af_dnai_mapping](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& doc : af_dnai_mapping.list(req.path_params.at("afId"))) {
                out.push_back(doc);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAfDnaiMappingApiRoot) + "/{afId}/subscriptions",
        [&verifier, &af_dnai_mapping](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::DnaiMapSub>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            const auto id = af_dnai_mapping.create(req.path_params.at("afId"), j);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kAfDnaiMappingApiRoot) + "/" +
                                     req.path_params.at("afId") + "/subscriptions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfDnaiMappingApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_dnai_mapping](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto doc = af_dnai_mapping.get(req.path_params.at("afId"),
                                           req.path_params.at("subscriptionId"));
            if (!doc.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such DNAI mapping subscription");
            }
            return sbi_core::http2::Response::json(200, doc->dump());
        });
    server.add_route(
        "DELETE",
        std::string(kAfDnaiMappingApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_dnai_mapping](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            if (!af_dnai_mapping.remove(req.path_params.at("afId"),
                                        req.path_params.at("subscriptionId"))) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such DNAI mapping subscription");
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });
    // TS29522_EcsAddressProvision -- /3gpp-ecs-address-provision/v1, root and method set read from
    // the YAML (ADR-0326).
    server.add_route(
        "GET",
        std::string(kAfEcsAddrProvApiRoot) + "/{afId}/configurations",
        [&verifier, &af_ecs_addr_prov](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& doc : af_ecs_addr_prov.list(req.path_params.at("afId"))) {
                out.push_back(doc);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAfEcsAddrProvApiRoot) + "/{afId}/configurations",
        [&verifier, &af_ecs_addr_prov](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::EcsAddressProvision>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            const auto id = af_ecs_addr_prov.create(req.path_params.at("afId"), j);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kAfEcsAddrProvApiRoot) + "/" +
                                     req.path_params.at("afId") + "/configurations/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfEcsAddrProvApiRoot) + "/{afId}/configurations/{configurationId}",
        [&verifier, &af_ecs_addr_prov](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto doc = af_ecs_addr_prov.get(req.path_params.at("afId"),
                                            req.path_params.at("configurationId"));
            if (!doc.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such ECS address provisioning");
            }
            return sbi_core::http2::Response::json(200, doc->dump());
        });
    server.add_route(
        "PUT",
        std::string(kAfEcsAddrProvApiRoot) + "/{afId}/configurations/{configurationId}",
        [&verifier, &af_ecs_addr_prov](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::EcsAddressProvision>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            if (!af_ecs_addr_prov.put(
                    req.path_params.at("afId"), req.path_params.at("configurationId"), j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such ECS address provisioning");
            }
            return sbi_core::http2::Response::json(200, j.dump());
        });
    server.add_route(
        "DELETE",
        std::string(kAfEcsAddrProvApiRoot) + "/{afId}/configurations/{configurationId}",
        [&verifier, &af_ecs_addr_prov](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            if (!af_ecs_addr_prov.remove(req.path_params.at("afId"),
                                         req.path_params.at("configurationId"))) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such ECS address provisioning");
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });
    // TS29522_ImsParamProvision -- /3gpp-ims-pp/v1, root and method set read from the YAML
    // (ADR-0326).
    server.add_route(
        "GET",
        std::string(kAfImsPpApiRoot) + "/pp",
        [&verifier, &af_ims_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& doc : af_ims_pp.list("")) {
                out.push_back(doc);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAfImsPpApiRoot) + "/pp",
        [&verifier, &af_ims_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::ImsPpData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            const auto id = af_ims_pp.create("", j);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location", std::string(kAfImsPpApiRoot) + "/pp/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfImsPpApiRoot) + "/pp/{ppId}",
        [&verifier, &af_ims_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto doc = af_ims_pp.get("", req.path_params.at("ppId"));
            if (!doc.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such IMS parameter provisioning");
            }
            return sbi_core::http2::Response::json(200, doc->dump());
        });
    server.add_route(
        "PUT",
        std::string(kAfImsPpApiRoot) + "/pp/{ppId}",
        [&verifier, &af_ims_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::ImsPpData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            if (!af_ims_pp.put("", req.path_params.at("ppId"), j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such IMS parameter provisioning");
            }
            return sbi_core::http2::Response::json(200, j.dump());
        });
    // IMS parameter provisioning: TS 29.522 declares this PATCH as application/json-patch+json --
    // RFC 6902, an ordered array of operations -- not the merge-patch every other service in this
    // batch uses. Applying a merge to a JSON Patch array would silently store the array itself as
    // the document. Same idiom as the AMF's own RFC 6902 handling (nfs/amf/src/main.cpp).
    server.add_route(
        "PATCH",
        std::string(kAfImsPpApiRoot) + "/pp/{ppId}",
        [&verifier, &af_ims_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json patch_doc;
            try {
                patch_doc = json::parse(req.body);
            } catch (const std::exception& e) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", std::string("Invalid JSON: ") + e.what());
            }
            // The YAML types the body as an array of PatchItem with minItems 1.
            if (!patch_doc.is_array() || patch_doc.empty()) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", "JSON Patch body must be a non-empty array");
            }
            for (const auto& entry : patch_doc) {
                try {
                    (void)entry.get<sbi_gen::PatchItem>();
                } catch (const std::exception& e) {
                    return sbi_core::http2::problem_response(
                        400, "Bad Request", std::string("Invalid PatchItem: ") + e.what());
                }
            }
            std::optional<json> patched;
            try {
                // Flat collection -- this service's paths carry no {afId} segment, so the
                // store scope is empty, matching how the GET/PUT/DELETE routes above address it.
                patched = af_ims_pp.json_patch("", req.path_params.at("ppId"), patch_doc);
            } catch (const std::exception& e) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", std::string("Invalid JSON Patch: ") + e.what());
            }
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such IMS parameter provisioning");
            }
            return sbi_core::http2::Response::json(200, patched->dump());
        });
    server.add_route(
        "DELETE",
        std::string(kAfImsPpApiRoot) + "/pp/{ppId}",
        [&verifier, &af_ims_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            if (!af_ims_pp.remove("", req.path_params.at("ppId"))) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such IMS parameter provisioning");
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });
    // TS29522_ImsSessionManagement -- /3gpp-ims-sm/v1, root and method set read from the YAML
    // (ADR-0326).
    server.add_route(
        "GET",
        std::string(kAfImsSmApiRoot) + "/ims-sessions",
        [&verifier, &af_ims_sm](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& doc : af_ims_sm.list("")) {
                out.push_back(doc);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAfImsSmApiRoot) + "/ims-sessions",
        [&verifier, &af_ims_sm](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::ImsSession>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            const auto id = af_ims_sm.create("", j);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location", std::string(kAfImsSmApiRoot) + "/ims-sessions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfImsSmApiRoot) + "/ims-sessions/{sessionId}",
        [&verifier, &af_ims_sm](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto doc = af_ims_sm.get("", req.path_params.at("sessionId"));
            if (!doc.has_value()) {
                return sbi_core::http2::problem_response(404, "Not Found", "No such IMS session");
            }
            return sbi_core::http2::Response::json(200, doc->dump());
        });
    server.add_route(
        "PUT",
        std::string(kAfImsSmApiRoot) + "/ims-sessions/{sessionId}",
        [&verifier, &af_ims_sm](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::ImsSession>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            if (!af_ims_sm.put("", req.path_params.at("sessionId"), j)) {
                return sbi_core::http2::problem_response(404, "Not Found", "No such IMS session");
            }
            return sbi_core::http2::Response::json(200, j.dump());
        });
    // IMS session: TS 29.522 declares this PATCH as application/json-patch+json -- RFC 6902, an
    // ordered array of operations -- not the merge-patch every other service in this batch uses.
    // Applying a merge to a JSON Patch array would silently store the array itself as the
    // document. Same idiom as the AMF's own RFC 6902 handling (nfs/amf/src/main.cpp).
    server.add_route(
        "PATCH",
        std::string(kAfImsSmApiRoot) + "/ims-sessions/{sessionId}",
        [&verifier, &af_ims_sm](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json patch_doc;
            try {
                patch_doc = json::parse(req.body);
            } catch (const std::exception& e) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", std::string("Invalid JSON: ") + e.what());
            }
            // The YAML types the body as an array of PatchItem with minItems 1.
            if (!patch_doc.is_array() || patch_doc.empty()) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", "JSON Patch body must be a non-empty array");
            }
            for (const auto& entry : patch_doc) {
                try {
                    (void)entry.get<sbi_gen::PatchItem>();
                } catch (const std::exception& e) {
                    return sbi_core::http2::problem_response(
                        400, "Bad Request", std::string("Invalid PatchItem: ") + e.what());
                }
            }
            std::optional<json> patched;
            try {
                // Flat collection -- this service's paths carry no {afId} segment, so the
                // store scope is empty, matching how the GET/PUT/DELETE routes above address it.
                patched = af_ims_sm.json_patch("", req.path_params.at("sessionId"), patch_doc);
            } catch (const std::exception& e) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", std::string("Invalid JSON Patch: ") + e.what());
            }
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(404, "Not Found", "No such IMS session");
            }
            return sbi_core::http2::Response::json(200, patched->dump());
        });
    server.add_route(
        "DELETE",
        std::string(kAfImsSmApiRoot) + "/ims-sessions/{sessionId}",
        [&verifier, &af_ims_sm](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            if (!af_ims_sm.remove("", req.path_params.at("sessionId"))) {
                return sbi_core::http2::problem_response(404, "Not Found", "No such IMS session");
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });
    // TS29522_MBSGroupMsgDelivery -- /3gpp-mbs-group-msg/v1, root and method set read from the YAML
    // (ADR-0326).
    server.add_route(
        "GET",
        std::string(kAfMbsGroupMsgApiRoot) + "/deliveries",
        [&verifier, &af_mbs_group_msg](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& doc : af_mbs_group_msg.list("")) {
                out.push_back(doc);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAfMbsGroupMsgApiRoot) + "/deliveries",
        [&verifier, &af_mbs_group_msg](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::MbsGroupMsgDel>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            const auto id = af_mbs_group_msg.create("", j);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kAfMbsGroupMsgApiRoot) + "/deliveries/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfMbsGroupMsgApiRoot) + "/deliveries/{delRef}",
        [&verifier, &af_mbs_group_msg](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto doc = af_mbs_group_msg.get("", req.path_params.at("delRef"));
            if (!doc.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such MBS group message delivery");
            }
            return sbi_core::http2::Response::json(200, doc->dump());
        });
    server.add_route(
        "PATCH",
        std::string(kAfMbsGroupMsgApiRoot) + "/deliveries/{delRef}",
        [&verifier, &af_mbs_group_msg](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            if (!sbi_core::http2::parse_json_body<sbi_gen::MbsGroupMsgDelPatch>(req, err)
                     .has_value()) {
                return err;
            }
            auto patched = af_mbs_group_msg.merge_patch(
                "", req.path_params.at("delRef"), json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such MBS group message delivery");
            }
            return sbi_core::http2::Response::json(200, patched->dump());
        });
    server.add_route(
        "DELETE",
        std::string(kAfMbsGroupMsgApiRoot) + "/deliveries/{delRef}",
        [&verifier, &af_mbs_group_msg](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            if (!af_mbs_group_msg.remove("", req.path_params.at("delRef"))) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such MBS group message delivery");
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });
    // TS29522_MSEventExposure -- /3gpp-ms-event-exposure/v1, root and method set read from the YAML
    // (ADR-0326).
    server.add_route(
        "GET",
        std::string(kAfMsEventApiRoot) + "/subscriptions",
        [&verifier, &af_ms_event](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& doc : af_ms_event.list("")) {
                out.push_back(doc);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAfMsEventApiRoot) + "/subscriptions",
        [&verifier, &af_ms_event](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::AfEventExposureSubsc>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            const auto id = af_ms_event.create("", j);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kAfMsEventApiRoot) + "/subscriptions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfMsEventApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &af_ms_event](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto doc = af_ms_event.get("", req.path_params.at("subscriptionId"));
            if (!doc.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such MS event subscription");
            }
            return sbi_core::http2::Response::json(200, doc->dump());
        });
    server.add_route(
        "PUT",
        std::string(kAfMsEventApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &af_ms_event](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::AfEventExposureSubsc>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            if (!af_ms_event.put("", req.path_params.at("subscriptionId"), j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such MS event subscription");
            }
            return sbi_core::http2::Response::json(200, j.dump());
        });
    server.add_route(
        "DELETE",
        std::string(kAfMsEventApiRoot) + "/subscriptions/{subscriptionId}",
        [&verifier, &af_ms_event](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            if (!af_ms_event.remove("", req.path_params.at("subscriptionId"))) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such MS event subscription");
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });
    // TS29522_MemberUESelectionAssistance -- /3gpp-musa/v1, root and method set read from the YAML
    // (ADR-0326).
    server.add_route(
        "GET",
        std::string(kAfMusaApiRoot) + "/{afId}/subscriptions",
        [&verifier, &af_musa](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& doc : af_musa.list(req.path_params.at("afId"))) {
                out.push_back(doc);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAfMusaApiRoot) + "/{afId}/subscriptions",
        [&verifier, &af_musa](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::MemUeSelectAssistSubsc>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            const auto id = af_musa.create(req.path_params.at("afId"), j);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kAfMusaApiRoot) + "/" + req.path_params.at("afId") +
                                     "/subscriptions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfMusaApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_musa](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto doc =
                af_musa.get(req.path_params.at("afId"), req.path_params.at("subscriptionId"));
            if (!doc.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such member UE selection subscription");
            }
            return sbi_core::http2::Response::json(200, doc->dump());
        });
    server.add_route(
        "PUT",
        std::string(kAfMusaApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_musa](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::MemUeSelectAssistSubsc>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            if (!af_musa.put(req.path_params.at("afId"), req.path_params.at("subscriptionId"), j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such member UE selection subscription");
            }
            return sbi_core::http2::Response::json(200, j.dump());
        });
    server.add_route(
        "PATCH",
        std::string(kAfMusaApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_musa](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            if (!sbi_core::http2::parse_json_body<sbi_gen::MemUeSelectAssistSubscPatch>(req, err)
                     .has_value()) {
                return err;
            }
            auto patched = af_musa.merge_patch(req.path_params.at("afId"),
                                               req.path_params.at("subscriptionId"),
                                               json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such member UE selection subscription");
            }
            return sbi_core::http2::Response::json(200, patched->dump());
        });
    server.add_route(
        "DELETE",
        std::string(kAfMusaApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_musa](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            if (!af_musa.remove(req.path_params.at("afId"), req.path_params.at("subscriptionId"))) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such member UE selection subscription");
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });
    // TS29522_PDTQPolicyNegotiation -- /3gpp-pdtq-policy-negotiation/v1, root and method set read
    // from the YAML (ADR-0326).
    server.add_route(
        "GET",
        std::string(kAfPdtqApiRoot) + "/{afId}/subscriptions",
        [&verifier, &af_pdtq](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& doc : af_pdtq.list(req.path_params.at("afId"))) {
                out.push_back(doc);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAfPdtqApiRoot) + "/{afId}/subscriptions",
        [&verifier, &af_pdtq](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::Pdtq>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            const auto id = af_pdtq.create(req.path_params.at("afId"), j);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kAfPdtqApiRoot) + "/" + req.path_params.at("afId") +
                                     "/subscriptions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfPdtqApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_pdtq](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto doc =
                af_pdtq.get(req.path_params.at("afId"), req.path_params.at("subscriptionId"));
            if (!doc.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such PDTQ policy negotiation");
            }
            return sbi_core::http2::Response::json(200, doc->dump());
        });
    server.add_route(
        "PATCH",
        std::string(kAfPdtqApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_pdtq](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            if (!sbi_core::http2::parse_json_body<sbi_gen::PdtqPatch>(req, err).has_value()) {
                return err;
            }
            auto patched = af_pdtq.merge_patch(req.path_params.at("afId"),
                                               req.path_params.at("subscriptionId"),
                                               json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such PDTQ policy negotiation");
            }
            return sbi_core::http2::Response::json(200, patched->dump());
        });
    server.add_route(
        "DELETE",
        std::string(kAfPdtqApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_pdtq](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            if (!af_pdtq.remove(req.path_params.at("afId"), req.path_params.at("subscriptionId"))) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such PDTQ policy negotiation");
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });
    // TS29522_RSLPPIParametersProvisioning -- /3gpp-rslppi-pp/v1, root and method set read from the
    // YAML (ADR-0326).
    server.add_route(
        "GET",
        std::string(kAfRslppiPpApiRoot) + "/pp",
        [&verifier, &af_rslppi_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& doc : af_rslppi_pp.list("")) {
                out.push_back(doc);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAfRslppiPpApiRoot) + "/pp",
        [&verifier, &af_rslppi_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::RslppiPpData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            const auto id = af_rslppi_pp.create("", j);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location", std::string(kAfRslppiPpApiRoot) + "/pp/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfRslppiPpApiRoot) + "/pp/{ppId}",
        [&verifier, &af_rslppi_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto doc = af_rslppi_pp.get("", req.path_params.at("ppId"));
            if (!doc.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such RSLPPI provisioning");
            }
            return sbi_core::http2::Response::json(200, doc->dump());
        });
    server.add_route(
        "PUT",
        std::string(kAfRslppiPpApiRoot) + "/pp/{ppId}",
        [&verifier, &af_rslppi_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::RslppiPpData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            if (!af_rslppi_pp.put("", req.path_params.at("ppId"), j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such RSLPPI provisioning");
            }
            return sbi_core::http2::Response::json(200, j.dump());
        });
    server.add_route(
        "PATCH",
        std::string(kAfRslppiPpApiRoot) + "/pp/{ppId}",
        [&verifier, &af_rslppi_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            if (!sbi_core::http2::parse_json_body<sbi_gen::RslppiPpDataPatch>(req, err)
                     .has_value()) {
                return err;
            }
            auto patched =
                af_rslppi_pp.merge_patch("", req.path_params.at("ppId"), json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such RSLPPI provisioning");
            }
            return sbi_core::http2::Response::json(200, patched->dump());
        });
    server.add_route(
        "DELETE",
        std::string(kAfRslppiPpApiRoot) + "/pp/{ppId}",
        [&verifier, &af_rslppi_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            if (!af_rslppi_pp.remove("", req.path_params.at("ppId"))) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such RSLPPI provisioning");
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });
    // TS29522_VFLInference -- /3gpp-vfl-inference/v1, root and method set read from the YAML
    // (ADR-0326).
    server.add_route(
        "GET",
        std::string(kAfVflInferApiRoot) + "/{afId}/subscriptions",
        [&verifier, &af_vfl_infer](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& doc : af_vfl_infer.list(req.path_params.at("afId"))) {
                out.push_back(doc);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAfVflInferApiRoot) + "/{afId}/subscriptions",
        [&verifier, &af_vfl_infer](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::VflInferSub_VFLInference>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            const auto id = af_vfl_infer.create(req.path_params.at("afId"), j);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kAfVflInferApiRoot) + "/" +
                                     req.path_params.at("afId") + "/subscriptions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfVflInferApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_vfl_infer](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto doc =
                af_vfl_infer.get(req.path_params.at("afId"), req.path_params.at("subscriptionId"));
            if (!doc.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such VFL inference subscription");
            }
            return sbi_core::http2::Response::json(200, doc->dump());
        });
    server.add_route(
        "PUT",
        std::string(kAfVflInferApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_vfl_infer](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::VflInferSub_VFLInference>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            if (!af_vfl_infer.put(
                    req.path_params.at("afId"), req.path_params.at("subscriptionId"), j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such VFL inference subscription");
            }
            return sbi_core::http2::Response::json(200, j.dump());
        });
    server.add_route(
        "PATCH",
        std::string(kAfVflInferApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_vfl_infer](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            if (!sbi_core::http2::parse_json_body<sbi_gen::VflInferSubPatch_VFLInference>(req, err)
                     .has_value()) {
                return err;
            }
            auto patched = af_vfl_infer.merge_patch(req.path_params.at("afId"),
                                                    req.path_params.at("subscriptionId"),
                                                    json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such VFL inference subscription");
            }
            return sbi_core::http2::Response::json(200, patched->dump());
        });
    server.add_route(
        "DELETE",
        std::string(kAfVflInferApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_vfl_infer](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            if (!af_vfl_infer.remove(req.path_params.at("afId"),
                                     req.path_params.at("subscriptionId"))) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such VFL inference subscription");
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });
    // TS29522_VFLTraining -- /3gpp-vfl-training/v1, root and method set read from the YAML
    // (ADR-0326).
    server.add_route(
        "GET",
        std::string(kAfVflTrainApiRoot) + "/{afId}/subscriptions",
        [&verifier, &af_vfl_train](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& doc : af_vfl_train.list(req.path_params.at("afId"))) {
                out.push_back(doc);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAfVflTrainApiRoot) + "/{afId}/subscriptions",
        [&verifier, &af_vfl_train](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::VflTrainingSubs_VFLTraining>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            const auto id = af_vfl_train.create(req.path_params.at("afId"), j);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kAfVflTrainApiRoot) + "/" +
                                     req.path_params.at("afId") + "/subscriptions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfVflTrainApiRoot) + "/{afId}/subscriptions/{subId}",
        [&verifier, &af_vfl_train](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto doc = af_vfl_train.get(req.path_params.at("afId"), req.path_params.at("subId"));
            if (!doc.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such VFL training subscription");
            }
            return sbi_core::http2::Response::json(200, doc->dump());
        });
    server.add_route(
        "PUT",
        std::string(kAfVflTrainApiRoot) + "/{afId}/subscriptions/{subId}",
        [&verifier, &af_vfl_train](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::VflTrainingSubs_VFLTraining>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            if (!af_vfl_train.put(req.path_params.at("afId"), req.path_params.at("subId"), j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such VFL training subscription");
            }
            return sbi_core::http2::Response::json(200, j.dump());
        });
    server.add_route(
        "PATCH",
        std::string(kAfVflTrainApiRoot) + "/{afId}/subscriptions/{subId}",
        [&verifier, &af_vfl_train](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            if (!sbi_core::http2::parse_json_body<sbi_gen::VflTrainingSubsPatch_VFLTraining>(req,
                                                                                             err)
                     .has_value()) {
                return err;
            }
            auto patched = af_vfl_train.merge_patch(
                req.path_params.at("afId"), req.path_params.at("subId"), json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such VFL training subscription");
            }
            return sbi_core::http2::Response::json(200, patched->dump());
        });
    server.add_route(
        "DELETE",
        std::string(kAfVflTrainApiRoot) + "/{afId}/subscriptions/{subId}",
        [&verifier, &af_vfl_train](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            if (!af_vfl_train.remove(req.path_params.at("afId"), req.path_params.at("subId"))) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such VFL training subscription");
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // ADR-0325: FetchAnalyticsInfo. Present in TS29522_AnalyticsExposure and missed when the
    // service was added in ADR-0324 -- the method sets were read per path, but only for the two
    // paths the six-operation shape expects, so a third path went unseen.
    //
    // The request is parsed against the real AnalyticsRequest. The response is 501: AnalyticsData
    // has to come from NWDAF, which is Phase 5 and not built. Synthesising a plausible-looking
    // analytics payload here would be the one failure mode this project cannot afford.
    server.add_route(
        "POST",
        std::string(kAfAnalyticsApiRoot) + "/{afId}/fetch",
        [&verifier](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            if (!sbi_core::http2::parse_json_body<sbi_gen::AnalyticsRequest>(req, err)
                     .has_value()) {
                return err;
            }
            return sbi_core::http2::problem_response(
                501,
                "Not Implemented",
                "Analytics fetch requires NWDAF, which is not deployed in this core");
        });

    server.add_route(
        "GET",
        std::string(kAfAnalyticsApiRoot) + "/{afId}/subscriptions",
        [&verifier, &af_analytics](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& d : af_analytics.list(req.path_params.at("afId"))) {
                out.push_back(d);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAfAnalyticsApiRoot) + "/{afId}/subscriptions",
        [&verifier, &af_analytics](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::AnalyticsExposureSubsc>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto af_id = req.path_params.at("afId");
            json j = *body;
            const auto id = af_analytics.create(af_id, j);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kAfAnalyticsApiRoot) + "/" + af_id +
                                     "/subscriptions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfAnalyticsApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_analytics](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto d =
                af_analytics.get(req.path_params.at("afId"), req.path_params.at("subscriptionId"));
            if (!d.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such analytics exposure subscription");
            }
            return sbi_core::http2::Response::json(200, d->dump());
        });

    server.add_route(
        "PUT",
        std::string(kAfAnalyticsApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_analytics](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::AnalyticsExposureSubsc>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            if (!af_analytics.put(
                    req.path_params.at("afId"), req.path_params.at("subscriptionId"), j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such analytics exposure subscription");
            }
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "DELETE",
        std::string(kAfAnalyticsApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_analytics](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            if (!af_analytics.remove(req.path_params.at("afId"),
                                     req.path_params.at("subscriptionId"))) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such analytics exposure subscription");
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // ADR-0325: RetrieveStatusofConfiguration. Registered BEFORE the item routes below on purpose:
    // sbi_core's matcher is first-registered-wins with no preference for a literal segment over a
    // parameter one (libs/sbi-core/src/http2_server.cpp:119), so "/configurations/retrieve" and
    // "/configurations/{configId}" are decided by registration order alone. They do not collide
    // today -- the item path defines no POST -- but that is a property of the current spec, not of
    // the router, and relying on it would leave a trap for whoever adds one.
    //
    // 501 for the same reason as FetchAnalyticsInfo: the status of an access-time distribution
    // configuration comes from TSCTSF, which is not wired to NEF here.
    server.add_route(
        "POST",
        std::string(kAfAstiApiRoot) + "/{afId}/configurations/retrieve",
        [&verifier](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            if (!sbi_core::http2::parse_json_body<sbi_gen::StatusRequestData>(req, err)
                     .has_value()) {
                return err;
            }
            return sbi_core::http2::problem_response(
                501,
                "Not Implemented",
                "Configuration status retrieval requires TSCTSF, which is not wired to NEF");
        });

    server.add_route(
        "GET",
        std::string(kAfAstiApiRoot) + "/{afId}/configurations",
        [&verifier, &af_asti](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& d : af_asti.list(req.path_params.at("afId"))) {
                out.push_back(d);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAfAstiApiRoot) + "/{afId}/configurations",
        [&verifier, &af_asti](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::AccessTimeDistributionData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto af_id = req.path_params.at("afId");
            json j = *body;
            const auto id = af_asti.create(af_id, j);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace(
                "location", std::string(kAfAstiApiRoot) + "/" + af_id + "/configurations/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfAstiApiRoot) + "/{afId}/configurations/{configurationId}",
        [&verifier, &af_asti](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto d = af_asti.get(req.path_params.at("afId"), req.path_params.at("configurationId"));
            if (!d.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such ASTI configuration");
            }
            return sbi_core::http2::Response::json(200, d->dump());
        });

    server.add_route(
        "PUT",
        std::string(kAfAstiApiRoot) + "/{afId}/configurations/{configurationId}",
        [&verifier, &af_asti](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::AccessTimeDistributionData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            if (!af_asti.put(
                    req.path_params.at("afId"), req.path_params.at("configurationId"), j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such ASTI configuration");
            }
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "DELETE",
        std::string(kAfAstiApiRoot) + "/{afId}/configurations/{configurationId}",
        [&verifier, &af_asti](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            if (!af_asti.remove(req.path_params.at("afId"),
                                req.path_params.at("configurationId"))) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such ASTI configuration");
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfImsEventApiRoot) + "/{afId}/subscriptions",
        [&verifier, &af_ims_event](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& d : af_ims_event.list(req.path_params.at("afId"))) {
                out.push_back(d);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAfImsEventApiRoot) + "/{afId}/subscriptions",
        [&verifier, &af_ims_event](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::ImsEESubsc>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto af_id = req.path_params.at("afId");
            json j = *body;
            const auto id = af_ims_event.create(af_id, j);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace(
                "location", std::string(kAfImsEventApiRoot) + "/" + af_id + "/subscriptions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfImsEventApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_ims_event](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto d =
                af_ims_event.get(req.path_params.at("afId"), req.path_params.at("subscriptionId"));
            if (!d.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such IMS event subscription");
            }
            return sbi_core::http2::Response::json(200, d->dump());
        });

    server.add_route(
        "PUT",
        std::string(kAfImsEventApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_ims_event](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::ImsEESubsc>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            if (!af_ims_event.put(
                    req.path_params.at("afId"), req.path_params.at("subscriptionId"), j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such IMS event subscription");
            }
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "PATCH",
        std::string(kAfImsEventApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_ims_event](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            if (!sbi_core::http2::parse_json_body<sbi_gen::ImsEESubscPatch>(req, err).has_value()) {
                return err;
            }
            auto patched = af_ims_event.merge_patch(req.path_params.at("afId"),
                                                    req.path_params.at("subscriptionId"),
                                                    json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such IMS event subscription");
            }
            return sbi_core::http2::Response::json(200, patched->dump());
        });

    server.add_route(
        "DELETE",
        std::string(kAfImsEventApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_ims_event](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            if (!af_ims_event.remove(req.path_params.at("afId"),
                                     req.path_params.at("subscriptionId"))) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such IMS event subscription");
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- ADR-0323: the four `/pp` parameter-provisioning services (TS 29.522) ---
    //
    // CagInfoParamProvision, AddressingParamProvision, SliceParamProvision and
    // GroupParametersProvisioning -- 24 operations, one shape.
    //
    // Note the path difference from every AF service before them: these collections are a flat
    // `/pp`, with NO `{afId}` segment. So unlike TrafficInfluence/QoS/Monitoring, the spec does
    // not scope these resources per-AF at all, and this build does not invent a scoping the API
    // does not have -- they share one namespace, which is what the path says. An operator wanting
    // per-AF isolation here would need it in the spec first.
    //
    // Downstream: these provision parameters that belong in UDM's `Nudm_PP` (which this project
    // implements, ADR-0237) or UDR's provisioned-data. Wiring each to its correct destination is
    // per-service work -- CAG info, static IP addressing, slice parameters and group data land in
    // different places -- and is NOT done here. They are accepted, validated against their real
    // generated DTOs, and stored. The APIs are real; nothing downstream applies them yet.

    server.add_route(
        "GET",
        std::string(kAfCagPpApiRoot) + "/pp",
        [&verifier, &af_cag_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& d : af_cag_pp.list(kPpNamespace)) {
                out.push_back(d);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAfCagPpApiRoot) + "/pp",
        [&verifier, &af_cag_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::CagInfoPpData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            const auto id = af_cag_pp.create(kPpNamespace, j);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location", std::string(kAfCagPpApiRoot) + "/pp/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfCagPpApiRoot) + "/pp/{ppId}",
        [&verifier, &af_cag_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto d = af_cag_pp.get(kPpNamespace, req.path_params.at("ppId"));
            if (!d.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such CAG info provisioning");
            }
            return sbi_core::http2::Response::json(200, d->dump());
        });

    server.add_route(
        "PUT",
        std::string(kAfCagPpApiRoot) + "/pp/{ppId}",
        [&verifier, &af_cag_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::CagInfoPpData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            if (!af_cag_pp.put(kPpNamespace, req.path_params.at("ppId"), j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such CAG info provisioning");
            }
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "PATCH",
        std::string(kAfCagPpApiRoot) + "/pp/{ppId}",
        [&verifier, &af_cag_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            if (!sbi_core::http2::parse_json_body<sbi_gen::CagInfoPpDataPatch>(req, err)
                     .has_value()) {
                return err;
            }
            auto patched = af_cag_pp.merge_patch(
                kPpNamespace, req.path_params.at("ppId"), json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such CAG info provisioning");
            }
            return sbi_core::http2::Response::json(200, patched->dump());
        });

    server.add_route(
        "DELETE",
        std::string(kAfCagPpApiRoot) + "/pp/{ppId}",
        [&verifier, &af_cag_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            if (!af_cag_pp.remove(kPpNamespace, req.path_params.at("ppId"))) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such CAG info provisioning");
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfAddrPpApiRoot) + "/pp",
        [&verifier, &af_addr_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& d : af_addr_pp.list(kPpNamespace)) {
                out.push_back(d);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAfAddrPpApiRoot) + "/pp",
        [&verifier, &af_addr_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::AddrPpData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            const auto id = af_addr_pp.create(kPpNamespace, j);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location", std::string(kAfAddrPpApiRoot) + "/pp/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfAddrPpApiRoot) + "/pp/{ppId}",
        [&verifier, &af_addr_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto d = af_addr_pp.get(kPpNamespace, req.path_params.at("ppId"));
            if (!d.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such addressing parameter provisioning");
            }
            return sbi_core::http2::Response::json(200, d->dump());
        });

    server.add_route(
        "PUT",
        std::string(kAfAddrPpApiRoot) + "/pp/{ppId}",
        [&verifier, &af_addr_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::AddrPpData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            if (!af_addr_pp.put(kPpNamespace, req.path_params.at("ppId"), j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such addressing parameter provisioning");
            }
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "PATCH",
        std::string(kAfAddrPpApiRoot) + "/pp/{ppId}",
        [&verifier, &af_addr_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            if (!sbi_core::http2::parse_json_body<sbi_gen::AddrPpDataPatch>(req, err).has_value()) {
                return err;
            }
            auto patched = af_addr_pp.merge_patch(
                kPpNamespace, req.path_params.at("ppId"), json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such addressing parameter provisioning");
            }
            return sbi_core::http2::Response::json(200, patched->dump());
        });

    server.add_route(
        "DELETE",
        std::string(kAfAddrPpApiRoot) + "/pp/{ppId}",
        [&verifier, &af_addr_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            if (!af_addr_pp.remove(kPpNamespace, req.path_params.at("ppId"))) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such addressing parameter provisioning");
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfSlicePpApiRoot) + "/pp",
        [&verifier, &af_slice_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& d : af_slice_pp.list(kPpNamespace)) {
                out.push_back(d);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAfSlicePpApiRoot) + "/pp",
        [&verifier, &af_slice_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::SlicePpData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            const auto id = af_slice_pp.create(kPpNamespace, j);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location", std::string(kAfSlicePpApiRoot) + "/pp/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfSlicePpApiRoot) + "/pp/{ppId}",
        [&verifier, &af_slice_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto d = af_slice_pp.get(kPpNamespace, req.path_params.at("ppId"));
            if (!d.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such slice parameter provisioning");
            }
            return sbi_core::http2::Response::json(200, d->dump());
        });

    server.add_route(
        "PUT",
        std::string(kAfSlicePpApiRoot) + "/pp/{ppId}",
        [&verifier, &af_slice_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::SlicePpData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            if (!af_slice_pp.put(kPpNamespace, req.path_params.at("ppId"), j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such slice parameter provisioning");
            }
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "PATCH",
        std::string(kAfSlicePpApiRoot) + "/pp/{ppId}",
        [&verifier, &af_slice_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            if (!sbi_core::http2::parse_json_body<sbi_gen::SlicePpDataPatch>(req, err)
                     .has_value()) {
                return err;
            }
            auto patched = af_slice_pp.merge_patch(
                kPpNamespace, req.path_params.at("ppId"), json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such slice parameter provisioning");
            }
            return sbi_core::http2::Response::json(200, patched->dump());
        });

    server.add_route(
        "DELETE",
        std::string(kAfSlicePpApiRoot) + "/pp/{ppId}",
        [&verifier, &af_slice_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            if (!af_slice_pp.remove(kPpNamespace, req.path_params.at("ppId"))) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such slice parameter provisioning");
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfGroupPpApiRoot) + "/pp",
        [&verifier, &af_group_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& d : af_group_pp.list(kPpNamespace)) {
                out.push_back(d);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAfGroupPpApiRoot) + "/pp",
        [&verifier, &af_group_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::GrpPpData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            const auto id = af_group_pp.create(kPpNamespace, j);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location", std::string(kAfGroupPpApiRoot) + "/pp/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfGroupPpApiRoot) + "/pp/{ppId}",
        [&verifier, &af_group_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto d = af_group_pp.get(kPpNamespace, req.path_params.at("ppId"));
            if (!d.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such group parameter provisioning");
            }
            return sbi_core::http2::Response::json(200, d->dump());
        });

    server.add_route(
        "PUT",
        std::string(kAfGroupPpApiRoot) + "/pp/{ppId}",
        [&verifier, &af_group_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::GrpPpData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            if (!af_group_pp.put(kPpNamespace, req.path_params.at("ppId"), j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such group parameter provisioning");
            }
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "PATCH",
        std::string(kAfGroupPpApiRoot) + "/pp/{ppId}",
        [&verifier, &af_group_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            if (!sbi_core::http2::parse_json_body<sbi_gen::GrpPpDataPatch>(req, err).has_value()) {
                return err;
            }
            auto patched = af_group_pp.merge_patch(
                kPpNamespace, req.path_params.at("ppId"), json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such group parameter provisioning");
            }
            return sbi_core::http2::Response::json(200, patched->dump());
        });

    server.add_route(
        "DELETE",
        std::string(kAfGroupPpApiRoot) + "/pp/{ppId}",
        [&verifier, &af_group_pp](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            if (!af_group_pp.remove(kPpNamespace, req.path_params.at("ppId"))) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such group parameter provisioning");
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- ADR-0322: three more AF-facing provisioning services (TS 29.522) ---
    //
    // IPTVConfiguration, LpiParameterProvision and ACSParameterProvision. All three are per-AF
    // provisioned documents with identical lifecycles, which is why they share one store class
    // rather than getting three near-identical ones.
    //
    // Only IPTV has a downstream: `application-data/iptvConfigData` is a real UDR resource this
    // project already serves, so IPTV configurations are provisioned there. LPI (Local Positioning
    // Information) and ACS (Auto-Configuration Server) parameters have NO UDR counterpart in this
    // project's data model -- they are accepted and stored at NEF, and nothing downstream applies
    // them. That is disclosed rather than dressed up: the API is real and conformant, the effect
    // is not there yet.

    server.add_route(
        "GET",
        std::string(kAfIptvApiRoot) + "/{afId}/configurations",
        [&verifier, &af_iptv_configs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& d : af_iptv_configs.list(req.path_params.at("afId"))) {
                out.push_back(d);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAfIptvApiRoot) + "/{afId}/configurations",
        [&verifier, &af_iptv_configs, &udr_client, udr_base_url, this_unused = 0](
            const sbi_core::http2::Request& req) {
            (void)this_unused;
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::IptvConfigData_IPTVConfiguration>(
                req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto af_id = req.path_params.at("afId");
            json j = *body;
            const auto id = af_iptv_configs.create(af_id, j);
            // ADR-0322: IPTV configurations have a real UDR home
            // (application-data/iptvConfigData), so they are provisioned there rather than kept
            // only at NEF. The other two services in this batch have no UDR counterpart -- see
            // their own comments.
            {
                sbi_core::http2::ClientRequest udr_req;
                udr_req.method = "PUT";
                udr_req.url = udr_base_url + "/nudr-dr/v2/application-data/iptvConfigData/" +
                              af_id + "-" + id;
                udr_req.headers.emplace("content-type", "application/json");
                udr_req.body = j.dump();
                if (auto r = udr_client.send(udr_req); !r.has_value() || r->status >= 300) {
                    spdlog::warn(
                        "nef: IPTV configuration for {}/{} did not reach UDR -- stored at NEF, "
                        "but nothing in the core will apply it",
                        af_id,
                        id);
                }
            }
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace(
                "location", std::string(kAfIptvApiRoot) + "/" + af_id + "/configurations/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfIptvApiRoot) + "/{afId}/configurations/{configurationId}",
        [&verifier, &af_iptv_configs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto d = af_iptv_configs.get(req.path_params.at("afId"),
                                         req.path_params.at("configurationId"));
            if (!d.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such IPTV configuration");
            }
            return sbi_core::http2::Response::json(200, d->dump());
        });

    server.add_route(
        "PUT",
        std::string(kAfIptvApiRoot) + "/{afId}/configurations/{configurationId}",
        [&verifier, &af_iptv_configs, &udr_client, udr_base_url, this_unused = 0](
            const sbi_core::http2::Request& req) {
            (void)this_unused;
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::IptvConfigData_IPTVConfiguration>(
                req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto af_id = req.path_params.at("afId");
            const auto id = req.path_params.at("configurationId");
            json j = *body;
            if (!af_iptv_configs.put(af_id, id, j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such IPTV configuration");
            }
            // ADR-0322: IPTV configurations have a real UDR home
            // (application-data/iptvConfigData), so they are provisioned there rather than kept
            // only at NEF. The other two services in this batch have no UDR counterpart -- see
            // their own comments.
            {
                sbi_core::http2::ClientRequest udr_req;
                udr_req.method = "PUT";
                udr_req.url = udr_base_url + "/nudr-dr/v2/application-data/iptvConfigData/" +
                              af_id + "-" + id;
                udr_req.headers.emplace("content-type", "application/json");
                udr_req.body = j.dump();
                if (auto r = udr_client.send(udr_req); !r.has_value() || r->status >= 300) {
                    spdlog::warn(
                        "nef: IPTV configuration for {}/{} did not reach UDR -- stored at NEF, "
                        "but nothing in the core will apply it",
                        af_id,
                        id);
                }
            }
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "PATCH",
        std::string(kAfIptvApiRoot) + "/{afId}/configurations/{configurationId}",
        [&verifier, &af_iptv_configs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            if (!sbi_core::http2::parse_json_body<sbi_gen::IptvConfigDataPatch>(req, err)
                     .has_value()) {
                return err;
            }
            auto patched = af_iptv_configs.merge_patch(req.path_params.at("afId"),
                                                       req.path_params.at("configurationId"),
                                                       json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such IPTV configuration");
            }
            return sbi_core::http2::Response::json(200, patched->dump());
        });

    server.add_route(
        "DELETE",
        std::string(kAfIptvApiRoot) + "/{afId}/configurations/{configurationId}",
        [&verifier, &af_iptv_configs, &udr_client, udr_base_url, this_unused = 0](
            const sbi_core::http2::Request& req) {
            (void)this_unused;
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto af_id = req.path_params.at("afId");
            const auto id = req.path_params.at("configurationId");
            if (!af_iptv_configs.remove(af_id, id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such IPTV configuration");
            }
            {
                sbi_core::http2::ClientRequest del;
                del.method = "DELETE";
                del.url = udr_base_url + "/nudr-dr/v2/application-data/iptvConfigData/" + af_id +
                          "-" + id;
                if (auto r = udr_client.send(del); !r.has_value() || r->status >= 300) {
                    spdlog::warn(
                        "nef: IPTV configuration for {}/{} may still be applied -- UDR delete "
                        "failed",
                        af_id,
                        id);
                }
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfLpiApiRoot) + "/{afId}/provisionedLpis",
        [&verifier, &af_lpi_provisionings](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& d : af_lpi_provisionings.list(req.path_params.at("afId"))) {
                out.push_back(d);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAfLpiApiRoot) + "/{afId}/provisionedLpis",
        [&verifier, &af_lpi_provisionings, this_unused = 0](const sbi_core::http2::Request& req) {
            (void)this_unused;
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::LpiParametersProvision>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto af_id = req.path_params.at("afId");
            json j = *body;
            const auto id = af_lpi_provisionings.create(af_id, j);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace(
                "location", std::string(kAfLpiApiRoot) + "/" + af_id + "/provisionedLpis/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfLpiApiRoot) + "/{afId}/provisionedLpis/{provisionedLpiId}",
        [&verifier, &af_lpi_provisionings](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto d = af_lpi_provisionings.get(req.path_params.at("afId"),
                                              req.path_params.at("provisionedLpiId"));
            if (!d.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such LPI provisioning");
            }
            return sbi_core::http2::Response::json(200, d->dump());
        });

    server.add_route(
        "PUT",
        std::string(kAfLpiApiRoot) + "/{afId}/provisionedLpis/{provisionedLpiId}",
        [&verifier, &af_lpi_provisionings, this_unused = 0](const sbi_core::http2::Request& req) {
            (void)this_unused;
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::LpiParametersProvision>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto af_id = req.path_params.at("afId");
            const auto id = req.path_params.at("provisionedLpiId");
            json j = *body;
            if (!af_lpi_provisionings.put(af_id, id, j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such LPI provisioning");
            }
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "PATCH",
        std::string(kAfLpiApiRoot) + "/{afId}/provisionedLpis/{provisionedLpiId}",
        [&verifier, &af_lpi_provisionings](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            if (!sbi_core::http2::parse_json_body<sbi_gen::LpiParametersProvisionPatch>(req, err)
                     .has_value()) {
                return err;
            }
            auto patched = af_lpi_provisionings.merge_patch(req.path_params.at("afId"),
                                                            req.path_params.at("provisionedLpiId"),
                                                            json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such LPI provisioning");
            }
            return sbi_core::http2::Response::json(200, patched->dump());
        });

    server.add_route(
        "DELETE",
        std::string(kAfLpiApiRoot) + "/{afId}/provisionedLpis/{provisionedLpiId}",
        [&verifier, &af_lpi_provisionings, this_unused = 0](const sbi_core::http2::Request& req) {
            (void)this_unused;
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto af_id = req.path_params.at("afId");
            const auto id = req.path_params.at("provisionedLpiId");
            if (!af_lpi_provisionings.remove(af_id, id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such LPI provisioning");
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfAcsApiRoot) + "/{afId}/subscriptions",
        [&verifier, &af_acs_subscriptions](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& d : af_acs_subscriptions.list(req.path_params.at("afId"))) {
                out.push_back(d);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAfAcsApiRoot) + "/{afId}/subscriptions",
        [&verifier, &af_acs_subscriptions, this_unused = 0](const sbi_core::http2::Request& req) {
            (void)this_unused;
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::AcsConfigurationData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto af_id = req.path_params.at("afId");
            json j = *body;
            const auto id = af_acs_subscriptions.create(af_id, j);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kAfAcsApiRoot) + "/" + af_id + "/subscriptions/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfAcsApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_acs_subscriptions](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto d = af_acs_subscriptions.get(req.path_params.at("afId"),
                                              req.path_params.at("subscriptionId"));
            if (!d.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such ACS configuration");
            }
            return sbi_core::http2::Response::json(200, d->dump());
        });

    server.add_route(
        "PUT",
        std::string(kAfAcsApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_acs_subscriptions, this_unused = 0](const sbi_core::http2::Request& req) {
            (void)this_unused;
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::AcsConfigurationData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto af_id = req.path_params.at("afId");
            const auto id = req.path_params.at("subscriptionId");
            json j = *body;
            if (!af_acs_subscriptions.put(af_id, id, j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such ACS configuration");
            }
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "PATCH",
        std::string(kAfAcsApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_acs_subscriptions](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            if (!sbi_core::http2::parse_json_body<sbi_gen::AcsConfigurationDataPatch>(req, err)
                     .has_value()) {
                return err;
            }
            auto patched = af_acs_subscriptions.merge_patch(req.path_params.at("afId"),
                                                            req.path_params.at("subscriptionId"),
                                                            json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such ACS configuration");
            }
            return sbi_core::http2::Response::json(200, patched->dump());
        });

    server.add_route(
        "DELETE",
        std::string(kAfAcsApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_acs_subscriptions, this_unused = 0](const sbi_core::http2::Request& req) {
            (void)this_unused;
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto af_id = req.path_params.at("afId");
            const auto id = req.path_params.at("subscriptionId");
            if (!af_acs_subscriptions.remove(af_id, id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such ACS configuration");
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- ADR-0321: TS 29.522 ServiceParameter, AF service-parameter provisioning ---
    //
    // Fifth of the 57, and a straightforward broker: an AF provisions service parameters that the
    // core network should apply to its traffic, and UDR's `application-data/serviceParamData` is
    // where the 5GC keeps them. Same principle as ADR-0302 -- parameters an AF provisions that
    // never reach UDR are parameters nothing applies.
    //
    // Real, disclosed field mapping. `ServiceParameterData` (AF-facing, TS 29.522) and
    // `ServiceParameterData_Application_Data` (UDR-facing, TS 29.519) share their identifying
    // fields, so the carried set is exactly the intersection and nothing is synthesised:
    // afServiceId, appId, dnn, snssai, externalGroupId, anyUeInd, gpsi, ueIpv4, ueIpv6, ueMac,
    // paramOverPc5, paramOverUu.
    //
    // NOT carried: `notificationDestination`/`requestTestNotification`/`websockNotifConfig` are
    // NEF-side subscription machinery, not service parameters, and have no UDR counterpart --
    // sending them would put NEF's own plumbing into the core network's data store.

    server.add_route(
        "GET",
        std::string(kAfServiceParamApiRoot) + "/{afId}/subscriptions",
        [&verifier, &af_service_param_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& sub : af_service_param_subs.list(req.path_params.at("afId"))) {
                out.push_back(sub);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAfServiceParamApiRoot) + "/{afId}/subscriptions",
        [&verifier, &af_service_param_subs, &udr_client, udr_base_url](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::ServiceParameterData_ServiceParameter>(
                    req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto af_id = req.path_params.at("afId");
            json j = *body;
            const auto sub_id = af_service_param_subs.create(af_id, j);

            // Only the fields UDR's own schema shares are sent; see this block's header.
            json udr_body = json::object();
            for (const char* field : {"afServiceId",
                                      "appId",
                                      "dnn",
                                      "snssai",
                                      "externalGroupId",
                                      "anyUeInd",
                                      "gpsi",
                                      "ueIpv4",
                                      "ueIpv6",
                                      "ueMac",
                                      "paramOverPc5",
                                      "paramOverUu"}) {
                if (j.contains(field)) {
                    udr_body[field] = j[field];
                }
            }

            sbi_core::http2::ClientRequest udr_req;
            udr_req.method = "PUT";
            udr_req.url = udr_base_url + "/nudr-dr/v2/application-data/serviceParamData/" + af_id +
                          "-" + sub_id;
            udr_req.headers.emplace("content-type", "application/json");
            udr_req.body = udr_body.dump();
            if (auto r = udr_client.send(udr_req); !r.has_value() || r->status >= 300) {
                spdlog::warn("nef: service parameters for {}/{} did not reach UDR (status={}) -- "
                             "the subscription exists at NEF but nothing will apply them",
                             af_id,
                             sub_id,
                             r.has_value() ? r->status : 0);
            } else {
                spdlog::info("nef: service parameters for {}/{} provisioned to UDR", af_id, sub_id);
            }

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kAfServiceParamApiRoot) + "/" + af_id +
                                     "/subscriptions/" + sub_id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfServiceParamApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_service_param_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto sub = af_service_param_subs.get(req.path_params.at("afId"),
                                                 req.path_params.at("subscriptionId"));
            if (!sub.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such service parameter subscription");
            }
            return sbi_core::http2::Response::json(200, sub->dump());
        });

    server.add_route(
        "PUT",
        std::string(kAfServiceParamApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_service_param_subs, &udr_client, udr_base_url](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::ServiceParameterData_ServiceParameter>(
                    req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto af_id = req.path_params.at("afId");
            const auto sub_id = req.path_params.at("subscriptionId");
            json j = *body;
            if (!af_service_param_subs.put(af_id, sub_id, j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such service parameter subscription");
            }
            // Unlike the QoS and monitoring slices, replacing here IS re-provisioned to UDR: the
            // UDR resource is a plain PUT-replaceable document with the same shape, so there is no
            // second translation to get wrong.
            json udr_body = json::object();
            for (const char* field : {"afServiceId",
                                      "appId",
                                      "dnn",
                                      "snssai",
                                      "externalGroupId",
                                      "anyUeInd",
                                      "gpsi",
                                      "ueIpv4",
                                      "ueIpv6",
                                      "ueMac",
                                      "paramOverPc5",
                                      "paramOverUu"}) {
                if (j.contains(field)) {
                    udr_body[field] = j[field];
                }
            }
            sbi_core::http2::ClientRequest udr_req;
            udr_req.method = "PUT";
            udr_req.url = udr_base_url + "/nudr-dr/v2/application-data/serviceParamData/" + af_id +
                          "-" + sub_id;
            udr_req.headers.emplace("content-type", "application/json");
            udr_req.body = udr_body.dump();
            if (auto r = udr_client.send(udr_req); !r.has_value() || r->status >= 300) {
                spdlog::warn(
                    "nef: replaced service parameters for {}/{} did not reach UDR", af_id, sub_id);
            }
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "PATCH",
        std::string(kAfServiceParamApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_service_param_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto patched = af_service_param_subs.merge_patch(req.path_params.at("afId"),
                                                             req.path_params.at("subscriptionId"),
                                                             json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such service parameter subscription");
            }
            return sbi_core::http2::Response::json(200, patched->dump());
        });

    server.add_route(
        "DELETE",
        std::string(kAfServiceParamApiRoot) + "/{afId}/subscriptions/{subscriptionId}",
        [&verifier, &af_service_param_subs, &udr_client, udr_base_url](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto af_id = req.path_params.at("afId");
            const auto sub_id = req.path_params.at("subscriptionId");
            if (!af_service_param_subs.remove(af_id, sub_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such service parameter subscription");
            }
            // Parameters must stop applying when the AF withdraws them.
            sbi_core::http2::ClientRequest del;
            del.method = "DELETE";
            del.url = udr_base_url + "/nudr-dr/v2/application-data/serviceParamData/" + af_id +
                      "-" + sub_id;
            if (auto r = udr_client.send(del); !r.has_value() || r->status >= 300) {
                spdlog::warn("nef: service parameters for {}/{} may still be applied -- UDR "
                             "delete failed",
                             af_id,
                             sub_id);
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- ADR-0319: TS 29.522 UEId, the AF-facing UE identifier API ---
    //
    // Fourth of the 57, and the file splits cleanly in two:
    //
    //   * THREE LOOKUPS (`/retrieve`, `/get-msisdn`, `/verify-msisdn`) ask "who is the UE at this
    //     IP address / does this MSISDN belong to them". Answering needs an IP-to-identity source
    //     -- a UPF session lookup or an H-NEF mapping database -- which this project does not
    //     have. `nnef-ueid/v1/fetch` (the NF-facing twin) already discloses exactly this and
    //     answers 204. These answer **404**, which the real YAML defines for all three, rather
    //     than fabricating an identity or -- worse for `/verify-msisdn` -- returning
    //     `verifResult: false`, which would ASSERT that an MSISDN does not belong to a UE when the
    //     truth is that nothing here can tell.
    //
    //   * SIX PROVISIONING OPERATIONS manage `UeIdMappingInfo`, which is a ProSe/Ranging
    //     application-layer-id <-> GPSI pair. That is a DIFFERENT mapping from the one the lookups
    //     need -- checked rather than assumed, after an initial plan to have provisioning feed the
    //     lookups turned out to be based on a misreading of the schema. These are real,
    //     store-backed and complete.

    server.add_route(
        "POST",
        std::string(kAfUeIdApiRoot) + "/retrieve",
        [&verifier](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            if (!sbi_core::http2::parse_json_body<sbi_gen::UeIdReq_UEId>(req, err).has_value()) {
                return err;
            }
            return sbi_core::http2::problem_response(
                404,
                "No UE identity available",
                "this deployment has no IP-to-identity source (no UPF session lookup, no H-NEF "
                "mapping database), so no external identifier can be returned for the supplied "
                "address");
        });

    server.add_route(
        "POST",
        std::string(kAfUeIdApiRoot) + "/get-msisdn",
        [&verifier](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            if (!sbi_core::http2::parse_json_body<sbi_gen::MsisdnReq>(req, err).has_value()) {
                return err;
            }
            return sbi_core::http2::problem_response(
                404,
                "No MSISDN available",
                "this deployment has no IP-to-identity source, so no MSISDN can be returned for "
                "the supplied address");
        });

    server.add_route(
        "POST",
        std::string(kAfUeIdApiRoot) + "/verify-msisdn",
        [&verifier](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            if (!sbi_core::http2::parse_json_body<sbi_gen::MsisdnVerifReq>(req, err).has_value()) {
                return err;
            }
            // Deliberately NOT `verifResult: false`. That would tell the AF the MSISDN does not
            // belong to the UE, which is a claim; the truth is that nothing here can determine it,
            // and an AF acting on a false negative could deny a legitimate user.
            return sbi_core::http2::problem_response(
                404,
                "MSISDN cannot be verified",
                "this deployment has no IP-to-identity source, so the supplied MSISDN can be "
                "neither confirmed nor denied for the supplied address");
        });

    server.add_route(
        "GET",
        std::string(kAfUeIdApiRoot) + "/{afId}/pp",
        [&verifier, &af_ueid_mappings](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& m : af_ueid_mappings.list(req.path_params.at("afId"))) {
                out.push_back(m);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAfUeIdApiRoot) + "/{afId}/pp",
        [&verifier, &af_ueid_mappings](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::UeIdMappingInfo>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto af_id = req.path_params.at("afId");
            json j = *body;
            const auto id = af_ueid_mappings.create(af_id, j);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kAfUeIdApiRoot) + "/" + af_id + "/pp/" + id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfUeIdApiRoot) + "/{afId}/pp/{ppId}",
        [&verifier, &af_ueid_mappings](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto m = af_ueid_mappings.get(req.path_params.at("afId"), req.path_params.at("ppId"));
            if (!m.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such UE ID mapping provisioning");
            }
            return sbi_core::http2::Response::json(200, m->dump());
        });

    server.add_route(
        "PUT",
        std::string(kAfUeIdApiRoot) + "/{afId}/pp/{ppId}",
        [&verifier, &af_ueid_mappings](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::UeIdMappingInfo>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            if (!af_ueid_mappings.put(req.path_params.at("afId"), req.path_params.at("ppId"), j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such UE ID mapping provisioning");
            }
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "PATCH",
        std::string(kAfUeIdApiRoot) + "/{afId}/pp/{ppId}",
        [&verifier, &af_ueid_mappings](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            if (!sbi_core::http2::parse_json_body<sbi_gen::UeIdMappingInfoPatch>(req, err)
                     .has_value()) {
                return err;
            }
            auto patched = af_ueid_mappings.merge_patch(
                req.path_params.at("afId"), req.path_params.at("ppId"), json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such UE ID mapping provisioning");
            }
            return sbi_core::http2::Response::json(200, patched->dump());
        });

    server.add_route(
        "DELETE",
        std::string(kAfUeIdApiRoot) + "/{afId}/pp/{ppId}",
        [&verifier, &af_ueid_mappings](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            if (!af_ueid_mappings.remove(req.path_params.at("afId"), req.path_params.at("ppId"))) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such UE ID mapping provisioning");
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- ADR-0318: QoS event delivery, completing what ADR-0315 wired only halfway ---
    //
    // ADR-0315 told PCF to notify NEF and ADR-0317 built delivery for monitoring, but the QoS
    // callback had no route -- so PCF's events reached NEF's door and stopped. Same mechanism,
    // second event source.
    server.add_route(
        "POST",
        "/nnef-callback/v1/qos-notify/{afId}/{subscriptionId}",
        [&af_qos_subs, &af_client, self_base_url](const sbi_core::http2::Request& req) {
            const auto af_id = req.path_params.at("afId");
            const auto sub_id = req.path_params.at("subscriptionId");
            const auto subscription = af_qos_subs.get(af_id, sub_id);
            if (!subscription.has_value()) {
                // Tells PCF the callback is dead rather than absorbing events for a subscription
                // the AF has deleted.
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such AsSessionWithQoS subscription");
            }
            const auto destination = subscription->value("notificationDestination", std::string{});
            if (destination.empty()) {
                spdlog::warn("nef: QoS event for {}/{} has nowhere to go -- the AF subscription "
                             "carries no notificationDestination",
                             af_id,
                             sub_id);
                sbi_core::http2::Response accepted;
                accepted.status = 204;
                return accepted;
            }

            // The AF subscribed through TS 29.122 and is entitled to that API's own shape, so the
            // notification names the AF's OWN resource rather than PCF's app-session.
            json payload;
            payload["subscription"] = self_base_url + std::string(kAfAsSessionWithQosApiRoot) +
                                      "/" + af_id + "/subscriptions/" + sub_id;
            // Same disclosure as ADR-0317: PCF's EventsNotification is carried through rather than
            // re-modelled into TS 29.122's own event types, because this build does not have that
            // field mapping and inventing one would show an AF values it never reported.
            try {
                payload["_npcfEventsNotification"] = json::parse(req.body);
            } catch (const json::exception&) {
                // A malformed body from PCF is not the AF's problem; send what NEF can vouch for.
            }

            sbi_core::http2::ClientRequest out;
            out.method = "POST";
            out.url = destination;
            out.headers.emplace("content-type", "application/json");
            out.body = payload.dump();
            if (auto resp = af_client.send(out); !resp.has_value() || resp->status >= 300) {
                spdlog::warn("nef: delivering a QoS event to AF {} at {} failed (status={})",
                             af_id,
                             destination,
                             resp.has_value() ? resp->status : 0);
            } else {
                spdlog::info(
                    "nef: QoS event delivered to AF {} for subscription {}", af_id, sub_id);
            }

            sbi_core::http2::Response accepted;
            accepted.status = 204;
            return accepted;
        });

    // --- ADR-0317: notification delivery, the piece every AF-facing service was missing ---
    //
    // ADR-0302, ADR-0315 and ADR-0316 each accepted `notificationDestination` and stored it, and
    // each disclosed the same gap: NEF delivered NOTHING. UDM and PCF dutifully call NEF's
    // callback URIs and the reports stopped there, so an AF that subscribed correctly was never
    // told anything. This closes it once for every service rather than per-service.
    //
    // Correlation is by URI: the callback NEF hands to UDM carries the AF id and subscription id
    // in its own path, and UDM calls back exactly that URI. Nothing to keep in sync.
    server.add_route(
        "POST",
        "/nnef-callback/v1/monitoring-notify/{afId}/{subscriptionId}",
        [&af_monitoring_subs, &af_client, self_base_url](const sbi_core::http2::Request& req) {
            // Deliberately NOT bearer-checked: the caller here is UDM over mTLS, and this URI is
            // one NEF generated and handed out itself. Requiring an OAuth2 token would mean UDM's
            // event-exposure path needing an NEF-scoped token it has no reason to hold.
            const auto af_id = req.path_params.at("afId");
            const auto sub_id = req.path_params.at("subscriptionId");
            const auto subscription = af_monitoring_subs.get(af_id, sub_id);
            if (!subscription.has_value()) {
                // The AF deleted its subscription but UDM has not stopped reporting yet. 404 tells
                // UDM this callback is dead rather than silently accepting reports for nobody.
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such monitoring subscription");
            }
            const auto destination = subscription->value("notificationDestination", std::string{});
            if (destination.empty()) {
                spdlog::warn("nef: monitoring report for {}/{} has nowhere to go -- the AF "
                             "subscription carries no notificationDestination",
                             af_id,
                             sub_id);
                sbi_core::http2::Response accepted;
                accepted.status = 204;
                return accepted;
            }

            // Rebuilt as the AF-facing TS 29.122 shape rather than forwarded verbatim: UDM sends a
            // Nudm_EE MonitoredResourceReport, and an AF is entitled to the API it subscribed
            // through. `subscription` points at the AF's OWN resource, not UDM's.
            sbi_gen::MonitoringNotification notification{};
            notification.subscription = self_base_url + std::string(kAfMonitoringEventApiRoot) +
                                        "/" + af_id + "/subscriptions/" + sub_id;

            json payload = notification;
            // The event content UDM reported is carried through under its own key rather than
            // being re-modelled: this build does not map every Nudm_EE report field onto
            // TS 29.122's MonitoringEventReport, and inventing that mapping would put fabricated
            // values in front of an AF. Stated here rather than silently dropped.
            try {
                payload["_nudmEeReport"] = json::parse(req.body);
            } catch (const json::exception&) {
                // A malformed body from UDM is not the AF's problem; the notification still goes
                // out, carrying only what NEF can vouch for.
            }

            sbi_core::http2::ClientRequest out;
            out.method = "POST";
            out.url = destination;
            out.headers.emplace("content-type", "application/json");
            out.body = payload.dump();
            if (auto resp = af_client.send(out); !resp.has_value() || resp->status >= 300) {
                spdlog::warn("nef: delivering a monitoring report to AF {} at {} failed "
                             "(status={})",
                             af_id,
                             destination,
                             resp.has_value() ? resp->status : 0);
            } else {
                spdlog::info(
                    "nef: monitoring report delivered to AF {} for subscription {}", af_id, sub_id);
            }

            sbi_core::http2::Response accepted;
            accepted.status = 204;
            return accepted;
        });

    // --- ADR-0316: TS 29.122 MonitoringEvent, the AF-facing event-monitoring API ---
    //
    // Third of the 57. Brokers to UDM's Nudm_EE ee-subscriptions, which is what actually reports
    // reachability, loss-of-connectivity, location and roaming events. Two enums that overlap but
    // differ (see monitoring_type_to_ee_event) and two identifier spaces that differ (see
    // af_target_to_gpsi) sit between the AF's request and UDM's -- which is the substance of this
    // slice, not the CRUD around it.

    server.add_route(
        "GET",
        std::string(kAfMonitoringEventApiRoot) + "/{scsAsId}/subscriptions",
        [&verifier, &af_monitoring_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& sub : af_monitoring_subs.list(req.path_params.at("scsAsId"))) {
                out.push_back(sub);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAfMonitoringEventApiRoot) + "/{scsAsId}/subscriptions",
        [&verifier, &af_monitoring_subs, &udr_client, udm_base_url, self_base_url](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::MonitoringEventSubscription>(req, err);
            if (!body.has_value()) {
                return err;
            }

            // A monitoring type with no Nudm_EE counterpart is refused rather than stored: a
            // subscription that can never report is worse than an explicit rejection.
            const auto ee_event = monitoring_type_to_ee_event(*body);
            if (!ee_event.has_value()) {
                return sbi_core::http2::problem_response(
                    400,
                    "Unsupported monitoringType",
                    body->monitoringType.value + " has no Nudm_EE event counterpart in this build");
            }

            const auto af_id = req.path_params.at("scsAsId");
            json j = *body;
            const auto sub_id = af_monitoring_subs.create(af_id, j);

            const auto gpsi = af_target_to_gpsi(*body);
            if (!gpsi.has_value()) {
                // A group subscription, or one identifying no UE this route can address. Stored,
                // and said plainly -- not silently treated as monitoring something.
                spdlog::warn("nef: monitoring subscription {} for AF {} names no single UE this "
                             "build can address (group subscriptions are not expanded) -- stored "
                             "at NEF, NOT brokered to UDM",
                             sub_id,
                             af_id);
            } else {
                sbi_gen::MonitoringConfiguration config{};
                config.eventType.value = *ee_event;
                sbi_gen::EeSubscription ee{};
                // UDM reports to NEF, which then notifies the AF -- the same reason ADR-0315 points
                // PCF at NEF rather than at the AF.
                // ADR-0317: the callback carries WHICH AF subscription it belongs to. UDM echoes
                // the URI it was given, so encoding the ids in the path is enough to correlate an
                // inbound report back to the AF that asked for it -- no correlation-id state to
                // keep in sync, and no lookup that could go stale.
                ee.callbackReference =
                    self_base_url + "/nnef-callback/v1/monitoring-notify/" + af_id + "/" + sub_id;
                ee.monitoringConfigurations = json{{"0", config}};
                if (body->maximumNumberOfReports.has_value()) {
                    sbi_gen::ReportingOptions options{};
                    options.maxNumOfReports = body->maximumNumberOfReports;
                    ee.reportingOptions = options;
                }

                sbi_core::http2::ClientRequest udm_req;
                udm_req.method = "POST";
                udm_req.url = udm_base_url + "/nudm-ee/v1/" + *gpsi + "/ee-subscriptions";
                udm_req.headers.emplace("content-type", "application/json");
                udm_req.body = json(ee).dump();
                auto udm_resp = udr_client.send(udm_req);
                if (udm_resp.has_value() && udm_resp->status < 300) {
                    if (const auto loc = udm_resp->headers.find("location");
                        loc != udm_resp->headers.end()) {
                        const auto slash = loc->second.rfind('/');
                        if (slash != std::string::npos) {
                            af_monitoring_subs.set_ee_subscription(
                                af_id, sub_id, *gpsi, loc->second.substr(slash + 1));
                        }
                    }
                    spdlog::info("nef: monitoring subscription {} brokered to UDM for {} "
                                 "(eventType={})",
                                 sub_id,
                                 *gpsi,
                                 *ee_event);
                } else {
                    spdlog::warn("nef: UDM ee-subscription failed for {} (status={}) -- the AF's "
                                 "monitoring request is recorded but NOTHING will report on it",
                                 *gpsi,
                                 udm_resp.has_value() ? udm_resp->status : 0);
                }
            }

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kAfMonitoringEventApiRoot) + "/" + af_id +
                                     "/subscriptions/" + sub_id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfMonitoringEventApiRoot) + "/{scsAsId}/subscriptions/{subscriptionId}",
        [&verifier, &af_monitoring_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto sub = af_monitoring_subs.get(req.path_params.at("scsAsId"),
                                              req.path_params.at("subscriptionId"));
            if (!sub.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such monitoring subscription");
            }
            return sbi_core::http2::Response::json(200, sub->dump());
        });

    server.add_route(
        "PUT",
        std::string(kAfMonitoringEventApiRoot) + "/{scsAsId}/subscriptions/{subscriptionId}",
        [&verifier, &af_monitoring_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::MonitoringEventSubscription>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            if (!af_monitoring_subs.put(
                    req.path_params.at("scsAsId"), req.path_params.at("subscriptionId"), j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such monitoring subscription");
            }
            // Disclosed, same shape as ADR-0315's PUT: the UDM ee-subscription is not updated to
            // match. Nudm_EE modifies via PATCH with its own shape; approximating it here would
            // risk a subscription reporting a different event than the AF now asks for.
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "PATCH",
        std::string(kAfMonitoringEventApiRoot) + "/{scsAsId}/subscriptions/{subscriptionId}",
        [&verifier, &af_monitoring_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto patched = af_monitoring_subs.merge_patch(req.path_params.at("scsAsId"),
                                                          req.path_params.at("subscriptionId"),
                                                          json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such monitoring subscription");
            }
            return sbi_core::http2::Response::json(200, patched->dump());
        });

    server.add_route(
        "DELETE",
        std::string(kAfMonitoringEventApiRoot) + "/{scsAsId}/subscriptions/{subscriptionId}",
        [&verifier, &af_monitoring_subs, &udr_client, udm_base_url](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto af_id = req.path_params.at("scsAsId");
            const auto sub_id = req.path_params.at("subscriptionId");
            const auto ee = af_monitoring_subs.ee_subscription(af_id, sub_id);
            if (!af_monitoring_subs.remove(af_id, sub_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such monitoring subscription");
            }
            // UDM must stop reporting: an ee-subscription outliving the AF request that created it
            // would keep pushing events to a NEF callback for a subscription that no longer exists.
            if (ee.has_value()) {
                sbi_core::http2::ClientRequest del;
                del.method = "DELETE";
                del.url =
                    udm_base_url + "/nudm-ee/v1/" + ee->first + "/ee-subscriptions/" + ee->second;
                if (auto r = udr_client.send(del); !r.has_value() || r->status >= 300) {
                    spdlog::warn("nef: UDM ee-subscription {} could not be deleted -- UDM may keep "
                                 "reporting events for a subscription the AF has removed",
                                 ee->second);
                }
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- ADR-0315: TS 29.122 AsSessionWithQoS, the AF-facing QoS request API ---
    //
    // Second of the 57 AF-facing services. Six operations on the same collection/item shape
    // TrafficInfluence uses, and the same principle: this BROKERS. An AF's QoS request becomes a
    // real PCF application session, because PCF is what installs QoS -- a NEF that records the
    // request and tells nobody has authorised nothing.

    server.add_route(
        "GET",
        std::string(kAfAsSessionWithQosApiRoot) + "/{scsAsId}/subscriptions",
        [&verifier, &af_qos_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json out = json::array();
            for (const auto& sub : af_qos_subs.list(req.path_params.at("scsAsId"))) {
                out.push_back(sub);
            }
            return sbi_core::http2::Response::json(200, out.dump());
        });

    server.add_route(
        "POST",
        std::string(kAfAsSessionWithQosApiRoot) + "/{scsAsId}/subscriptions",
        [&verifier, &af_qos_subs, &pcf_client, pcf_base_url, self_base_url](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::AsSessionWithQoSSubscription>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto af_id = req.path_params.at("scsAsId");
            json j = *body;
            const auto sub_id = af_qos_subs.create(af_id, j);

            // PCF is told where to notify NEF, not where to notify the AF: routing PCF straight to
            // the AF would bypass the exposure function, which is the one thing NEF exists to do.
            const auto app_session = create_pcf_app_session(
                pcf_client,
                pcf_base_url,
                // ADR-0318: same correlation-by-URI as ADR-0317 -- PCF echoes the notifUri it was
                // given, so the path identifies which AF subscription an event belongs to.
                self_base_url + "/nnef-callback/v1/qos-notify/" + af_id + "/" + sub_id,
                *body);
            if (app_session.has_value()) {
                af_qos_subs.set_app_session(af_id, sub_id, *app_session);
            }

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 std::string(kAfAsSessionWithQosApiRoot) + "/" + af_id +
                                     "/subscriptions/" + sub_id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        std::string(kAfAsSessionWithQosApiRoot) + "/{scsAsId}/subscriptions/{subscriptionId}",
        [&verifier, &af_qos_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto sub = af_qos_subs.get(req.path_params.at("scsAsId"),
                                       req.path_params.at("subscriptionId"));
            if (!sub.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such AsSessionWithQoS subscription");
            }
            return sbi_core::http2::Response::json(200, sub->dump());
        });

    server.add_route(
        "PUT",
        std::string(kAfAsSessionWithQosApiRoot) + "/{scsAsId}/subscriptions/{subscriptionId}",
        [&verifier, &af_qos_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::AsSessionWithQoSSubscription>(req, err);
            if (!body.has_value()) {
                return err;
            }
            json j = *body;
            if (!af_qos_subs.put(
                    req.path_params.at("scsAsId"), req.path_params.at("subscriptionId"), j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such AsSessionWithQoS subscription");
            }
            // Disclosed: the PCF app-session is NOT updated to match. Npcf_PolicyAuthorization
            // modifies a session via PATCH with its own AppSessionContextUpdateData shape, which
            // is a different translation from the one above and is not attempted here rather than
            // being approximated. A replaced subscription therefore keeps its original
            // authorisation until it is deleted.
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "PATCH",
        std::string(kAfAsSessionWithQosApiRoot) + "/{scsAsId}/subscriptions/{subscriptionId}",
        [&verifier, &af_qos_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto patch_dto =
                sbi_core::http2::parse_json_body<sbi_gen::AsSessionWithQoSSubscriptionPatch>(req,
                                                                                             err);
            if (!patch_dto.has_value()) {
                return err;
            }
            auto patched = af_qos_subs.merge_patch(req.path_params.at("scsAsId"),
                                                   req.path_params.at("subscriptionId"),
                                                   json::parse(req.body));
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such AsSessionWithQoS subscription");
            }
            return sbi_core::http2::Response::json(200, patched->dump());
        });

    server.add_route(
        "DELETE",
        std::string(kAfAsSessionWithQosApiRoot) + "/{scsAsId}/subscriptions/{subscriptionId}",
        [&verifier, &af_qos_subs, &pcf_client, pcf_base_url](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto af_id = req.path_params.at("scsAsId");
            const auto sub_id = req.path_params.at("subscriptionId");
            // Read the app-session id BEFORE removing the subscription -- remove() erases it.
            const auto app_session = af_qos_subs.app_session(af_id, sub_id);
            if (!af_qos_subs.remove(af_id, sub_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such AsSessionWithQoS subscription");
            }
            // The QoS authorisation must not outlive the request that asked for it.
            if (app_session.has_value()) {
                delete_pcf_app_session(pcf_client, pcf_base_url, *app_session);
            }
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
