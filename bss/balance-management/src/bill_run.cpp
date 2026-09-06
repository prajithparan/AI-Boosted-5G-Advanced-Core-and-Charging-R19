#include "bill_run.hpp"

#include <optional>

namespace billing {
namespace {

// Sum a Money field across line items, keeping the currency of the first value that has one.
// A mixed-currency account is NOT summed into a meaningless number: the currency of the result is
// whatever the items agreed on, and if they disagree the caller sees the first currency with a
// total that is wrong -- so this refuses instead, returning nullopt, and run_bill leaves the
// amount absent rather than stating a figure it cannot justify.
std::optional<bss_sid::Money>
sum_money(const std::vector<const bss_sid::AppliedCustomerBillingRate*>& items,
          const std::optional<bss_sid::Money> bss_sid::AppliedCustomerBillingRate::*field) {
    std::optional<std::string> currency;
    double total = 0.0;
    bool any = false;
    for (const auto* item : items) {
        const auto& money = item->*field;
        if (!money.has_value()) {
            continue;
        }
        if (money->unit.has_value()) {
            if (!currency.has_value()) {
                currency = money->unit;
            } else if (*currency != *money->unit) {
                return std::nullopt; // mixed currencies -- see this function's own comment
            }
        }
        total += money->value.value_or(0.0);
        any = true;
    }
    if (!any) {
        return std::nullopt;
    }
    bss_sid::Money out{};
    out.value = total;
    out.unit = currency;
    return out;
}

} // namespace

BillRunResult run_bill(const std::string& billing_account_id,
                       const std::string& bill_no,
                       const std::string& bill_date,
                       const std::string& period_start,
                       const std::string& period_end,
                       const std::string& payment_due_date,
                       const std::vector<bss_sid::AppliedCustomerBillingRate>& items) {
    BillRunResult result;

    std::vector<const bss_sid::AppliedCustomerBillingRate*> unbilled;
    for (const auto& item : items) {
        if (item.isBilled.value_or(false)) {
            continue; // already on a previous bill -- skipping is what makes a re-run safe
        }
        unbilled.push_back(&item);
    }

    bss_sid::CustomerBill bill{};
    bill.id = bill_no;
    bill.billNo = bill_no;
    bill.billDate = bill_date;
    bill.paymentDueDate = payment_due_date;
    bill.lastUpdate = bill_date;
    bill.category = "normal";
    bill.runType = "onCycle";
    // `new` is the real TMF678 initial state. NOT `sent`: nothing here delivers a bill, and
    // claiming a state the system cannot reach would misrepresent the bill's lifecycle to whatever
    // consumes it next.
    bill.state = bss_sid::CustomerBillState::kNew;

    bss_sid::TimePeriod period{};
    period.startDateTime = period_start;
    period.endDateTime = period_end;
    bill.billingPeriod = period;

    bss_sid::BillingAccountRef account{};
    account.id = billing_account_id;
    bill.billingAccount = account;

    bill.taxExcludedAmount =
        sum_money(unbilled, &bss_sid::AppliedCustomerBillingRate::taxExcludedAmount);
    bill.taxIncludedAmount =
        sum_money(unbilled, &bss_sid::AppliedCustomerBillingRate::taxIncludedAmount);
    // Amount due is the tax-inclusive total when there is one, else the tax-exclusive total. It is
    // NOT defaulted to zero when neither exists: a bill with no derivable amount says so by
    // omitting it, rather than asserting that nothing is owed.
    bill.amountDue =
        bill.taxIncludedAmount.has_value() ? bill.taxIncludedAmount : bill.taxExcludedAmount;
    // Nothing has been paid against a bill that was just created, so the whole amount remains.
    bill.remainingAmount = bill.amountDue;

    bss_sid::BillRef ref{};
    ref.id = bill_no;
    for (const auto* item : unbilled) {
        auto billed = *item;
        billed.isBilled = true;
        billed.bill = ref;
        result.billed_items.push_back(std::move(billed));
    }

    result.bill = std::move(bill);
    return result;
}

} // namespace billing
