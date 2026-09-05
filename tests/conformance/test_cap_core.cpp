#include <algorithm>

#include "cap_core/cap_dictionary.hpp"
#include "cap_core/cap_operations.hpp"
#include "cap_core/cap_types.hpp"
#include "tcap_core/component.hpp"

#include <gtest/gtest.h>

// P4.5/ADR-0059 Stage 6 (CAP) kickoff. Real, cited field subset -- see cap_operations.hpp's own
// header comment for exactly which real TS 29.078 fields each round trip below exercises.

namespace {

using namespace cap_core;

TEST(CapTypes, SendingSideIdRoundTripsLeg1) {
    const auto tlv = encode_sending_side_id(LegType::kLeg1);
    const auto leg = decode_sending_side_id(tlv);
    ASSERT_TRUE(leg.has_value());
    EXPECT_EQ(*leg, LegType::kLeg1);
}

TEST(CapTypes, ReceivingSideIdRoundTripsLeg2) {
    const auto tlv = encode_receiving_side_id(LegType::kLeg2);
    const auto leg = decode_receiving_side_id(tlv);
    ASSERT_TRUE(leg.has_value());
    EXPECT_EQ(*leg, LegType::kLeg2);
}

TEST(CapTypes, ReceivingSideIdRejectsSendingSideIdTag) {
    const auto tlv = encode_sending_side_id(LegType::kLeg1);
    EXPECT_FALSE(decode_receiving_side_id(tlv).has_value());
}

TEST(CapTypes, TimeInformationNoTariffSwitchRoundTrips) {
    const auto tlv = encode_time_information_no_tariff_switch(1234);
    const auto v = decode_time_information_no_tariff_switch(tlv);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 1234);
}

TEST(CapTypes, ExplicitWrapRoundTrips) {
    const auto inner = encode_sending_side_id(LegType::kLeg2);
    const auto outer = wrap_explicit(2, inner);
    EXPECT_TRUE(outer.constructed);
    const auto unwrapped = unwrap_explicit(outer, 2);
    ASSERT_TRUE(unwrapped.has_value());
    EXPECT_EQ(unwrapped->tag_number, 0u);
    EXPECT_EQ(unwrapped->value, inner.value);
}

TEST(CapTypes, ExplicitUnwrapRejectsWrongTag) {
    const auto inner = encode_sending_side_id(LegType::kLeg1);
    const auto outer = wrap_explicit(2, inner);
    EXPECT_FALSE(unwrap_explicit(outer, 3).has_value());
}

TEST(CapOperations, InitialDpArgRoundTripsMinimalRealFields) {
    InitialDpArg arg;
    arg.service_key = 100;
    arg.called_party_number = {0x91, 0x14, 0x27, 0x34}; // opaque, ISUP-encoded (not decoded here)
    arg.calling_party_number = std::vector<std::uint8_t>{0x03, 0x14, 0x27};
    arg.event_type_bcsm = EventTypeBcsm::kCollectedInfo;
    arg.imsi = std::vector<std::uint8_t>{0x21, 0x43, 0x65, 0x87, 0x09};
    arg.cause = std::vector<std::uint8_t>{0x02, 0x82, 0x9f}; // arbitrary, opaque ISUP Cause octets

    const auto bytes = encode_initial_dp_arg(arg);
    const auto decoded = decode_initial_dp_arg(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->service_key, 100);
    EXPECT_EQ(decoded->called_party_number, arg.called_party_number);
    ASSERT_TRUE(decoded->calling_party_number.has_value());
    EXPECT_EQ(*decoded->calling_party_number, *arg.calling_party_number);
    EXPECT_EQ(decoded->event_type_bcsm, EventTypeBcsm::kCollectedInfo);
    ASSERT_TRUE(decoded->imsi.has_value());
    EXPECT_EQ(*decoded->imsi, *arg.imsi);
    ASSERT_TRUE(decoded->cause.has_value());
    EXPECT_EQ(*decoded->cause, *arg.cause);
}

TEST(CapOperations, InitialDpArgRoundTripsWithoutOptionalFields) {
    InitialDpArg arg;
    arg.service_key = 7;
    arg.called_party_number = {0x91, 0x99};
    arg.event_type_bcsm = EventTypeBcsm::kTermAttemptAuthorized;

    const auto bytes = encode_initial_dp_arg(arg);
    const auto decoded = decode_initial_dp_arg(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->service_key, 7);
    EXPECT_FALSE(decoded->calling_party_number.has_value());
    EXPECT_FALSE(decoded->imsi.has_value());
    EXPECT_FALSE(decoded->cause.has_value());
}

