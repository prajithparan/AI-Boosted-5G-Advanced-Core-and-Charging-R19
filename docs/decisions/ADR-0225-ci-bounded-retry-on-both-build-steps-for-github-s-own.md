## ADR-0225: CI -- bounded retry on both `Build` steps for GitHub's own transient runner reclaim

### Context

User reported (repeatedly) that a red commit status on GitHub is unacceptable, even after
ADR-0222/ADR-0224 fixed two real, previously-undiagnosed bugs and left `lint`/`build`/
`sanitize (tsan)` genuinely green on run `33013599336` -- only `sanitize (asan-ubsan)` remained
red, and a manual rerun of that one job failed again, both times from the same, already-documented,
pre-existing pattern (ADR-0124/ADR-0163): GitHub's own hosted-runner infra reclaiming the job mid-
build ("The runner has received a shutdown signal", exit 143), external to this repo, not a code
defect. Asked the user directly (`AskUserQuestion`) how to proceed given that reality; the user
initially chose "leave it as documented pre-existing flakiness," then immediately re-raised the
same "I don't want any errors on GitHub" concern -- taken here as the more recent, controlling
instruction: keep working the problem rather than accepting a red status as final, even though the
underlying cause is external.

### Implementation

Both `Build` steps (the plain `build` job, and the `sanitize` matrix job's shared step, which
already had ADR-0163/ADR-0222's `-j2` cap) now wrap `cmake --build build -- -j2` in a bounded
3-attempt bash retry loop, emitting a `::warning::` annotation on each failed attempt before
retrying. `ninja` is incremental, so a retry after a transient runner-reclaim resumes from whatever
objects already compiled in the interrupted attempt rather than rebuilding from scratch -- cheap
when it actually helps.

**Explicitly does not silence real failures**: nothing about the source changes between retry
attempts, so a genuine compile error (a real bug in this project's own code) fails identically on
every attempt and still fails the job once all 3 are exhausted -- the loop only ever absorbs the
specific transient case where a re-attempt with an unchanged tree would plausibly succeed (an
external runner-infra interruption), not correctness bugs.

### What this ADR does NOT include

Does not fix GitHub's own runner-reclaim behavior (outside this repo's control, same as ADR-0124/
ADR-0163's own disclosed limits) -- this raises the odds of a clean run without requiring a human
to notice and manually re-run the job, it does not guarantee one, and 3 consecutive external
reclaims in the same job would still surface as a real failure. Real verification is watching
whether future CI runs on `main` stop showing this specific exit-143 pattern as an overall run
failure -- flagged as follow-up, same disclosure standard as every other ADR in this investigation.

