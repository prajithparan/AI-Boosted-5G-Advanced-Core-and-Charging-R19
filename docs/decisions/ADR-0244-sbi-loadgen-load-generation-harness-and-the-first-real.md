## ADR-0244: `sbi-loadgen` load-generation harness, and the first real performance defect it found (TCP_NODELAY)

### Context

ADR-0049 made commercialization a mandate ("performance and reliability must **exceed** free5GC")
and stated honestly that **zero benchmarking of any kind** had ever been performed. ADR-0238 then
selected TS 28.552 + TS 28.554 as the measurement framework and listed four concrete steps in
order. Step (2), the synchronous-HTTP-client debt, is closed (ADR-0239 Phase 1 server concurrency,
ADR-0241 Phase 2 client handle pooling). This ADR delivers **step (3): a real load-generation
harness** -- Phase 8's long-planned "synthetic traffic generator", never started until now.

### Decision: `tools/sbi-loadgen`

A real, closed-loop load generator that drives this project's own SBI NFs. It deliberately links
`sbi_core` and issues load through `Client::send()` -- the exact production client path every NF
uses for outbound calls, including ADR-0241's handle pool, real TLS 1.3 + mTLS and real HTTP/2 --
rather than a benchmark-only transport that would measure a configuration no NF actually runs.
One shared `Client` across all worker threads, matching how an NF really holds one per peer.

Reports throughput, status-code histogram, transport errors, and min/mean/p50/p90/p95/p99/p99.9/max
latency, optionally as JSON for later comparison runs. Percentiles are nearest-rank on the sorted
samples, no interpolation -- an interpolated percentile reports a latency no request experienced.
Per-worker latency vectors are merged only at the end; a shared vector under a mutex would become
a contention point and distort the very number being measured.

**Stated limitations, up front rather than discovered in review:**
- **Closed-loop.** Offered load is `concurrency` outstanding requests, not a fixed arrival rate.
- **Therefore subject to coordinated omission**: when the server stalls, workers stall with it and
  stop issuing requests, so recorded latencies under-represent what a fixed-rate open-loop client
  would see. A real open-loop mode is a named gap for a future turn, not quietly approximated.
- **No TS 28.552/28.554 mapping.** That is ADR-0238's step (1), and those specs are **not vendored**
  in `specs/` (confirmed: only 5G_APIs YAML, NGAP, PFCP, and TS 32.298/33.501/33.503/29.002/29.078/
  29.219 PDFs). Inventing counter-family names to fill that gap is exactly what CLAUDE.md forbids,
  so step (1) is **blocked on real spec material** -- flagged, not faked.
- **No comparison against free5GC.** That is step (4) and needs step (1) first. No claim of
  superiority over anything is made here, by this ADR or by the numbers below.

### The real defect the very first benchmark run found: Nagle's algorithm on every NF socket

The first run showed an anomaly no one had reason to suspect: a **p99 of ~50 ms against a p50 of
0.3-0.7 ms** at low concurrency, which *vanished* at concurrency 32. That shape -- a ~40-50 ms
stall that disappears once there is always more data queued -- is the classic Nagle's-algorithm /
delayed-ACK interaction, so it was checked rather than assumed: `grep` confirmed **`TCP_NODELAY`
was set nowhere in `libs/sbi-core`**, and Boost.Asio does not set it by default. Every accepted
connection in every NF therefore ran with Nagle enabled, and SBI responses are exactly the small-
message pattern Nagle penalises.

**Fix**: set `boost::asio::ip::tcp::no_delay(true)` on each accepted socket in
`http2_server.cpp`'s accept path. A failure to set it is logged and non-fatal -- a connection with
Nagle still on is slow, not broken.

**Measured, same host, same `nrf`, same real `GET /bootstrapping` over real mTLS, 2s warmup +
10s measurement per point** (not a projection):

| concurrency | before | after | change | p99 before | p99 after |
|---|---|---|---|---|---|
| 1 | 152.29 req/s | 1272.64 req/s | **8.36x** | 52.430 ms | 10.011 ms |
| 4 | 1384.02 req/s | 5156.38 req/s | **3.73x** | 50.972 ms | 6.604 ms |
| 8 | 3308.81 req/s | 8386.83 req/s | **2.53x** | 47.031 ms | 7.650 ms |
| 32 | 15399.56 req/s | 15472.75 req/s | 1.00x (unchanged) | 6.329 ms | 6.495 ms |

Zero transport errors and 100% HTTP 200 at every point, before and after. The concurrency-32 row
is the control: it is exactly where Nagle should make no difference, and it doesn't -- which is
what makes this a confirmed diagnosis rather than a plausible story fitted to a speedup.

**Why this matters beyond one endpoint**: every NF is both an SBI server and a client of other
NFs, so this penalty compounded along every real call chain (AMF->SMF->PCF->CHF and so on), and
it hit hardest exactly in the low-concurrency regime that integration tests and interactive
verification actually exercise.

### What this ADR does NOT claim

It does not claim this project is now faster than free5GC, or fast in absolute terms. It is one
endpoint (`GET /bootstrapping`, a cheap read with no datastore access) on one machine, with no
cross-implementation comparison and no TS 28.552 KPI framing. It is a real baseline and a real
fixed defect -- nothing more, and the remaining steps are named above.

