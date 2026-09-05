#include "map_core/map_dictionary.hpp"
#include "map_core/map_operations.hpp"
#include "tbcd_core/tbcd.hpp"
#include "tcap_core/component.hpp"

#include <gtest/gtest.h>

// P4.5/ADR-0059 Stage 7 (MAP) kickoff. Real, cited field subset -- see map_operations.hpp's own
// header comment for exactly which real TS 29.002 fields each round trip below exercises.

namespace {

using namespace map_core;

TEST(MapOperations, InsertSubscriberDataArgRoundTripsImsiAndMsisdnOnly) {
    InsertSubscriberDataArg arg;
    arg.imsi = std::vector<std::uint8_t>{0x21, 0x43, 0x65, 0x87, 0x09}; // opaque TBCD digits
    arg.msisdn = std::vector<std::uint8_t>{0x91, 0x14, 0x27, 0x34};     // opaque AddressString

    const auto bytes = encode_insert_subscriber_data_arg(arg);
    const auto decoded = decode_insert_subscriber_data_arg(bytes);
    ASSERT_TRUE(decoded.has_value());
    ASSERT_TRUE(decoded->imsi.has_value());
    EXPECT_EQ(*decoded->imsi, *arg.imsi);
    ASSERT_TRUE(decoded->msisdn.has_value());
    EXPECT_EQ(*decoded->msisdn, *arg.msisdn);
    EXPECT_FALSE(decoded->vlr_camel_subscription_info.has_value());
}

TEST(MapOperations, InsertSubscriberDataArgRoundTripsWithNoFieldsPresent) {
    InsertSubscriberDataArg arg; // every real field is OPTIONAL -- an all-absent Arg is valid
    const auto bytes = encode_insert_subscriber_data_arg(arg);
    const auto decoded = decode_insert_subscriber_data_arg(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_FALSE(decoded->imsi.has_value());
    EXPECT_FALSE(decoded->msisdn.has_value());
    EXPECT_FALSE(decoded->vlr_camel_subscription_info.has_value());
}

TEST(MapOperations, VlrCamelSubscriptionInfoWithOCsiRoundTrips) {
    InsertSubscriberDataArg arg;
    arg.imsi = std::vector<std::uint8_t>{0x21, 0x43, 0x65, 0x87, 0x09};

    OBcsmCamelTdpData tdp;
    tdp.trigger_detection_point = OBcsmTriggerDetectionPoint::kCollectedInfo;
    tdp.service_key = 100;
    tdp.gsm_scf_address = {0x91, 0x99, 0x11, 0x22};
    tdp.default_call_handling = DefaultCallHandling::kContinueCall;

    OCsi o_csi;
    o_csi.tdp_data_list = {tdp};
    o_csi.camel_capability_handling = 4; // CAMEL phase 4

    VlrCamelSubscriptionInfo vlr;
    vlr.o_csi = o_csi;
    arg.vlr_camel_subscription_info = vlr;

    const auto bytes = encode_insert_subscriber_data_arg(arg);
    const auto decoded = decode_insert_subscriber_data_arg(bytes);
    ASSERT_TRUE(decoded.has_value());
    ASSERT_TRUE(decoded->vlr_camel_subscription_info.has_value());
    ASSERT_TRUE(decoded->vlr_camel_subscription_info->o_csi.has_value());
    const auto& decoded_o_csi = *decoded->vlr_camel_subscription_info->o_csi;
    ASSERT_EQ(decoded_o_csi.tdp_data_list.size(), 1u);
    EXPECT_EQ(decoded_o_csi.tdp_data_list[0].trigger_detection_point,
              OBcsmTriggerDetectionPoint::kCollectedInfo);
    EXPECT_EQ(decoded_o_csi.tdp_data_list[0].service_key, 100);
    EXPECT_EQ(decoded_o_csi.tdp_data_list[0].gsm_scf_address, tdp.gsm_scf_address);
    EXPECT_EQ(decoded_o_csi.tdp_data_list[0].default_call_handling,
              DefaultCallHandling::kContinueCall);
    ASSERT_TRUE(decoded_o_csi.camel_capability_handling.has_value());
    EXPECT_EQ(*decoded_o_csi.camel_capability_handling, 4);
    EXPECT_FALSE(decoded->vlr_camel_subscription_info->d_csi.has_value());
}

TEST(MapOperations, VlrCamelSubscriptionInfoWithDCsiRoundTrips) {
    DpAnalysedInfoCriterion criterion;
    criterion.dialled_number = {0x91, 0x14, 0x27, 0x34};
    criterion.service_key = 55;
    criterion.gsm_scf_address = {0x91, 0x99, 0x33, 0x44};
    criterion.default_call_handling = DefaultCallHandling::kReleaseCall;

    DCsi d_csi;
    d_csi.dp_analysed_info_criteria_list = {criterion};
    d_csi.camel_capability_handling = 3;

    VlrCamelSubscriptionInfo vlr;
    vlr.d_csi = d_csi;

    InsertSubscriberDataArg arg;
    arg.vlr_camel_subscription_info = vlr;

    const auto bytes = encode_insert_subscriber_data_arg(arg);
    const auto decoded = decode_insert_subscriber_data_arg(bytes);
    ASSERT_TRUE(decoded.has_value());
    ASSERT_TRUE(decoded->vlr_camel_subscription_info.has_value());
    ASSERT_TRUE(decoded->vlr_camel_subscription_info->d_csi.has_value());
    const auto& decoded_d_csi = *decoded->vlr_camel_subscription_info->d_csi;
    ASSERT_EQ(decoded_d_csi.dp_analysed_info_criteria_list.size(), 1u);
    const auto& c = decoded_d_csi.dp_analysed_info_criteria_list[0];
    EXPECT_EQ(c.dialled_number, criterion.dialled_number);
    EXPECT_EQ(c.service_key, 55);
    EXPECT_EQ(c.gsm_scf_address, criterion.gsm_scf_address);
    EXPECT_EQ(c.default_call_handling, DefaultCallHandling::kReleaseCall);
    ASSERT_TRUE(decoded_d_csi.camel_capability_handling.has_value());
    EXPECT_EQ(*decoded_d_csi.camel_capability_handling, 3);
}

TEST(MapOperations, DCsiWithoutOptionalFieldsRoundTrips) {
    DCsi d_csi; // both real fields OPTIONAL, absent here
    VlrCamelSubscriptionInfo vlr;
    vlr.d_csi = d_csi;
    InsertSubscriberDataArg arg;
    arg.vlr_camel_subscription_info = vlr;

    const auto bytes = encode_insert_subscriber_data_arg(arg);
    const auto decoded = decode_insert_subscriber_data_arg(bytes);
    ASSERT_TRUE(decoded.has_value());
    ASSERT_TRUE(decoded->vlr_camel_subscription_info.has_value());
    ASSERT_TRUE(decoded->vlr_camel_subscription_info->d_csi.has_value());
    EXPECT_TRUE(
        decoded->vlr_camel_subscription_info->d_csi->dp_analysed_info_criteria_list.empty());
    EXPECT_FALSE(
        decoded->vlr_camel_subscription_info->d_csi->camel_capability_handling.has_value());
}

TEST(MapOperations, InsertSubscriberDataResEmptyRoundTrips) {
    const auto bytes = encode_insert_subscriber_data_res();
    EXPECT_TRUE(decode_insert_subscriber_data_res(bytes));
}

TEST(MapOperations, InsertSubscriberDataResRejectsMalformedBytes) {
    const std::vector<std::uint8_t> garbage = {0xFF};
    EXPECT_FALSE(decode_insert_subscriber_data_res(garbage));
}

// --- Composition: a real insertSubscriberData travels inside a TCAP Invoke component, matching
// the same composition-verification discipline this project's own CAP/SS7/TCAP work already
// established. ---

TEST(MapTcapComposition, InsertSubscriberDataTravelsInsideTcapInvoke) {
    InsertSubscriberDataArg arg;
    arg.imsi = std::vector<std::uint8_t>{0x21, 0x43, 0x65, 0x87, 0x09};

    OBcsmCamelTdpData tdp;
    tdp.trigger_detection_point = OBcsmTriggerDetectionPoint::kCollectedInfo;
    tdp.service_key = 100;
    tdp.gsm_scf_address = {0x91, 0x99, 0x11, 0x22};
    tdp.default_call_handling = DefaultCallHandling::kContinueCall;
    OCsi o_csi;
    o_csi.tdp_data_list = {tdp};
    VlrCamelSubscriptionInfo vlr;
    vlr.o_csi = o_csi;
    arg.vlr_camel_subscription_info = vlr;

    tcap_core::Invoke invoke;
    invoke.invoke_id = 1;
    invoke.operation_code.local = Opcode::kInsertSubscriberData;
    invoke.parameter = encode_insert_subscriber_data_arg(arg);

    const auto invoke_tlv = tcap_core::encode_invoke(invoke);
    const auto component = tcap_core::decode_component(invoke_tlv);
    ASSERT_TRUE(component.has_value());
    ASSERT_TRUE(component->invoke.has_value());
    EXPECT_EQ(component->invoke->invoke_id, 1);
    ASSERT_TRUE(component->invoke->operation_code.local.has_value());
    EXPECT_EQ(*component->invoke->operation_code.local, Opcode::kInsertSubscriberData);

    const auto decoded_arg = decode_insert_subscriber_data_arg(component->invoke->parameter);
    ASSERT_TRUE(decoded_arg.has_value());
    ASSERT_TRUE(decoded_arg->imsi.has_value());
    EXPECT_EQ(*decoded_arg->imsi, *arg.imsi);
    ASSERT_TRUE(decoded_arg->vlr_camel_subscription_info.has_value());
    ASSERT_TRUE(decoded_arg->vlr_camel_subscription_info->o_csi.has_value());
    ASSERT_EQ(decoded_arg->vlr_camel_subscription_info->o_csi->tdp_data_list.size(), 1u);
    EXPECT_EQ(decoded_arg->vlr_camel_subscription_info->o_csi->tdp_data_list[0].service_key, 100);
}

TEST(MapTcapComposition, InsertSubscriberDataResTravelsInsideTcapReturnResultLast) {
    tcap_core::ReturnResult rr;
    rr.invoke_id = 3;
    tcap_core::ReturnResult::Result result;
    result.operation_code.local = Opcode::kInsertSubscriberData;
    result.parameter = encode_insert_subscriber_data_res();
    rr.result = result;

    const auto rr_tlv = tcap_core::encode_return_result(rr, /*is_last=*/true);
    const auto component = tcap_core::decode_component(rr_tlv);
    ASSERT_TRUE(component.has_value());
    ASSERT_TRUE(component->return_result_last.has_value());
    ASSERT_TRUE(component->return_result_last->result.has_value());
    EXPECT_TRUE(
        decode_insert_subscriber_data_res(component->return_result_last->result->parameter));
}

} // namespace

// --- cancelLocation (ADR-0296) ---
//
// The two things this operation does differently from insertSubscriberData are exactly what these
// tests pin, because both are silent-on-the-wire mistakes: a CONTEXT [3] CONSTRUCTED wrapper
// instead of a UNIVERSAL SEQUENCE, and an untagged CHOICE identified by position.

TEST(MapOperations, CancelLocationArgRoundTrips) {
    map_core::CancelLocationArg arg;
    arg.imsi = tbcd_core::encode_tbcd("999700000000901");
    arg.cancellation_type = map_core::CancellationType::kSubscriptionWithdraw;

    const auto bytes = map_core::encode_cancel_location_arg(arg);
    const auto decoded = map_core::decode_cancel_location_arg(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(tbcd_core::decode_tbcd(decoded->imsi), "999700000000901");
    ASSERT_TRUE(decoded->cancellation_type.has_value());
    EXPECT_EQ(*decoded->cancellation_type, map_core::CancellationType::kSubscriptionWithdraw);
}

TEST(MapOperations, CancelLocationArgWithoutCancellationTypeRoundTrips) {
    // The field is genuinely OPTIONAL, and UDM omits it for every deregReason but one -- so the
    // omitted case is the common path, not an edge case.
    map_core::CancelLocationArg arg;
    arg.imsi = tbcd_core::encode_tbcd("001010000000001");

    const auto decoded =
        map_core::decode_cancel_location_arg(map_core::encode_cancel_location_arg(arg));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(tbcd_core::decode_tbcd(decoded->imsi), "001010000000001");
    EXPECT_FALSE(decoded->cancellation_type.has_value());
}

TEST(MapOperations, CancelLocationArgIsWrappedInContextTagThreeNotAUniversalSequence) {
    map_core::CancelLocationArg arg;
    arg.imsi = tbcd_core::encode_tbcd("999700000000901");
    const auto bytes = map_core::encode_cancel_location_arg(arg);

    ASSERT_FALSE(bytes.empty());
    // Context class (0b10) + constructed (0b1) + tag 3 == 0xA3. A plain SEQUENCE would be 0x30,
    // which is what the other operation in this codec encodes and what a copy-paste would produce.
    EXPECT_EQ(bytes[0], 0xA3) << "CancelLocationArg must carry its real [3] CONSTRUCTED tag";
}

TEST(MapOperations, CancelLocationArgRejectsAUniversalSequenceWrapper) {
    // Guards the decoder against accepting the shape the encoder must not produce.
    std::vector<std::uint8_t> mis_wrapped = {0x30, 0x02, 0x04, 0x00};
    EXPECT_FALSE(map_core::decode_cancel_location_arg(mis_wrapped).has_value());
}

TEST(MapOperations, CancelLocationArgRejectsAnImsiWithLmsiIdentityArm) {
    // Identity's other real arm is a constructed SEQUENCE in the same first position. This codec
    // does not model it, and must say so rather than read its bytes as an IMSI.
    std::vector<std::uint8_t> with_lmsi = {0xA3, 0x04, 0x30, 0x02, 0x04, 0x00};
    EXPECT_FALSE(map_core::decode_cancel_location_arg(with_lmsi).has_value());
}

TEST(MapOperations, CancelLocationResEmptyRoundTrips) {
    EXPECT_TRUE(map_core::decode_cancel_location_res(map_core::encode_cancel_location_res()));
}
