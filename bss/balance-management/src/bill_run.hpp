#pragma once

#include <string>
#include <vector>

#include "bss_sid/rating.hpp"

// ADR-0310 (C6 of ADR-0300): turning rated usage into a TMF678 CustomerBill.
//
// README listed postpaid billing/invoicing as Partial: "real TS 32.298 BER-encoded CDRs land in
// Doris with retention and archival; there is no bill generation, invoicing or dunning."
// `AppliedCustomerBillingRate` -- TMF678's own line-item type -- has existed in `libs/bss-sid`
// since ADR-0060 and **nothing has ever produced one**. This is the aggregation step that does.
//
// Scope, stated up front so this is not read as a billing system: this produces a BILL from line
// items. It does not produce the line items from CDRs (that is a query over the Doris CDR store),
// it does not deliver, dun, or take payment, and it has no bill-cycle scheduler. Those are named
// in the ADR rather than implied.

namespace billing {

// The result of a bill run: the bill, plus the line items it covers with their `bill` reference
// and `isBilled` set. Returned together because a bill whose line items were not marked billed
// would be re-billed on the next run -- the two are one atomic outcome, not two steps a caller
// could get half-right.
struct BillRunResult {
    bss_sid::CustomerBill bill;
    std::vector<bss_sid::AppliedCustomerBillingRate> billed_items;
};

// Aggregate `items` into one bill for `billing_account_id`.
//
// Totals are DERIVED from the line items -- the same discipline ADR-0306 applied to TAP audit
// totals, and for the same reason: a bill total that disagrees with the charges it is made of is
// the error a customer notices and an operator cannot defend. There is no parameter to pass one.
//
// Only items that are NOT already billed contribute. Passing an already-billed item is not an
// error (a caller may hand over a whole account's history); it is skipped, which is what makes the
// run safely repeatable.
BillRunResult run_bill(const std::string& billing_account_id,
                       const std::string& bill_no,
                       const std::string& bill_date,
                       const std::string& period_start,
                       const std::string& period_end,
                       const std::string& payment_due_date,
                       const std::vector<bss_sid::AppliedCustomerBillingRate>& items);

} // namespace billing
