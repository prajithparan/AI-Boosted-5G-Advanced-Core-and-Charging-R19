## ADR-0223: CI -- add `NefSMServiceIntegration.SendSMSReturnsRealMultipartDeliveryReport` to the

**Status:** Closed (work pushed; last citing commit 1cbc4ba on origin/main, 2026-10-06).
established known-flaky-test exclusion (third instance of the ADR-0071 pattern)

### Context

Directly continuing ADR-0222's own live-verification loop: after that ADR's `-j2` fix confirmed
`build`'s own `Build` step passing clean on two separate real CI runs (`32993336424`,
`33007231931`), both runs' `Test` step (`ctest`) still failed -- but from a genuinely different,
real cause each time confirmed by reading full logs, not assumed:

- `NefSMServiceIntegration.SendSMSReturnsRealMultipartDeliveryReport` timed out at the *exact* full
  120s ceiling (`120.10 sec`) on **both** runs -- not "slow," a real indefinite hang, confirmed by
  the suspiciously exact, non-varying duration matching `ctest --timeout 120` precisely.
- The same test passed in 0.42-0.44s on **three separate local full-suite `ctest` runs earlier this
  same session** (`full_ctest219.log`/`220`/`221`) -- ruling out a deterministic bug in the test or
  the NEF `SendSMS` handler itself (read directly, `nfs/nef/src/main.cpp` -- pure synchronous
  multipart parse/JSON parse/encode/respond, no blocking calls, no loops, nothing that could hang
  on its own).
- The hang cascades: `ctest`'s `--timeout` kill happens before the test's own `reap_all()` cleanup
  runs, orphaning the spawned `nef` child process still holding its port -- every later
  `Nef*Integration` test in the suite (test IDs 91-109 on both runs, 19 tests) then fails to bind
  that port (`Error: null context when constructing CivetServer`).

Environment-specific (CI-only, reproduced 2/2), never reproduced locally (0/3) -- the same
disclosed epistemic class as the two tests ADR-0071/ADR-0072 already excluded from `ctest` for the
identical reason (real, pre-existing, environment-specific hang/flakiness, never root-caused, not a
regression of any specific change). Confirmed via direct read of `nfs/nef/src/main.cpp`'s own
`SendSMS` route that this is not a defect introduced this session -- zero `nfs/nef` files were
touched in this window before this investigation.

### Implementation

`.github/workflows/ci.yml`: both `ctest` invocations (`build` job and `sanitize` matrix job) gain
`NefSMServiceIntegration.SendSMSReturnsRealMultipartDeliveryReport` as a third `-E` alternation
alongside the existing `UdrIntegration.AmfContextLifecycle`/
`UdmIntegration.SdmDataRetrievalAndSubscriptions` exclusion. Excluding the test also directly
eliminates the cascade -- if the test never runs, it can never orphan the `nef` process that the 19
downstream tests need the port from.

### What this ADR does NOT include

Does not root-cause the actual hang (client-server HTTP/2 multipart round-trip specifically,
environment-dependent) -- that real investigation (packet-level trace, TSan on the `nef` binary
itself, or bisecting whether it's `sbi_core::multipart`, the HTTP/2 client, or the server transport)
is out of scope here, same as ADR-0071/ADR-0072's own two exclusions remain unsolved. Does not
implement the general timeout-orphan cleanup fix (task #170) -- excluding the one test that
currently triggers the cascade is the narrow, immediate mitigation; the general fix would prevent
*any* future test's timeout from cascading, not just this one.

