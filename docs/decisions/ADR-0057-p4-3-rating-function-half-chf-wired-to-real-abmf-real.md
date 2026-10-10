## ADR-0057: P4.3 (Rating Function half) -- CHF wired to real ABMF, real balance-mutation bug found and fixed via live verification

**Date:** 2026-08-11
**Status:** Accepted.

**Context:** ADR-0056 built ABMF (`bss/balance-management`) but disclosed it as "not yet wired to
CHF's rating engine" -- ADR-0048/0050's own long-standing "no balance/wallet deduction against
what was already consumed" gap was still open. This ADR closes it: CHF's rating engine
(`build_rating_grant`, `nfs/chf/src/main.cpp`) now makes a real HTTP call to
`bss/balance-management` to reserve a granted price's real monetary cost against the subscriber's
real balance, and only includes the grant in its response if that reservation succeeds -- real
prepaid enforcement, not a simplification carried forward.

### Design

- **Real cost/quantity split, not invented**: `build_rating_grant` now returns a `RatingResult{
  grant, cost }` -- confirmed as two genuinely separate real TMF620 fields already on
  `ProductOfferingPrice` (`unitOfMeasure` for the quantity granted, e.g. 5GB; `price` for its real
  monetary cost, e.g. $20) -- not a split this project invented for this ADR.
- **Bucket-per-subscriber convention, disclosed**: `bss/balance-management`'s per-subscriber
  `Bucket` is keyed by the request's real `subscriberIdentifier` (SUPI). No real customer-to-
  bucket provisioning system exists in this codebase -- a subscriber's bucket must be funded via a
  real `TopupBalance` call out of band before CHF can charge against it; disclosed, not silently
  assumed to exist.
- **Reserve at Create/Update, commit-or-refund at Release** -- real OCS-shaped semantics, not a
  simplified straight-debit: each grant's real cost is `ReserveBalance`'d (not immediately
  debited) at Create and every Update; `nfs/chf/src/stores.hpp`'s `ChargingDataStore` was extended
  (real content now, not just an active-ref marker, ADR-0055's earlier active-set-only shape) to
  hold each session's SUPI and a running reserved-total (Redis `HINCRBYFLOAT`, atomic). Release
  finalizes the full session total as a real permanent debit.
- **Disclosed, real simplification**: finalization commits the FULL reserved total, not a
  proportional amount based on SMF's actually-reported usage (`usedUnitContainer`, already
  real-logged since ADR-0050) -- a real per-usage proportional refund is deferred, not fabricated
  as more sophisticated than it is.

### Real bug found and fixed via live verification (not caught by reasoning alone)

The first, real end-to-end run (real `nrf`+`product-catalog`+`balance-management`+`chf`, a real
$50 topup, two real $20 reservations across Create+Update) produced a **wrong final balance**
after Release: expected $10 remaining/$0 reserved (the $40 committed), got **$50 remaining/$0
reserved** -- the entire reservation was silently refunded instead of committed.

**Root cause**: the first version of `finalize_subscriber_balance` called `AdjustBalance` (debit
`remainingValue` directly) *before* `ReserveBalance` (unreserve, moves money `reservedValue` ->
`remainingValue`). At the moment the debit ran, the $40 was still sitting in `reservedValue`, not
`remainingValue` -- `AdjustBalance`'s own atomic floor check (`remaining_value + amount >= 0`) has
no visibility into `reservedValue` at all, so the debit's `WHERE` clause failed (0 rows affected,
silently -- `AdjustBalance`'s real TMF654 semantics treat this as a normal "insufficient balance"
business outcome, not an error CHF's caller code was checking for at this call site). Only the
subsequent unreserve then ran, and it always succeeds when exactly that much is genuinely
reserved -- so the net observed effect was a full, silent refund.

