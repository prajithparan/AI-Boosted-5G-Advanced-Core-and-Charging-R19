// Real TAP3 wiring for roaming_interconnect::RoamingCdrFile (ADR-0067) -- covers
// make_tap3_roaming_cdr_file/decode_tap3_roaming_cdr_file (pure functions, no PostgreSQL
// connection needed) separately from tests/integration/test_roaming_interconnect_postgres.cpp,
// which covers the real DB-backed CRUD paths.

#include "../../bss/roaming-interconnect/src/store.hpp"
#include "tap_batch.hpp"

#include <gtest/gtest.h>

TEST(RoamingInterconnectTap3, MakeAndDecodeRoundTripsThroughRawPayload) {
    tap3_core::DataInterchange data;
    tap3_core::TransferBatch batch;
    tap3_core::BatchControlInfo bci;
    bci.sender = "OPERA";
    bci.recipient = "OPERB";
    bci.fileSequenceNumber = "00007";
    batch.batchControlInfo = bci;
    data.transferBatch = batch;

    const auto file = roaming_interconnect::make_tap3_roaming_cdr_file("agreement-1", data);
    EXPECT_EQ(file.format, "TAP3");
    EXPECT_EQ(*file.agreementId, "agreement-1");
    ASSERT_FALSE(file.rawPayload.empty());

    const auto decoded = roaming_interconnect::decode_tap3_roaming_cdr_file(file);
    ASSERT_TRUE(decoded.has_value());
    ASSERT_TRUE(decoded->transferBatch.has_value());
    ASSERT_TRUE(decoded->transferBatch->batchControlInfo.has_value());
    EXPECT_EQ(*decoded->transferBatch->batchControlInfo->sender, "OPERA");
    EXPECT_EQ(*decoded->transferBatch->batchControlInfo->fileSequenceNumber, "00007");
}

TEST(RoamingInterconnectTap3, DecodeRejectsNonTap3Format) {
    roaming_interconnect::RoamingCdrFile file;
    file.format = "STUB";
    EXPECT_FALSE(roaming_interconnect::decode_tap3_roaming_cdr_file(file).has_value());
}

// --- TAP OUT / TAP IN batch processing (ADR-0306, C5 settlement half) ---
//
// The property under test is the one that gets real files RETURNED by a clearing house:
// AuditControlInfo must agree with the call events actually in the batch. A file whose totals
// disagree with its own contents costs an operator a settlement cycle via RAP.

TEST(TapBatch, FileSequenceNumberIsTheRealFiveCharacterFixedWidth) {
    EXPECT_EQ(roaming::format_file_sequence_number(1), "00001");
    EXPECT_EQ(roaming::format_file_sequence_number(42), "00042");
    EXPECT_EQ(roaming::format_file_sequence_number(99999), "99999");
    // TD.57 rolls over at 99999 by specification -- documented behaviour, not an overflow bug.
    EXPECT_EQ(roaming::format_file_sequence_number(100000), "00000");
}

TEST(TapBatch, BuiltBatchDerivesItsOwnAuditTotals) {
    std::vector<roaming::RoamingUsageRecord> records;
    roaming::RoamingUsageRecord a;
    a.imsi = "999700000000901";
    a.data_volume_incoming = 1'000;
    a.data_volume_outgoing = 2'000;
    a.charge = 150;
    a.chargeable_units = 3'000;
    a.timestamp = "20260905120000";
    roaming::RoamingUsageRecord b = a;
    b.imsi = "999700000000902";
    b.charge = 250;
    b.timestamp = "20260905130000";
    records.push_back(a);
    records.push_back(b);

    const auto batch =
        roaming::build_transfer_batch("HOMEOP", "VISITEDOP", 7, "20260905235959", records);

    ASSERT_TRUE(batch.batchControlInfo.has_value());
    EXPECT_EQ(batch.batchControlInfo->sender.value_or(""), "HOMEOP");
    EXPECT_EQ(batch.batchControlInfo->fileSequenceNumber.value_or(""), "00007");

    ASSERT_TRUE(batch.auditControlInfo.has_value());
    EXPECT_EQ(batch.auditControlInfo->callEventDetailsCount.value_or(-1), 2);
    EXPECT_EQ(batch.auditControlInfo->totalCharge.value_or(-1), 400)
        << "the audit total must be the sum of the records, derived and not supplied";
    ASSERT_TRUE(batch.auditControlInfo->earliestCallTimeStamp.has_value());
    EXPECT_EQ(batch.auditControlInfo->earliestCallTimeStamp->localTimeStamp.value_or(""),
              "20260905120000");
    EXPECT_EQ(batch.auditControlInfo->latestCallTimeStamp->localTimeStamp.value_or(""),
              "20260905130000");

    // A batch this project built must validate clean against its own inbound checker.
    EXPECT_TRUE(roaming::validate_transfer_batch(batch).empty());
}

TEST(TapBatch, ABatchSurvivesEncodeDecodeAndStillValidates) {
    std::vector<roaming::RoamingUsageRecord> records(1);
    records[0].imsi = "999700000000901";
    records[0].charge = 500;
    records[0].chargeable_units = 1'024;
    records[0].data_volume_incoming = 512;
    records[0].data_volume_outgoing = 512;
    records[0].timestamp = "20260906010203";

    tap3_core::DataInterchange interchange;
    interchange.transferBatch =
        roaming::build_transfer_batch("HOMEOP", "VISITEDOP", 1, "20260906020000", records);
    const auto bytes = tap3_core::encode_data_interchange(interchange);

    // The whole point of TAP IN: a file goes out as bytes and comes back as bytes, and the
    // validation must hold across that boundary rather than only on in-memory structures.
    EXPECT_TRUE(roaming::validate_tap_file(bytes).empty())
        << "a batch this project produced did not survive its own encode/decode round trip";
}

TEST(TapBatch, AMismatchedAuditTotalIsReportedRatherThanAccepted) {
    std::vector<roaming::RoamingUsageRecord> records(1);
    records[0].imsi = "999700000000901";
    records[0].charge = 100;
    records[0].timestamp = "20260906010203";

    auto batch = roaming::build_transfer_batch("HOMEOP", "VISITEDOP", 1, "20260906020000", records);
    // Corrupt the totals the way a real defective partner file does.
    batch.auditControlInfo->totalCharge = 999;
    batch.auditControlInfo->callEventDetailsCount = 5;

    const auto findings = roaming::validate_transfer_batch(batch);
    ASSERT_EQ(findings.size(), 2u) << "both the count and the charge mismatch must be reported";
    EXPECT_NE(findings[0].find("callEventDetailsCount"), std::string::npos);
    EXPECT_NE(findings[1].find("totalCharge"), std::string::npos);
}

TEST(TapBatch, AMissingControlBlockIsAFinding) {
    tap3_core::TransferBatch empty;
    const auto findings = roaming::validate_transfer_batch(empty);
    EXPECT_FALSE(findings.empty());
}

TEST(TapBatch, GarbageIsReportedNotThrown) {
    const std::vector<std::uint8_t> garbage = {0x01, 0x02, 0x03, 0x04};
    const auto findings = roaming::validate_tap_file(garbage);
    ASSERT_EQ(findings.size(), 1u);
    EXPECT_NE(findings[0].find("not a decodable"), std::string::npos);
}
