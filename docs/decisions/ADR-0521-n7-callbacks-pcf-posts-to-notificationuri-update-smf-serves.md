## ADR-0521: N7 callbacks: PCF posts to {notificationUri}/update, SMF serves /update and /terminate (SPEC-05)

**Date:** 2026-10-24. **Status:** Proposed.

**Context.** REVIEW-5ae0ef1 SPEC-05: the PCF posted `SmPolicyNotification` to the SMF's bare
`notificationUri`, and the SMF served only that bare path. TS 29.512's callbacks
(TS29512_Npcf_SMPolicyControl.yaml) are `{notificationUri}/update` (SmPolicyUpdateNotification,
TS 29.512 4.2.3.2) and `{notificationUri}/terminate` (SmPolicyControlTerminationRequestNotification,
4.2.3.3). So this PCF and SMF worked only with each other, and a PCF-requested termination could
not be delivered at all.

**Decision.**
- PCF: `notify_smf_of_policy_change` posts to `notificationUri + "/update"`.
- SMF: `.../sm-contexts/{ref}/pcf-notify/update` (the old handler, unchanged otherwise) and
  `.../pcf-notify/terminate` (body `TerminationNotification`). The bare `.../pcf-notify` is no
  longer served. An unknown context is 404 with `POLICY_CONTEXT_NOT_FOUND` (TS 29.512 table
  5.7.3-2) on both. The `notificationUri` the SMF sends is unchanged.
- `/terminate` answers 204, stores the request in the SM context (`pcfTerminationRequest`) and
  counts it (`smf_pcf_termination_requests_total`).

**Rejected alternatives.**
- *Serving both the bare path and the suffixed ones.* That keeps a non-spec endpoint alive; no
  caller in the tree needs it after the PCF change.
- *Deleting the SM policy on /terminate without releasing the PDU session.* TS 29.512 4.2.3.3
  pairs Npcf_SMPolicyControl_Delete with initiating PDU session release; a session left up with no
  policy association is a state the spec does not describe.

**Disclosed.**
- **The SMF does not act on a termination request yet.** TS 29.512 4.2.3.3 says the consumer
  "shall invoke the Npcf_SMPolicyControl_Delete ... and initiate the procedure to terminate the PDU
  session". That is the network-requested PDU session release, TS 23.502 4.3.4.2, which this SMF
  does not implement (nothing sends N1 PDU SESSION RELEASE COMMAND / N2 release). The SMF logs a
  warning and the session stays up. It needs its own procedure-list approval.
- The project's PCF never sends `/terminate` (nothing in it decides to end an SM policy).
- The SMF's ReleaseSMContext also sends no N4 Session Deletion (seen while reading for this ADR,
  not changed here).

**Update (ADR-0523, 2026-10-24).** The SMF now acts on `/terminate`: N4 release, charging
termination, PDU SESSION RELEASE COMMAND via N1N2MessageTransfer, and steps 11-12 on the UE's
RELEASE COMPLETE. The first two Disclosed items above are superseded.

**Verified (2026-10-24).**
- Built: `pcf`, `smf`, `integration_tests` (0 failures).
- `N28SyEndToEnd.SpendingLimitStatusChangeReachesTheSmfThatOwnsTheSession` (real NRF, CHF, UDR,
  UPF, PCF, SMF) extended: its existing update counters can only move if the PCF posts to
  `/update` (the bare path now 404s, asserted); `/terminate` answers 204 and moves
  `smf_pcf_termination_requests_total`; an unknown context answers 404 POLICY_CONTEXT_NOT_FOUND.
  `SmfAuthorization` notification check moved to `/pcf-notify/update`. **Not run locally** (CI
  shares this host and was running); CI is the first run.
