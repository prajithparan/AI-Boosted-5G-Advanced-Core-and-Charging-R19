## ADR-0314: a bill must cover the account, not one subscriber on it

**Date:** 2026-09-07. **Status:** accepted. **Fixes:** a hole in ADR-0311, shipped two commits earlier.

ADR-0311's bill-run endpoint took `billingAccountId` and `subscriberIdentifier` as **independent**
inputs, with nothing connecting them. For a single-subscriber account that is correct. For a family
account -- which ADR-0307's shared buckets had just made possible -- it bills only the named
subscriber and silently omits the usage of everyone else on the account.

**Under-billing is the failure mode that generates no complaint.** Nobody rings to report a bill
that is too low, so this would have sat there indefinitely. That asymmetry is why it is worth
fixing immediately rather than filing.

### Resolution order, most explicit first

1. an explicit `subscriberIdentifiers` array -- the operator stated exactly who
2. a single `subscriberIdentifier` -- unchanged from ADR-0311, so nothing existing breaks
3. otherwise, **the members of the billing account's own shared bucket**, read from
   balance-management's TMF654 `relatedParty`
4. no shared bucket -> every subscriber in the period, the pre-existing whole-cycle behaviour

Step 3 deliberately reads **the same membership record the charging path already draws down
against** (ADR-0307). Two separate notions of "who is on this account" would drift, and the drift
would show up as money -- usage reserved against a family bucket but billed to nobody, or the
reverse.

### Disclosed: one query per member

The store's filter takes a single subscriber, so N members mean N queries rather than one
`IN (...)`. Widening the billing query is a change worth its own test rather than something
smuggled into this fix.

### Also corrected

I hardcoded `/tmf-api/accountManagement/v4/` for the bucket lookup. The real root is
`prepayBalanceManagement/v4`, already exported as `kBalanceManagementApiRoot` -- now used, rather
than a second, wrong copy of a path that already had a constant.

### Method note, since it cost real time

Three OOM kills during this small change were NOT compile size. Measured: CHF's `main.cpp` needs
2.55 GB and 19 s -- unremarkable. The cause was a Doris container I had started for ADR-0311's
tests and left resident (~1.6 GB JVM), which consumed exactly the headroom ADR-0313's split had
just created. Stopping it took free memory 8 GB -> 10 GB and the identical `-j2` build completed
clean.

Two real, separate causes -- an oversized generated TU (fixed in ADR-0313) and a container I left
running -- plus one explanation I invented to bridge them ("CHF's main.cpp has become expensive")
that measurement disproved. The working sequence is: stop Doris, build, start Doris, run tests.
Only the two billing tests and the CDR retention test need it.

580/580 against a verified-current build.

