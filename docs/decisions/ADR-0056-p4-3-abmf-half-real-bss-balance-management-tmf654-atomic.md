## ADR-0056: P4.3 (ABMF half) -- real bss/balance-management (TMF654), atomic strong consistency proven under real concurrent load

**Date:** 2026-08-11
**Status:** Accepted.

**Context:** P4.2 closed (ADR-0055); CHARGING_PROMPT.md's P4.3 ("Rating engine (E5) + ABMF (E6)")
is next. Given ABMF exposes a real TM Forum Open API (TMF654), not a 3GPP Nchf_* service, it
belongs as its own standalone BSS component -- same "align to TM Forum ODA component boundaries"
reasoning `bss/product-catalog` already established, not code folded into `nfs/chf`.

### Real TMF654 research, before any code

Fetched and parsed the real TMF654 v4.0.0 swagger directly
(`tmforum-apis/TMF654_PrepayBalanceManagement`,
`TMF654-PrepayBalance-v4.0.0.swagger.json`) -- confirmed field lists for `Bucket`,
`AccumulatedBalance`, `TopupBalance`, `AdjustBalance`, `ReserveBalance`, plus the enum types
`BucketStatusType`, `UsageType`, `AdjustType`, `ActionStatusType`.

**Real, important correction to this session's own earlier work**: `docs/DATA_MODEL.md`'s E6
section (written during P4.1) had guessed `Bucket.usageType`'s enum was
`{MAIN, BONUS, PROMOTIONAL}` (a "which balance pool" distinction). Re-checking the real swagger
directly found the actual enum is `{monetary, voice, data, sms, other}` -- what KIND of quantity a
bucket tracks, not which pool it belongs to. TMF654 has no fixed enum for
main/bonus/promotional at all; that distinction is modeled as **separate `Bucket` resources**,
distinguished by `name`/`description`, matching real telco balance-management practice. Fixed in
`docs/DATA_MODEL.md` directly (not silently carried forward into this ADR's schema) before writing
any balance-management code.

**Real, disclosed API-surface finding**: TMF654's real `/bucket` path has **only `GET`** -- no
`POST /bucket` exists at all. A bucket only comes into being via a real operation that funds it.
This project's own interpretation (not confirmed from spec prose, disclosed as such in
`bss_sid/balance.hpp` and `bss/balance-management/src/main.cpp`'s own file headers): `TopupBalance`
implicitly creates the bucket it references if that bucket doesn't already exist, since
`TopupBalance.bucket`'s own field description says "a reference to the bucket impacted by the
request -- or the value itself." Similarly, `ReserveBalance`'s real spec text names both "Reserve"
and "Unreserve" as real operations on the same resource but does not document the mechanism
distinguishing them -- resolved here as sign-of-`amount` (positive reserves, negative
unreserves/refunds), the least-invented interpretation for one endpoint serving two directionally
opposite real operations, disclosed rather than silently assumed.

### Scope

`Bucket` (GET only, per the finding above), `TopupBalance` (POST/GET), `AdjustBalance` (POST/GET
-- real signed debit/credit), `ReserveBalance` (POST/GET -- real signed reserve/unreserve),
`AccumulatedBalance` (GET, filtered by `partyAccount.id` -- disclosed: this exact query parameter
is not itemized in the real swagger's own `parameters` list for this operation, only
`fields`/`offset`/`limit` are; implemented per TM Forum's well-known general attribute-path
filtering convention, not confirmed from this specific spec file's text). Deliberately not
modeled: `TransferBalance`, `BalanceActionHistory` -- real resources, not needed to prove P4.3's
core ask.

### Persistence: PostgreSQL alone, a disclosed deviation from `docs/DATA_MODEL.md`'s original E6 sketch

`docs/DATA_MODEL.md`'s E6 originally sketched two stores (Redis hot balance + PostgreSQL durable
ledger, mirroring how `nfs/chf`'s own stores were extended earlier this session). This ADR
deviates: `Bucket.remainingValue`/`reservedValue` live in PostgreSQL alone, mutated via a
single-statement atomic `UPDATE ... WHERE remaining_value >= $amount` (or the reserve/unreserve
equivalent). **Reasoning**: this single statement already gives genuine, provable strong
consistency via PostgreSQL's own row-level locking and MVCC -- CHARGING_PROMPT.md's P4.3 explicit
ask ("prove it under concurrent debit tests") is fully satisfiable this way, with none of the
two-store desync risk a Redis-hot-value-plus-Postgres-ledger design would introduce if a process
crashed between the two writes. A Redis hot-path cache remains a real, valid future optimization
once real throughput numbers justify it (nothing benchmarked yet, ADR-0049's standing disclosure)
-- not added speculatively now for a correctness property already fully met.

### Real infrastructure fix along the way: query-string parsing added to `sbi_core::http2::Server`

`AccumulatedBalance`'s real filter needed a query parameter (`?partyAccount.id=...`), and
`libs/sbi-core/include/sbi_core/http2_server.hpp`'s `Request` had **no query-string support at
all** -- confirmed by reading `http2_server.cpp` directly: the existing code already split the
query string off (`ctx.path.substr(0, ctx.path.find('?'))`) for path-template matching but simply
discarded it, a real, pre-existing infrastructure gap, not something worked around with a
non-standard path-param route. Fixed properly: added a real RFC 3986 percent-decoder (`+` also
decoded to space, the `application/x-www-form-urlencoded` convention real HTTP clients follow for
query strings) and a real query-string parser, wired into `Request::query_params` (a
`std::multimap`, matching `headers`' own repeated-key convention). Small, bounded, verified
addition -- `sbi_core` rebuilt clean before use.

### Live-verified for real, including the actual point of this ADR

Started a real `balance-management` process against a real `postgres:16-alpine` container:

1. **Full lifecycle, real HTTP**: `TopupBalance` creates a bucket with a real balance;
   `AdjustBalance` debits/credits correctly, and a debit that would overdraw is rejected with
   `status: "failed"` (a real business outcome, not an HTTP error -- the attempt is still recorded,
   same real-audit-trail discipline as everywhere else in this project) while leaving the real
   balance provably unchanged; `ReserveBalance` reserves and unreserves correctly (verified moving
   value between `remainingValue`/`reservedValue` in both directions, and rejecting an over-large
   reserve); `AccumulatedBalance` correctly aggregates.
2. **The real point of this ADR -- concurrent-debit strong consistency, proven empirically**:
   funded a bucket with exactly $1000, then fired **100 real, concurrent** `AdjustBalance` debit
   requests of $15 each ($1500 total demand, 20-way real OS-level parallelism via `xargs -P 20`)
   against the running server. Result: **exactly 66 succeeded, 34 correctly rejected, final
   balance exactly $10.00 (= $1000 - 66x$15)** -- mathematically exact, proving no lost updates and
   no overdraft occurred under real concurrent load, not asserted or unit-tested in isolation.
   Independently confirmed via a direct SQL query against the real audit ledger (66
   `completed`/34 `failed` rows), not just the app's own HTTP responses.

Full rebuild (whole project, including the `sbi_core` query-string change) + `clang-format` clean
+ 146/146 `ctest` before and after.

### Disclosed, NOT done by this ADR

- Not wired into `deploy/docker/docker-compose.yml`/CI yet -- same disclosed gap
  `bss/product-catalog` had before its own separate follow-up closed it; verified via a manually-run
  postgres container here too.
- **Not yet wired to CHF's rating engine** (`build_rating_grant` in `nfs/chf/src/main.cpp`) -- CHF
  still does not call this service to actually debit a real balance when it grants units, so
  ADR-0048/0050's own disclosed "no balance/wallet deduction against what was already consumed"
  gap is still open. That real integration is P4.3's **Rating Function (E5)** half -- a separate,
  not-yet-built piece; this ADR is ABMF (E6) only.
- No `TransferBalance`/`BalanceActionHistory`, no multi-currency conversion in `AccumulatedBalance`
  (disclosed directly in its own response via a `description` warning if a party account's buckets
  mix units, rather than silently summing incompatible values), no tariff versioning (that's E5's
  concern, not E6's).
- No automated integration test exists for `balance-management` specifically -- same "real manual
  live-verification recorded in its ADR" pattern this project already established for `nfs/chf`,
  followed here, not newly introduced.

---

