## ADR-0446: CHF's catalog/policy snapshot -- increment 1 of ADR-0445, design + baseline

**Date:** 2026-10-02. **Status:** in progress -- the fixed-point money type (step 1) is built and
tested; the baseline measurement (step 2) and the snapshot itself (step 3) are this ADR's remaining
work, recorded incrementally rather than written up only after everything is done, per this
project's own "small, reviewable increments" rule.

**Step 1, done:** `bss_sid::Micros` (`libs/bss-sid/include/bss_sid/money_exact.hpp`/`.cpp`), a
fixed-point int64 micro-unit type, exact for decimal-text input, documented-lossy for the
`double`-typed wire boundary it has to interoperate with today. 8 unit tests including a
2000-iteration property-based round-trip, 21/21 `bss_sid` tests pass (no regressions),
clang-format clean. Full detail in that commit's own message. Deliberately does NOT change
`bss_sid::Money`/`Quantity`'s field types (still real TMF620/654 `double`) -- that ripples into
`gui/schema-gen`'s derived schemas, `bill_run`, `billing_items`, `rating_decision_store` and
`balance-management`, out of scope for this increment per ADR-0445.

**Snapshot design decisions, resolved by checking the real code rather than assuming:**

1. **Bulk routes already exist -- no new product-catalog endpoint needed.** Checked directly:
   `bss/product-catalog/src/main.cpp` already serves `GET /productOffering` (collection, no ID) and
   `GET /productOfferingPrice` (collection, no ID) -- the second one is what closes finding #1's
   N+1 loop (`charging_engine.cpp` fetches the offering collection once already; it is the
   per-price `GET /productOfferingPrice/{id}` loop that is the real N+1 call site). The snapshot's
   refresh fetches both collections, not individual resources.
2. **Refresh mechanism: periodic re-fetch + `lastUpdate`/`version` comparison, not an invented
   endpoint.** Checked: `bss/product-catalog` implements no TMF Hub/listener callback mechanism
   (disclosed in its own file header as deliberately deferred) -- there is no push notification to
   build against. `ProductOffering`/`ProductOfferingPrice` both carry a real `lastUpdate` field
   (confirmed TMF620 field, already modeled in `bss_sid`). The snapshot polls on a bounded interval
   (config-driven, default TBD at implementation time) and compares `lastUpdate` per resource to
   decide whether to replace its cached copy -- no catalog-version endpoint invented.
3. **ADR-0330 pinning is already satisfied at the price level -- no new schema needed.**
   `RatingDecisionRecord.tariffVersion` (`nfs/chf/src/rating_decision_store.hpp`) is already
   populated from the real `ProductOfferingPrice.version` field at rating time
   (`charging_engine.cpp:313`, `result.tariffVersion = price.version`) and already written to
   `rating_decision` (`:663`). As long as the snapshot preserves each cached price's own `.version`
   field unchanged, this guarantee carries over automatically -- a session rated against snapshot
   generation N keeps exactly the price version it was actually rated against, which is a tighter,
   more precise pin than a single whole-snapshot version number would have been. No new column on
   `rating_decision` is needed for this.
4. **No regression found against CHARGING_PROMPT P4.7's "new tariff, no code change, no restart"
   requirement -- but it has never actually been tested, and the snapshot must still honor it.**
   Searched `tests/` for an existing test proving this specific behavior against an
   already-running, already-warmed CHF process: none exists. The integration tests that do
   provision-then-charge (`test_cap_scoped_charging.cpp`, `test_chaos_charging.cpp`-style) seed
   catalog data directly into Postgres *before* spawning `chf`, so a startup-time snapshot would
   see that data at warm-up with no staleness window -- these tests do not regress. The underlying
   requirement is still real for a long-lived production CHF process an operator updates live, so
   the snapshot's bounded refresh interval (item 2) is what keeps this true going forward, not a
   test that happens to pass today.
5. **Unknown rating group at request time: grant nothing, log it, same behavior as today.**
   `charging_engine.cpp` already handles "no Active/isSellable ProductOfferingPrice configures
   ratingGroup N" by granting nothing (not a fabricated grant) -- the snapshot preserves this
   exactly; a rating group genuinely absent from the current snapshot (new tariff not yet
   refreshed in, or truly nonexistent) is indistinguishable from today's live-lookup miss and gets
   the same, already-correct, already-disclosed treatment. No new "stale vs. missing" distinction
   is invented.
