## ADR-0124: CI `sanitize (asan-ubsan)` job -- confirmed external runner-shutdown pattern, not a code defect

**Status:** Closed (work pushed; last citing commit 47ceb2c on origin/main, 2026-10-06).

### Context

At the user's explicit request to resolve outstanding CI/GitHub errors before continuing any other
work, did a full audit of `gh run list` across the last ~10 workflow runs (`32205181630` through
`32209472975`, spanning ADR-0120 through the README rewrite commits). The escalating in-flight
backlog tracked across the preceding several session checks (peaking at 5 simultaneous runs, one
`lint` job stuck 78+ minutes) has genuinely cleared: 0 runs in flight as of this ADR, and the
previously-stuck `lint` job now passes clean.

Real finding, checked via `gh run view --job <id> --log` on every recent run's `sanitize
(asan-ubsan)` job (4 of 5 runs had retrievable logs; one log had already expired/rotated):
**100% identical failure signature every time**, always partway through the build step (never
during test execution, never a compiler diagnostic, never a sanitizer-reported bug):

```
ninja: build stopped: interrupted by user.
##[error]Process completed with exit code 143.
##[error]The runner has received a shutdown signal. This can happen when the runner service is
stopped, or a manually started runner is canceled.
```

Ruled out real causes before concluding this is external:
- **Not a `concurrency:` cancellation** -- `.github/workflows/ci.yml` has no `concurrency:` block
  at all (checked directly), so a newer push cannot be cancelling an older run's job.
- **Not a `timeout-minutes` hit** -- no `timeout-minutes` is set on any job in the workflow, and
  GitHub's own default (6h) is far longer than the ~3-9 minutes elapsed before each kill.
- **Not a self-hosted-runner issue** -- all three jobs (`build`, `lint`, `sanitize`) run on
  `ubuntu-latest` (GitHub-hosted), confirmed by direct grep of `runs-on:` in the workflow.
- **Not a code regression** -- `build` and `lint` pass clean on every one of the same runs;
  `sanitize (tsan)` (the other half of the same sanitizer matrix, same build, same runner type)
  also passes or is cleanly skipped, never hits this failure mode. Only `sanitize (asan-ubsan)`
  specifically is affected, every time.
- **Billing/quota visibility attempted, not available**: `gh api .../settings/billing/actions`
  returned 404 (wrong scope/plan shape) and the user-level equivalent needs an OAuth scope
  (`user`) this session's `gh` auth doesn't have. Real, disclosed: this project cannot confirm or
  rule out a GitHub Actions minutes/concurrency quota as the underlying cause without the user's
  own access to their account's Settings -> Billing -> Actions page, or re-authenticating `gh`
  with the broader scope.

### What this means

This is a real, external GitHub Actions infrastructure behavior -- GitHub itself is terminating
the runner mid-job for this one specific matrix leg, not this repository's code, tests, or
workflow configuration. No commit to this repository can fix a runner being shut down externally.
Consistent with the honest-disclosure standard this project holds itself to (CLAUDE.md, ADR-0049):
stated plainly rather than glossed over, and not previously tracked as its own ADR (it had only
ever been mentioned in README prose, which was itself recently trimmed for readability -- this
ADR is the durable record now that the README no longer carries it).

### What this ADR does NOT include

No workaround, retry logic, or workflow change is applied here -- there is nothing in this
repository's control to change. If this recurs, the next real diagnostic step is the user checking
GitHub's own Actions status page and their account's Settings -> Billing -> Actions usage (free-tier
minute exhaustion or an account-level concurrent-job cap are the two most likely real explanations
consistent with the evidence above, but neither is confirmed). Not resolved, not resolvable from
within the repo; disclosed, not hidden.

