## ADR-0163: CI -- real, disclosed mitigation for a recurring `sanitize (asan-ubsan)` runner

**Status:** Closed (work pushed; last citing commit 47ceb2c on origin/main, 2026-10-06).
shutdown pattern, root-caused past the earlier ADR-0124/ADR-0132 disclosure

### Context

User reported CI's `sanitize` job as "always" failing and asked for it to be fixed, having
previously flagged this in a way ("git commit has always errors") that first needed clarifying --
the real complaint was GitHub Actions' `sanitize` job, not local `git commit` itself (confirmed
clean: `git status`, `git log`, `git fsck --full`, and GitHub's own commit-verification API all
showed no issue on any recent commit).

Investigated the real CI run history (`gh run list`/`gh run view`/`gh api .../jobs/<id>/logs`)
across the last ~15 runs rather than re-asserting the prior ADR-0124 diagnosis. Real findings:

- `sanitize (asan-ubsan)` genuinely `failure`d 5 times recently (not merely `cancelled` by this
  project's own concurrency-group mitigation, ADR-0132 -- a real, distinct GitHub Actions
  conclusion). `sanitize (tsan)` and `build` essentially never show this same `failure` pattern in
  the same window (0 real failures found for either in the sample checked), strongly implicating
  something specific to ASan+UBSan instrumentation, not workflow-wide instability.
- An earlier same-session pass at this investigation mis-identified the stall point as always
  landing on the same three generated files (`ErrorIndication.c`/`PacketErrorRate.c`/
  `TypeOfError.c`) -- a real, disclosed grep artifact: the filter pattern included the substring
  `error` case-insensitively, which `TypeOfError.c.o` matches by name, not because that's where
  the build actually stalled. Corrected by reading full, untruncated logs around each failure's
  real last progress line.
- The real stall point across all 4 failures checked in full: always very late in the build
  (targets 1222-1228 of 1309, ~93%+ complete, deep into individual NF `.cpp.o` compilation --
  `nfs/pcf/.../stores.cpp.o`, `nfs/chf/.../cdr_asn1.cpp.o`, `nfs/chf/.../rating_decision_store.cpp.o`),
  never early or randomly distributed through the build. The gap between the last logged progress
  line and the runner's own "shutdown signal" ranged ~22s-135s across the 4 instances -- not a
  fixed duration, but consistently late-build.
- Read `.github/workflows/ci.yml`'s own `sanitize` job in full: no `timeout-minutes` set on any
  job (ruling out a workflow-level timeout as the direct cause under builds this short), and
  critically, **no compiler cache (ccache/sccache) and no explicit build-parallelism cap** -- every
  `sanitize` run does a full from-scratch `ninja` build of all 1309 targets at default (full-core)
  parallelism, identical in this respect to the `build` job.

### Real, disclosed hypothesis (not a proven root cause)

ASan+UBSan instrumentation is well-documented to use substantially more memory per compilation
unit than a plain Debug build or TSan (heavier code generation, redzone/shadow-memory bookkeeping
baked into each translation unit at compile time). Running full-parallelism `ninja` with several
such heavy compilations in flight simultaneously on GitHub's resource-constrained hosted
`ubuntu-latest` runners is a plausible, concrete explanation for genuine memory pressure --
consistent with: (a) only the `asan-ubsan` leg showing this failure class, not `tsan`/`build`;
(b) the consistent lateness (cumulative memory pressure from a long-running parallel build,
rather than an instant, early failure); (c) the "runner has received a shutdown signal" wording,
which matches GitHub's own host-level runner reclaim/health-check behavior rather than a
workflow-level cancellation (those show as job conclusion `cancelled`, already confirmed working
correctly and separately via ADR-0132's concurrency group). This is a real, evidence-based
hypothesis, not a certainty -- GitHub's own external scheduling/resource-allocation behavior is
not something this project can fully observe or control, same epistemic caveat ADR-0124's own
original disclosure already carried.

### Implementation

`.github/workflows/ci.yml`'s `sanitize` job's `Build` step now caps `ninja` parallelism to `-j2`
specifically for the `asan-ubsan` matrix leg (via a GitHub Actions expression conditional on
`matrix.sanitizer`), leaving `tsan` and the separate `build` job's own full build step untouched
-- both of those don't show this failure pattern, so narrowing the parallelism cap to only the
leg that does keeps the change minimally invasive rather than slowing down every job "just in
case." Real, disclosed limitation: this cannot be verified locally (this project's own dev
machine doesn't reproduce GitHub's exact hosted-runner resource constraints), so its
effectiveness can only be judged by watching real future CI runs for `asan-ubsan` -- if the
`failure` pattern recurs after this change, the memory-pressure hypothesis would be weakened and
a different real cause (or a lower `-j` value, or splitting the sanitize build into a smaller
target set) would need investigating next.

### What this ADR does NOT include

No code behavior change -- CI workflow configuration only. Does not touch the already-working
concurrency-group mitigation (ADR-0132) or the already-disclosed `AiQuotaSizer` latency-budget fix
(ADR-0150), both real, separate, already-resolved CI concerns. Does not add compiler caching
(ccache/sccache) or reduce the sanitize build's actual scope (e.g. building only sanitizer-tested
targets rather than the full project) -- both real, larger potential mitigations left for a future
turn if `-j2` alone doesn't resolve the pattern.

