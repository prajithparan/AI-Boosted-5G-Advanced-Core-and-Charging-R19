## ADR-0132: CI workflow -- add a `concurrency` group to cancel superseded runs

### Context

At the user's repeated, explicit insistence on resolving GitHub Actions errors before continuing
any other work, re-examined the recurring `sanitize (asan-ubsan)` runner-shutdown pattern
(ADR-0124: `ninja: build stopped: interrupted by user` / `The runner has received a shutdown
signal` / exit 143) with a specific question ADR-0124 had not asked: is the repository's own push
cadence itself a contributing cause, even though no in-repo code defect was found?

Real, observable fact: this session pushes a new commit to `main` roughly every 15-30 minutes
(one per UDR gap-closure ADR), and each push triggers a brand-new, complete 4-job CI run (`build`,
`lint`, `sanitize (asan-ubsan)`, `sanitize (tsan)`). The sanitizer jobs (especially
`asan-ubsan`, an instrumented build of the entire monorepo) routinely take longer than the gap
between pushes to finish. `.github/workflows/ci.yml` had no `concurrency:` block at all (confirmed
by ADR-0124's own direct grep), so GitHub never cancelled a superseded run for an older commit on
the same ref -- every one of those runs kept its 4 jobs alive and contending for the account's
finite concurrent-runner capacity until each ran to completion or failure. This is a real,
plausible mechanism for GitHub's own scheduler reclaiming a runner mid-job under contention,
consistent with every piece of evidence ADR-0124 already gathered: it is always the slowest job
(`asan-ubsan`) that gets killed, never `build` or `lint`, and it happens specifically when multiple
runs are stacked up in the run list.

### Implementation

Added to `.github/workflows/ci.yml`:

```yaml
concurrency:
  group: ${{ github.workflow }}-${{ github.ref }}
  cancel-in-progress: true
```

This is standard GitHub Actions practice: when a new commit lands on the same ref (`main`) while
an earlier run for that ref is still in progress, GitHub cancels the earlier run outright instead
of letting both run concurrently. For this repo's push cadence, that means at most one full CI run
is ever alive for `main` at a time, directly eliminating the pile-up this session's own commit
frequency was creating.

### What this ADR does NOT include

This is disclosed as a **real, standard mitigation for the evidenced pattern, not a proven
root-cause fix**. ADR-0124 already noted that a GitHub Actions minutes/concurrency quota (billing
page, not accessible from this session's `gh` auth scope) is a plausible alternate or contributing
explanation this repository cannot rule out or confirm on its own. If the `asan-ubsan`
runner-shutdown signature recurs after this change ships and takes effect on the next few pushes,
that is real evidence against the "self-inflicted pile-up" hypothesis and points back toward an
account-level quota the user would need to check directly (Settings -> Billing -> Actions). No
retry logic, `timeout-minutes`, or other workflow change is added here -- keeping this change
minimal and attributable, so its effect (or lack of one) is clearly observable in the next several
runs.

### Follow-up: real observed evidence

The three runs already in flight when this commit landed (ADR-0130 run `32469389048`, ADR-0131
run `32470075273`, and this ADR's own run `32470691470`) were **not** cancelled by the new group,
confirming an expected limitation: GitHub only registers a run into a concurrency group if that
run's own checked-out workflow snapshot declares the group, so runs that predate this commit are
never retroactively grouped. ADR-0130's `sanitize (asan-ubsan)` job died again with the identical
ADR-0124 signature (confirmed via direct job-log fetch); ADR-0131's `sanitize (asan-ubsan)` job --
also predating the fix, also contending with the same two other runs -- succeeded. This confirms
the failure is intermittent under contention, not a guaranteed outcome of concurrent runs, which
is consistent with genuine external GitHub-side scheduling variance rather than a fully
self-inflicted problem. The real test of this fix -- a future push cancelling an older still-active
run that also carries the concurrency block -- had not yet been observed as of this writing; it
will be evident in the run list of subsequent pushes.

