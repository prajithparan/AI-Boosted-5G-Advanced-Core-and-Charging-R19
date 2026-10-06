## ADR-0029: SMF -> PCF wiring (SM Policy Association Establishment/Termination); AMF -> PCF deferred

**Date:** 2026-08-07
**Status:** Accepted

**Context:** The first cross-NF wiring turn since Phase 2's seven NFs were all stood up standalone
(ADR-0025 through ADR-0028). User asked to wire both AMF and SMF to PCF; before implementing,
research turned up a real asymmetry between the two that changed the scope agreed with the user.

**SMF -> PCF has a clean, correct trigger.** `CreateSMContext` (`POST /sm-contexts`) is exactly TS
23.502 §4.3.2.2.1's SM Policy Association Establishment trigger, and `nfs/smf/src/main.cpp`'s file
header already named "no real PCF" as the specific, disclosed reason CreateSMContext couldn't do
this before PCF existed (ADR-0021).

**AMF -> PCF has no correct trigger in this build, and was NOT forced onto one.** AMF only
implements `Namf_Communication` (ADR: initial AMF turn) -- there is no NAS/N1 Registration entry
point in this codebase at all (no NGAP stack, disclosed since ADR-0016), so nothing in AMF's
current surface corresponds to "a UE just registered," which is the real trigger for AM Policy
Association Establishment (TS 23.502 §4.2.2.2.2). The closest existing AMF operation,
`CreateUEContext`, is specifically the *inter-AMF mobility/handover* operation (a UE context
arriving from a different AMF) -- attaching fresh AM Policy Association Establishment to it would
misrepresent which real procedure it belongs to (at real inter-AMF mobility, the new AMF typically
reuses/modifies an existing association, not creates one the way initial Registration does).
Presented three options to the user: (1) use `CreateUEContext` anyway, disclosed as a proxy;
(2) defer AMF->PCF entirely until AMF has a real registration-adjacent trigger; (3) write and test
the calling logic but leave it unwired. **User chose (2)** -- this turn is SMF->PCF only. AMF->PCF
remains unimplemented, not attached to the wrong procedure to force something to ship.

**Decision (SMF's side):** `CreateSMContext` is now a real SBI client to PCF -- a second
`http2::Client`/`OAuth2Client` pair (scope `npcf-smpolicycontrol`, target `PCF`), same pattern
ADR-0027 established for AUSF calling UDM (separate from `run_nrf_lifecycle`'s own client; safe
without a mutex because route handlers and the lifecycle thread never touch the same `Client`
instance). On success, the returned `SmPolicyDecision` and PCF-assigned `smPolicyId` are stored
alongside the SM context (not exposed in `SmContextCreatedData` -- TS29502 has no field for it,
matching `n2SmInfo`'s already-disclosed unpopulated state) so `ReleaseSMContext` can call PCF's
`DeleteSMPolicy` later. `UpdateSMContext` is NOT wired to PCF's `UpdateSMPolicy` this turn --
scope stayed to the two operations TS 23.502 §4.3.2.2.1 actually needs (establishment and
termination), not every SM Policy operation that exists.

**Two real gaps, disclosed rather than smoothed over:**
- `SmContextCreateData`'s schema allows `supi`/`pduSessionId`/`dnn`/`sNssai` to all be absent
  (e.g. unauthenticated-SUPI edge cases), but PCF's `SmPolicyContextData` requires all four. This
  build's PCF wiring has nothing to fall back to without them, so `CreateSMContext` now returns
  400 if any are missing -- a real, disclosed narrowing of what this build accepts versus what the
  full spec permits, confirmed by reading `SmContextCreateData`'s generated struct (all four
  fields are `std::optional`), not assumed.
- `SmContextCreateData` has **no `pduSessionType` field at all** -- confirmed by grepping the full
  generated struct, not assumed absent. It's negotiated inside the NAS SM message (`n1SmMsg`, an
  opaque binary blob this build never decodes, the same class of gap as every other NAS-decoding
  simplification here). SMF sends a fixed default (`IPV4`) to PCF instead of the UE's real
  requested type.

**Fail-closed on create, best-effort on release -- a deliberate asymmetry, not an inconsistency:**
`CreateSMContext` fails (500) if PCF is unreachable or errors, matching TS 23.502's real intent
that SM Policy Association Establishment failure fails PDU session establishment -- verified with
a dedicated test that starts SMF without PCF running at all. `ReleaseSMContext`'s `DeleteSMPolicy`
call, by contrast, is best-effort: local release still succeeds (204) even if PCF is unreachable,
so a downstream PCF outage can't strand SMF's own cleanup path (logged on failure, not silently
swallowed) -- the same reasoning a real SMF's teardown path would use.

**Verification:** manual `curl` end-to-end (recorded in `docs/TRACEABILITY.md`) plus updates to
`tests/integration/test_smf_pdu_session.cpp`: the existing full-lifecycle test now spawns a real
`pcf` alongside `nrf`/`smf` and, after `CreateSMContext`, queries PCF *directly* for the SM Policy
it should have created (`GET /npcf-smpolicycontrol/v1/sm-policies/smpolicy-1` -- relying on PCF's
sequential id assignment from a freshly-spawned process, disclosed as a test-only assumption, not
an SMF behavior), confirming `context.supi`/`context.dnn` and the `default` session rule are real,
not just that SMF returned 201; after `ReleaseSMContext`, the same PCF query now returns 404,
proving `DeleteSMPolicy` was really called too. Two new tests: `CreateSMContextFailsClosedWhenPcfUnreachable`
(SMF spawned without PCF at all -- 500, not a silent success) and
`CreateSMContextRequiresSupiPduSessionIdDnnAndSNssai` (the old ADR-0020-era minimal body, now 400).
All 58 project tests pass, stable across three repeated runs (the PCF-dependent tests are the ones
most exposed to spawn-order flakiness, checked specifically).

**Rejected alternative:** attach AM Policy Association Establishment to `CreateUEContext` anyway
(option 1 above). Rejected per explicit user choice -- see the AMF section above.

**Consequence:** SMF is now the second NF in this build (after AUSF) whose route handlers make a
real synchronous SBI client call to another NF for actual business logic, not just to NRF. AMF->PCF
wiring remains open; per this ADR, the real blocker is a NAS/N1 Registration trigger, which is a
substantially larger effort (NGAP stack, ADR-0016) than a wiring turn -- worth surfacing as its own
decision point (build NGAP now vs. keep deferring AMF-side policy) rather than assuming the next
turn is automatically "wire AMF too."

---

