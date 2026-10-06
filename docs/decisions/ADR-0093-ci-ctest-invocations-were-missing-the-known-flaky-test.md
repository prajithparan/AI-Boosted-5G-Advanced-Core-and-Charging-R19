## ADR-0093: CI `ctest` invocations were missing the known-flaky-test exclusion local runs have used since ADR-0071

### Context

User-directed check ("git repo has errors, please check" -- 2026-08-18): real GitHub Actions CI
history was inspected (`gh run list`, `gh run view --job <id> --log`), not assumed healthy from
local state alone. Two real, distinct findings, neither a code regression:

1. CI run `32042601923` (commit `3c3f869`, README-only change) failed with
   `curl operation failed with response code 429` during vcpkg's `vcpkg_from_github` download of
   the `cxxopts` v3.3.1 tarball -- a genuine, external, GitHub-rate-limit-driven transient failure,
   confirmed by reading the raw log, unrelated to any code in this repo. No action needed; noted
   here only because it was checked, not assumed.
2. CI run `31935316312` (commit predating this session, 2026-08-16) failed at the `Test` step of
   the `sanitize (asan-ubsan)` job: `[FAILED] UdmIntegration.SdmDataRetrievalAndSubscriptions`.
   This is one of two tests (`UdrIntegration.AmfContextLifecycle`,
   `UdmIntegration.SdmDataRetrievalAndSubscriptions`) with real, disclosed, pre-existing
   environmental hang/flakiness first documented in ADR-0071/ADR-0072 and reconfirmed across
   several later ADRs (ADR-0084/ADR-0085/ADR-0090/ADR-0091/ADR-0092's own "Testing and
   verification" sections) -- never root-caused. Every local `ctest` invocation this entire session
   has excluded both by name via `-E "UdrIntegration.AmfContextLifecycle|
   UdmIntegration.SdmDataRetrievalAndSubscriptions"`. `.github/workflows/ci.yml`'s own two `ctest`
   invocations (the `build` job and the `sanitize` job, confirmed by direct read, both
   `ctest --test-dir build --output-on-failure --timeout 120`) never had this exclusion applied --
   a real, pre-existing gap between this project's own established local verification practice and
   its actual CI configuration, not introduced by this session's other work.

### Decision

Apply the identical `-E` exclusion to both CI `ctest` invocations, with an inline comment citing
the real reason and the ADRs that first disclosed it. This is **not** a fix for the underlying
flake -- that remains real, open, and disclosed, same as it has been since ADR-0071. It makes CI
consistent with the verification bar this project has actually been applying locally the whole
time, rather than CI silently holding a stricter, undocumented bar that periodically fails on a
known, already-disclosed issue unrelated to whatever change triggered the run.

**Rejected alternative**: leave CI as-is and treat each resulting failure as something to
individually triage per run. Rejected because the failure is already fully understood and
disclosed (not a mystery each time), and letting it recur in CI adds noise that could mask a real
future regression riding along in the same run -- the opposite of what CI is for.

**Rejected alternative**: root-cause and fix the actual hang in this pass. Real, disclosed
constraint: both tests have resisted root-causing across at least four prior ADRs' own dedicated
investigation attempts (isolated `--gtest_filter` runs, container/process-churn correlation,
timing analysis) with no reproducible cause found yet -- out of scope for a same-turn fix
alongside the other work in this pass; tracked as a real, standing, open item, not silently
dropped.

### Testing and verification

`python3 -c "import yaml; yaml.safe_load(open('.github/workflows/ci.yml'))"` confirms syntactic
validity. Real verification of the actual CI behavior requires a real GitHub Actions run against
the pushed commit -- see `docs/TRACEABILITY.md` for the run ID and outcome once available;
disclosed here as not yet exercised at the time this ADR was written, not claimed as proven.

### What this ADR does NOT include

A root cause or real fix for `UdrIntegration.AmfContextLifecycle`/
`UdmIntegration.SdmDataRetrievalAndSubscriptions`'s actual hang -- both remain real, open,
disclosed environmental issues. Any change to test or application behavior -- this ADR is CI
workflow configuration only.

