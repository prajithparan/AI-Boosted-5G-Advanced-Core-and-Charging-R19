## ADR-0051: Pending-items cleanup -- real per-ChargingDataRef invocation sequencing + a real concurrent-libcurl-handle bug found and fixed

**Date:** 2026-08-10
**Status:** Accepted.

**Context:** Before starting the next new subsystem (NSSF), the user asked for a full audit of
what's genuinely pending from Phases 0-4 (as opposed to deliberately deferred future scope). The
audit (real, dedicated read-through of the entire `docs/DECISIONS.md`/`docs/TRACEABILITY.md` log
plus repo-wide greps) surfaced several real items; this ADR covers the first one closed: ADR-0050's
own disclosed gap where `perform_n40_charging_data_release` hardcoded
`invocationSequenceNumber=2` instead of sharing the real per-session counter Stage 3's Update call
used, a genuine TS 32.291 "strictly increasing per invocation" violation if both an Update and a
Release landed on the same `ChargingDataRef`.

**Real fix: one counter, keyed by the right thing.** `CpSeidSessionStore`'s own per-`cp_seid`
invocation counter (Stage 3) and Release's hardcoded literal were two separate, inconsistent
mechanisms. Neither was the right design on its own: `cp_seid` isn't the right key for invocation
sequencing at all, since a session with no granted quota never gets a `CpSeidSessionStore` entry
(Stage 3's own registration guard, only URR'd sessions can produce a Usage Report) but its
`ChargingDataRef` can still be Released. New `ChargingDataInvocationSeqStore`, keyed by
`charging_data_ref` (a string) instead: seeded to 2 unconditionally by every real
`Nchf_ConvergedCharging_Create` call site (not just ones with a grant), read-and-advanced by both
`perform_n40_charging_data_update` and `perform_n40_charging_data_release`. `CpSeidSessionStore`
itself is now purely session-info resolution (`get()`, no counter, no advancing) -- the invocation-
sequencing concern moved out of it entirely, correcting the original design mistake rather than
patching around it.

**A real, previously-undetected concurrency bug found during live re-verification, not by
inspection.** Live-testing the fix (same real `nr-gnb`/`nr-ue` session + GTP-U burst pattern
ADR-0050's own stages used) hit a real failure: two Session Reports (VOLTH then VOLQU, ~100ms
apart) each spawn their own detached `std::thread` calling `Nchf_ConvergedCharging_Update` (Stage
5's own deadlock-avoidance design) -- and both threads call `send()` on the SAME shared
`chf_report_client` (one `sbi_core::http2::Client` instance) at nearly the same time. `Client` holds
a single, reused libcurl easy handle (`CURL*`) with zero synchronization -- and libcurl's own
contract is explicit that one easy handle must never be driven by two threads concurrently. The
result was real, live, malformed-looking failures (`curl_easy_perform` effectively returning
"Failed initialization" / empty responses) under genuine concurrent access -- this had never
surfaced before because every other `Client` instance in this codebase is only ever touched from
one thread by convention (explicitly documented at several call sites, e.g. "second client safe on
the shared ioc thread"), and Stage 3's own original design called Update inline on `PfcpPeer`'s
receive thread (serialized by construction) rather than from concurrent detached threads. Stage 5
was the first design in this codebase to give one `Client` instance two genuinely concurrent
callers.

**Real fix, at the source, not the call site.** Added a `std::mutex` to `sbi_core::http2::Client`
itself (`libs/sbi-core/include/sbi_core/http2_client.hpp`/`.cpp`), serializing `send()` -- fixes
this foundationally for every current and future caller of a shared `Client` instance, not just
`chf_report_client`. `Client` was already documented as synchronous/blocking per call (ADR-0006);
serializing concurrent callers doesn't change any already-documented characteristic, it just makes
concurrent-caller safety real instead of accidentally-never-tested.

**Live-verified, the exact scenario that failed before now succeeds.** Same real `nr-gnb`/`nr-ue`
session, small seeded grant, 25-packet GTP-U burst producing two Session Reports 100ms apart: both
real `Nchf_ConvergedCharging_Update` calls now succeed (previously one failed with the concurrent-
handle error), both real Session Modifications land. A subsequent real
`POST /sm-contexts/{ref}/release` (invoked directly, since no real deregistration flow exists yet)
returned a real 204, with `smf`'s log confirming `Nchf_ConvergedCharging_Release succeeded` and no
"no invocation-sequence counter registered" fallback warning -- confirming the real counter
(Create=1, Update=2, Update=3, Release=4) was found and used, not defaulted. Full rebuild (the
`sbi_core` change touches every NF that links it) + 140/140 conformance tests + all 31 integration
tests (`tests/integration/integration_tests`, run directly -- see the audit's own separate finding
that these aren't currently registered with `ctest`, a distinct, smaller pending item not fixed by
this ADR) pass, zero regressions.

**Consequence:** two real, independent correctness gaps closed in one turn -- one found by the
audit, one found only by live-testing the audit's own fix. Confirms this project's own established
practice (live verification over self-consistency testing, see `feedback-crypto-verification`
memory) catches real bugs unit tests alone would not have -- this concurrency bug would not have
been caught by any of the 140 conformance tests, all of which are single-threaded.

