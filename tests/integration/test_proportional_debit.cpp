// ADR-0297: the arithmetic that decides how much of a reservation a subscriber actually pays.
//
// This is the highest-consequence pure function in the charging path -- it converts "we held 5 GB
// worth of money" into "you owe for the 1 GB you used". Before ADR-0297 the two were forced equal
// and a subscriber who used a fifth of a grant was billed the whole thing, on every Release, on
// both the HTTP and Diameter Gy paths. So the cases below are the money cases, stated as money.

#include "charging_engine.hpp"

#include <gtest/gtest.h>

namespace {
constexpr double kReserved = 10.0;
} // namespace

TEST(ProportionalDebit, ChargesOnlyTheFractionConsumed) {
    // 1 GB used of a 5 GB grant -> a fifth of the money. The whole point of the change.
    const double debit = chf::proportional_debit(kReserved,
                                                 /*granted_volume=*/5'000'000'000.0,
                                                 /*used_volume=*/1'000'000'000.0,
                                                 /*granted_service_units=*/0.0,
                                                 /*used_service_units=*/0.0);
    EXPECT_DOUBLE_EQ(debit, 2.0);
}

TEST(ProportionalDebit, FullyConsumedGrantChargesTheFullReservation) {
    EXPECT_DOUBLE_EQ(chf::proportional_debit(kReserved, 1'000.0, 1'000.0, 0.0, 0.0), kReserved);
}

TEST(ProportionalDebit, UsageBeyondTheGrantNeverChargesMoreThanWasReserved) {
    // Over-usage must not overdraw: the subscriber's balance never held more than the reservation,
    // so debiting more would take money that was never authorised.
    EXPECT_DOUBLE_EQ(chf::proportional_debit(kReserved, 1'000.0, 9'999'999.0, 0.0, 0.0), kReserved);
}

TEST(ProportionalDebit, NothingUsedChargesNothing) {
    EXPECT_DOUBLE_EQ(chf::proportional_debit(kReserved, 1'000.0, 0.0, 0.0, 0.0), 0.0);
}

TEST(ProportionalDebit, NoGrantRecordedFallsBackToTheFullReservation) {
    // Failing toward charging what was reserved, not toward giving usage away. A session whose
    // grant was never recorded is unmeasurable, not free -- and silently zero-rating it would turn
    // a data gap into lost revenue that nothing would ever surface.
    EXPECT_DOUBLE_EQ(chf::proportional_debit(kReserved, 0.0, 500.0, 0.0, 0.0), kReserved);
    EXPECT_DOUBLE_EQ(chf::proportional_debit(kReserved, 0.0, 0.0, 0.0, 0.0), kReserved);
}

TEST(ProportionalDebit, ServiceSpecificUnitsProportionTheSameWay) {
    EXPECT_DOUBLE_EQ(chf::proportional_debit(kReserved, 0.0, 0.0, 100.0, 25.0), 2.5);
}

TEST(ProportionalDebit, MixedDimensionsAreWeightedByEachGrantRatherThanSummedBlindly) {
    // Volume and service units are not commensurable, so each is proportioned against its own
    // grant. Half of each here -> half the money, which a naive sum of raw numbers would not give.
    const double debit = chf::proportional_debit(kReserved, 1'000.0, 500.0, 1'000.0, 500.0);
    EXPECT_DOUBLE_EQ(debit, 5.0);
}

TEST(ProportionalDebit, ZeroOrNegativeReservationChargesNothing) {
    EXPECT_DOUBLE_EQ(chf::proportional_debit(0.0, 1'000.0, 500.0, 0.0, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(chf::proportional_debit(-1.0, 1'000.0, 500.0, 0.0, 0.0), 0.0);
}

// --- the time dimension (ADR-0304, C2) ---
//
// Duration grants are what make CAP's ApplyChargingReport proportionable at all: it reports
// elapsed seconds, and before C2 the rating engine never produced a `GrantedUnit.time` for those
// seconds to be measured against.

TEST(ProportionalDebit, ATimeGrantIsProportionedByElapsedSeconds) {
    // A 60-second grant, 15 seconds used -> a quarter of the money. The per-minute voice case.
    const double debit = chf::proportional_debit(kReserved,
                                                 /*granted_volume=*/0.0,
                                                 /*used_volume=*/0.0,
                                                 /*granted_service_units=*/0.0,
                                                 /*used_service_units=*/0.0,
                                                 /*granted_time=*/60.0,
                                                 /*used_time=*/15.0);
    EXPECT_DOUBLE_EQ(debit, 2.5);
}

TEST(ProportionalDebit, ACallLongerThanItsGrantNeverChargesMoreThanWasReserved) {
    EXPECT_DOUBLE_EQ(chf::proportional_debit(kReserved, 0.0, 0.0, 0.0, 0.0, 60.0, 600.0),
                     kReserved);
}

TEST(ProportionalDebit, AZeroLengthCallChargesNothing) {
    EXPECT_DOUBLE_EQ(chf::proportional_debit(kReserved, 0.0, 0.0, 0.0, 0.0, 60.0, 0.0), 0.0);
}

TEST(ProportionalDebit, TimeAndVolumeAreProportionedIndependentlyNotSummed) {
    // Seconds and octets are not commensurable; each is measured against its own grant. Half of
    // each here -> half the money.
    EXPECT_DOUBLE_EQ(chf::proportional_debit(kReserved, 1'000.0, 500.0, 0.0, 0.0, 60.0, 30.0), 5.0);
}

TEST(ProportionalDebit, ExistingCallersWithNoTimeGrantAreUnaffected) {
    // The backward-compatibility guarantee: the two-dimension call still behaves exactly as it did
    // before the time dimension existed.
    EXPECT_DOUBLE_EQ(chf::proportional_debit(kReserved, 5'000.0, 1'000.0, 0.0, 0.0), 2.0);
}