6. **Readiness: revised during implementation -- graceful fallback, not a 503 gate.** The plan above
   (503 until the first snapshot load succeeds) was reconsidered while building step 3 and changed,
   because a real alternative turned out to be strictly better: `build_rating_grant` already falls
   back to the exact pre-ADR-0446 live-fetch behaviour whenever `catalog_snapshot` is null or not
   yet `ready()` (see `try_rate_against`'s extraction, same function, same match semantics, used by
   both paths). A snapshot that has not warmed up yet therefore costs latency, not availability --
   CHF serves every request correctly from the moment it starts, exactly as it always has, and
   transparently switches to the fast path the instant the first refresh succeeds. This avoids
   adding a new crash/refuse-on-startup class of failure (COMPLIANCE_P1_P15.md blocker 0a already
   names enough of those) for a pure performance optimization that does not need one. Observability
   instead of a gate: `chf_catalog_snapshot_generation` (bumped per successful refresh) and
   `chf_catalog_snapshot_age_seconds` (-1 until the first refresh) are real Prometheus gauges, so
   "never refreshed" and "stale" are both visible without refusing service over either.
7. **Replica skew, disclosed rather than solved.** Round-robin CHF replicas (the existing
   comma-separated peer-base mechanism, `project_autoscaling_mandate`) can briefly hold different
   snapshot generations after a refresh. Not a new problem this increment introduces -- today's
   live per-request lookups have zero skew because there is no cache at all -- but a real,
   disclosed cost of adding one. Bounded by the same refresh interval as item 2; no cross-replica
   coordination is built to tighten it further in this pass.
8. **SUPI->bucket resolution stays per-request -- not decided by this ADR, said so rather than left
   implied.** `charging_engine.cpp`'s `resolve_bucket_id` (ADR-0307, shared/family bucket lookup)
   is a separate SBI call to balance-management, not a catalog/policy lookup, and this increment's
   snapshot does not cache it. Caching bucket membership would need its own invalidation story (a
   subscriber can join/leave a shared bucket at any time) that this ADR has not designed --
   deferred, explicitly, not silently kept out of scope.

**Baseline measurement (step 2): method fixed here, numbers appended once run.**
`scripts/run-chf-rating-baseline.sh`, same "method committed before seeing numbers" discipline
`docs/BENCHMARK_METHOD.md` established for ADR-0329. Measures CHF's real
`Nchf_ConvergedCharging_Create` path end to end -- real NRF-registered CHF, a real product-catalog
`ProductOfferingPrice` (`ratingGroup=10`), a real balance-management bucket -- at closed-loop
concurrency 1/8/32 and open-loop 200 rps, 15s per case after a 2s warmup, asserting every run is
all-201. Disclosed scope limits, stated before any number exists so they cannot be read as
after-the-fact excuses: every request in a run carries the same `ChargingDataRequest` (same SUPI,
same bucket) because `sbi-loadgen` has no per-request body templating -- this measures single-SUPI/
single-bucket serialized-reservation contention, not independent-subscriber traffic; load generator
and CHF share one host over loopback (inflates latency, caps throughput, same caveat
`run-baseline-benchmark.sh` already discloses for NRF); ADR-0009's synchronous HTTP client is still
open.

**Result (2026-10-02), raw files in `docs/benchmark-chf-2026-10-02/`:**

| Case | Responses | Throughput | p50 | p90 | p99 | p99.9 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| closed c1 | 776 | 51.72 req/s | 17.7 ms | 27.8 ms | 43.7 ms | 71.6 ms | 71.6 ms |
| closed c8 | 1485 | 98.49 req/s | 75.7 ms | 99.0 ms | 189.3 ms | 264.3 ms | 269.6 ms |
| closed c32 | 1591 | 104.80 req/s | 301.0 ms | 366.1 ms | 463.6 ms | 517.0 ms | 518.5 ms |
| open 200 rps | 3000 (93.19 delivered) | 93.19 req/s | 9500.4 ms | 15495.7 ms | 17023.5 ms | 17209.3 ms | 17211.1 ms |

