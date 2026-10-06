## ADR-0394: a real AMF/NSACF/SMF shutdown crash, found while testing ADR-0393 -- root-caused and fixed

**Date:** 2026-09-27. **Status:** accepted, fixed. Found by accident while verifying ADR-0393 (not
introduced by it), disclosed the same day, then root-caused and fixed the same day once the actual
scope became clear: not one AMF-specific race, but the same bug class independently present in
three NFs (AMF, NSACF, SMF), because each had a detached thread holding references to `main()`'s
own local state with no way to be told to stop before `main()` returned and destructed it.

**What was observed.** `test_amf_deregistration.cpp`'s own AMF child process reliably crashes
during test teardown, moments after the test's own gNB association closes and the process
receives SIGTERM -- `terminate called after throwing an instance of 'std::system_error'` (`what():
Invalid argument`) on some runs, `Fatal glibc error: pthread_mutex_lock.c:... assertion failed`
(two different assertion messages seen across runs) on others. The non-identical failure signature
across otherwise-identical runs is itself informative: this is a genuine data race, not a
deterministic logic bug.

**It is not caused by ADR-0393.** Before concluding otherwise, the SAME crash was reproduced,
unchanged, in `test_amf_ngap_handover.cpp`'s own PRE-EXISTING tests --
`TargetGnbRefusalIsRelayedToTheSourceWithItsOwnCause`, `RegistrationAndPduSessionReachNsacf`, and
`UeIsRejectedWhenNsacfRefusesItsSlice` -- none of which touch Deregistration, NAS COUNT tracking,
or anything else ADR-0393 added. `RegisteredUeEstablishesARealPduSession`, run in isolation, does
NOT crash; the same test run alongside others sometimes does. The common factor across every
crashing case: the AMF child process's own detached per-association NGAP thread (ADR-0030/ADR-0095)
is still alive, blocked in `SctpSocket::receive()` or freshly returned from it, at the exact moment
SIGTERM arrives and the rest of the process begins tearing down shared state (io_contexts, SBI
HTTP/2 clients, Redis connections) -- a plain, timing-dependent shutdown race that no earlier test
happened to expose long enough to hit.

**Root cause, as far as this investigation went (not a fix).**
`run_ngap_lifecycle`'s own accept loop already carries the informal, disclosed comment
"Detached: this lab has no coordinated shutdown path for in-flight associations ... the process
exiting is what ends them" -- true, but understating the real risk: "the process exiting" is not
instantaneous or synchronized with a detached thread's own lifetime, and this investigation is the
first time that gap visibly crashed rather than merely leaking a thread past process exit.
`libs/sbi-core`'s own `ShutdownWatcher` (`io_context_pool.cpp`) already learned this exact lesson
once, for its OWN thread -- its header comment records "the first version detached it, and the
process then SIGSEGVed on exit ... RAII all the way -- stop the loop, join the thread, then the
members go." The per-association NGAP threads never got the same fix: they are spawned via
`std::thread(...).detach()` with no equivalent join/wait, and `ngap_core::SctpSocket` has no
thread-safe way to interrupt a blocking `receive()` from outside (no `shutdown()`/close-from-
another-thread method), so there is no cheap way to even ask them to stop promptly on SIGTERM.

**Resolution -- the AMF fix (`ngap_core::SctpSocket`, `nfs/amf/src/ngap_task.cpp`/`main.cpp`).**
Built the three pieces this ADR originally deferred, in order:
1. `SctpSocket::shutdown_now()` -- `shutdown(fd, SHUT_RDWR)` on the association's own fd, callable
   from any thread. Well-defined POSIX behaviour for a CONNECTED socket another thread is blocked
   reading from (unlike `close()`, which risks the classic fd-reuse race); the interrupted
   `receive()` returns `{}`, which its own existing ECONNRESET path already treated as a graceful
   disconnect, so no new logic was needed there. `fd_` became `std::atomic<int>` to make the
   concurrent read safe (move ctor/assignment updated to `exchange(-1)` accordingly).
