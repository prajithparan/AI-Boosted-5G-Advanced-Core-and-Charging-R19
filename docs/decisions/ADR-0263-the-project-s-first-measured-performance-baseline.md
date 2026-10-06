## ADR-0263: the project's first measured performance baseline

> **Correction (2026-09-10, ADR-0329):** this baseline was measured on the default `build/` tree,
> which is `CMAKE_BUILD_TYPE=Debug`, and this ADR never said so. The numbers below are therefore
> Debug-build numbers and should not be compared against anything built with optimisation. Found
> while setting up the free5GC comparison, where the same omission would have made the comparison
> meaningless. `scripts/run-free5gc-comparison.sh` records the build type in `environment.txt` for
> every run so this cannot recur silently; `scripts/run-baseline-benchmark.sh` should be given the
> same line before it is run again.


### What this is, and emphatically is not

ADR-0049 recorded that **zero benchmarking of any kind** had ever been performed here. That is no
longer true. This ADR records the first real measurement.

It is a **baseline of this build on one machine**. It is **not** ADR-0238 step (4) -- no comparison
against free5GC or anything else was run, and none is implied. The repo still contains no claim of
superiority over anything, which remains correct.

### Method

`scripts/run-baseline-benchmark.sh` (new, committed so the run is reproducible) drives NRF's
`SearchNFInstances` -- the hottest SBI path in a real core, and the one whose TS 28.552 clause
5.10.3 measurement family ADR-0262 just instrumented -- through `tools/sbi-loadgen` over real
HTTP/2 + TLS 1.3 + mTLS, using the same `sbi_core::http2::Client` every NF uses for outbound SBI.

Environment, because it materially bounds the numbers:

| | |
|---|---|
| CPU | 11th Gen Intel Core i7-11370H @ 3.30GHz, **8 cores** |
| Memory | 15.4 GiB |
| Kernel | Linux 7.0.0-28-generic |
| Topology | **Load generator and system under test share this one host; all traffic over loopback** |
| Commit | `5dd2e28` |

### Results (15 s measurement window, 2 s warmup, all runs 100% HTTP 200)

| Run | Throughput | p50 | p90 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|
| closed-loop, concurrency 1 | 979 req/s | 0.823 ms | 1.288 ms | 6.292 ms | 8.392 ms | 8.940 ms |
| closed-loop, concurrency 8 | 7,113 req/s | 0.915 ms | 1.494 ms | 5.977 ms | 7.594 ms | 21.254 ms |
| closed-loop, concurrency 32 | 9,717 req/s | 3.144 ms | 4.031 ms | 5.987 ms | 11.125 ms | 34.718 ms |
| open-loop, 500 req/s target | 500.01 req/s achieved | 1.691 ms | 1.864 ms | 8.863 ms | 10.597 ms | 11.322 ms |

Closed-loop numbers are subject to coordinated omission and are latency-at-a-given-concurrency,
not latency-at-an-arrival-rate. The open-loop run is the coordinated-omission-corrected view:
latency measured from each slot's *intended* due time, with 1 slot issued late out of 7,500.

Transport errors: 0, 1, 0, 0 respectively. The single error at concurrency 8 ("Failed sending data
to the peer") is recorded rather than dropped; it is 1 in 106,703 and was not investigated.

### The counters corroborate the run independently

After the four runs, NRF's own ADR-0262 metrics read `nrf_nfs_disc_req_total = 311835` and
`nrf_nfs_disc_succ_total = 311835`, with `nrf_nfs_disc_fail_unauth_total` absent (never
incremented). Request count equals success count exactly, from the *server's* side, which is
independent evidence that the client-side "100% 200" figure is real. The 311,835 exceeds the sum of
the four measured windows (274,681) by the warmup traffic, which the load generator discards from
its statistics but NRF still counts -- as it should.

### A wrong result was produced first, and the fix is in the script

The first run of this benchmark reported concurrency-32 at 3,296 req/s and the open-loop run at
500 req/s -- and **every request in both was a 401**. The script fetched one OAuth2 token up front,
the run drifted past its 3600 s lifetime (this machine was still under load from a concurrent
build), and those two cases measured the *rejection* path at full speed. The numbers looked
entirely plausible.

The script now fetches a fresh token per case and **asserts that each completed run recorded only
200s**, failing the whole benchmark otherwise. A benchmark that silently measures error responses
is worse than no benchmark, and the guard is in the tool rather than in a reviewer's memory.

### Disclosed limits -- all of these bound what the numbers mean

- **ADR-0009's synchronous HTTP client is still open.** ADR-0238 deliberately sequenced the client
  fix *before* benchmarking, on the reasoning that a synchronous client produces throughput not
  reflective of the target architecture. This baseline is taken with that debt open, on purpose,
  and the numbers must be read as "this build as it stands", not "this design".
- **Loopback, one host, 8 cores.** The load generator competes with the SUT for the same cores.
  This characterises this machine, not the design's ceiling.
- **One endpoint.** `SearchNFInstances` is a read-mostly in-memory lookup. It is not representative
  of `CreateSMContext`, which fans out to PCF, UPF (PFCP) and CHF.
- **No soak, no HA, no failure injection.** Nothing here speaks to carrier-grade reliability.

### What remains open in ADR-0049/ADR-0238

Unchanged and stated plainly: the **async HTTP/2 client** (ADR-0009), **HA/clustering across NF
instances**, and **the free5GC comparison** (ADR-0238 step (4)). This ADR closes none of them. What
it closes is "zero benchmarking of any kind has been performed", which was true from ADR-0049 until
now.