All-201 on every case (7852 total `Create` responses; CHF's own counters after the run:
`chf_charging_data_create_total` = `chf_rating_grant_total` = 7784 -- the small gap from the
responses total above is warmup-period traffic, which reaches CHF and increments its counters but
is excluded from `sbi-loadgen`'s own reported percentiles by design).

**What this shows, read against the disclosed single-bucket-contention scope above, not beyond
it:** throughput essentially plateaus between c8 (98.5 req/s) and c32 (104.8 req/s) while p50
latency keeps climbing linearly with concurrency (75.7 ms -> 301.0 ms) -- the signature of a
system where added concurrency buys queueing delay, not more completed work, because every request
in this run serializes through the same `balance_mgmt.bucket` row's lock (`reserve()`'s conditional
`UPDATE`, ADR-0445's own finding). The open-loop case makes the ceiling unambiguous: at an offered
200 req/s against a ~100 req/s real ceiling, the queue cannot drain within the 32 s it took to
issue all 3000 scheduled requests, and coordinated-omission-corrected latency (measured from each
slot's *scheduled* time, not when it was finally sent) correctly reports p50 at 9.5 s rather than
hiding the backlog in a closed-loop number that would have looked merely "slow."

**What this baseline does NOT yet isolate**, honestly: this run measures the whole
`Nchf_ConvergedCharging_Create` path -- the N+1 catalog SBI calls (finding #1), the bucket-level
PostgreSQL serialization, and ADR-0009's synchronous HTTP client -- together, because that is what
a real request experiences today. It does not, by itself, say what fraction of the ~17.7 ms single-
request latency (c1, no contention) is the catalog N+1 calls specifically versus the balance
round-trip versus the synchronous client. The snapshot (step 3) removes the catalog N+1 calls only;
the honest comparison once it exists is this same script run again, same provisioning, same
machine state as close to idle as this one was -- not a claim that the whole gap above will close.

**Step 3, done (2026-10-02): the snapshot itself.** `nfs/chf/src/catalog_snapshot.{hpp,cpp}` -- a
dumb cache of the real `GET /productOffering`/`GET /productOfferingPrice` collections, preserving
collection order. `build_rating_grant` (`charging_engine.cpp`) refactored to extract its entire
matching+grant-building body (isSellable/Active/ratingGroup/chargingScope/unitOfMeasure, AI quota
sizing) into a shared `try_rate_against` helper, used identically by a new snapshot-backed path and
the original live-fetch path -- a null or not-yet-`ready()` snapshot falls back to the exact
pre-existing behaviour byte-for-byte, no extra fetches, no new crash/503 path (see the revised
item 6 above). Wired through all three real call sites sharing `charge_one_usage`
(`main.cpp`'s Nchf Create/Update, `diameter_server.cpp`'s Gy CCR-I/U, `cap_server.cpp`'s CAP
InitialDP/renewal), each passing the same CHF-wide `CatalogSnapshot` instance, kept warm by its own
dedicated background thread (own `sbi_core::http2::Client`, never sharing the route handlers'
thread-confined one -- same discipline `run_nrf_lifecycle` already established). Two Prometheus
gauges (`chf_catalog_snapshot_generation`, `chf_catalog_snapshot_age_seconds`) for observability.
Config: `catalog_snapshot_refresh_interval_seconds` (default 30s, `config/chf.json`).

**Verification.** Every touched file compiles clean under `-Wall -Wextra -Wpedantic -Wshadow
-Wconversion -Wsign-conversion` and clang-format-18. A real build of `chf` and `integration_tests`
succeeded (0 `FAILED:` lines). `CapScopedCharging.AnInitialDpRatesAgainstTheOfferingScopedToItsServiceKey`
-- the test that specifically proves ratingGroup+chargingScope matching with decoy-offering
ordering -- passed on 5 separate runs (real timing non-determinism between this test's request and
the background refresh thread's own first attempt means different runs could exercise either code
path; it was not pinned to one). Both `ChaosCharging.*` tests passed, including a real, informative
log line confirming the designed-for degradation: with no product-catalog process running at all,
`catalog_snapshot.refresh()` logged "could not reach product-catalog... keeping previous snapshot"
and `build_rating_grant` fell back to live-fetch, producing the identical pre-existing "could not
reach bss/product-catalog for rating, granting nothing" outcome -- not a crash, not a hang.
Three unrelated tests (`N28SyEndToEnd`, `PcfN28Integration`, `PcfChfN28Integration`) failed in this
local run on a UDR OAuth2 token issue -- confirmed unrelated: that feature area (Nchf_
SpendingLimitControl/N28) does not call `build_rating_grant`/`charge_one_usage` at all, and
`postgres-udr` was simply never brought up in this local session (a local environment gap, not
exercised or explained by anything this ADR changed).

**"After" result (2026-10-02), raw files in `docs/benchmark-chf-2026-10-02-after/`**, same script,
same provisioning, same machine, CI idle (waited for a fully clear run, not just a momentary port
gap -- the self-hosted runner's own tests bind the same loopback ports this script needs):

| Case | Responses | Throughput | p50 | p90 | p99 | p99.9 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| closed c1 | 3410 | **227.29 req/s** | **4.1 ms** | 4.8 ms | 9.1 ms | 22.2 ms | 38.4 ms |
| closed c8 | 7372 | **491.11 req/s** | **15.8 ms** | 17.6 ms | 28.7 ms | 59.7 ms | 75.6 ms |
| closed c32 | 7387 | **490.77 req/s** | **64.6 ms** | 77.4 ms | 108.1 ms | 201.7 ms | 243.1 ms |
| open 200 rps | 3000 (**199.98** delivered) | 199.98 req/s | **7.9 ms** | 13.0 ms | 25.8 ms | 41.4 ms | 48.2 ms |

All-201 again (21,169 total `Create` responses). CHF's own counters confirm the snapshot was
actually in use throughout: `chf_catalog_snapshot_generation` = 3 (three successful background
refreshes during the run, consistent with the 30s default interval), `chf_catalog_snapshot_age_
seconds` = 8.2 at scrape time -- not a cold/unused snapshot coincidentally doing nothing.

**Before -> after, same machine, same contention pattern:**

| Case | Throughput before | Throughput after | Ratio | p50 before | p50 after | Ratio |
|---|---:|---:|---:|---:|---:|---:|
| closed c1 | 51.72 | 227.29 | **4.4x** | 17.7 ms | 4.1 ms | **4.3x** |
| closed c8 | 98.49 | 491.11 | **5.0x** | 75.7 ms | 15.8 ms | **4.8x** |
| closed c32 | 104.80 | 490.77 | **4.7x** | 301.0 ms | 64.6 ms | **4.7x** |
| open 200 rps (delivered) | 93.19 | 199.98 | **2.1x** | 9500.4 ms | 7.9 ms | **~1203x** |

**What this shows, read precisely:** removing the N+1 catalog SBI calls alone (nothing else in
ADR-0445's plan has been touched -- balance is still a live SBI round-trip, settlement is still
two calls, the HTTP client is still synchronous) roughly quadrupled throughput and quartered
latency at every concurrency level. Critically, **c8 and c32 now plateau at the same ~490 req/s**
(before: c8 and c32 plateaued at ~100 req/s) -- the bottleneck has visibly moved from the catalog
N+1 calls to the next real constraint in line, the single-bucket PostgreSQL row-lock serialization
ADR-0445 itself identified (every request in this run contends on the same bucket, by this script's
own disclosed design). The open-loop case makes the qualitative change unmistakable: at a 200 rps
offered load, the system went from catastrophically unable to keep up (p50 9.5 **seconds**, queue
growing for the entire run) to fully sustaining the target rate with single-digit-millisecond p50 --
not a tuning win, a different regime. This is exactly, and only, what ADR-0445 predicted removing
finding #1 would do -- it does not and cannot speak to the balance-reservation contention,
settlement atomicity, or synchronous-client findings the same ADR named as separate, unaddressed
items.

Increment 1 (ADR-0445/0446) is complete: fixed-point money type, snapshot design decisions, real
before/after baseline.

