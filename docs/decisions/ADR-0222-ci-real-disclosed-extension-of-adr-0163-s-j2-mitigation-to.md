## ADR-0222: CI -- real, disclosed extension of ADR-0163's `-j2` mitigation to `build` and
`sanitize (tsan)`, correcting ADR-0221's premature "known flakiness" characterization

### Context

User reported real CI errors and asked for verification, not assumption. ADR-0221's own "What this
ADR does NOT include" section had characterized the `build`/`sanitize (tsan)` failures on run
`32965047394` (the ADR-0219 push) as "already-documented CI-runner shutdown flakiness ... not a
code issue" -- reasonable at the time from that single run's `build` job annotation ("The hosted
runner lost communication with the server"), but **premature**: it treated one run's annotation as
conclusive without testing whether the pattern reproduced. It didn't just recur -- it recurred at
the *identical* point in the build graph on two separate attempts.

Real investigation performed at the user's request, not assumed:

1. Triggered `gh run rerun 32969733375 --failed` (the lint-fix commit's own CI run) to test whether
   the failures were transient. They recurred: `build` and `sanitize (tsan)` both failed again,
   `lint` passed clean (confirming the real clang-format fix from the same push).
2. Read each failed job's full log around its actual stall point (`gh run view --log-failed`), not
   just the top-level annotation. Both jobs stalled compiling the exact same file --
   `nfs/smf/src/pfcp_peer.cpp.o` -- at ~89% through the 1407-target build, with a real,
   multi-minute gap in `ninja`'s own progress output (20 min for `build`, 8.5 min for `tsan`)
   before GitHub killed the job (exit 143 / "runner lost communication").
3. Ruled out "this file is just slow": `rm`+rebuilt `nfs/smf/src/pfcp_peer.cpp.o` alone, locally,
   single-threaded (`-j1`) -- real wall time 12.5s. The file itself is not the bottleneck.
4. Read `.github/workflows/ci.yml`'s own ADR-0163 comment: it explicitly recorded that at the time
   of that ADR, only `sanitize (asan-ubsan)` showed this failure class, and `tsan`/`build`
   deliberately were NOT given the same `-j2` parallelism cap because they "don't show this
   pattern." That was true at ADR-0163's time (5 genuine `asan-ubsan` failures found, 0 for
   `tsan`/`build` in the sample checked then) -- but is no longer true now, evidenced directly
   above.

### Real, disclosed hypothesis (not a proven root cause, same standard as ADR-0163)

The codebase has grown substantially since ADR-0163 (more NFs wired, more `sbi-codegen`-generated
types per the codegen's own reported breakdown -- 2886 types across 110 generated files as of this
session). More/larger translation units compiling in parallel at default (full-core) `ninja`
parallelism plausibly now pushes memory pressure on GitHub's resource-constrained hosted runners
past the threshold in `build`/`tsan` too, not just the historically-worse `asan-ubsan` leg --
consistent with the stall consistently landing late in the build (~89%, deep into a dense cluster
of `nfs/amf`/`nfs/smf`/`nfs/nrf` object compiles happening concurrently) rather than early or
randomly. This is evidence-based, not certain -- GitHub's own scheduler/resource-allocation
behavior remains outside this project's direct observation, same epistemic caveat ADR-0124/
ADR-0163 already carried.

### Implementation

`.github/workflows/ci.yml`: both the `build` job's own `Build` step and the `sanitize` matrix
job's shared `Build` step (previously conditional on `matrix.sanitizer == 'asan-ubsan'`) now run
`cmake --build build -- -j2` unconditionally. `sanitize (asan-ubsan)` already had this; `tsan` and
`build` are the real extension. Both step-level comments updated to reflect the current, correct
scope and cite this ADR.

### Correction to ADR-0221

ADR-0221's "What this ADR does NOT include" / Testing section's claim that the `build`/
`sanitize (tsan)` failures on run `32965047394` were "not a code issue" without qualification is
corrected here, not silently edited in place (this project's own git-safety rule: no amending
already-committed history) -- the annotation-level read was accurate as far as it went, but calling
it "known flakiness" was premature before the reproduction test this ADR performed. The underlying
claim -- that it isn't a defect in this session's own UDM route code -- still holds; the more
precise finding is a real, previously-uncharacterized CI resource-constraint pattern with a
targeted, evidence-based fix, not an unexplained transient.