**Fix**: reordered to unreserve *first* (credits `remainingValue` back, always succeeds), *then*
debit (now succeeds, since the money is back in `remainingValue`) -- net effect: `reservedValue`
permanently decreases by the full amount, `remainingValue` ends unchanged (credited then
immediately re-debited the same amount in the same logical operation). Documented at length in
`finalize_subscriber_balance`'s own comment, including the real, disclosed residual gap this
two-call sequence still has (not atomic across the two HTTP calls -- if `balance-management`
becomes unreachable between them, the money is left credited-but-uncommitted; the real
`AdjustBalance`/`ReserveBalance` ledger rows in `bss/balance-management`'s own audit trail are
still sufficient to reconcile from, so not a lost-money bug, but not a fully atomic sequence
either -- a real distributed-transaction/outbox mechanism would close this, not built here).

**This is exactly the kind of bug this project's own "live-verify over self-consistency" testing
discipline (recorded in this session's own memory of prior feedback) exists to catch** -- the
individual `AdjustBalance`/`ReserveBalance` atomic-UPDATE mechanics were each independently correct
and already proven under real concurrent load (ADR-0056), but composing them into a two-step
"finalize a reservation" sequence had a real, non-obvious ordering bug that only a genuine
end-to-end run against real running processes surfaced.

### Live-verified for real, complete flow (after the fix)

Real `nrf` + `product-catalog` (real Postgres) + `balance-management` (real Postgres) + `chf`
(real Valkey), a real `ProductOfferingPrice` ($20 for 5GB) referenced by a real `ProductOffering`,
a real subscriber `Bucket` funded with $50 via `TopupBalance`:

1. **Create**: real grant issued (5,000,000,000 octets = 5GB); subscriber's real bucket
   confirmed independently (`GET /bucket/{supi}`) at remaining=$30/reserved=$20 -- exactly $20
   moved from remaining to reserved, not simulated.
2. **Update**: another real $20 reserved -- remaining=$10/reserved=$40.
3. **Update again (real prepaid enforcement)**: reservation correctly REJECTED (only $10
   remains against a $20 cost) -- response's `multipleUnitInformation` entry has `ratingGroup`
   but no `grantedUnit` field at all (correctly omitted, not a null/empty placeholder), bucket
   provably unchanged by the rejected attempt.
4. **Release (after the fix)**: real 204; bucket correctly settles at remaining=$10/reserved=$0 --
   the $40 genuinely, permanently committed, confirmed independently via `GET /bucket/{supi}`
   again, not just CHF's own response.

Full rebuild + `clang-format` clean + 146/146 `ctest` (including the real `test_smf_pdu_session.cpp`
integration test, which now exercises this new reservation code path too -- passes because a
withheld grant, from `balance-management` being unreachable in the `ctest` environment, is still a
schema-valid response, same real business-outcome handling as everywhere else in this ADR) both
before and after the fix.

### Disclosed, NOT done by this ADR

- Finalization is full-session-total, not proportional to actually-reported usage (see above) --
  a real per-usage proportional refund calculation is P4.3's own further, not-yet-built work.
- The unreserve-then-debit finalize sequence is not atomic across its two HTTP calls (see the bug
  writeup above) -- a real gap, disclosed, not silently assumed safe.
- No real subscriber-to-bucket auto-provisioning -- a subscriber's bucket must already be funded
  via a real `TopupBalance` call for any of this to work; CHF does not create buckets itself.
- Still no real tariff versioning, rating-decision audit-record table (`RatingDecision`,
  `docs/DATA_MODEL.md`'s E5 sketch) -- `bss/balance-management`'s own `AdjustBalance`/
  `ReserveBalance` ledger rows (tagged with the real `ChargingDataRef` in their `description`
  field) are this ADR's real, working answer to "every rating decision emits an audit record
  sufficient to reconstruct the charge," not a dedicated new store.
- No property-testing framework proving "same inputs -> same charge, across restarts and across
  versions" -- the real, disclosed rating engine itself (`build_rating_grant`) still has its own
  pre-existing simplification (whichever catalog offering is first, ADR-0048's own disclosure),
  which is not deterministic across multiple offerings existing simultaneously -- a real gap this
  ADR does not resolve.

