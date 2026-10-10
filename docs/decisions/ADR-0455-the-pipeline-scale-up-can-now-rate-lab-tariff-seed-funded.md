## ADR-0455: the pipeline scale-up can now rate -- LAB tariff seed, funded buckets, 5M CDR rows

**Date:** 2026-10-05. **Status:** accepted; run results appended below. Phase 4 of the user's
ordered list (the 2026-09-19 directive: clean the 3M corpus, re-run at 5M CDRs / 100K customers,
Consumer + Enterprise).

**What was found before running anything.** The existing corpus (3,334,649 Doris rows, 833,672
sessions, 75,000 SUPIs) carries **zero granted units and zero cost** -- 1 stray row aside, every
Create/Update/Release is "granting nothing". Two causes, both verified:
1. The catalog has no ProductOfferingPrice for the generator's rating groups. The generator emits
   RG 1-11 (`tools/cdr-traffic-gen/src/profiles.cpp`); the catalog held only test leftovers: four
   Active offerings on RG 10 (three "CHF Rating Baseline" runs, plus "API Onboarding Plan" whose
   price has **no unitOfMeasure**), RG 20, and CAP-test RG 4242. Because CHF stops at the first
   matching Active price and a price with no unitOfMeasure means "grant nothing" (ADR-0446's
   preserved short-circuit), every RG 10 request was captured by the onboarding plan.
2. Only 9 balance buckets exist. The old run's buckets lived in the `postgres-balance` container
   ADR-0384 consolidated away (it is now an exited orphan); the generator never funded any.
Also: 85 sessions in the corpus have a Create with no Release (in flight when the old run was
killed) -- a real created-vs-closed gap, removed by the clean.

**Decisions (user-chosen via AskUserQuestion, 2026-10-05).**
1. **Seed a LAB tariff** rather than run as-is. `scripts/pipeline-seed.sh`: one TMF620 price +
   offering per RG 1-11, every name prefixed "LAB pipeline tariff ... (ADR-0455)" so it can never
   pass for a commercial rate card. Rates are round lab values (data 0.01 EUR/MB, voice 0.02
   EUR/min, SMS 0.05, MMS 0.20, enterprise slice data 5 EUR/GB, content 0.0025-0.004 EUR/MB,
   fair-use tier 0.005 EUR/MB). Disclosed simplification: the RG 4 price carries no chargingScope,
   so it rates slice-scoped traffic without checking the slice.
