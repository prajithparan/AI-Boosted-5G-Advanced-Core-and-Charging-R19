// ADR-0310 (C6): turning rated usage into a TMF678 CustomerBill.
//
// The properties here are the ones a customer notices and an operator has to defend: the total
// must equal the charges it is made of, a re-run must not bill the same usage twice, and a bill
// that cannot derive an amount must not claim one.

#include "../../bss/balance-management/src/bill_run.hpp"

#include <gtest/gtest.h>

namespace {

bss_sid::AppliedCustomerBillingRate
charge(double amount, const std::string& currency = "EUR", bool already_billed = false) {
    bss_sid::AppliedCustomerBillingRate rate{};
    rate.type = "appliedBillingCharge";
    rate.isBilled = already_billed;
    bss_sid::Money money{};
    money.value = amount;
    money.unit = currency;
    rate.taxIncludedAmount = money;
    rate.taxExcludedAmount = money;
    return rate;
}

billing::BillRunResult run(const std::vector<bss_sid::AppliedCustomerBillingRate>& items) {
    return billing::run_bill("acct-1",
                             "BILL-0001",
                             "2026-09-06T00:00:00Z",
                             "2026-08-01T00:00:00Z",
                             "2026-08-31T23:59:59Z",
                             "2026-09-20T00:00:00Z",
                             items);
}

} // namespace

TEST(BillRun, TheTotalIsTheSumOfItsLineItems) {
    const auto result = run({charge(10.50), charge(4.50), charge(85.00)});
    ASSERT_TRUE(result.bill.amountDue.has_value());
    EXPECT_DOUBLE_EQ(result.bill.amountDue->value.value_or(0.0), 100.00)
        << "a bill total that disagrees with its charges is the error a customer notices";
    EXPECT_EQ(result.bill.amountDue->unit.value_or(""), "EUR");
    EXPECT_EQ(result.billed_items.size(), 3u);
}

TEST(BillRun, NothingIsPaidYetSoTheWholeAmountRemains) {
    const auto result = run({charge(42.00)});
    ASSERT_TRUE(result.bill.remainingAmount.has_value());
    EXPECT_DOUBLE_EQ(result.bill.remainingAmount->value.value_or(0.0), 42.00);
}

TEST(BillRun, AlreadyBilledUsageIsNotBilledAgain) {
    // The property that makes a bill run safe to repeat -- an operator re-running a cycle after a
    // failure must not double-charge.
    const auto result = run({charge(10.0), charge(999.0, "EUR", /*already_billed=*/true)});
    ASSERT_TRUE(result.bill.amountDue.has_value());
    EXPECT_DOUBLE_EQ(result.bill.amountDue->value.value_or(0.0), 10.00)
        << "an already-billed line item was charged a second time";
    EXPECT_EQ(result.billed_items.size(), 1u);
}

TEST(BillRun, BilledItemsAreMarkedAndPointAtTheBill) {
    // If the items are not marked, the next run bills them again -- so this is the other half of
    // the re-runnable property above, and it is why run_bill returns both together.
    const auto result = run({charge(1.0)});
    ASSERT_EQ(result.billed_items.size(), 1u);
    EXPECT_TRUE(result.billed_items[0].isBilled.value_or(false));
    ASSERT_TRUE(result.billed_items[0].bill.has_value());
    EXPECT_EQ(result.billed_items[0].bill->id.value_or(""), "BILL-0001");
}

TEST(BillRun, MixedCurrenciesProduceNoAmountRatherThanAWrongOne) {
    // Summing EUR and USD into one number would be silently, confidently wrong. Omitting the
    // amount is visible; a wrong total is not.
    const auto result = run({charge(10.0, "EUR"), charge(10.0, "USD")});
    EXPECT_FALSE(result.bill.amountDue.has_value())
        << "a mixed-currency account produced a single meaningless total";
}

TEST(BillRun, ABillWithNoUsageClaimsNoAmount) {
    // Absent, not zero: "we could not derive an amount" and "you owe nothing" are different
    // statements, and only one of them is true here.
    const auto result = run({});
    EXPECT_FALSE(result.bill.amountDue.has_value());
    EXPECT_TRUE(result.billed_items.empty());
    // The bill itself is still well-formed -- it is a real, valid zero-activity bill.
    EXPECT_EQ(result.bill.billNo.value_or(""), "BILL-0001");
    EXPECT_EQ(result.bill.state.value_or(""), bss_sid::CustomerBillState::kNew);
}

TEST(BillRun, TheBillStateIsTheRealInitialStateNotSent) {
    // Nothing in this project delivers a bill, so claiming `sent` would misrepresent its lifecycle
    // to whatever consumes it next.
    const auto result = run({charge(5.0)});
    EXPECT_EQ(result.bill.state.value_or(""), "new");
}

TEST(BillRun, TheBillingPeriodAndAccountAreCarried) {
    const auto result = run({charge(5.0)});
    ASSERT_TRUE(result.bill.billingPeriod.has_value());
    EXPECT_EQ(result.bill.billingPeriod->startDateTime.value_or(""), "2026-08-01T00:00:00Z");
    EXPECT_EQ(result.bill.billingPeriod->endDateTime.value_or(""), "2026-08-31T23:59:59Z");
    ASSERT_TRUE(result.bill.billingAccount.has_value());
    EXPECT_EQ(result.bill.billingAccount->id.value_or(""), "acct-1");
}
