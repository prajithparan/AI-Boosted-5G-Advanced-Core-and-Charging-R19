## ADR-0231: real fix for `UdmIntegration.SdmDataRetrievalAndSubscriptions`'s CI exclusion (task #166)

### Context

`test_udm_uecm_sdm.cpp`'s `SdmDataRetrievalAndSubscriptions` test has been excluded from CI's own
`ctest -E` since ADR-0071/ADR-0072, bucketed together with the genuinely still-unexplained
`UdrIntegration.AmfContextLifecycle` hang (task #165, still open) under one "pre-existing
environmental flakiness" label. Direct evidence gathered this session while building and verifying
`test_udm_sdm_gap_closure_228.cpp`/`test_udm_sdm_gap_closure_230.cpp` (both real, live, 3-process
`nrf`+`udr`+`udm` tests) showed a real, specific, non-mysterious cause for a first, real local
failure: `udr`'s own startup (TLS 1.3 + mTLS setup, real PostgreSQL seed-data writes) genuinely
takes longer (~800-1200ms observed) than `udm`'s own port coming up -- so a test that waits only on
`udm` reachability before firing its first UDM->UDR-dependent request can race ahead of `udr` still
being mid-startup. `SdmDataRetrievalAndSubscriptions` has this exact same shape (spawns `udr` then
`udm` with only a fixed 200ms sleep between them, waits on `udm` reachability, then immediately
calls `GetAmData` which proxies to `udr`) -- a real, plausible match for the same root cause, not
the same "never root-caused" bucket as `UdrIntegration.AmfContextLifecycle`.

### Implementation

Added an explicit `wait_reachable` call on `udr`'s own `/nudr-dr/v2/subscription-data/nonexistent`
endpoint, before spawning `udm`, using the exact same helper/pattern already proven working in
ADR-0228/ADR-0230's own new tests. No other change to the test's own logic or assertions.

### Live verification (real, live mTLS + OAuth2, not self-consistency)

Ran the fixed `SdmDataRetrievalAndSubscriptions` test 5 consecutive times locally after the fix --
5/5 clean passes (previously excluded specifically for unreliability). No strays before or after
any run (`ps aux` checked explicitly).

### Testing

Full reconfigure + rebuild clean. `.github/workflows/ci.yml`'s both `ctest -E` invocations updated
to drop `UdmIntegration.SdmDataRetrievalAndSubscriptions` from the exclusion list (now only
`UdrIntegration.AmfContextLifecycle` remains excluded, task #165, still genuinely unexplained).
YAML syntax validated (`python3 -c "import yaml; yaml.safe_load(...)"`).

### What this ADR does NOT include

`UdrIntegration.AmfContextLifecycle`'s own real hang (task #165) remains genuinely unexplained and
still excluded -- this ADR did not investigate or fix it, and the two tests' surface similarity
(both real UDM<->UDR integration tests) does not imply the same root cause without its own direct
evidence, which has not yet been gathered. Task #166's other half, "cleanup-on-failure" (an
RAII-style scope guard so any `ASSERT_*` failure mid-test still reaps spawned NF processes), is not
addressed here -- a real, larger, more invasive change deferred to its own turn, overlapping with
task #170's own broader "reap orphaned processes on ctest timeout" initiative.

