## ADR-0454: ADR-0445's baseline, re-measured after all four increments -- method first

**Date:** 2026-10-05. **Status:** method committed; results appended below once run. User-directed:
"Follow 1,2,3 and 4 order and complete ASAP" -- phase 1 of that order is this baseline.

**Why now.** ADR-0445's four increments are all in (ADR-0446 snapshot, ADR-0447/0448 correlator +
RAR, ADR-0449/0450 PgPool hardening + metrics, ADR-0451 UDSF Valkey Cluster). ADR-0445 asked for a
baseline covering Create/Update/Release, Gy CCR-I/U/T, reserve/adjust/topup, CPU/RSS and PgPool
wait/connections. The only numbers so far (ADR-0446) are Create-only.

**Method (fixed before any number exists).** `scripts/run-chf-rating-baseline.sh`, extended:
- Create: unchanged from ADR-0446 -- closed c1/c8/c32 + open 200 rps, 15 s after 2 s warm-up,
  single SUPI/single bucket, `build/` (Debug, the same build type ADR-0446's numbers used, so the
  two are comparable; NOT a Release-build figure).
- New, closed c1 only (proportional, not a second matrix): Nchf_ConvergedCharging **Update** on one
  pre-created ref (expects all-200); TMF654 **reserveBalance**, **adjustBalance**, **topupBalance**
  direct to balance-management (expect all-201).
- Resources: CHF's own RSS/CPU sampled from its PID once a second per case (min/mean/max);
  `pg_stat_activity` count on the charging DB before/after; the `chf_rating_db_pool_*` gauges.
- **Not measured, disclosed:** Gy CCR-I/U/T (Diameter -- no HTTP target for sbi-loadgen), Release,
  and multi-SUPI traffic (sbi-loadgen has no per-request body templating).
- Run only with the box idle: the self-hosted CI runner shares this host's 8 cores and loopback
  ports. A first attempt at 08:53 IST overlapped CI's TSan job; its one number (188.69 req/s c1)
  is **discarded**, not reported as a result. That attempt also exposed a bug in the new CPU/RSS
  sampler (it only wrote its file when its loop exited, but the script kills it from outside, so
  the file never existed and `set -e` aborted the run) -- fixed before the real run.
- Script hygiene: each run now DELETEs its own TMF620 offering/price on exit. Every earlier run
  left an Active ratingGroup=10 offering behind, and CHF rates against the first matching Active
  price in catalog order (see ADR-0455 for what that broke).

