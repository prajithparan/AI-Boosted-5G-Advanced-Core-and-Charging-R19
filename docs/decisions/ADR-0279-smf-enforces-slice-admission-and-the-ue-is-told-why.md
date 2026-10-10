## ADR-0279: SMF enforces slice admission -- and the UE is told why

**Date:** 2026-09-05
**Status:** Accepted

### Why this needed more than moving a call

ADR-0278 enforced NSAC on the AMF side and named SMF's half as still open. That half was not
symmetric, for two structural reasons found by reading the code rather than assuming:

- **SMF's error responses used generic `ProblemDetails`**, a deliberate project-wide simplification
  its own file header records and that NRF and AMF share. `ProblemDetails` has nowhere to carry a
  NAS message; only TS 29.502's `SmContextCreateError` has `n1SmMsg`.
- **AMF discarded SMF's error bodies entirely** -- on a non-201 it logged and returned. Even a
  correct reject from SMF would have gone nowhere.

So a "refused" session would have left the UE waiting for a message that never came, which is worse
than the honest report-only behaviour it replaced. **User-directed decision:** adopt
`SmContextCreateError` for this one operation and teach AMF to relay it, rather than either
converting every NF's error model or shipping a refusal the UE never learns about.

### What was built

- **SMF** checks admission immediately after the mandatory-IE check and **before** the SM context,
  the PCF SM Policy Association and the N4 session -- a refused session leaves nothing to unwind.
  On refusal it answers **403** with `SmContextCreateError` as `multipart/related`: the
  `ExtProblemDetails` root part plus the real NAS reject as `n1SmMsg`.
- **`encode_establishment_reject`** (TS 24.501 §8.3.6): message type `0xC3`, 5GSM cause `0x45`,
  both read from `simulators/ransim/vendor/UERANSIM/src/lib/nas/enums.hpp` -- the same source this
  codec already cites for `0xC1`/`0xC2`. The PTI is echoed, as the Accept echoes it, because a UE
  matches the answer to its own request by that value.
- **AMF** parses the multipart error, extracts `n1SmMsg` by the contentId the root part names, and
  delivers it through `NgapUeRegistry::send_dl_nas_transport` -- the same path the Accept takes
  when SMF sends it via N1N2MessageTransfer. AMF does not decode the container; it carries the
  bytes, the same opaque-payload discipline as the success path.

**The exception to the generic-error convention is confined to this operation**, and is recorded
here and in the source. It is defensible precisely because this is the operation whose error body
carries UE-bound NAS content rather than diagnostics for an operator.

### A semantic error this surfaced in ADR-0278's own code

Enforcing broke `RegistrationAndPduSessionReachNsacf`, and the cause was a real defect, not test
fragility: **`SLICE_NOT_FOUND` was being treated as a refusal.**

TS 29.536's `AcuFailureReason` carries both `EXCEED_MAX_*` (quota exhausted) and `SLICE_NOT_FOUND`
(NSACF holds no admission configuration for that S-NSSAI). Only the first is a refusal. Treating
the second as one lets an NSACF that simply does not manage a slice block every session on it --
the same reasoning as the fail-open on an unreachable NSACF, applied to an answer rather than to a
failure to answer.

`acu_failure_is_a_real_refusal` now makes that distinction in **both** NFs, so ADR-0278's AMF path
is corrected too. The test that caught it creates sessions on `sst=1` with no SD, which is a
different S-NSSAI from the configured `sst=1/sd=000001` -- NSACF was answering correctly the whole
time.

### Test

`AmfNgapTestGnb.PduSessionIsRejectedWhenNsacfRefusesTheSlice` takes the slice's **PDU** capacity to
zero while leaving its UE capacity alone -- so the same slice admits the UE and refuses its session,
which is what makes this a test of SMF's half rather than AMF's. It asserts on the deciphered N1 SM
container: `0xC3` where a passing UE gets `0xC2`, cause `0x45`, PTI echoed, MAC-verified with the
UE's own KNASint. 17/17 local tests pass, including the regression above.

### The pair is now symmetric

Both admission points enforce, both fail open on an NSACF that cannot answer, both refuse only on a
real quota exhaustion, and in both cases the UE is told why in a real NAS message it can verify.

---