2. `SctpSocket::accept_or_timeout()` -- the listening socket's own blocking `accept()` cannot be
   interrupted the same way (shutdown() on a LISTENING socket's pending accept() is not portably
   well-defined the way it is for a connected one), so `run_ngap_lifecycle`'s accept loop instead
   reuses the EXISTING `set_receive_timeout` (SO_RCVTIMEO, which the kernel also honours for
   accept()) to poll a `stop_requested` flag every 500ms instead of blocking forever.
3. `NgapShutdownCoordinator` (`ngap_task.cpp`, new class) -- a live-association registry +
   `sbi_core::on_shutdown_signal` callback that calls `shutdown_now()` on every currently-live
   association the instant SIGTERM arrives (closing the exact race window between "shutdown began"
   and "a brand-new association registers" too), plus a bounded (5s) wait for every association
   thread to actually finish. `handle_association` brackets its own `assoc` with RAII
   register/unregister so every return path is covered.
4. The piece this ADR's original three-piece list did not yet name, found while wiring the above
   together: `run_ngap_lifecycle`'s own thread was itself `.detach()`ed in `main.cpp` -- so even
   with (1)-(3) making that thread stop promptly and cleanly, nothing made `main()` actually WAIT
   for it before proceeding to `return 0` and destruct the shared stores/clients it was passed by
   reference. Fixed by not detaching it and `.join()`ing it after `run_multi_threaded(ioc)`
   returns -- the same "join after the blocking call returns" shape UPF's own `main()` already used
   for `run_pfcp_lifecycle` (ADR-0357), just AMF has two independent blocking loops (the SBI
   io_context and the NGAP thread) racing on shutdown where UPF effectively has one.

**Resolution -- NOT an AMF-only bug: the identical class, found by testing the fix.** Re-running
the AMF test suite after the fix above still crashed intermittently -- but now reproducibly
isolated to the **NSACF** child process, confirmed by process-tagged log output
(`amf-ngap: shutdown complete` logging cleanly, then the crash appearing between NSACF's own
`signal 15 received` line and PCF's). NSACF's `run_periodic_reporting` (PERIODIC subscription scan
thread) was spawned exactly the same way the AMF NGAP thread used to be: `std::thread(...,
std::ref(subscriptions), std::ref(slices)).detach()`, an infinite `while(true) { sleep_for(1s);
...touches subscriptions/slices... }` loop with zero shutdown awareness. Fixed the same way:
`stop_requested` atomic checked before and after the sleep, `on_shutdown_signal` sets it, the
thread is joined (not detached) after `run_multi_threaded(ioc)` returns.

A targeted audit (`grep -rn "std::ref(" nfs/*/src/main.cpp`, checking each hit for whether it is
passed into a `.detach()`ed thread AND references something `main()` itself owns and destructs on
return -- the two conditions together are what make a detached thread dangerous, not detaching
alone) found the SAME bug shape twice more:
- **UPF's `run_sbi_server`** -- detached, holds `std::ref(event_subs)`. Its own `io_context` was
  already correctly self-registered for SIGTERM via `run_multi_threaded`, so no internal loop
  change was needed; not detaching it and joining it (after `run_pfcp_lifecycle`'s own blocking
  call returns, mirroring the AMF fix's own shape) was sufficient. Found and fixed while here:
  `run_pfcp_lifecycle`'s own header comment claimed "Never returns" and "UPF has no HTTP2 server to
  share time with" -- both stale since ADR-0357 gave it a real stop_requested+UDP-nudge shutdown and
  `run_sbi_server` was added; corrected in the same pass, not left for someone else to trip over.
- **SMF's `run_pfcp_lifecycle`** (a different function of the same name from UPF's own) -- detached,
  holds `std::ref(upf_endpoint_store)`/`std::ref(pfcp_peer)`. Fixed with a `stop_requested` flag
  checked in its own outer retry loop, not detached, joined. **A real second-order bug found while
  fixing this one, not separately later:** this function's FIRST call is `discover_upf_ipv4`, whose
  own `while(true)` NRF-discovery retry loop (real and necessary -- a UPF this SMF needs may
  legitimately not have registered yet; this project's own test logs show it happening routinely:
  "smf: no UPF registered with NRF yet, retrying discovery in 2s") had NO stop awareness of its
  own. Adding a stop check only to the OUTER `run_pfcp_lifecycle` loop would have been a fix that
  looked complete and was not: joining that thread on shutdown would have hung indefinitely in
  every test or deployment where no UPF is present yet -- trading ADR-0394's original crash for a
  guaranteed hang, a worse regression, not a smaller one. `discover_upf_ipv4` now takes the same
  `stop_requested` reference and returns `std::optional<std::string>` (`std::nullopt` meaning
  "asked to stop before ever discovering a UPF"), which `run_pfcp_lifecycle` checks before
  proceeding. Verified directly, not assumed: `SmfNiddIntegration.DeliverMissingContextIs404ThenRealContextIs204`
  (a test that spawns SMF alone, no UPF) now logs `"smf: PFCP lifecycle stopping before UPF
  discovery ever succeeded (SIGTERM)"` and exits cleanly, where it would previously have hung.
- **SMF's own Nsmf_EventExposure QOS_MON producer** (an anonymous lambda thread, not a named
  function) -- detached, captures `&event_subs, &sm_contexts` by reference, same shape again. Fixed
  with a condition-variable-based interruptible wait rather than a plain `sleep_for` + flag check:
  this thread's own configured interval defaults to 30s, and a plain post-sleep flag check would
  have made every SMF shutdown wait up to that long -- correct in the sense of not crashing, but an
  unacceptably slow bound for a test suite or a real rolling restart alike, so `std::condition_
  variable::wait_for` with a predicate is used instead, woken immediately by the shutdown callback's
  own `notify_all()`.

