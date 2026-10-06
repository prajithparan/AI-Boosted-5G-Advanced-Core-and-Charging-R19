## ADR-0287: task #165 closed -- the CI exclusion was stale, proved rather than assumed

**Date:** 2026-09-05
**Status:** Accepted

`UdrIntegration.AmfContextLifecycle` had been excluded from every CI `ctest` invocation since
ADR-0071/0072 as "a hang, never root-caused", and ADR-0231 had already retracted the *other*
long-standing exclusion once its real cause (a UDR-startup timing race) was found. This closes the
remaining one.

### Why it was reasonable to suspect staleness

Two things had changed since the exclusion was written:

- **ADR-0231** found the sibling exclusion was a readiness race, not a hang.
- **ADR-0272** widened every `wait_reachable` budget in the suite from 5 s to 20 s -- the same
  class of fix, applied suite-wide because all 54 call sites shared the same 50-attempt budget.

### Why suspicion was not enough

The full local suite with nothing excluded was 498/498 green with this test passing in 1.26 s. That
proved little: **the hang was always CI-specific** -- loaded free runners, sanitizer builds -- which
is precisely the environment a local run cannot reproduce. So the exclusion was removed as a
deliberate experiment with a bounded downside: `ctest --timeout 120` would fail the job in a
bounded way and hand back a real data point instead of a five-month-old shrug.

### The result

Run 33952950176, all four jobs green, with the test executing in both sanitizer legs:

```
sanitize (asan-ubsan):  69/504 UdrIntegration.AmfContextLifecycle ... Passed  2.85 s
sanitize (tsan):        69/503 UdrIntegration.AmfContextLifecycle ... Passed  2.87 s
build:                  100% tests passed, 0 tests failed out of 504
```

`.github/workflows/ci.yml` now carries **no `-E` exclusion at all**.

### What this does and does not establish

It establishes that the test passes today, in CI, under both sanitizers, on the same runner class
that used to hang it. It does **not** establish the original root cause -- ADR-0272's budget
widening is the plausible mechanism, not a proven one, and no failing run was ever captured with
diagnostics. If it ever hangs again, that is new evidence rather than a return to the old shrug,
and the timeout bounds it.

---

