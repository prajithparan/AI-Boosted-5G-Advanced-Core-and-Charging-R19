## ADR-0179: real automated integration test for `onDataChange` webhook delivery

### Context

Every ADR from 0171 through 0178 disclosed the same real testing gap: `onDataChange` delivery was
verified manually per batch (curl + a standalone Python HTTPS receiver script), never inside
`ctest`. With the ADR-0171 through ADR-0178 series' own per-route wiring effort now fully closed
(87 real write-route call sites, 41 resource types), presented as a genuine decision point (task
#106's pattern had no more resource left to wire); user chose to close this real testing gap next.

New test file `tests/integration/test_udr_ondatachange_webhook.cpp`,
`UdrIntegration.OnDataChangeWebhookDeliveredOnPutPatchDelete`: spawns real `nrf`+`udr` OS
processes (same pattern as the existing `test_udr_context_data.cpp`), plus a real subscriber's
callback endpoint built from `sbi_core::http2::Server` -- the *exact same* TLS 1.3 + mTLS server
implementation every NF in this project runs, not a stub, mock, or external script -- listening
in-process on its own `io_context` thread. Subscribes via a real `POST subs-to-notify`, then
drives `PUT`+RFC 6902 `PATCH`+`DELETE` against `smf-registrations` (chosen specifically because it
has all three write verbs on one document, unlike `amf-3gpp-access`, which genuinely has no
`DELETE` at all -- confirmed by direct read, see the real bug found below), asserting the receiver
independently captured a correctly-shaped `DataChangeNotify` after each one, covering all three
real `ChangeItem` shapes this project's `notify_subscribers()` can produce
(`change_replace`/`change_from_json_patch`/`change_remove`) in a single test.

### Two real bugs found and fixed while writing this test

1. **Test design bug, not a UDR bug**: the first draft asserted a `DELETE` against
   `amf-3gpp-access` would return `204`. Direct read of `nfs/udr/src/main.cpp` confirmed
   `amf-3gpp-access` genuinely has real `GET`+`PUT`+`PATCH` only -- no `DELETE` operation exists in
   the spec at all (same class of real, disclosed narrowing as several other resources in this
   project). The real `404` UDR correctly returned was flagged as a `Failure` by the test, not a
   production defect. Fixed by moving the delete-coverage portion of the test onto
   `smf-registrations`, which does have a real `DELETE`.
2. **Real crash bug in the test itself**: a `GoogleTest` `ASSERT_*` failure (triggered while
   chasing bug 1 above) returns immediately from the test body, which skipped this test's own
   manual `kill(pid, SIGTERM); waitpid(...)` cleanup for the spawned `nrf`/`udr` processes and the
   `receiver_thread.join()` call for the in-process receiver's `io_context` thread -- a still
   -joinable `std::thread` destructing without `join()` calls `std::terminate()` per the C++
   standard, which is exactly what happened (`terminate called without an active exception`,
   observed directly, not inferred). Fixed with two small RAII wrappers, `SpawnedProcess` (owns a
   forked child's `pid_t`, `SIGTERM`+`waitpid` in its destructor) and `IoContextThread` (owns the
   receiver's `io_context`+`std::thread`, stops and joins in its destructor) -- cleanup now runs
   unconditionally via scope exit regardless of which assertion fails or where.
3. **Real test-isolation bug, found while re-running manually**: a fixed literal `ue_id` collided
   with a leftover row from an earlier run that failed before reaching its own `DELETE` (real,
   persistent PostgreSQL backing store, ADR-0079 -- state genuinely survives across separate `udr`
   process lifetimes by design). A re-run's `PUT` then received a real `204` (already-exists,
   correct upsert-replace semantics) instead of the expected `201`, which is not itself a bug, but
   made the test flaky against its own residual state. Fixed using the exact same precedent this
   project already established for this exact problem: `test_n28_spending_limit.cpp`'s own
   `"...counter-" + std::to_string(getpid())` pattern, applied here as the `ue_id` suffix so each
   process invocation gets a fresh, uncollided row regardless of how the previous run ended.
   Verified by running the rebuilt test twice in direct succession -- both passed.

### Testing and verification

Ran the new test standalone (twice, back-to-back, confirming bug 3's fix); ran the full
`conformance_tests`+`integration_tests` suite (`ctest`, excluding the two disclosed pre-existing
flaky tests): 332/332 pass, zero regressions (331 baseline + this one new test). `udr` and the new
test binary both built clean (zero warnings) after `clang-format-18`.

### What this ADR does NOT include

This is one representative test covering the shared `notify_subscribers()`/`deliver_onDataChange()`
delivery core end-to-end (PUT/PATCH/DELETE on a per-UE resource) -- it does not add a dedicated
automated test per each of the 41 wired resource types, matching this whole series' own
established "one new live proof, not every individual resource" verification-scope discipline
(ADR-0172 onward), now applied to the automated-test layer instead of manual curl. The non-per-UE
delivery path (`notify_subscribers_ue_less()`) remains covered only by the manual live
verifications already recorded in ADR-0176/ADR-0177/ADR-0178, not by this automated test.