TEST(CapOperations, InitialDpArgDecodeFailsWithoutRequiredCalledPartyNumber) {
    // Hand-build a SEQUENCE containing only serviceKey [0] -- calledPartyNumber [2] is real,
    // mandatory (TS 29.078 clause 6.1.1 InitialDPArg), so decode must reject this.
    tcap_core::Tlv service_key_tlv;
    service_key_tlv.tag_class = tcap_core::TagClass::kContext;
    service_key_tlv.constructed = false;
    service_key_tlv.tag_number = 0;
    service_key_tlv.value = tcap_core::encode_integer(1);

    std::vector<std::uint8_t> seq_body;
    tcap_core::encode_tlv(seq_body, service_key_tlv);

    tcap_core::Tlv seq;
    seq.tag_class = tcap_core::TagClass::kUniversal;
    seq.constructed = true;
    seq.tag_number = tcap_core::UniversalTag::kSequence;
    seq.value = seq_body;

    std::vector<std::uint8_t> bytes;
    tcap_core::encode_tlv(bytes, seq);

    EXPECT_FALSE(decode_initial_dp_arg(bytes).has_value());
}

TEST(CapOperations, ApplyChargingArgRoundTripsWithReleaseIfExceededAndPartyToCharge) {
    ApplyChargingArg arg;
    arg.max_call_period_duration = 3000; // 300s in 100ms units
    arg.release_if_duration_exceeded = true;
    arg.party_to_charge = LegType::kLeg2;

    const auto bytes = encode_apply_charging_arg(arg);
    const auto decoded = decode_apply_charging_arg(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->max_call_period_duration, 3000);
    EXPECT_TRUE(decoded->release_if_duration_exceeded);
    ASSERT_TRUE(decoded->party_to_charge.has_value());
    EXPECT_EQ(*decoded->party_to_charge, LegType::kLeg2);
}

TEST(CapOperations, ApplyChargingArgRoundTripsWithDefaultsOmitted) {
    ApplyChargingArg arg;
    arg.max_call_period_duration = 600;
    // release_if_duration_exceeded left at its real DEFAULT FALSE, party_to_charge left absent
    // (real DEFAULT sendingSideID:leg1).

    const auto bytes = encode_apply_charging_arg(arg);
    const auto decoded = decode_apply_charging_arg(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->max_call_period_duration, 600);
    EXPECT_FALSE(decoded->release_if_duration_exceeded);
    EXPECT_FALSE(decoded->party_to_charge.has_value());
}

TEST(CapOperations, ApplyChargingReportArgRoundTrips) {
    ApplyChargingReportArg arg;
    arg.party_to_charge = LegType::kLeg1;
    arg.elapsed_hundred_ms_units = 45678;

    const auto bytes = encode_apply_charging_report_arg(arg);
    const auto decoded = decode_apply_charging_report_arg(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->party_to_charge, LegType::kLeg1);
    EXPECT_EQ(decoded->elapsed_hundred_ms_units, 45678);
}

TEST(CapOperations, RequestReportBcsmEventArgRoundTripsOAnswerAndODisconnect) {
    RequestReportBcsmEventArg arg;
    BcsmEvent answer;
    answer.event_type_bcsm = EventTypeBcsm::kOAnswer;
    answer.monitor_mode = MonitorMode::kNotifyAndContinue;
    answer.leg_id = LegType::kLeg1;
    BcsmEvent disconnect;
    disconnect.event_type_bcsm = EventTypeBcsm::kODisconnect;
    disconnect.monitor_mode = MonitorMode::kNotifyAndContinue;
    arg.bcsm_events = {answer, disconnect};

    const auto bytes = encode_request_report_bcsm_event_arg(arg);
    const auto decoded = decode_request_report_bcsm_event_arg(bytes);
    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->bcsm_events.size(), 2u);
    EXPECT_EQ(decoded->bcsm_events[0].event_type_bcsm, EventTypeBcsm::kOAnswer);
    ASSERT_TRUE(decoded->bcsm_events[0].leg_id.has_value());
    EXPECT_EQ(*decoded->bcsm_events[0].leg_id, LegType::kLeg1);
    EXPECT_EQ(decoded->bcsm_events[1].event_type_bcsm, EventTypeBcsm::kODisconnect);
    EXPECT_FALSE(decoded->bcsm_events[1].leg_id.has_value());
}

TEST(CapOperations, EventReportBcsmArgRoundTrips) {
    EventReportBcsmArg arg;
    arg.event_type_bcsm = EventTypeBcsm::kODisconnect;
    arg.leg_id = LegType::kLeg1;

    const auto bytes = encode_event_report_bcsm_arg(arg);
    const auto decoded = decode_event_report_bcsm_arg(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->event_type_bcsm, EventTypeBcsm::kODisconnect);
    ASSERT_TRUE(decoded->leg_id.has_value());
    EXPECT_EQ(*decoded->leg_id, LegType::kLeg1);
}

TEST(CapOperations, ReleaseCallArgRoundTripsBareCause) {
    const std::vector<std::uint8_t> cause = {0x02, 0x82, 0x9f}; // arbitrary opaque ISUP Cause
    const auto bytes = encode_release_call_arg(cause);
    const auto decoded = decode_release_call_arg(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, cause);
}

// --- Composition: a real InitialDP travels inside a TCAP Invoke component, matching the same
// composition-verification discipline this project's own SS7/TCAP work already established. ---

