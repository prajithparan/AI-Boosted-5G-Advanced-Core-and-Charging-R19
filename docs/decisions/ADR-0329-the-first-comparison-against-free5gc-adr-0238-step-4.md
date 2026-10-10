## ADR-0329: the first comparison against free5GC -- ADR-0238 step (4)

**Date:** 2026-09-10. **Status:** accepted.

ADR-0049 mandated exceeding free5GC and recorded that zero benchmarking had been done. ADR-0263
produced this project's first self-baseline and was explicit that it was *not* a comparison. This
is the comparison.

**Result: 1.82x free5GC's NFDiscovery throughput at concurrency 1, 2.92x at 8, 4.11x at 32.** Full
table, latencies and conditions in `docs/BENCHMARK_RESULTS.md`; method in
`docs/BENCHMARK_METHOD.md`, committed before the first run.

### The method was fixed before any number existed

Deliberately, and in a separate commit. A benchmark run under a mandate to win is exactly where a
thumb lands on the scale unnoticed, so the commitments -- publish whichever direction it lands, no
post-hoc tuning without disclosing it, list every asymmetry up front -- were written down while the
outcome was still unknown. Nothing in this project was changed after seeing free5GC's numbers.

### free5GC was configured to its strengths, not crippled

Its default `scheme: http` was switched to `https` so both systems pay TLS. Its `oauth: true`
default was kept so both validate a bearer per request. The alternative -- benchmarking our
TLS-terminating, token-validating NRF against a cleartext, unauthenticated one -- would have
produced a bigger number and meant nothing.

### Three checks, any of which would have invalidated the result

1. **Build type.** `build/` is `Debug`. Benchmarking a Debug build against free5GC's release image
   would have been meaningless -- and would have flattered free5GC, so the error was not in the
   direction the mandate pulls. A Release tree was built for this. The same check found that
   **ADR-0263's published baseline was measured on the Debug tree without disclosing it**;
   that ADR now carries a correction.
2. **Load-generator saturation.** If the generator saturated first, both systems would measure
   identical and read as "we match free5GC" -- pure artifact. Two generators at concurrency 16
   each aggregated 16349 req/s against one at 32's 16435: effectively identical, so the generator
   was not the limit.
3. **Connection reuse**, which is what makes the mTLS asymmetry amortise rather than dominate:
   concurrency 8 produced 9 connections carrying ~10258 requests each.

An intermediate run also recorded **12358 req/s of 401s** after a token expired. The all-200 guard
refused to report it. That is the single best argument for the guard existing: the number looked
excellent.

### What is NOT claimed

This is **one path on one machine**. `SearchNFInstances` only. AMF registration, PDU session
establishment and user-plane throughput are unmeasured -- they need UERANSIM and the `gtp5g`
kernel module, and no kernel module was installed on the developer's machine. This ADR does not
say this project is faster than free5GC in general, and no such sentence should be written from it.

**free5GC wins on tail latency at concurrency 1** -- p99 1.618 ms against ours 2.536 ms, p99.9
2.227 against 3.030 -- while serving half the throughput. Reported in the table with the same
weight as the rest, not omitted.

### The architectural asterisk that matters most

**free5GC's NRF performs one MongoDB query per discovery request**, measured: 2004 Mongo query
opcounters for 2003 requests. This project's NRF answers from an in-process map. That is a genuine
design difference and was deliberately not neutralised -- it is part of what free5GC is. But the
ratio should be read knowing that a durable, restart-surviving, shareable profile store is being
compared against an in-process map that is none of those things. This project's own P11
state-externalisation work moves toward free5GC's design and would cost some of this margin. The
honest reading is "faster on this path today, partly by not yet doing something free5GC already
does", not "better architecture".

### What this does and does not do for ADR-0049

It converts "zero benchmarking of any kind" into one measured, reproducible, adversarially-checked
comparison on the hottest SBI path. It does not make the system carrier-grade, does not cover the
user plane, and does not retire ADR-0009's synchronous-client debt (still open, and this number was
achieved with it open). The commercialization mandate's performance half now has evidence behind it
for one path, and explicitly nowhere else.

