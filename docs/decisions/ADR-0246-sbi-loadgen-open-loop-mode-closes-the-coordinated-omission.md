## ADR-0246: `sbi-loadgen` open-loop mode -- closes the coordinated-omission gap ADR-0244 disclosed

### Context

ADR-0244 shipped the load-generation harness closed-loop and named its own limitation honestly:
offered load was `concurrency` outstanding requests rather than a fixed arrival rate, so the tool
was subject to **coordinated omission** -- when the server stalls, workers stall with it and stop
issuing requests, so the recorded latencies under-represent what a fixed-rate client would have
seen. That ADR called an open-loop mode "a genuine, named gap for a future turn". This ADR closes
it rather than leaving a disclosed weakness sitting in a benchmarking tool whose entire purpose is
producing numbers that can be trusted.

### Decision

`--rate <req/s>` selects open-loop mode. Request *i* is due at `start + i/rate` regardless of
whether earlier responses have returned, drawn from a shared atomic slot cursor so all workers
service one schedule. **Latency is measured from the slot's scheduled due time, not from the
moment a worker actually became free** -- that is precisely the coordinated-omission correction:
queueing delay caused by a saturated pool lands *inside* the reported latency instead of vanishing
because no request was issued. With `--rate`, `--concurrency` sizes the worker pool servicing the
schedule, not the offered load. The count of slots already overdue when picked up is reported, so
an unachievable target rate is visible rather than silently reinterpreted as a lower one.

### Live verification -- both the achievable and the overload case

Real `nrf`, real `GET /bootstrapping` over real mTLS, 2s warmup + 10s measurement.

**Achievable (target 500 req/s, pool 8)**: 499.62 req/s achieved, 5000 responses, 100% HTTP 200,
**1** slot late, p50 0.972 ms / p99 12.439 ms. The tool tracks a rate it can sustain.

**Overload (target 12000 req/s, pool 8 -- `nrf` sustains ~8400 at that concurrency)**:
7508.24 req/s achieved, **144000** slots late, **p50 3404.830 ms**, p99 5937.259 ms, max
5985.558 ms, still 100% HTTP 200.

That second row is the whole point, and is worth stating plainly: **closed-loop at the same pool
size against the same server reports p50 ~0.5 ms and 8387 req/s** (ADR-0244's own measured
numbers). Both descriptions are arithmetically true, and the difference between "sub-millisecond"
and "3.4 seconds" is entirely coordinated omission. A benchmark that only ever reported the first
number would be quietly flattering -- exactly the failure mode this project's own honesty rules
exist to prevent, and exactly the kind of number that would have been repeated in a comparison
against free5GC.

### A residual latency tail, recorded as an observation and explicitly NOT diagnosed

Open-loop measurement surfaced something the closed-loop runs had obscured: at a steady,
comfortably unsaturated 200 req/s (pool 4, 30s, 6000 responses, 5 slots late), latency spreads
from a 0.329 ms minimum and 1.100 ms p50 to a 10.527 ms p95 and 23.497 ms max. That is a ~10x
p50-to-p95 spread at a load the server handles trivially, so it is not queueing from saturation.

Two candidate causes were checked and **eliminated**: NRF does not log per request (3 log lines
across 6000 requests), and ADR-0239's `io_context_pool` genuinely runs `max(2,
hardware_concurrency)` = 8 threads here, so it is not a single-threaded event loop either.

Beyond that the cause is **unknown, and deliberately not guessed at**. Unlike the TCP_NODELAY
finding in ADR-0244 -- which had a control experiment (the concurrency-32 row) confirming the
diagnosis -- there is no evidence here that would distinguish TLS handle-pool behaviour, CPU
contention from the datastore containers co-resident on this dev box, or something in the server
path. Naming one would be a plausible story, not a finding. Recorded as a real, reproducible
observation for a future profiling turn.

### What is still NOT claimed

Unchanged from ADR-0244: no TS 28.552/28.554 mapping (those specs are not vendored -- ADR-0238
step (1) remains blocked on real spec material), and no comparison against free5GC or any other
implementation. This ADR improves the instrument's honesty; it makes no performance claim.

