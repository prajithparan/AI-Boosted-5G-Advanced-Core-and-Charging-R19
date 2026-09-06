#pragma once

#include <string>
#include <vector>

#include "bss_sid/rating.hpp"
#include "cdr.hpp"

// ADR-0311: native CDR -> TMF678 billing line items.
//
// This is the step ADR-0310 explicitly said it did NOT do: `billing::run_bill` aggregates
// `AppliedCustomerBillingRate` items into a `CustomerBill`, but nothing produced those items, so
// the bill run had no input and postpaid billing stopped one step short of working. CHF holds the
// rated usage in its own CDR store; this turns it into the BSS's own line-item type.
//
// Direction matters for layering: this lives in `nfs/chf` because `CdrRecord` is CHF's own private
// type, and it produces `bss_sid::AppliedCustomerBillingRate`, which is a SHARED library type both
// sides already depend on. The reverse -- teaching the BSS to read CHF's private CDR schema --
// would breach this project's own "no component reaches into another's private headers" rule.

namespace chf {

// One line item per CDR row.
//
// Real, disclosed mapping, every field either copied from the CDR or derived from it, nothing
// invented:
//   date                -> invocation_time_stamp, as RFC 3339
//   taxExcludedAmount   -> reserved_cost + reserved_cost_currency (the ABMF amount actually
//                          committed for this session, ADR-0057/ADR-0297)
//   taxIncludedAmount   -> the SAME value, because this project has no tax engine. It is set
//                          rather than omitted so a bill total exists, and the equality is stated
//                          here: these are pre-tax amounts labelled as both, NOT a tax
//                          calculation of zero.
//   type                -> "appliedBillingCharge" (real TMF678 enum value)
//   isBilled            -> false; `billing::run_bill` is what flips it
//   periodCoverage      -> the session's own invocation timestamp as both bounds; CHF does not
//                          record a session start/stop pair on the CDR, so a wider period would
//                          be fabricated
//   characteristic      -> ratingGroup and chargingDataRef, so a disputed charge can be traced
//                          back to the exact session and rating group that produced it
//
// A row with no `reserved_cost` produces NO line item: nothing was committed against the
// subscriber's balance for it, so billing it would charge for usage that was never rated.
std::vector<bss_sid::AppliedCustomerBillingRate>
cdrs_to_billing_items(const std::vector<CdrRecord>& cdrs);

} // namespace chf
