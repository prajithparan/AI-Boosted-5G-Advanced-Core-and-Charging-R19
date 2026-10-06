## ADR-0150: CHF -- raise AiQuotaSizer's hardcoded inference latency budget after a real,

**Status:** Closed (work pushed; last citing commit 984794a on origin/main, 2026-10-06).
observed CI failure

### Context

CI run 32508134662 (the push carrying ADR-0149) failed its `build` job's own ctest step:
`AiQuotaSizer.LoadsRealOnnxModelAndPredicts` asserted `result.has_value()` and got `false`. Root
cause, read directly from the raw job log: the real ONNX Runtime inference call took `40777us`
on that runner -- 8x over `AiQuotaSizer`'s own hardcoded `kDefaultLatencyBudget{5000}` (5ms) --
so the class's own designed fail-safe correctly discarded the late result (logged as a real
warning, "AI quota sizing inference exceeded latency budget... discarding result -- deterministic
grant proceeds unaffected") and returned `std::nullopt`, which is exactly what the test asserts
should NOT happen. This is unrelated to any UDR work this session -- zero CHF/ONNX files were
touched by ADR-0143 through ADR-0149 -- and this exact commit passed 325/325 locally, confirming
the failure is CI-runner-contention-specific timing, not a code regression.

The 5ms budget's own header comment (`ai_inference.cpp`) claimed "generous headroom... not tuned
to be tight," which the real observed evidence now contradicts -- a genuine, disclosed
correction, not a silent adjustment. Presented to the user with three real options (add to the
known-flaky ctest exclusion list, loosen the latency budget, or leave as-is and re-flag if it
recurs); user chose to loosen the budget rather than mask the symptom.

### Implementation

- `nfs/chf/src/ai_inference.hpp`: moved the default budget out of `ai_inference.cpp`'s anonymous
  namespace into a new public constant, `chf::kDefaultAiQuotaLatencyBudget{50000}` (50ms, up from
  5ms) -- a real, evidence-based value: real headroom above the one observed data point
  (40777us), while still a materially tight bound relative to this project's own SBI
  response-time budgets (hundreds of ms to seconds), not an arbitrary round number.
  `AiQuotaSizer`'s constructor gained a third, defaulted parameter
  (`std::chrono::microseconds latency_budget = kDefaultAiQuotaLatencyBudget`) -- every existing
  call site (`nfs/chf/src/main.cpp`, all six `tests/conformance/test_ai_inference.cpp`
  constructions) compiles unchanged.
- `nfs/chf/src/main.cpp`: new optional `CHF_AI_QUOTA_LATENCY_BUDGET_US` env var, parsed and passed
  through to the constructor -- same getenv-based, never-hardcode-config precedent already used
  for `CHF_AI_QUOTA_SIZING_ENABLED`/`CHF_QUOTA_MODEL_PATH` right above it. Falls back to the new
  default on missing or unparseable input.
- `deploy/docker/docker-compose.yml`: documented the new env var alongside its two existing
  siblings, defaulting to `50000` if unset.

### Testing and verification

`AiQuotaSizer.LoadsRealOnnxModelAndPredicts` run 5 times in direct succession locally (isolated
via `--gtest_filter`): passed all 5 times (85-110ms total per run, well within the model's own
real, tiny inference cost -- the failure mode was never about the model being slow, only about a
too-tight budget under CI contention). Full `conformance_tests`: 325/325 pass (excluding the two
disclosed, pre-existing, unrelated flaky tests), zero regressions. `chf` and `conformance_tests`
both built clean.

### What this ADR does NOT include

This is a genuine best-effort mitigation based on one observed data point (40777us), not a
statistically-derived percentile from repeated CI runs -- if this specific test still flakes
occasionally at 50ms under worse contention, that would be new evidence, not a sign this ADR's
reasoning was wrong, and should be reported rather than silently re-adjusted again. The real
latency-budget-enforcement mechanism itself (post-hoc discard, not preemptive cancellation) is
unchanged and still disclosed in `ai_inference.hpp`'s own header comment -- this ADR only changes
the threshold, not the enforcement design.

**Real, live CI verification (added after this ADR's own triggering push)**: the very next CI run
(32541157031) hit the same known, unrelated external runner-shutdown pattern on
`sanitize (asan-ubsan)`/`sanitize (tsan)` (exit 143, documented repeatedly this session), but its
`build` job's own ctest step passed `AiQuotaSizer.LoadsRealOnnxModelAndPredicts` in 0.16s --
`100% tests passed, 0 tests failed out of 325`. Confirms this fix holds in the real CI
environment, not only locally.

