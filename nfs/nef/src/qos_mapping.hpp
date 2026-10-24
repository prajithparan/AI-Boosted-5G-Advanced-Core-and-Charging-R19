#pragma once

// AF AsSessionWithQoS (TS 29.122 5.14, TS29122_AsSessionWithQoS.yaml) -> Npcf_PolicyAuthorization
// (TS 29.514, TS29514_Npcf_PolicyAuthorization.yaml), the NEF's side of TS 23.502 4.15.6.6
// steps 3 and 6 and 4.15.6.6a steps 3-4 (ADR-0519), and back: the PCF's
// Npcf_PolicyAuthorization_Notify
// -> the AF's UserPlaneNotificationData, steps 7-8 (ADR-0520). Private to nfs/nef.

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

#include "TS26510_CommonData_grp.hpp"

namespace nef {

// The PCF events (TS 29.514 AfEvent) the NEF subscribes for an AF subscription (TS 23.502
// 4.15.6.6 step 6), and the AF events it cannot subscribe for.
struct PcfEventPlan {
    std::vector<std::string> pcf_events;
    std::vector<std::string> not_subscribed;
};

// Always SUCCESSFUL_RESOURCES_ALLOCATION and FAILED_RESOURCES_ALLOCATION (step 6: "shall
// subscribe to notifications of Resource allocation status"). Of the AF's `events` (TS 29.522
// 4.4.9.2): QOS_GUARANTEED, QOS_NOT_GUARANTEED, QOS_NOT_GUARANTEED_DL/_UL -> QOS_NOTIF;
// USAGE_REPORT, ACCESS_TYPE_CHANGE and PLMN_CHG by the same name. SESSION_TERMINATION needs no
// subscription (the PCF reports a terminated session regardless). USAGE_REPORT also when the AF
// gave a usage threshold (TS 29.122 4.4.13). Every other AF event lands in `not_subscribed`.
PcfEventPlan pcf_events_for(const sbi_gen::AsSessionWithQoSSubscription& sub);

// The part of the PCF app-session an AF subscription determines, as AppSessionContextReqData
// members: `medComponents` (one media component, number 1: `qosReference`, and one media
// subcomponent per `flowInfo` entry, fNum = flowId, fDescs = flowDescriptions -- both TS 29.214
// 5.3.8 encoded), `afAppId` (from `exterAppId`) and `evSubsc` (events above, `usgThres` from
// `usageThreshold`). The same members exist in AppSessionContextUpdateData, so this is also what
// an update sends.
nlohmann::json pcf_session_state(const sbi_gen::AsSessionWithQoSSubscription& sub);

// Npcf_PolicyAuthorization_Create's AppSessionContextReqData: pcf_session_state plus the UE address
// (ueIpv4Addr/ueIpv6Addr/macAddr -> ueIpv4/ueIpv6/ueMac), dnn, snssai -> sliceInfo, the NEF's
// callback as notifUri and evSubsc.notifUri (the PCF appends "/terminate" and "/notify", TS 29.514
// callbacks terminationRequest and eventNotification), and suppFeat "0" (the NEF negotiates no
// optional Npcf_PolicyAuthorization feature; the AF's supportedFeatures are TS 29.122 bits, a
// different list).
sbi_gen::AppSessionContextReqData
app_session_request(const sbi_gen::AsSessionWithQoSSubscription& sub,
                    const std::string& nef_notif_uri);

// The RFC 7396 merge patch that turns `from` into `to`: members only in `from` become null,
// objects are compared member by member, anything else that differs is replaced whole. Empty
// object when they are equal.
nlohmann::json merge_patch_between(const nlohmann::json& from, const nlohmann::json& to);

// Npcf_PolicyAuthorization_Update's `ascReqData` (AppSessionContextUpdateData) for an AF update
// from `before` to `after`: merge_patch_between of their pcf_session_state, with `medCompN` kept in
// every media component and `fNum` in every media subcomponent the patch still names --
// MediaComponentRm and MediaSubComponentRm require them even when unchanged. Empty when nothing
// changed.
nlohmann::json pcf_update_patch(const sbi_gen::AsSessionWithQoSSubscription& before,
                                const sbi_gen::AsSessionWithQoSSubscription& after);

// TS 23.502 4.15.6.6 steps 7-8, TS 29.522 4.4.9.2.2: the AF's UserPlaneEventReports for one PCF
// EventsNotification, and the PCF events that have no AF event in this build.
//   SUCCESSFUL_/FAILED_RESOURCES_ALLOCATION -> the same event, flowIds from the matching
//     succ/failedResourcAllocReports and the event's own flows;
//   QOS_NOTIF -> one report per qncReports entry: notifType GUARANTEED / NOT_GUARANTEED /
//     NOT_GUARANTEED_DL / NOT_GUARANTEED_UL -> QOS_GUARANTEED / QOS_NOT_GUARANTEED / _DL / _UL,
//     altSerReqNotSuppInd -> altQosNotSuppInd (none when the PCF sent no qncReports);
//   USAGE_REPORT -> accumulatedUsage from usgRep; ACCESS_TYPE_CHANGE -> ratType; PLMN_CHG ->
//   plmnId.
// flowIds are the fNums of media component 1 (the only one the NEF creates, ADR-0519), which are
// the AF's flowIds; omitted when the PCF names no flow numbers (the event applies to all flows).
struct AfReportPlan {
    std::vector<sbi_gen::UserPlaneEventReport> reports;
    std::vector<std::string> not_mapped;
};
AfReportPlan
af_reports_for(const sbi_gen::EventsNotification_Npcf_PolicyAuthorization& notification);

// TS 29.122 4.4.13: the PCF's terminationRequest (TerminationInfo) is a SESSION_TERMINATION report.
sbi_gen::UserPlaneEventReport session_termination_report();

} // namespace nef
