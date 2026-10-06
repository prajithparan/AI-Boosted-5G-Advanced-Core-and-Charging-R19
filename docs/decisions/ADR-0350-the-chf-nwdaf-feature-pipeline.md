## ADR-0350: the CHF -> NWDAF feature pipeline

**Date:** 2026-09-12. **Status:** accepted. User-directed: this must exist before NWDAF starts.

`CLAUDE.md`'s data plane is *NFs emit events -> Kafka -> feature store -> Python training -> ONNX ->
in-process C++ inference in AnLF*. **None of it existed.** The only way to reach charging data was a
C++ query API or raw SQL against the CDR table.

`chf_features.subscriber_features` is the feature store: one materialised row per subscriber per
day, partitioned and bucketed like the CDR table so NWDAF's per-subscriber reads are single-tablet
prefix scans.

### Why a materialised feature row rather than "query the CDRs"

A model, a revenue query and a bill run each computing "this subscriber's usage last week" from raw
CDRs will each write their own SQL. The first time one forgets `operation` filtering or
double-counts an Update, the numbers disagree and **nothing says which is right**. Computed once,
by one query, every consumer reads the same number.

Two rules that query enforces, both of which are silent when wrong:

1. **Usage comes from rows that carry it.** A Release CDR is written with `rating_group` and
   `used_total_volume` both NULL; summing all operations counts usage-less rows and dilutes every
   average.
2. **Sessions are `COUNT(DISTINCT charging_data_ref)`, not row count.** One session emits a Create,
   several Updates and a Release. Measured on live data: **43,331 distinct sessions against 215,851
   rows — a naive count inflates by 5x.** The feature table records 43,191, matching the distinct
   count (the small delta is the live soak adding rows between the two queries).

Features carry **dispersion, not just means** (`stddev_session_octets`): two subscribers with
identical averages and very different variability are different customers, and anomaly detection
cannot separate them from a mean alone. Product mix is kept as **counts per service**, not a
dominant label, because collapsing the mix destroys what distinguishes a heavy video user from a
heavy messaging one.

### What this is NOT, stated plainly

**It is not the event bus.** The mandated stack names Kafka or Redpanda, and this is the batch half.
Streaming matters for low-latency analytics — an anomaly learned about a day later is a report, not
a signal. The broker is deferred because a JVM does not fit alongside Doris on this hardware while
a 2M-CDR soak is running, not because batch is sufficient. That remains open work before NWDAF's
real-time analytics can be honest.

Verified against live soak data: 28,757 subscriber rows, 3,706 SMS and 12,198 roaming CDRs
attributed correctly.

