## ADR-0240: Kernel-enforced reaping of test-spawned NF processes (task #170)

### Context

Every integration test in `tests/integration/` drives real NFs as separate OS processes:
`fork()` + `execl()` on the real `nrf`/`udr`/`udm`/... binaries, torn down with `kill(SIGTERM)` +
`waitpid()` at the end of the test. That teardown is correct for every path the test binary itself
controls, and it stays.

It cannot run on one path: when `ctest --timeout` expires, ctest kills the test binary with
SIGKILL. SIGKILL is uncatchable, so no `atexit` handler, no RAII destructor, and no signal handler
in the test process gets a chance to reap the NFs it forked. The observed consequence, hit
repeatedly in real sessions (four separate times in the session that produced this ADR alone, and
documented earlier as the "leaked-process pipe-hang" pattern): a timed-out run leaves `nrf`/`udr`
alive holding their listen ports, and every subsequent run of any test then fails with
"Address already in use" or hangs, until someone finds and `kill -9`s them by hand. The failure is
also misleading -- the next run's failure has nothing to do with the change under test.

No amount of user-space cleanup in the test binary can fix this, because the process that would do
the cleaning is the one being killed. The signal has to come from the kernel.

### Implementation

New header-only helper `tests/integration/spawn_guard.hpp`, `nf_test::arm_parent_death_signal()`,
called in the CHILD immediately after `fork()` and before `execl()` in all 43 spawning test files
(the shared header is the single place the logic lives; the per-file change is one include plus one
call, with no change to any test's own SIGTERM/waitpid teardown):

```cpp
inline void arm_parent_death_signal() {
    const pid_t parent_before = getppid();
    if (parent_before <= 1) {
        _exit(127); // already reparented -- the test binary is gone
    }
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() != parent_before) {
        _exit(127); // parent died inside the fork/prctl window
    }
}
```

`PR_SET_PDEATHSIG` asks the kernel to deliver a signal to the calling process when its PARENT
dies, for any reason including SIGKILL -- which is exactly the case user-space cleanup cannot
cover. Two real behaviours this depends on, both load-bearing and both stated rather than assumed:
it is CLEARED across `fork()` (hence set in the child, not inherited from the test binary), and it
SURVIVES `execve()` for ordinary binaries (it is reset only when exec'ing a setuid/setgid or
file-capability binary -- the NF binaries are ordinary). Only async-signal-safe calls
(`getppid`/`prctl`/`_exit`) are used, so the child branch stays legal in the child of a
multithreaded parent. SIGKILL rather than SIGTERM because on the dead-parent path there is no
longer anything coordinating a graceful shutdown.

### Live verification (a real orphan repro, not a self-consistency test)

The proving run does exactly what ctest does on timeout, and then looks for survivors:

1. Confirm no NF processes are running.
2. Launch `integration_tests --gtest_filter=NrfGapClosureIntegration.*` directly (not via ctest).
3. Wait until it has actually forked an NF child; record the child pid from `/proc`.
4. `kill -9` the TEST BINARY (uncatchable, exactly ctest's `--timeout` kill).
5. Check whether the child survived.

Result: spawned child pid was the real `build/nfs/nrf/nrf`; after the test binary was SIGKILLed it
was reaped by the kernel -- `RESULT: PASS -- every spawned NF was killed by the kernel when the
test binary died.` The "before" behaviour needed no fresh demonstration: it is the documented
failure this task exists to fix, observed four times in this session.

No regression on the normal (non-killed) path, where each test's own SIGTERM/waitpid teardown is
still what does the work: full regular-build `ctest` suite with the standard
`-E "UdrIntegration.AmfContextLifecycle"` exclusion, 461/461 passed (6 legitimately skipped,
the PostgreSQL-dependent BSS tests), 92.4s.

### What this ADR does NOT include -- disclosed, not silently scoped away

- **It ties an NF's lifetime to the TEST BINARY, not to the individual test case.** A GoogleTest
  `ASSERT_*` that returns early from the middle of a test body still skips that test's own
  SIGTERM/waitpid teardown, leaking the NF until the whole binary exits. That is task #166's
  RAII-cleanup-on-failure territory and remains open. `SpawnedProcess` in
  `test_udr_ondatachange_webhook.cpp` is the existing correct pattern for that half of the problem
  and is deliberately not generalised here.
- **A real, microsecond-wide residual race**, disclosed rather than engineered around: if the
  parent dies between `fork()` returning and the child's first `getppid()`, neither guard can
  observe it and the child survives. Its failure mode is exactly today's behaviour, so this is a
  strict improvement, not a total guarantee.
- **NFs started manually outside ctest are unaffected** -- they have no test-binary parent. The
  existing operational note (kill manually-started lab NFs before running ctest) still applies.
- **Linux-specific.** `PR_SET_PDEATHSIG` is a Linux `prctl`. This project already targets Linux
  (bare-metal Ubuntu 24.04, CI on ubuntu runners) and the tests already use `fork`/`execl`/
  `waitpid` directly, so this narrows nothing that was portable before.

