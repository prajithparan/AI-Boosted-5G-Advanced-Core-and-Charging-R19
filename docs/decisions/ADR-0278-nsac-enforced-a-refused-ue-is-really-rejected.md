## ADR-0278: NSAC enforced -- a refused UE is really rejected

**Date:** 2026-09-05
**Status:** Accepted

### What changed, and why placement was the whole point

ADR-0277 wired AMF to NSACF but called it **after** `RegistrationAccept`, and said so: it could
report a refusal but never act on one, because by then the UE had already been told it was
registered. TS 23.501 §5.15.11 makes NSAC a control on how many UEs may **be** registered, so the
decision has to precede the Accept.

The call now sits in the SecurityModeComplete handler, immediately before `RegistrationAccept` is
built. On refusal AMF sends a real `RegistrationReject` (TS 24.501 §8.2.8) with 5GMM cause **0x45**
(insufficient resources for slice) and persists nothing -- no security context, no PCF association
-- because a UE that was refused is not registered and leaving either behind would make it look
like it was.

Everything numeric here is read from the source this codec already cites for every other message
type and cause, `simulators/ransim/vendor/UERANSIM/src/lib/nas/enums.hpp`: `REGISTRATION_REJECT`
= `0b01000100` = 0x44, `EMmCause::INSUFFICIENT_RESOURCES_FOR_SLICE` = `0b01000101` = 0x45. Nothing
was assumed from memory.

`encode_registration_reject` is **secured**, unlike the existing `encode_service_reject_plain`.
That difference is real: this reject is only reachable after SecurityModeComplete has succeeded, so
a NAS security context exists and TS 24.501 §4.4.4.2 has the message protected with it. The plain
form exists for the case where there is no context to protect *with*.

### The fail-open, named rather than left implicit

`report_ue_to_nsacf` returns false **only** when NSACF genuinely refused the slice. NSACF
unreachable, no OAuth2 token, an unexpected status -- all admit the UE.

That is a deliberate choice with a reason: NSAC is an operator quota mechanism, not an
authentication step, and TS 23.501 §5.15.11 gives it no authority over a registration it could not
evaluate. A UE must not lose service because an admission controller failed to answer. It is
stated in the function's own comment as well as here, because a reader skimming the control flow
would otherwise see "returns true" on an error path and reasonably wonder whether it was an
oversight.

### The retry path, and what was deliberately not built

The rejected association's phase is **not** rewound. `UeAuthState::Phase` has no
"awaiting a fresh RegistrationRequest" value -- the machine starts at
`AwaitingAuthenticationResponse` -- and adding one would be a state-machine change dressed up as
an error path. A retrying UE arrives as a new `InitialUEMessage`, which gets a fresh `UeAuthState`
of its own, so the real retry path already works and the rejected association is simply finished.

### Test

`AmfNgapTestGnb.UeIsRejectedWhenNsacfRefusesItsSlice` takes the lab slice's UE capacity to zero
through NSACF's own `LocalNumberUpdate` -- the operation that exists for exactly this, rather than
registering 100 UEs to fill it -- then drives a real registration over NGAP/NAS and asserts on the
**deciphered** downlink: message type `0x44` where a passing UE gets `0x42`, and cause `0x45`. The
UE verifies the MAC with its own KNASint, so this also proves AMF protected the reject with the
context it had just established rather than sending it in the clear.

### Still open: SMF's half

SMF still **reports** its PDU-session admission after the 201 and does not refuse. Enforcing there
means moving the call inside `create_sm_context_inner`, ahead of the N4 session and the PCF SM
policy association, and failing the establishment back with 5GSM cause 0x45
(`INSUFFICIENT_RESOURCES_FOR_SPECIFIC_SLICE`, same file, same verification). That is a larger
restructuring of a long function than this increment, and it is named here rather than quietly
carried as "wired".

---

