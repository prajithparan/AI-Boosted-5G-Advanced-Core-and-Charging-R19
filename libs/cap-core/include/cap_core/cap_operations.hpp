#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "cap_core/cap_types.hpp"

// CAP (TS 29.078) circuit-switched call-control operation arguments -- the real, minimal subset
// needed for the InitialDP -> RequestReportBCSMEvent -> ApplyCharging -> EventReportBCSM ->
// ApplyChargingReport prepaid-charging flow (TS 29.078 clause 11). P4.5/ADR-0059 Stage 6 kickoff.
// Every encode_* function returns bytes suitable for tcap_core::Invoke::parameter (a single,
// self-contained TLV per tcap_core::component.cpp's own real framing -- verified against its
// `Invoke.parameter` handling before writing this file).
//
// Real, disclosed scope narrowing (each ARGUMENT below is the real TS 29.078 SEQUENCE with only a
// subset of its real OPTIONAL fields implemented -- every field modeled here has a real, cited
// tag; every field NOT modeled is a real, disclosed gap, not an invented omission):
//   InitialDPArg:            serviceKey, calledPartyNumber, callingPartyNumber, eventTypeBCSM,
//                             iMSI, cause. ~24 other real OPTIONAL fields (locationInformation,
//                             iPSSPCapabilities, redirectingPartyID, ...) not yet modeled.
//   ApplyChargingArg:        aChBillingChargingCharacteristics (timeDurationCharging variant:
//                             maxCallPeriodDuration, releaseIfdurationExceeded only),
//                             partyToCharge.
//   ApplyChargingReportArg:  CallResult's timeDurationChargingResult variant (partyToCharge,
//                             timeInformation only -- legActive/extensions/aChChargingAddress not
//                             modeled).
//   RequestReportBCSMEventArg / BCSMEvent: eventTypeBCSM, monitorMode, legID.
//   EventReportBCSMArg:      eventTypeBCSM, legID only (eventSpecificInformationBCSM,
//                             miscCallInfo, extensions not modeled).
//   ReleaseCallArg:          allCallSegments (Cause) variant only.
//   continue (no ARGUMENT at all per TS 29.078 clause 6.1.1 -- has no encode/decode function here).

namespace cap_core {

// Real EventTypeBCSM ENUMERATED values actually needed for the prepaid voice-charging flow --
// TS 29.078 clause 5.1 ("EventTypeBCSM"). The full real enum has ~20 values; only the ones this
// codebase's own charging flow arms/reports are named here.
namespace EventTypeBcsm {
constexpr std::int32_t kCollectedInfo = 2;
constexpr std::int32_t kAnalyzedInformation = 3;
constexpr std::int32_t kOCalledPartyBusy = 5;
constexpr std::int32_t kONoAnswer = 6;
constexpr std::int32_t kOAnswer = 7;
constexpr std::int32_t kODisconnect = 9;
constexpr std::int32_t kTermAttemptAuthorized = 12;
constexpr std::int32_t kTBusy = 13;
constexpr std::int32_t kTNoAnswer = 14;
constexpr std::int32_t kTAnswer = 15;
constexpr std::int32_t kTDisconnect = 17;
} // namespace EventTypeBcsm

// Real MonitorMode ENUMERATED values -- TS 29.078 clause 5.1 ("MonitorMode").
namespace MonitorMode {
constexpr std::int32_t kInterrupted = 0;
constexpr std::int32_t kNotifyAndContinue = 1;
constexpr std::int32_t kTransparent = 2;
} // namespace MonitorMode

struct InitialDpArg {
    std::int32_t service_key = 0;                                  // [0] ServiceKey (INTEGER)
    std::vector<std::uint8_t> called_party_number;                 // [2] CalledPartyNumber (opaque)
    std::optional<std::vector<std::uint8_t>> calling_party_number; // [3] (opaque, OPTIONAL)
    std::int32_t event_type_bcsm = 0;                              // [28] EventTypeBCSM
    std::optional<std::vector<std::uint8_t>> imsi;                 // [50] IMSI (TBCD, OPTIONAL)
    std::optional<std::vector<std::uint8_t>> cause;                // [17] Cause (opaque, OPTIONAL)
};

std::vector<std::uint8_t> encode_initial_dp_arg(const InitialDpArg& arg);
std::optional<InitialDpArg> decode_initial_dp_arg(const std::vector<std::uint8_t>& parameter);

struct ApplyChargingArg {
    std::int32_t max_call_period_duration = 0; // 100ms units, real range 1..864000
    bool release_if_duration_exceeded = false; // DEFAULT FALSE
    std::optional<LegType> party_to_charge;    // DEFAULT sendingSideID:leg1 if absent
};

std::vector<std::uint8_t> encode_apply_charging_arg(const ApplyChargingArg& arg);
std::optional<ApplyChargingArg>
decode_apply_charging_arg(const std::vector<std::uint8_t>& parameter);

struct ApplyChargingReportArg {
    LegType party_to_charge = LegType::kLeg1;  // partyToCharge [0] ReceivingSideID
    std::int32_t elapsed_hundred_ms_units = 0; // timeInformation [1], no-tariff-switch variant
    // ADR-0298: legActive [2] BOOLEAN DEFAULT TRUE -- the field that distinguishes a PERIODIC
    // report (the call is still up, maxCallPeriodDuration just expired, grant more or release)
    // from a FINAL one (the call ended). Without it a gsmSCF cannot tell the two apart, which is
    // why this codec could only ever handle one report per call.
    //
    // DEFAULT TRUE is honoured literally: BER omits a DEFAULT-valued field, so ABSENT means TRUE.
    // Defaulting this to false instead would silently treat every periodic report as call-end.
    bool leg_active = true;
};

std::vector<std::uint8_t> encode_apply_charging_report_arg(const ApplyChargingReportArg& arg);
std::optional<ApplyChargingReportArg>
decode_apply_charging_report_arg(const std::vector<std::uint8_t>& parameter);

struct BcsmEvent {
    std::int32_t event_type_bcsm = 0; // [0]
    std::int32_t monitor_mode = 0;    // [1]
    std::optional<LegType> leg_id;    // [2] LegID, OPTIONAL
};

struct RequestReportBcsmEventArg {
    std::vector<BcsmEvent> bcsm_events; // bcsmEvents [0], real SIZE(1..30)
};

std::vector<std::uint8_t>
encode_request_report_bcsm_event_arg(const RequestReportBcsmEventArg& arg);
std::optional<RequestReportBcsmEventArg>
decode_request_report_bcsm_event_arg(const std::vector<std::uint8_t>& parameter);

struct EventReportBcsmArg {
    std::int32_t event_type_bcsm = 0; // [0]
    std::optional<LegType> leg_id;    // [3] ReceivingSideID, OPTIONAL
};

std::vector<std::uint8_t> encode_event_report_bcsm_arg(const EventReportBcsmArg& arg);
std::optional<EventReportBcsmArg>
decode_event_report_bcsm_arg(const std::vector<std::uint8_t>& parameter);

// ReleaseCallArg -- allCallSegments variant only (a bare Cause, TS 29.078 clause 6.1.1).
std::vector<std::uint8_t> encode_release_call_arg(const std::vector<std::uint8_t>& cause_octets);
std::optional<std::vector<std::uint8_t>>
decode_release_call_arg(const std::vector<std::uint8_t>& parameter);

} // namespace cap_core
