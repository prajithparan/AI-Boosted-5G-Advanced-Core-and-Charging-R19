## ADR-0523: UE- and PCF-requested PDU session release (TS 23.502 4.3.4.2, N1 path)

**Date:** 2026-10-24. **Status:** Proposed.

**Context.** The user approved the network-requested PDU session release on 2026-10-24 ("strictly
per 3GPP"). Before this, the SMF had no release procedure apart from ReleaseSMContext (trigger 1c,
ADR-0522), the AMF treated every uplink 5GSM message as a new establishment, and a PCF termination
request (ADR-0521) was only recorded. Procedures: TS 23.502 4.3.4.2 triggers 1a (UE-requested) and
1b (PCF-initiated), steps 2, 3a/3b, 4, 8-10, 11, 12; TS 24.501 6.3.3 / 6.4.3 (5GSM), 5.4.5.2.2 (AMF
routing); TS 29.502 (UpdateSMContext, SMContextStatusNotify); TS 29.518 5.2.2.3.1 (N1N2MessageTransfer).

**Decision.**
- **SMF 5GSM codec**: PDU SESSION RELEASE COMMAND (TS 24.501 8.3.14: header + mandatory 5GSM cause)
  and a header decoder (EPD, PSI, PTI, message type; optional 5GSM cause). Message types from TS
  24.501 table 9.7.2: REQUEST 0xD1, COMMAND 0xD3, COMPLETE 0xD4.
- **Shared release steps** (`ReleaseDeps`, used by 1a, 1b and 1c): step 2 + 2ch (ADR-0522), step 11
  (SmContextStatusNotification `statusInfo.resourceStatus` RELEASED to the stored
  `smContextStatusUri`), step 12 (Npcf_SMPolicyControl_Delete), local removal with the LI release
  record. The N4/charging identifiers are erased once released, so a second trigger cannot repeat
  them.
- **1a**: UpdateSMContext with an `n1SmMsg` PDU SESSION RELEASE REQUEST -> step 2, then 3a: the 200
  response carries the PDU SESSION RELEASE COMMAND (multipart, `n1SmMsg`) with the request's PTI
  and 5GSM cause #36 "regular deactivation" (TS 24.501 6.3.3.2; 6.4.3.3 has the SMF run the
  network-requested procedure).
- **1b**: `/pcf-notify/terminate` -> step 2, then 3b: N1N2MessageTransfer with the COMMAND (PTI 0,
  "no procedure transaction identity assigned") and `skipInd` true (TS 24.501 6.3.3.2: allowed, the
  cause is not #39 and no Access type IE is sent). If the AMF answers anything but
  N1_N2_TRANSFER_INITIATED the UE is unreachable and the SMF goes to step 11 (step 3, first
  paragraph).
- **10a**: UpdateSMContext with PDU SESSION RELEASE COMPLETE for a release under way -> 204 (10b),
  steps 11, 12, removal.
- **No N2 SM Resource Release request** in 3a/3b: TS 23.502 step 3 says it "shall not" be included
  when the UP connection is not activated, and this AMF never sends NGAP PDU Session Resource Setup
  outside handover, so no session's UP connection is active at the RAN. AMF step 4 is therefore the
  "transmits the NAS message" branch (steps 6-7 skipped).
- **AMF**: in Phase::Done an UL NAS TRANSPORT with N1 SM information, no Request type and a PDU
  session ID the UE holds goes to that session's SMF as UpdateSMContext (`n1SmMsg`, TS 24.501
  5.4.5.2.2); an `n1SmMsg` in the answer goes to the UE as DL NAS TRANSPORT. The Request type IE is
  now decoded. `smContextStatusUri` is per PDU session
  (`/namf-callback/v1/sm-context-status/{supi}/{pduSessionId}`, served, no OAuth2), because
  SmContextStatusNotification names no SM context; RELEASED drops the session's SM context ref. For
  a UE with no NGAP association and `skipInd` true, N1N2MessageTransfer answers 200
  N1_MSG_NOT_TRANSFERRED (TS 29.518 5.2.2.3.1.2 case A a).
- **5GSM cause for a PCF-requested release**: no specification maps TS 29.512
  `SmPolicyAssociationReleaseCause` to a TS 24.501 5GSM cause. INSUFFICIENT_RES -> #26
  "insufficient resources", every other value -> #36 "regular deactivation". Recorded as a
  specification gap in README.md (user rule, 2026-10-24).

**Rejected alternatives.**
- *Including the N2 SM Resource Release request anyway.* Step 3 forbids it for a session whose UP
  connection is not active; and nothing at the RAN holds resources to release.
- *Removing the SMF context as soon as the COMMAND is sent.* Step 11 waits for the UE's reply;
  the UE's RELEASE COMPLETE would then find no context.
- *One shared smContextStatusUri.* The AMF could not tell which session the notification is about.

**Disclosed.**
- **T3592 is not run** (TS 24.501 6.3.3.2): a COMMAND the UE never answers is not retransmitted and
  the context stays "release pending" until the AMF releases it (ReleaseSMContext, e.g. at
  deregistration).
- No User Location Information in the AMF's UpdateSMContext (steps 1a/10a list it).
- No paging: without `skipInd` a CM-IDLE UE still gets 404 at N1N2MessageTransfer.
- Steps 13-15 not done: this SMF has no ULI subscribers, no Nudm_SDM subscription and no
  Nudm_UECM registration to undo (all pre-existing gaps).
- Triggers 1d (RAN), 1e (SMF-initiated) and 1f (AMF release indication) are not wired; 1e needs a
  local SMF policy to decide on.
- RELEASE REJECT (TS 24.501 6.4.3.4) is never sent: every UE request is accepted.
- The project's PCF still never sends a termination request; the test plays the PCF.

**Verified (2026-10-24).**
- Built: `smf`, `amf`, `integration_tests` (0 failures); `-fsyntax-only` clean on every changed TU
  first.
- `PduSessionRelease.UeRequestedReleaseCompletesThroughAmfAndSmf` and
  `PduSessionRelease.PcfRequestedReleaseReachesTheUeAndCompletes`
  (tests/integration/test_amf_deregistration.cpp; real NRF, UDR, UDM, AUSF, PCF, SMF, AMF and an
  NGAP test gNB/UE with real NAS security): the UE receives a PDU SESSION RELEASE COMMAND with the
  right PTI (2 / 0) and cause #36; after its RELEASE COMPLETE `smf_pdu_session_release_completed_total`
  is 1; in the UE-requested test a later deregistration makes no ReleaseSMContext call (the AMF
  dropped the session on step 11). **Not run locally** (CI shares this host and was running); CI is
  the first run.
