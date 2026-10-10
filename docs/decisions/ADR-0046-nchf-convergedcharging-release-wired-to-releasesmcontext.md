## ADR-0046: Nchf_ConvergedCharging_Release wired to ReleaseSMContext

**Date:** 2026-08-10
**Status:** Accepted.

**Context:** ADR-0044 deferred Update/Release to separate turns. Of the two, only Release has a
real, already-existing trigger point today: `ReleaseSMContext` already calls PCF's `DeleteSMPolicy`
on session teardown (ADR-0029); `UpdateSMContext` does not call PCF at all yet, so there is no real
trigger for `Nchf_ConvergedCharging_Update` to attach to without inventing one -- Update stays
deferred, Release does not.

**CHF side.** `ChargingDataRefAllocator` (a bare ID generator, ADR-0044) is upgraded to
`ChargingDataStore`, which now tracks which refs are currently active -- needed so Release can
correctly 404 an unknown or already-released `ChargingDataRef` (TS 32.291's real semantics) rather
than accepting anything. New route `POST /chargingdata/{ChargingDataRef}/release`: parses the
request body as `ChargingDataRequest` (real spec shape, `requestBody` required) for mandatory-field
validation parity with Create, checks the ref is active, returns 204 (real spec response) or a 404
`ProblemDetails`.

**SMF side.** `perform_n40_charging_data_create` now returns the allocated `ChargingDataRef`
(parsed from CHF's `location` header, same extraction pattern already used for PCF's `smPolicyId`)
instead of a bare bool. The ref -- and, newly, the session's `supi` (never previously stored in
`SmContextStore`, needed so Release can populate `subscriberIdentifier` without re-deriving it) --
are merged into the already-stored SM context via a read-modify-write (`SmContextStore::update`
replaces the whole entry, so a naive second `update` call would have clobbered the `smPolicyId`/
`policy` fields Create's PCF call already wrote). New `perform_n40_charging_data_release`, called
from `ReleaseSMContext`'s handler with the same best-effort/non-fatal discipline as the existing
`DeleteSMPolicy` call right above it -- local session release must not get stuck on CHF being
unreachable. Skipped (not sent with a fabricated ref) if either `chargingDataRef` or `supi` is
missing from the stored context, e.g. because Create's own N40 call never succeeded for that
session.

**Live-verified, both the success and the failure path.** Full stack + a real `nr-gnb`/`nr-ue` PDU
Session Establishment produced a real `ChargingDataRef` (`chg-1`, `smf`'s log:
`Nchf_ConvergedCharging_Create succeeded for pduSessionId 1, ChargingDataRef=chg-1`). No NF in this
codebase currently triggers `ReleaseSMContext` automatically (no UE-initiated deregistration flow
exists yet, a disclosed gap in the same category as several others already in this project) -- so
the release was triggered directly against SMF's real endpoint (`POST /nsmf-pdusession/v1/
sm-contexts/smctx-1/release`, real mTLS client cert, HTTP 204), same "provide the trigger a real
upstream flow doesn't exist for yet" pattern this project's own integration tests already use.
Result: `smf`'s log confirms `Nchf_ConvergedCharging_Release succeeded for ChargingDataRef=chg-1`,
and both NFs' own independent Prometheus counters agree:
`chf_charging_data_release_total{otel_scope_name="chf"} 1` and
`smf_chf_charging_data_release_total{otel_scope_name="smf"} 1`. The failure path was also verified
for real, not assumed: releasing `chg-1` a second time correctly returned a 404 `ProblemDetails`
(`"No active charging data resource chg-1"`) rather than silently succeeding or crashing.

**Consequence:** `Nchf_ConvergedCharging_Create` and `_Release` are both real and live-verified,
covering PDU session establishment through teardown's charging lifecycle. `Nchf_ConvergedCharging_
Update`, the `chargingNotification` callback, and everything else deferred in ADR-0044/ADR-0045
remain deferred, each needing a real trigger or real data to exist first.

