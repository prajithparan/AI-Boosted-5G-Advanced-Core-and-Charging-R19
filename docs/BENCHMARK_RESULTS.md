# NRF NFDiscovery: this project vs free5GC v3.4.4

ADR-0329. Method fixed in `docs/BENCHMARK_METHOD.md` **before** these numbers existed. Reproduce
with `scripts/run-free5gc-comparison.sh`.

## Result

Median of 3 runs, 15 s each after 2 s warmup. Every run verified all-200; any run recording a
non-200 is refused rather than reported.

| Case | System | Throughput (req/s) | p50 ms | p99 ms | p99.9 ms |
|---|---|---:|---:|---:|---:|
| closed-loop c1 | **this project** | **1932** | 0.310 | 2.536 | 3.030 |
| closed-loop c1 | free5GC | 1061 | 0.910 | **1.618** | **2.227** |
| closed-loop c8 | **this project** | **10363** | 0.565 | 3.987 | 5.404 |
| closed-loop c8 | free5GC | 3551 | 2.106 | 4.702 | 6.739 |
| closed-loop c32 | **this project** | **16401** | 1.748 | 4.921 | 7.758 |
| closed-loop c32 | free5GC | 3988 | 7.251 | 21.484 | 28.041 |
| open-loop 2000 rps | this project | 2000 | 0.445 | 2.917 | 4.265 |
| open-loop 2000 rps | free5GC | 2000 | 1.076 | 2.981 | 10.831 |

Throughput ratio: **1.82x at c1, 2.92x at c8, 4.11x at c32.**

**free5GC is better at one thing here and it is not buried:** at concurrency 1 its p99 (1.618 ms)
and p99.9 (2.227 ms) beat this project's (2.536 / 3.030 ms), despite lower throughput. Our tail at
low concurrency is worse than theirs. At c32 that reverses sharply -- their p99 is 21.5 ms against
our 4.9 ms.

Neither system saturated at the 2000 rps open-loop rate; both delivered it. The latency at that
fixed rate is the coordinated-omission-corrected comparison, and it favours this project at p50
(0.445 vs 1.076 ms) with a much closer p99 (2.917 vs 2.981 ms).

## What this measures

One path -- `SearchNFInstances` -- on one machine. It is **not** a general claim that this project
is faster than free5GC. AMF registration, PDU session establishment and user-plane throughput are
not measured, because they need UERANSIM and the `gtp5g` kernel module, and installing a kernel
module on the developer's machine was out of scope. Nothing here says anything about those paths.

## Why free5GC's number is what it is

**free5GC's NRF performs one MongoDB query per discovery request** -- measured, not assumed: 2004
Mongo query opcounters for 2003 discovery requests. This project's NRF holds NF profiles in
process. That is a real architectural difference, not a handicap imposed by the test setup, and it
was deliberately not neutralised: it is part of what free5GC is. A reader should weigh the ratio
knowing that a durable, restart-surviving, horizontally-shareable profile store is being compared
against an in-process map that is none of those things. This project's own state-externalisation
work (P11) would move it toward free5GC's design, and would cost some of this margin.

## Conditions

- 11th Gen Intel Core i7-11370H, 8 cores, 15.4 GiB, Linux 7.0.0-30.
- Both systems: HTTP/2 over TLS, OAuth2 bearer validated per request, identical query string, one
  UDM and one AMF registered, same `sbi-loadgen` binary, loopback, load generator on the same host.
- This project built **Release**. free5GC is its published v3.4.4 release image.
- Firefox was resident during the runs and is recorded in `environment.txt`. The machine was not
  otherwise quiesced.

## Checks that were run before believing any of this

- **Build type.** `CMAKE_BUILD_TYPE=Release`. The default `build/` tree is Debug, and ADR-0263's
  earlier baseline was measured there without disclosing it -- see that ADR's correction.
- **The load generator is not the bottleneck.** Two generator processes at concurrency 16 each
  aggregated 16349 req/s against one process at concurrency 32's 16435 req/s -- effectively
  identical, so the single generator was not the limit and these are the server's numbers.
- **Connections are reused,** so the mTLS asymmetry amortises: concurrency 8 produced 9 established
  connections carrying ~10258 requests each, not a handshake per request.
- **All-200 enforced.** One intermediate run recorded 12358 req/s of 401s after a token expired --
  a good-looking number measuring the rejection path. The guard refused it.