**What was deliberately NOT changed.** `run_nrf_lifecycle` (duplicated per-NF, spawned detached in
every NF including all three touched here) holds no references to any `main()`-local state --
confirmed by reading its call sites (every argument is passed by VALUE: instance id, NRF base URL,
advertised IP), not assumed safe. A detached thread with no dangling reference is a real, disclosed,
lower-severity category (an orphaned thread that keeps running briefly past `main()` return, doing
at most one harmless failed HTTP call as the process exits) -- NOT the bug class this ADR fixes,
and left alone rather than rewritten under time pressure for a problem it does not have.

**Verification.** The exact crash reproduction this ADR originally recorded --
`AmfNgapTestGnb.TargetGnbRefusalIsRelayedToTheSourceWithItsOwnCause`,
`RegistrationAndPduSessionReachNsacf`, `UeIsRejectedWhenNsacfRefusesItsSlice`, plus the two new
`AmfDeregistration.*` tests, run together repeatedly -- now passes with zero crashes across
multiple consecutive full runs (previously: crashed on a majority of runs). NSACF's own
`NsacfIntegration.ThresholdReportAndEacNotificationAreReallyDelivered` (which exercises
`run_periodic_reporting`'s real PERIODIC-subscription behaviour, not just its shutdown) still
passes, confirming the fix changed only shutdown timing, not runtime behaviour. The full
`SmfIntegration`/`SmfEventExposureIntegration`/`SmfNiddIntegration`/`SmfHandoverN2SmInfoIntegration`/
`SmfN2SmInfoDispatch`/`SmfHandoverCancel` suites pass unchanged. `clang-format-18 --dry-run
--Werror` clean on every touched file.

**Impact this closes.** In a real deployment, a gNB's NGAP association ending (a real, routine
event -- a gNB restart, a transport-layer reset) at the same moment an AMF instance is asked to
shut down (a rolling restart, a scale-down) could have crashed the instance instead of exiting
cleanly -- and the identical risk existed in NSACF (any shutdown while its 1s periodic-reporting
scan was live, i.e. almost always) and SMF (any shutdown before UPF association succeeded, or
during a QOS_MON tick). This project's own commercialization mandate (ADR-0049) requires
reliability exceeding free5GC's; this was a real gap against that bar, found, root-caused, and
closed in the same investigation rather than left disclosed for someone else to eventually hit in
production.

