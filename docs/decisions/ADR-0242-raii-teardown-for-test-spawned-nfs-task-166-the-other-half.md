## ADR-0242: RAII teardown for test-spawned NFs (task #166, the other half of ADR-0240)

### Context

ADR-0240 made the kernel reap a test's NF processes when the test BINARY dies (`PR_SET_PDEATHSIG`),
which is the only thing that works against `ctest --timeout`'s uncatchable SIGKILL. It explicitly
disclosed what it did NOT cover: a test that returns early while the binary keeps running.

That gap is not hypothetical. A GoogleTest `ASSERT_*` is a `return` from the middle of the test
body. `tests/integration/test_udm_uecm_sdm.cpp` had **44 `ASSERT_*` macros across 4 tests**, each
test spawning 2-3 real NFs (`nrf`, `udr`, `udm`) and tearing them down with `kill`/`waitpid` at the
END of the body. Any one of those 44 assertions firing skipped the teardown entirely, orphaning
every NF that test had started and leaving them holding their listen ports for the remainder of the
run -- which then makes unrelated later tests fail with "Address already in use", pointing at the
wrong change.

### Implementation

`nf_test::SpawnedProcess` added to the shared `tests/integration/spawn_guard.hpp` (already the home
of ADR-0240's child-side guard, so both halves of the orphan problem now live in one reviewable
place). It forks + `arm_parent_death_signal()` + `execl`s in the constructor, and `kill(SIGTERM)` +
`waitpid()` in the destructor -- so teardown runs on every exit path out of a test body, including
early `ASSERT_*` returns and stack unwinding.

`test_udm_uecm_sdm.cpp` converted: its local `spawn()` helper and all 9 raw `pid_t` locals replaced
by `SpawnedProcess` locals, and all 4 blocks of trailing manual `kill`/`waitpid` deleted.
Destruction runs in reverse declaration order (`nrf`, `udr`, `udm` declared -> `udm`, `udr`, `nrf`
destroyed), which matches the manual teardown order those blocks used, so shutdown sequencing is
unchanged.

This generalises an existing, real precedent rather than inventing one: the same RAII wrapper was
introduced locally in `test_udr_ondatachange_webhook.cpp` after an early-returning `ASSERT` there
orphaned `nrf`/`udr` during development (ADR-0171 series). Moving it to the shared header makes it
available to the other ~40 spawning tests without another repo-wide sweep.

### Live verification (the failure path, not the happy path)

A passing test proves nothing here -- the whole defect lives on the early-return path. So that path
was exercised directly: a deliberate `ASSERT_TRUE(false)` was injected immediately after both
`SpawnedProcess` constructions in `AmfRegistrationLifecycle`, the single TU rebuilt, and the test
run standalone.

Result: the test failed early as intended (`[  FAILED  ] 1 test`), and `pgrep` immediately after
found **no surviving `nrf` or `udm`** -- the destructors ran during unwinding. Before this change
that early return would have leaked both. The injection was then reverted and the file rebuilt
(verified: zero injection remnants remain in the committed source).

Incidental corroboration of ADR-0240 from the same session: a full `ctest` run was SIGKILLed
mid-flight by the environment, and a subsequent scan found zero orphaned NF processes.

### What this does NOT cover -- disclosed

- **The timing half of task #166** (UDR-dependent readiness waits in this file) is untouched here;
  this ADR closes only the cleanup-on-failure half that task named.
- **Scope at the time this ADR was written: only `test_udm_uecm_sdm.cpp`.** SUPERSEDED -- see the
  addendum below; every spawning integration test has since been converted.

### Addendum (task #172): conversion completed across all spawning tests

This ADR originally disclosed that only one file was converted and that the other ~40 spawning
tests kept the same early-return exposure. That is no longer true, and the disclosure above is
struck rather than left to rot. All 41 remaining files were converted in two passes, and the work
was NOT the uniform sed sweep the task assumed -- three genuinely different shapes existed, found
by inspection rather than presumed:

1. **17 files, simple locals** -- direct replacement of `const pid_t x_pid = spawn(X_PATH)` with a
   `SpawnedProcess` local and deletion of the trailing `kill`/`waitpid` blocks.
2. **23 files, holder-struct shape** -- a `Solo`/`Duo`/`Trio` struct of `pid_t` members built by a
   `spawn_all()`-style helper returning it BY VALUE, with a matching `reap()`/`reap_all()` free
   function. These could not compile against the original class: it was non-copyable AND
   non-movable, so `return t;` had no viable constructor. Required a real API addition, not an
   edit -- a move constructor and move assignment on `SpawnedProcess`, with the moved-from object
   dropping ownership (`pid_ = -1`) so exactly one destructor ever reaps a pid, plus a private
   `reap()` so the destructor and move-assignment share a single teardown path. The structs now
   hold `SpawnedProcess` members and the `reap*()` free functions are deleted outright.
3. **1 file, `test_hello_nf_registration.cpp`** -- a genuine special case: `hello-nf` is expected to
   RUN TO COMPLETION and the test asserts on `WIFEXITED`/`WEXITSTATUS`. A plain SIGTERM-on-destruct
   wrapper would have broken it. Handled by a new `wait_for_exit()` that waits, returns the raw
   status, and drops ownership so the destructor does not signal an already-reaped pid.

Also removed real drift found during the sweep: `test_udr_ondatachange_webhook.cpp` still carried
its OWN local `SpawnedProcess` class -- the original this shared wrapper was generalised from --
leaving two definitions in the tree. Exactly one now exists, in `spawn_guard.hpp`, used by 42 test
files.

The struct-shape conversion was done with a parser-driven transform that extracts each file's own
struct/member/helper names rather than matching a fixed pattern, and refuses any file it cannot
fully understand instead of guessing; it converted 23/23 with zero skips.

