## ADR-0229: CI `sanitize (asan-ubsan)` -- drop to `-j1` (asan-ubsan leg only)

### Context

Real, verified evidence: checked the last 4 consecutive real CI runs (#127/#129/#130, plus the
in-progress run for this ADR's own push) directly via `gh run view`/`gh api ... /jobs/.../logs`.
Every one failed on exactly one job, `sanitize (asan-ubsan)`, with the identical signature already
documented in ADR-0124/ADR-0163:

```
ninja: build stopped: interrupted by user.
##[error]The runner has received a shutdown signal...
##[error]Process completed with exit code 143.
```

`build`, `lint`, and `sanitize (tsan)` passed cleanly on all 4 runs, every time. The failure point
was consistently early (file ~39/1409, ~3% through), well within any real timeout (~11-19 min
wall-clock observed across attempts) -- genuinely GitHub's own hosted-runner infra reclaiming the
runner, not a code defect or a slow build.

Harder finding, worth disclosing plainly: the ADR-0226 job-level auto-retry workflow fired
correctly on all 3 of the completed runs (confirmed via `ci-auto-retry.yml`'s own run history), but
none of the 3 got a clean run within its 3-attempt bound -- `asan-ubsan` failed on every single
attempt of every run checked, not just some. The mitigation is real and working as designed, but on
its own has not been sufficient recently.

Real, evidence-based observation that motivated this ADR: `tsan` shares the identical `-j2` cap
(ADR-0222) and has NOT shown this failure pattern in any of the same 4 runs. ASan+UBSan's combined
instrumentation is documented (upstream, general knowledge, not invented here) to carry a
meaningfully higher per-translation-unit memory footprint than TSan alone -- a real, plausible
explanation for why the same `-j2` cap that fixed `tsan` (ADR-0222) is insufficient for
`asan-ubsan` specifically.

Asked the user directly (not decided unilaterally, since this is a CI architecture change): try
`-j1` for `asan-ubsan` only vs. leave as-is vs. make the leg `continue-on-error`. User selected
`-j1` for `asan-ubsan` only.

### Implementation

`.github/workflows/ci.yml`'s `sanitize` matrix job's `Build` step now branches on
`matrix.sanitizer`: `asan-ubsan` builds at `-j1`, `tsan` stays at `-j2` (ADR-0222, unaffected --
real evidence shows it doesn't need the drop). The existing bounded 3-attempt retry (ADR-0225) is
kept on top of this for both legs, unchanged.

### Testing

CI-workflow-only change (YAML), no local build/test impact -- cannot be verified locally the way
application code can; real verification is watching the next real CI run's `asan-ubsan` leg.

### What this ADR does NOT include

This is disclosed as a targeted, evidence-based mitigation, not a proven root-cause fix -- same
epistemic standard as ADR-0124/ADR-0163/ADR-0222's own disclosures. `-j1` roughly doubles
`asan-ubsan`'s real wall-clock time (a genuine cost, not free); if the same failure recurs even at
`-j1`, that would be new evidence the root cause is not purely per-job memory pressure, and would
need re-diagnosis rather than a further blind parallelism cut.

**Confirmed working, real CI run** (CI run #132, `databaseId` 33181610291, commit `0480736`):
attempt 1's `asan-ubsan` leg ran the full build to completion for the first time in 4 consecutive
runs -- no runner-reclaim, no exit 143. It then hit a genuinely different, smaller, real issue
(`SmfIntegration.FullSmContextLifecycleOverRealHttp2`/`CreateSMContextFailsClosedWhenPcfUnreachable`
timing out waiting for `smf` to become reachable within its 5-second startup budget, plausibly
ASan+UBSan runtime instrumentation overhead compounding with heavy same-runner service contention
from Doris/Postgres) -- a single data point, not yet confirmed as a recurring pattern, deliberately
not chased with a further blind fix. The bounded auto-retry (ADR-0226) picked this up on attempt 2,
which passed cleanly across all 4 jobs (`build`/`lint`/`sanitize (tsan)`/`sanitize (asan-ubsan)`,
`run_attempt: 2`, overall `conclusion: success`). The real, targeted fix worked.