### Testing

Cannot be verified via local `ctest`/build (this is a CI-infrastructure change, not application
code) -- verification is watching the next real CI run(s) on `main` for `build`/`sanitize (tsan)`
reaching a genuine `success` (or, if the hypothesis is wrong, a *different* failure mode than the
identical `pfcp_peer.cpp` stall, which would falsify this ADR's own hypothesis and warrant further
investigation rather than declaring victory). Flagged as follow-up watching, same as ADR-0163's own
disclosed need to watch future runs.

### What this ADR does NOT include

A guarantee this fully resolves the pattern -- `-j2` is the same targeted mitigation ADR-0163 used,
not a redesign of the build (no compiler cache/ccache, no reduced translation-unit count). If
`build`/`tsan` still fail after this change, the next step is a deeper investigation (e.g. adding
`ccache`, splitting the sanitizer matrix across more runners, or an even lower `-j` cap) rather than
re-declaring this "flakiness" again.

### Real, live verification of this ADR's own fix (run `32993336424`, the push containing it)

- **`build`**: real `success` on the `Build` step itself -- direct confirmation the `-j2` cap fixed
  the `pfcp_peer.cpp` stall this ADR targeted. The job's overall conclusion was still `failure`,
  but from a *different*, later step (`Test`/`ctest`) -- see below, a real but separate finding.
- **`sanitize (asan-ubsan)`**: failed again, but from a *different* signature than this ADR's own
  target pattern -- died only 5.5 minutes in, at target 42 of 1408 (far too early for memory
  pressure), with GitHub's own explicit annotation "The runner has received a shutdown signal...
  This can happen when the runner service is stopped, or a manually started runner is canceled."
  This is the account-level runner-reclaim pattern ADR-0124/ADR-0132 already disclosed and
  mitigated (concurrency-group cancellation of *superseded* runs) -- not new, and this ADR's `-j2`
  extension was never meant to address it; the two patterns are genuinely distinct despite both
  ending in exit 143.
- **`sanitize (tsan)`**: `cancelled`, not because of its own health -- GitHub Actions' default
  `fail-fast: true` (no override previously set on this matrix) killed the still-running, unrelated
  `tsan` leg the instant `asan-ubsan` failed. Real, observed waste: `tsan` might have passed cleanly
  and told us nothing was wrong with it, but fail-fast discarded that signal. Fixed in this same
  ADR: `strategy.fail-fast: false` added to the `sanitize` matrix job.
- **`Test` step, `build` job -- a real, separate finding, not yet fixed here**: `ctest` reported
  `96% tests passed, 20 tests failed out of 447`. All 20 were `Nef*Integration` tests (test IDs
  91-109, every remaining NEF test in the suite) failing with `Error: null context when
  constructing CivetServer. Possible problem binding to port.` The real root: test #90,
  `NefSMServiceIntegration.SendSMSReturnsRealMultipartDeliveryReport`, genuinely timed out at the
  full 120s (`ctest --timeout 120`) -- confirmed via three local runs *this same session*
  (`full_ctest219.log`/`220`/`221`) where this exact test passed in 0.42-0.44s every time, ruling
  out a deterministic bug in the SendSMS handler itself. The real, disclosed structural gap this
  exposes: this project's own integration-test pattern (`fork()`+`execl()` to spawn real NF child
  processes, reaped by an explicit `reap()` call at the end of each `TEST()` body) has no cleanup
  path for the case where `ctest`'s own `--timeout` kills the test binary *before* it reaches that
  `reap()` call -- the spawned NF child (here, `nef`) is orphaned still holding its port, and every
  later test needing that same port fails to bind. This is the same underlying class of issue as
  the already-tracked, not-yet-root-caused `UdrIntegration.AmfContextLifecycle` hang (task #165)
  and `test_udm_uecm_sdm.cpp` timing fragility (task #166) -- not new, not caused by this session's
  own UDM/CI changes (zero `nfs/nef` files touched this session), and **not fixed by this ADR**.
  A real, general fix (e.g. process-group-based cleanup so a `SIGKILL`/`SIGTERM` to the test binary
  also reaps its children, or a per-test RAII guard) is real, valuable work but a distinct,
  larger-scoped initiative from this ADR's own `-j2`/`fail-fast` build-parallelism fix -- flagged
  as follow-up, not silently absorbed into "still flaky."

