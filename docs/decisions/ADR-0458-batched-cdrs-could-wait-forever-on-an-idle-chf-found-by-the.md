## ADR-0458: batched CDRs could wait forever on an idle CHF -- found by the pipeline's own probe

**Date:** 2026-10-05. **Status:** fixed.

**Found by ADR-0455's created == stored probe, which did exactly its job.** After a 10,000-session
warm-up with ADR-0338 batching on (batch 500, flush 1000 ms), Doris held 39,940 of the expected
40,000 rows, and the run refused to start. No CDR write had failed and the generator saw every
request succeed. The 60 rows (3 Creates, ~29 Updates, 28 Releases -- the tail of the run) were
still missing an hour later; one further session made all 60 appear (40,002 = 40,000 + 2 of the
new session's 4, whose own last rows were then stuck in turn).

**Root cause.** `CdrWriter::write` was the only place the flush interval was checked. With no next
write there was no flush: the last partial batch stayed in CHF's memory until traffic resumed or a
clean shutdown. ADR-0338 says the interval "bounds the exposure in time" -- on an idle CHF it
bounded nothing, and those rows are billing records that a SIGKILL would have lost.

**Fix.** When batching is on, `CdrWriter` runs one background thread that wakes every flush
interval and flushes an overdue, non-empty batch under the same mutex `write()` uses. The
destructor stops and joins it before its own final flush. Batching off (the default, batch 1) is
unchanged -- no thread.

**Proof, before/after.** `CdrBatching.AnIdleWriterStillFlushesWithinTheInterval` (real Doris,
batch 500, flush 300 ms, three rows, no further writes, counted through an independent
connection): **0 of 3 rows without the fix, 3 of 3 with it.** CdrRetention still passes.

**Still true, unchanged:** a batched CDR is not durable until flushed; a SIGKILL inside the interval
loses up to one interval's rows. That is ADR-0338's stated trade, now actually bounded.

