## ADR-0310: bill generation -- and fetching the spec instead of recalling it

**Date:** 2026-09-06. **Status:** accepted. **Implements:** ADR-0300's C6, the last mandate item.

README listed postpaid billing as Partial: "CDRs land in Doris with retention and archival; there
is no bill generation, invoicing or dunning." TMF678's own line-item type,
`AppliedCustomerBillingRate`, has existed in `libs/bss-sid` since ADR-0060 and **nothing has ever
produced one**. This adds the aggregation step: line items -> a real `CustomerBill`.

### The spec was fetched, not remembered

`libs/bss-sid/include/bss_sid/rating.hpp` already records why this matters: an earlier pass had
`appliedBillingRateType` in `docs/DATA_MODEL.md`, and fetching the real swagger found the field is
simply `type` -- the other name does not exist anywhere in TMF678.

So `CustomerBill`'s field list, the `stateValue` enum
(`new`/`onHold`/`validated`/`sent`/`partiallyPaid`/`settled`) and `BillingAccountRef`'s shape all
come from cloning `github.com/tmforum-apis/TMF678_CustomerBill` and parsing
`TMF678-CustomerBill-v4.0.0.swagger.json` directly -- the same source and the same method this
file's header already cites. The real definition has **no required fields**, so every member is
optional, matching it.

Six real fields -- `appliedPayment`, `billDocument`, `financialAccount`, `paymentMethod`,
`relatedParty`, `taxItem` -- are **omitted** rather than modelled and left permanently empty. This
project has no payment, attachment, financial-account or tax subsystem to source them from, and an
always-empty array would let a consumer conclude a bill genuinely had no payments.

### Three fail-visible choices

Same instinct as ADR-0306's TAP audit totals: when a number cannot be justified, do not state one.

- **Mixed currencies produce NO amount.** Summing EUR and USD gives a confidently wrong figure that
  nothing downstream can detect. An absent total is visible; a wrong one is not.
- **A zero-activity bill omits `amountDue`** rather than setting it to zero. "We could not derive an
  amount" and "you owe nothing" are different statements and only one is true.
- **State is `new`, not `sent`.** Nothing in this project delivers a bill, and claiming a state the
  system cannot reach misrepresents the lifecycle to whatever consumes it next.

### Re-runnability is one atomic result

`run_bill` returns the bill **and** its line items with `isBilled` set and their `bill` reference
populated. They are returned together because a bill whose items were not marked would re-bill the
same usage on the next run -- that is one outcome, not two steps a caller could get half-right.
Already-billed items passed in are skipped rather than rejected, so handing over an account's whole
history is safe and a cycle can be re-run after a failure without double-charging. Both halves are
tested.

### Scope, so this is not read as a billing system

It produces a bill FROM line items. It does not produce the line items from CDRs (a query over the
Doris CDR store), does not deliver, dun or take payment, and has no bill-cycle scheduler.

578/578 against a verified-current build.

---

