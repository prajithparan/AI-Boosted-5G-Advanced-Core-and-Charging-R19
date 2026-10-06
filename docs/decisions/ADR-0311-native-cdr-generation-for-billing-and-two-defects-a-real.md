## ADR-0311: native CDR generation for billing -- and two defects a real engine found

**Date:** 2026-09-06. **Status:** accepted. **Closes:** the input gap ADR-0310 disclosed.

ADR-0310 built `billing::run_bill` and said plainly that nothing produced its input. The reason was
narrower than it looked: **`CdrWriter` could only INSERT.** It could count sequence gaps and sweep
old rows, and that was all -- no code path anywhere read a CDR back. So a bill could never be built
from real usage, and neither could a TAP batch.

### The chain

`CdrWriter::query()` (period, subscriber, and -- for later -- serving PLMN / roaming) ->
`chf::cdrs_to_billing_items()` -> `billing::run_bill()`, driven by `POST /internal-billing/v1/runs`.

**Only `Release` rows are billed.** A session writes Create/Update/Release and reserved cost
accumulates across them; billing every row charges a session several times over. The test proves
this with a session whose three rows carry 2.0 + 3.0 + 5.0 -- the bill is 5.0, not 10.0. That error
would look entirely plausible on an invoice, which is why it is the test's headline assertion.

A row with no `reserved_cost` produces **no** line item: nothing was committed against the
subscriber's balance, so billing it would charge for usage that was never rated.

`taxIncludedAmount` is set equal to `taxExcludedAmount` because this project has no tax engine. It
is populated so a bill total exists, and stated in the header as pre-tax amounts labelled as both
-- NOT a tax calculation returning zero.

### Two defects a real Doris found that reasoning did not

1. **Doris rejects `DEFAULT FALSE` for `BOOLEAN`** ("mismatched input 'FALSE'"). It wants a
   string/integer literal. My schema change used portable SQL; standing up a real
   `apache/doris:all-in-one-4.1.3` and applying the schema is what caught it. Deferred to CI this
   would have failed there -- or worse, the column would silently not exist in a hand-applied
   deployment.

2. **CDR timestamps were written with `std::localtime`.** The column is an unqualified `DATETIME`,
   so a local-time value carries no offset at all. TS 32.291's `invocationTimeStamp` is an ABSOLUTE
   time; flattening it to whatever timezone the CHF host runs in means two CHF instances in
   different regions write incomparable timestamps into one table, and a billing-period query
   silently selects the wrong rows. It was also the non-reentrant variant. Now `gmtime_r`.
   **Disclosed migration consequence:** rows written before this hold local time, rows after hold
   UTC, and nothing rewrites the old ones.

The second is the more serious, and it is exactly the class of bug that survives review: the code
looked right, the write succeeded, and the data was wrong in a way only a cross-component query
reveals.

### A test mistake worth recording

The first version of the test hardcoded a fixture timestamp, **commented it as a date it was not**
(`1'788'000'000` is 2026-08-29, not "2026-09-05-ish"), and queried a window the row fell outside --
so the test failed while the code under test was correct. The window is now DERIVED from the same
constant, which removes the class of error rather than fixing one instance of it.

### Still missing, so "billing works" is not over-read

- **No bill-cycle scheduler.** The run is on demand only.
- **No account -> subscriber mapping.** The endpoint takes `billingAccountId` and
  `subscriberIdentifier` independently and nothing checks the subscriber belongs to that account.
  For a family account (ADR-0307's shared buckets) the correct query is "every subscriber on this
  account" -- this is the gap most likely to bite, because the code works perfectly for a
  single-subscriber account and quietly under-bills a family.
- **No delivery, dunning or payment** (unchanged from ADR-0310).
- **The runner lives in CHF as staging, not architecture.** Billing is BSS work; it is here because
  CHF owns the CDR store and `CdrRecord` is its private type. The correct end state is a BSS-side
  runner pulling CDRs over SBI. The endpoint is deliberately mounted OUTSIDE `kApiRoot`
  (`/internal-billing/v1/runs`) so it cannot be mistaken for a real Nchf operation -- TS 32.291 has
  no bill-run operation and inventing one under `/nchf-convergedcharging/v3/` would fabricate a
  3GPP API.

580/580 against a verified-current build, with the CDR tests running for real against Doris.

