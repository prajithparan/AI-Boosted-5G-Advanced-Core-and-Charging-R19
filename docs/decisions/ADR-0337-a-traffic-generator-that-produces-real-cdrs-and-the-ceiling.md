## ADR-0337: a traffic generator that produces real CDRs, and the ceiling it exposed

**Date:** 2026-09-11. **Status:** accepted.

ADR-0336 raised the training bar to 2000 examples over 200 subscribers and measured 12 usable
pairs. No model fixes that — nothing pretrained knows this network's subscribers or tariffs. Only
volume does. `tools/cdr-traffic-gen` produces it.

### It drives the real N40 path, and that is the whole point

Create, Update, Release over HTTP/2 + mTLS, through the real rating engine, the real balance
reservation and the real CDR writer. **It does not insert rows into Doris.**

Inserting would be far faster and worthless: those CDRs would carry whatever this tool chose, so a
model trained on them would learn this file's arithmetic rather than the charging system's
behaviour — while being tagged `data_source=real_cdr` because they came out of the real table.
That is ADR-0336's mislabelling arrived at from the other direction.

What is synthetic is the **offered load** — which subscriber, when, how much usage — which is
honest and unavoidable in a lab with no real subscribers. Usage is drawn lognormal because real
usage is heavily right-skewed; a constant would teach a model a constant. What is **real** is every
charging decision made about that load.

### Measured, and the answer is not what concurrency suggests

| Concurrency | CDRs/sec |
|---|---|
| 8 | **26** |
| 16 | 17 |
| 48 | 15 |

Throughput **falls** as clients are added. That is not a client limit, it is contention:
`CdrWriter::write` takes a single `std::mutex` around a **single MySQL connection** performing a
**single-row INSERT**, with BER encoding done inside the lock. Every CDR write in CHF is serialized
process-wide.

**This is a production finding, not a test-harness one.** A CHF that writes ~25 CDRs/sec is not
carrier-grade, and ADR-0049's mandate is explicit about exceeding free5GC. The fix — batched
inserts or Doris stream load — is a real increment with a real hazard attached: CDRs are revenue
evidence, so any buffer that has not been flushed is billing data lost on a crash. It is named here
rather than bolted on, because getting it wrong loses money rather than throughput.

### 1M CDRs over 10k subscribers: feasible, and here is the arithmetic

- **Storage**: ~1M rows at a few hundred bytes ≈ **200-300 MB** in Doris. Trivial against 584 GB free.
- **Subscribers**: 10k SUPIs already exercised; the generator spreads load across them.
- **Time**: at the measured ~25/s ceiling, **≈ 11 hours**. An overnight soak, not an afternoon.
  Batched writes would cut that by an order of magnitude.
- **Training**: 1M rows x 4 features is ~30 MB in memory; a RandomForest over it fits comfortably
  in this machine's RAM and trains in minutes. The GPU is irrelevant here — this is a tabular
  model, and the MX450's 2 GB would not be used either way.

This run took the lab from 63 CDRs to **15669**, across **7027** distinct subscribers, with
**24** usage-bearing. Still short of ADR-0336's bar, deliberately — the bar is there to be
met by a real soak, not by a demonstration.

### ADR-0337 addendum: two defects in the generator, and what `real_cdr` does and does not mean

**Defect 1 — every Release returned 400.** The `usedUnitContainer` omits `localSequenceNumber`,
which TS 32.291 makes mandatory. The first run produced **15,637 Create CDRs and 22 Releases** and
looked productive by row count.

**Defect 2 — the rows it did produce were untrainable.** Usage was reported only in the Release,
and a Release CDR is written with `rating_group` NULL and `used_total_volume` NULL. The training
query requires **both** non-null, so 16,000 CDRs yielded **zero** usable examples. Usage has to be
reported in **Updates** — which is also what a real SMF does. Fixed, and the generator now prints
`usage-bearing CDRs` separately from total CDRs, because the totals were the misleading number.

Both defects share a shape worth naming: the run reported success, the table filled up, and
nothing usable was produced. Only checking the rows against the *consumer's* query found it.

### The loop now closes, and here is exactly what that proves

Generated through CHF's real N40 path, then trained:

```
trainable rows: 6976 across 261 subscribers   (both ADR-0336 gates cleared)
training on 6376 REAL CDR-derived examples
test MAE: 10378353.51 octets (data_source=real_cdr, n_examples=6376)
```

**What this proves:** the pipeline works end to end — traffic → real charging decisions → real
CDRs → threshold cleared honestly → a model tagged `real_cdr`.

**What it does NOT prove, and the label cannot say:** that the model predicts real subscriber
behaviour. The *offered load* is synthetic — a lognormal this generator chose. So the model has
learned this generator's distribution, made real by the charging engine but not by any real user.
`data_source=real_cdr` distinguishes "produced by the real charging engine" from "fabricated rows",
which is a genuine and useful distinction; it does **not** mean "real user behaviour", and nobody
should read it that way.

The MAE improving from 585M octets (synthetic bootstrap) to 10.4M is likewise not evidence of a
better model — it largely reflects that this generator's distribution is narrower than the
bootstrap's. Comparing them as if they measured the same thing would be the error.

### Throughput, corrected

~5 sessions/sec at concurrency 8, each producing 3 usage-bearing Updates ≈ **10 trainable rows/sec**
(~20 CDRs/sec all-operations). So **1M CDRs ≈ 14 hours**, 1M *trainable* rows ≈ 28 hours. The
serialized `CdrWriter::write` is still the ceiling, and batching it remains the fix.

