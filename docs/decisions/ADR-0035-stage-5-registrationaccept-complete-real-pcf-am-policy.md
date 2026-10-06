## ADR-0035: Stage 5 (RegistrationAccept/Complete -> real PCF AM Policy Association call) -- the goal this whole effort was for

**Date:** 2026-08-07
**Status:** Accepted

**Context:** Stage 5 closes the loop ADR-0025 -> ADR-0029 left open: AMF sends `RegistrationAccept`
(integrity-protected **and** ciphered -- the normal secured-message case, not
`SecurityModeCommand`'s "new security context" variant, since the NAS security context is now
established), receives `RegistrationComplete`, and makes the real call this entire staged
NGAP/NAS effort existed to produce: `Npcf_AMPolicyControl`'s `CreateIndividualAMPolicyAssociation`
(TS 29.507) to real PCF.

**New code:** `amf::nas::encode_registration_accept`/`decode_registration_complete`
(`nfs/amf/src/nas_codec.{hpp,cpp}`), built on ADR-0034's `encode_secured_downlink`/
`decode_secured_uplink` refactor. Only the one mandatory IE (`registrationResult`, fixed to
THREEGPP_ACCESS) is sent -- GUTI reassignment and other optional IEs are a disclosed
simplification, moot given this project's single-registration-per-association scope (ADR-0031).
`handle_uplink_nas_transport_registration_complete` (`ngap_task.cpp`) calls PCF using the same
client-call template SMF->PCF already established (ADR-0029), storing the resulting
`PolicyAssociation` in `nfs/amf/src/ue_context_store.hpp` keyed by SUPI -- the first thing that
ever populates that store for real (see its own long-standing disclosed-gap comment).

**Real interop blocked at the same SQN point as Stages 3/4 (expected, not re-investigated) --
verified the PCF call directly instead, and it caught a real bug.** Obtained a genuine OAuth2
token from NRF via `curl` using AMF's own mTLS client cert, then POSTed the *exact* JSON body
`handle_uplink_nas_transport_registration_complete` builds directly to real PCF's
`/npcf-am-policy-control/v1/policies`. First attempt: **real HTTP 400**,
`"key 'suppFeat' not found"` -- `PolicyAssociationRequest.suppFeat` (TS 29.571 `SupportedFeatures`,
a hex-encoded optional-feature bitmask) is mandatory in the generated schema and had been omitted.
Fixed (`preq.suppFeat = ""`, meaning "none of PCF's optional features requested," the correct value
given none are implemented), re-sent the identical request: **real HTTP 201**, a genuine
`PolicyAssociation` body back. This is a bug the generated-schema type system didn't catch at
compile time (`suppFeat` is a plain, default-constructed `std::string`, not `std::optional`, so it
serializes as `""` either way -- the omission was leaving the field *unset in the request builder's
intent*, not a type error) and unit tests alone would not have caught either, since nothing in this
codebase independently re-validates PCF's actual mandatory-field schema -- only a real request
against a real server does.

**Also found during this stage's verification, unrelated to any of the code above:** leftover
`nrf`/`udm`/`ausf`/`pcf`/`amf` processes manually started earlier in this session (for live
`nr-gnb`/`nr-ue` interop testing) were still running and squatting the same fixed ports
`tests/integration/*.cpp`'s own `spawn_all()`-style helpers bind to, causing 2-3 tests
(`AusfIntegration.*`, `SmfIntegration.CreateSMContextFailsClosedWhenPcfUnreachable`) to fail
intermittently across Stages 3-5's own regression runs -- previously misdiagnosed as "flaky
parallel port contention." Killing every manually-started process before the next `ctest` run
produced a clean 80/80 pass in 15s (down from ~60s with retries/timeouts). Documented as a
recorded lesson (not a code change) so future manual-verification sessions clean up before the
final regression pass.

**Consequence:** Stage 5 complete, 80 tests total. The full staged NGAP/NAS Registration procedure
(NG Setup -> InitialUEMessage/RegistrationRequest -> AuthenticationRequest/Response ->
SecurityModeCommand/Complete -> RegistrationAccept/Complete -> real PCF AM Policy Association) is
now implemented end-to-end in code, verified stage-by-stage via whichever proof (real `nr-gnb`/
`nr-ue` interop, an independent cross-check harness, or a direct real-service HTTP call) was
actually reachable at each point -- never assumed correct from code review alone. The one
structural gap every stage from 3 onward shares -- SQN resynchronization -- remains explicitly
out of scope and is the reason a single, fully-automated real `nr-ue` end-to-end registration
cannot be demonstrated in one run; `docs/TRACEABILITY.md` records this plainly rather than
implying full automated coverage exists.

