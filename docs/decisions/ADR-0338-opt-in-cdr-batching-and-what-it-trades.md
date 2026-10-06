## ADR-0338: opt-in CDR batching, and what it trades

**Date:** 2026-09-11. **Status:** accepted.

ADR-0337 measured the ceiling: ~20 CDRs/sec, *falling* as clients were added. `CdrWriter::write`
took one mutex around one MySQL connection issuing a **single-row INSERT**, with BER encoding done
inside the lock. Doris is an OLAP store; row-at-a-time insert is pathological for it.

### The trade is stated, not buried

A buffered CDR is **not durable**. A CHF that is SIGKILLed loses every unflushed row, and those are
**billing records, not throughput**. So batching is **opt-in and defaults to off**: `batch_size = 1`
preserves the exact durability every deployment has today — one CDR, one INSERT, on disk when
`write()` returns.

Enabling it logs a warning naming what is at risk. A flush interval bounds the exposure in *time*
as the batch size bounds it in *rows*. The destructor flushes before closing the connection, so a
*clean* shutdown never drops a row — losing data on the one path where it is entirely avoidable
would be indefensible. A failed batch logs **how many rows went with it**: "a CDR write failed"
badly understates losing 500 billing records.

One shared column-list constant serves both the single-row and batched paths, so the two cannot
drift into writing different columns.