TEST(CapTcapComposition, InitialDpTravelsInsideTcapInvoke) {
    InitialDpArg arg;
    arg.service_key = 100;
    arg.called_party_number = {0x91, 0x14, 0x27};
    arg.event_type_bcsm = EventTypeBcsm::kCollectedInfo;

    tcap_core::Invoke invoke;
    invoke.invoke_id = 1;
    invoke.operation_code.local = Opcode::kInitialDp;
    invoke.parameter = encode_initial_dp_arg(arg);

    const auto invoke_tlv = tcap_core::encode_invoke(invoke);
    const auto component = tcap_core::decode_component(invoke_tlv);
    ASSERT_TRUE(component.has_value());
    ASSERT_TRUE(component->invoke.has_value());
    EXPECT_EQ(component->invoke->invoke_id, 1);
    ASSERT_TRUE(component->invoke->operation_code.local.has_value());
    EXPECT_EQ(*component->invoke->operation_code.local, Opcode::kInitialDp);

    const auto decoded_arg = decode_initial_dp_arg(component->invoke->parameter);
    ASSERT_TRUE(decoded_arg.has_value());
    EXPECT_EQ(decoded_arg->service_key, 100);
    EXPECT_EQ(decoded_arg->called_party_number, arg.called_party_number);
    EXPECT_EQ(decoded_arg->event_type_bcsm, EventTypeBcsm::kCollectedInfo);
}

TEST(CapTcapComposition, ApplyChargingReportTravelsInsideTcapReturnResultLast) {
    ApplyChargingReportArg arg;
    arg.party_to_charge = LegType::kLeg1;
    arg.elapsed_hundred_ms_units = 300;

    tcap_core::ReturnResult rr;
    rr.invoke_id = 7;
    tcap_core::ReturnResult::Result result;
    result.operation_code.local = Opcode::kApplyChargingReport;
    result.parameter = encode_apply_charging_report_arg(arg);
    rr.result = result;

    const auto rr_tlv = tcap_core::encode_return_result(rr, /*is_last=*/true);
    const auto component = tcap_core::decode_component(rr_tlv);
    ASSERT_TRUE(component.has_value());
    ASSERT_TRUE(component->return_result_last.has_value());
    ASSERT_TRUE(component->return_result_last->result.has_value());

    const auto decoded_arg =
        decode_apply_charging_report_arg(component->return_result_last->result->parameter);
    ASSERT_TRUE(decoded_arg.has_value());
    EXPECT_EQ(decoded_arg->party_to_charge, LegType::kLeg1);
    EXPECT_EQ(decoded_arg->elapsed_hundred_ms_units, 300);
}

} // namespace

// --- legActive, ADR-0298 ---
//
// The one field that separates a periodic ApplyChargingReport from a final one. Its ASN.1 DEFAULT
// is TRUE, and BER omits a DEFAULT-valued field -- so ABSENT MEANS TRUE, and a decoder that
// defaulted it to false would silently treat every mid-call report as call end.

TEST(CapOperations, ApplyChargingReportLegActiveDefaultsToTrueWhenAbsent) {
    cap_core::ApplyChargingReportArg arg;
    arg.party_to_charge = cap_core::LegType::kLeg1;
    arg.elapsed_hundred_ms_units = 600;
    arg.leg_active = true;

    const auto bytes = cap_core::encode_apply_charging_report_arg(arg);
    // A conformant encoder omits the field entirely when it carries its default value.
    const auto decoded = cap_core::decode_apply_charging_report_arg(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_TRUE(decoded->leg_active) << "an absent legActive must decode as TRUE, not FALSE";
    EXPECT_EQ(decoded->elapsed_hundred_ms_units, 600);
}

TEST(CapOperations, ApplyChargingReportLegActiveFalseRoundTrips) {
    cap_core::ApplyChargingReportArg arg;
    arg.elapsed_hundred_ms_units = 1200;
    arg.leg_active = false;

    const auto decoded =
        cap_core::decode_apply_charging_report_arg(cap_core::encode_apply_charging_report_arg(arg));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_FALSE(decoded->leg_active) << "call-end must survive the round trip";
    EXPECT_EQ(decoded->elapsed_hundred_ms_units, 1200);
}

TEST(CapOperations, ApplyChargingReportDecodesANonZeroLegActiveAsTrue) {
    // X.690 encodes TRUE as any non-zero octet (conventionally 0xFF). A real peer that sends an
    // explicit TRUE rather than omitting it must still be understood.
    cap_core::ApplyChargingReportArg arg;
    arg.elapsed_hundred_ms_units = 10;
    arg.leg_active = false;
    auto bytes = cap_core::encode_apply_charging_report_arg(arg);
    // Flip the encoded FALSE (0x00 content octet) to 0xFF in place.
    const auto pos = std::find(bytes.begin(), bytes.end(), 0x82); // [2] primitive tag
    ASSERT_NE(pos, bytes.end()) << "the encoder did not emit legActive [2] for a false value";
    *(pos + 2) = 0xFF;
    const auto decoded = cap_core::decode_apply_charging_report_arg(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_TRUE(decoded->leg_active);
}
