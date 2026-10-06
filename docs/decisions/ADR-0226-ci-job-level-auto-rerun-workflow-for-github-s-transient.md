## ADR-0226: CI -- job-level auto-rerun workflow for GitHub's transient runner reclaim, since a
step-local retry cannot survive it

### Context

Pushed ADR-0225's own step-local retry and watched the very next real CI run (`33035217687`):
`sanitize (asan-ubsan)` failed again, at only 39/1408 targets (~3 minutes in) -- and reading the
log directly showed the retry loop's own `for attempt in 1 2 3; do ...; done` shell had started
(confirmed: its own echoed command appears in the log) but never reached a second iteration before
"The runner has received a shutdown signal" / exit 143 ended the step. This proves, not just
theorizes, that GitHub's reclaim kills the **entire step process** -- including any retry loop
running inside that same `run:` block -- not just the `cmake --build`/ninja child process. No
retry logic executing *inside* the step that's being killed can survive this; recovery has to
happen from a process that starts fresh *after* the failure is already recorded, i.e. a genuinely
separate job.

Reported this honestly to the user rather than silently trying another step-local variant, and
asked (`AskUserQuestion`) whether to build the only mechanism that could actually work: a second
workflow watching CI's own completion and calling the GitHub API to rerun the failed job. User
confirmed: build it, bounded to 2-3 auto-retries.

### Implementation

New `.github/workflows/ci-auto-retry.yml`: triggers on `workflow_run` completion of the `CI`
workflow; if the run's conclusion was `failure` and its own `run_attempt` is still below `3`, calls
`gh run rerun <run-id> --failed` (rerunning only the failed jobs, not the whole run) using the
default `GITHUB_TOKEN` with `actions: write` permission. Because this runs as a distinct job in a
distinct workflow, triggered only after the original run has already fully completed and recorded
its failure, it cannot be killed by the same event that killed the original job -- it starts clean
on its own runner.

**Bounding, and why it's not "hiding real bugs"**: capped at 3 total attempts
(`run_attempt < 3` gates the *next* rerun, so attempts 2 and 3 can be triggered but not a 4th). A
genuine code defect (a real compile error, a real test failure) reproduces identically on every
attempt regardless of which runner executes it, so it still ends up red after 3 attempts, exactly
as intended -- this mechanism only ever helps the specific case where a fresh attempt with an
unchanged tree would plausibly succeed (external, transient runner-infra reclaim), same principle
as ADR-0225's own step-local retry, just implemented at the layer that can actually survive this
specific failure mode.

### What this ADR does NOT include

Does not distinguish "transient infra failure" from "real defect" before deciding to retry -- it
retries on any `failure` conclusion, relying on the attempt-count bound (not failure
classification) to keep a real, persistent bug from being retried forever. Does not address
`workflow_run`'s own real, disclosed quirk: this file's *default-branch* version is what always
executes, so any future edit to this workflow itself only takes effect after it lands on `main`,
never on the version present in the commit that triggered the run it's reacting to. Real
verification is watching whether a future `sanitize (asan-ubsan)` failure actually gets
auto-rerun and clears within the 3-attempt bound.