2. **Fund buckets through the real API.** New `cdr-traffic-gen --fund-buckets` derives every bucket
   key from `profile_for` itself (no second implementation to drift): a shared line's family/
   cost-centre key, else the SUPI (CHF's own fallback in `resolve_bucket_id`). Each gets one real
   TMF654 topupBalance (100,000 EUR headroom -- lab, so a 1.25M-session run cannot exhaust a
   household and start measuring the rejection path). Membership rows go into
   `balance_mgmt.bucket_related_party` + `is_shared` by SQL: TMF654 as built here has no membership
   operation, and `test_balance_shared_bucket.cpp` seeds it the same way.
3. **"5M CDRs" = 5M Doris rows**, the same unit as the old "3M" corpus: 1,250,000 sessions x
   (Create + 2 Updates + Release). `pipeline-run.sh`'s header said "one session = one CDR"; it is
   corrected, and its default is now 1,250,000 sessions.
4. **Catalog collision fix.** The three benchmark leftovers are deleted through the real TMF620
   DELETE; "API Onboarding Plan" (origin not found in any script/test) is flipped to
   `lifecycle_status = 'Retired'` by SQL instead -- reversible, record kept. product-catalog
   implements TMF620 POST/GET/DELETE but **not PATCH** -- a real coverage gap, recorded here, not
   fixed in this ADR.

**Pipeline-run hardening.** It no longer `docker compose up`s the lab NF images when CHF is down
(that path once silently dropped every CDR on stale images); it refuses and says to start the
current host binaries. It now enforces the user's 2026-09-20 consistency rule itself: after the
warm-up it requires Doris to hold exactly `warmup x (updates + 2)` new rows, else it refuses the
full run. Default generator is `build-release/`; LOGDIR no longer points at a dead session's
scratchpad.

**Results (2026-10-05, commit fe96db7 + the uncommitted script/test changes of this ADR; raw files
of run B in `docs/benchmark-chf-2026-10-05/`).** Box idle: CI run cancelled, no runner processes,
Release build stopped; 1-min load average ~2 at start, decaying from that build. Every case all-2xx,
zero transport errors.

| Case (run B) | Responses | Throughput | p50 | p90 | p99 | p99.9 | max | CHF RSS max | CHF CPU (1 core) mean |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Create closed c1 | 2418 | 161.18 req/s | 4.9 ms | 7.9 ms | 14.1 ms | 203.3 ms | 213.4 ms | 46 MB | 25% |
| Create closed c8 | 7340 | 488.94 req/s | 16.0 ms | 17.7 ms | 26.1 ms | 39.8 ms | 41.5 ms | 54 MB | 80% |
| Create closed c32 | 7219 | 479.75 req/s | 66.2 ms | 79.1 ms | 96.5 ms | 216.4 ms | 246.5 ms | 64 MB | 77% |
| Create open 200 rps | 3000 | 200.00 req/s | 4.7 ms | 5.6 ms | 14.0 ms | 22.0 ms | 24.2 ms | 69 MB | 33% |
| Update closed c1 | 2315 (all 200) | 154.32 req/s | 6.2 ms | 7.1 ms | 10.6 ms | 16.6 ms | 21.8 ms | 70 MB | 27% |
| reserveBalance c1 | 11592 | 772.78 req/s | 1.25 ms | 1.43 ms | 1.75 ms | 7.6 ms | 11.6 ms | -- | -- |
| adjustBalance c1 | 11813 | 787.48 req/s | 1.24 ms | 1.39 ms | 1.70 ms | 7.4 ms | 12.5 ms | -- | -- |
| topupBalance c1 | 11236 | 748.72 req/s | 1.27 ms | 1.47 ms | 2.18 ms | 7.9 ms | 15.6 ms | -- | -- |

PostgreSQL connections on the charging DB: 78 before, 78 after (the bounded pools hold their
connections; nothing leaked). `chf_rating_db_pool_in_use` 0 and `chf_rating_db_pool_exhaustion_total`
0 after the run -- the ADR-0449 deadline never fired. The balance rows show CHF at ~0% CPU because
CHF is not on that path (direct TMF654 calls); balance-management's own CPU/RSS was not sampled.

**Run A (same method, minutes earlier)** gave Create c1 212.63 / c8 469.25 / c32 476.95 req/s,
open-loop 200.00 (p50 4.9 ms), Update 140.09, reserve 783.83, adjust 783.57, topup 763.29 -- its
latency/throughput numbers are valid, but its CPU column is NOT: that sampler used `ps %cpu`, which
is the process's lifetime average, so each case's figure smeared all earlier cases in. Fixed to
per-interval `/proc/<pid>/stat` tick deltas and re-run as run B; run A's CPU figures are discarded.

**What this shows, read narrowly.**
- c8/c32/open-loop match ADR-0446's "after" numbers (491/491 req/s, open-loop sustained) within
  noise. Expected: increments 2-4 (correlator header, PgPool deadline/metrics, UDSF cluster) do not
  change Create's hot path, and the numbers confirm they did not regress it either. No new gain is
  claimed.
- Create c1 varies run to run (161-213 req/s against ADR-0446's 227). Run B's p99.9 shows a single
  ~200 ms stall; one run pair is not enough to call the c1 difference real -- not claimed as a
  regression or attributed.
- The single-bucket ceiling (~480-490 req/s) is unchanged, as ADR-0445 predicted: it is the
  bucket-row serialization, which none of these increments touched.
- Update costs more than Create at c1 (6.2 vs 4.7-4.9 ms p50): it does the same rating plus
  CdrWriter gap detection on every Update.
- balance-management alone answers in ~1.25 ms p50 at ~770-790 req/s single-threaded-client; CHF's
  Create at c1 is ~4x that latency, so most of a Create's time is CHF-side (rating, two SBI hops,
  Redis, CDR write), not the balance store.
- All Debug-build figures (same as ADR-0446). Not a Release figure, not a free5GC comparison,
  single host over loopback, single SUPI/bucket. Gy/Release/multi-SUPI not measured (above).

**Full pipeline run (2026-10-05 12:38 -> 22:16 IST, host Release binaries, 4 CHFs).** 1,240,000
sessions over 100,000 subscribers requested; 1,240,000 created and released, `failed=0`, 0 release
replays; 2,480,000 usage-bearing CDRs; 991,341 Consumer / 248,659 Enterprise sessions; 34,720 s at
35.7 sessions/s. `chf_cdr.cdr` = exactly 5,000,000 rows (the 5M target). All four CHFs: 0 error/
critical log lines. `chf_features.subscriber_features` (2026-10-05) = 99,999 rows.
**The 99,999 is not an extract bug.** The SUPI index 99395 (`imsi-999700000099395`) has no row in
`chf_cdr.cdr` at all and never appears in any CHF log: the generator draws subscribers from
`uniform_int_distribution(0, subscribers-1)`, so with ~12.4 sessions per subscriber on average the
chance that some subscriber is never drawn is about 1 - exp(-100000 * e^-12.4) ~ 33%. It was simply
never drawn. Disclosed consequence: the corpus has 99,999 subscribers, not 100,000, and one
funded bucket was never used. Not claimed as a CHF correctness result beyond "no failures".
Consistency (the 2026-09-20 rule): `COUNT(DISTINCT charging_data_ref)` = 1,250,000 = 1,240,000
released + 10,000 warm-up sessions (`WARMUP_SESSIONS`), x 4 rows (Create + 2 Updates + Release) =
5,000,000 rows. Reconciles exactly. Not measured: the feature table was not checked against the raw
CDRs beyond row counts and distinct-subscriber counts.

