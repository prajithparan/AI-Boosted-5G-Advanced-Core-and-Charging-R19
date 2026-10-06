## ADR-0340: 1M CDRs delivered, and the model still learned nothing until the data had structure

**Date:** 2026-09-11. **Status:** accepted.

The target was met: **1,009,128 CDRs across 10,012 unique subscribers**, generated through CHF's
real N40 path. Training consumed **565,016 real CDR-derived examples**. And the resulting model was
worthless.

| | MAE (octets) | mean of target | MAE/mean | examples |
|---|---:|---:|---:|---:|
| **v1** — one global lognormal | 10,907,994 | 10,064,386 | **1.08** | 565,016 |
| **v2** — per-subscriber profiles | 1,809,906 | 4,536,222 | **0.40** | **25,368** |

An MAE equal to the mean of the target means the model is no better than predicting the average for
everybody. **v2 uses 22x fewer examples and predicts far better.**

### Why, and it is structural rather than statistical

The generator drew every session's usage from one global lognormal, independently. Independent
draws are **memoryless**: a subscriber's past usage carries no information about their next
session. But `avg_used_last3`, `velocity` and `prior_granted_total_volume` — **three of the model's
four features** — exist precisely to exploit that link. Against memoryless data they are noise, and
the model correctly learns to ignore them and predict the mean.

Real subscribers are autocorrelated: heavy users stay heavy. Generating without that property is
generating **noise at scale**, and no amount of volume repairs an absent signal. Each SUPI now has
a stable profile derived from its index (so any worker computes the same profile without shared
state) and sessions vary around it.

**The lesson worth keeping: more data cannot fix data with no structure in it.** The 1M-row dataset
was not too small, not mislabelled and not malformed. It was correct in every respect that is easy
to check, and contained nothing to learn.

### What was required to make the records complete at all

Three environment defects, each of which silently produced valid-looking but useless rows:

1. **Every Release returned 400** — `usedUnitContainer` omitted `localSequenceNumber`, mandatory per
   TS 32.291. 15,637 Creates, 22 Releases, and the row count looked healthy.
2. **Usage reported only in Release** — a Release CDR is written with `rating_group` NULL *and*
   `used_total_volume` NULL, and the training query requires both. 16,000 CDRs, **zero** usable
   examples. Usage belongs in Updates, which is also what a real SMF does.
3. **No grants recorded** — `granted_total_volume` is only written when the balance reservation
   succeeds. That needed catalog offerings whose `ProductOfferingPrice` exists as a **standalone
   resource** (an inline price is not retrievable at `/productOfferingPrice/{id}`, which is where
   CHF fetches it) and 10k balance buckets marked **`is_shared`** — the `?relatedParty.id=` lookup
   resolves only shared buckets. Until both, one of four features was a dead constant.

All three shared a shape: the run reported success and the table filled up.

### Honest limits of this dataset

`data_source=real_cdr` means **produced by the real charging engine**, not **real user behaviour**.
The offered load is still synthetic. v2's structure is a better *model* of real usage — persistent
per-subscriber intensity — but it is still a model, chosen by this generator. A real distribution
comes from real subscribers and nothing here substitutes for that.

### Throughput

~250 CDRs/sec initially, decaying to ~100 as Doris compaction grew with the table on a saturated
8-core host. Sustained, 1M CDRs took **~2.5 hours**, not the 71 minutes a 90-second burst against a
small table suggested. Burst rates do not extrapolate.

**Self-inflicted:** 83,535 failures came from killing a CHF instance mid-run. The generator pins one
worker per endpoint with no retry or failover, so one instance restart fails a proportional slice of
the load. That is a real robustness gap in the tool, recorded rather than excused.

