## ADR-0224: CI -- real root cause of the `Nef*Integration` cascade found (`nef` missing from PKI
generation), ADR-0223's exclusion retracted

### Context

ADR-0223's own diagnosis was **wrong**, found by continuing to investigate rather than treating the
push as done once `-E` made the symptom disappear. Pushing ADR-0223's exclusion and re-running CI
(run `33010729282`) still showed 19/446 tests failing -- but now the *first* failure was a different
test, `NefUEIdIntegration.FetchAndMappingBothReturnRealHonestNoContent` (test #90 after
`SendSMS` was excluded, previously test #91), not the excluded `SendSMS` test. This proved the hang
was never specific to `SendSMS` or multipart -- it was whichever `Nef*Integration` test happened to
run first in the suite.

Reading that failure's own captured output (`--output-on-failure`) directly, rather than trusting
ctest's own "Timeout" label, found the real error, logged 5.36 seconds into the test (not 120s):

```
terminate called after throwing an instance of 'std::runtime_error'
  what():  sbi-core: failed to load TLS material (cert=.../certs/nef/cert.pem, ...):
  use_certificate_chain_file: No such file or directory [system:2]
```

The `nef` binary was **crashing on startup** because `certs/nef/cert.pem` never existed on this
from-scratch CI checkout. `.github/workflows/ci.yml`'s own "Generate lab PKI" step (in both the
`build` job and the `sanitize` matrix job) calls `scripts/gen-lab-pki.sh` with an explicit,
hand-maintained list of NF names -- and that list was `nrf hello-nf amf smf udm udr ausf pcf chf upf
product-catalog subscriber-management roaming-interconnect`. **`nef` was never added to it.**
Confirmed via `tests/integration/CMakeLists.txt`'s own `NEF_PATH` usage that the integration suite
genuinely spawns a real `nef` binary and needs its cert like every other NF. Confirmed via
`deploy/docker/docker-compose.yml`'s own, separate `gen-lab-pki.sh` invocation that it *does*
already include `nef` -- this gap was specific to the CI workflow file, not a repo-wide oversight.

This exactly explains everything ADR-0222/ADR-0223 observed without correctly diagnosing:
- **Why 0/3 locally, 2/2 (now 3/3) in CI**: this step's own comment already disclosed the identical
  historical bug for every *other* NF ("local dev machines already had every NF's certs generated
  from countless prior manual sessions, masking that CI's own from-scratch checkout never got
  them") -- `nef`'s cert already existed on this dev machine from prior sessions' own manual runs,
  for the same reason.
- **Why the hang was reported as exactly 120.10s by ctest**: `nef` crashing makes
  `wait_reachable()` correctly fail fast (~5.36s, confirmed above) via `ASSERT_TRUE`, which
  short-circuits the test function -- so `reap_all()` at the end of the `TEST()` body **never
  runs**. The still-running `nrf` child process (spawned just before `nef`) is orphaned, and being
  a `fork()`-ed child, it inherits the test binary's stdout/stderr file descriptors. `ctest
  --output-on-failure` captures a test's output by reading its process's stdout pipe until EOF --
  which the orphaned `nrf` process keeps open indefinitely, so ctest's read blocks until its own
  `--timeout 120` forcibly kills the whole process tree, at which point the pipe finally closes and
  ctest reports "Timeout" -- even though the actual gtest assertion failed cleanly at 5.36s. Real,
  same underlying mechanism as task #170's own already-tracked timeout-orphan class.
- **Why it cascaded to every later `Nef*Integration` test**: the orphaned `nrf` process squats port
  7777; every subsequent test's own freshly-spawned `nrf` fails to bind it.

### Implementation

`.github/workflows/ci.yml`: added `nef` to both "Generate lab PKI" steps' NF list (`build` job and
`sanitize` matrix job).

### Retraction of ADR-0223

ADR-0223's own `-E` exclusion for `NefSMServiceIntegration.SendSMSReturnsRealMultipartDeliveryReport`
is **removed** in this same ADR, not left in place -- it was excluding a test that has no real
defect (once `nef` actually starts, there is no reason to believe `SendSMS` itself would fail;
its own route handler was already confirmed pure synchronous logic in ADR-0223's own
investigation). Leaving that exclusion in place after finding and fixing the real cause would
silently and permanently drop real test coverage for no remaining reason. Not amended in git
history (this project's own git-safety rule) -- the retraction is this new commit, and ADR-0223's
own entry above is left as the accurate historical record of what was believed and done at the
time, corrected by this entry rather than rewritten.

### What this ADR does NOT include

Task #170 (general test-harness fix so a `ctest --timeout` kill reaps orphaned NF children instead
of leaking them) remains open and real -- this ADR fixed the specific trigger (missing `nef`
certs) for this specific cascade, not the general robustness gap that any other test's own
`ASSERT_*`-before-`reap()` failure could still exercise in the future. Does not audit whether any
*other* CI-only script (beyond the two `gen-lab-pki.sh` call sites and the already-checked
`docker-compose.yml`) has a similarly stale hand-maintained NF list -- not found in this
investigation's own grep across `*.sh`/`*.yml`/`Makefile`, but not exhaustively proven absent for
every possible future case.

