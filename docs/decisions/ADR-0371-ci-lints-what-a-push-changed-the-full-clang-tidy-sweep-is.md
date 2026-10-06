## ADR-0371: CI lints what a push changed; the full clang-tidy sweep is weekly and on demand

**Date:** 2026-09-16. **Status:** accepted.

**Found.** The `lint` job's full clang-tidy sweep over every `.cpp` under `libs/` and `nfs/`
takes 4.5-5.5 hours on the self-hosted runner even at `-P 4` (the last two that finished:
2a67753 4 h 25 min, 64eea1a 5 h 33 min; the generated `TS26510_CommonData_grp.hpp` is parsed
195 times). With this repo's push cadence and `cancel-in-progress`, every lint since 2026-09-15
02:13Z was cancelled by the next push: the MFAF, DCCF, ADRF, NWDAF Phase C, MTLF and
MLModelMonitor increments (ADR-0365..0370) were never linted in CI. The job also occupies the
single runner for its whole duration, so the build and sanitizer jobs of the next push queue
behind it. `WarningsAsErrors` is empty in `.clang-tidy`, so a finished lint only ever failed on
compile errors -- but a lint that never finishes reports nothing at all.

**Decision.** On `push`, the lint step analyses the `.cpp` files the push changed (the diff
between `github.event.before` and the pushed commit, `--diff-filter=ACMR`, `fetch-depth: 0`);
when there is no usable base (a new branch, a force push, a pull request) it falls back to the
full sweep. The full sweep also runs weekly (`schedule`, Sunday 03:00 UTC) and on demand
(`workflow_dispatch`, `full_lint` true by default). The file set of the full sweep is unchanged.
The NWDAF sources of ADR-0369/0370 were clang-tidied locally before this change was pushed.

**Rejected.** Running lint on GitHub-hosted runners in parallel with the self-hosted build:
the 2-vCPU free-tier runner is where the 2.5-hour serial lint of ADR-0363's history came from.
`run-clang-tidy` with a header-filter or a precompiled header: changes which files are analysed
or how, and does not attack the cause (the per-file parse of the generated header). Turning
lint off: eight increments unlinted is the problem, not the symptom.

