## ADR-0522: SMF releases the N4 session at the UPF on PDU session release; final usage to the CHF

**Date:** 2026-10-24. **Status:** Proposed.

**Context.** Found while doing SPEC-05 (ADR-0521): the SMF never sent a PFCP Session Deletion, so
every released PDU session stayed installed on the UPF. The UPF has handled Session Deletion since
ADR-0071. The user approved the fix on 2026-10-24 ("FIX IT", strictly per 3GPP). Procedure: TS
23.502 4.3.4.2 steps 2a-2b (N4 Session Release Request/Response) on the step 1c path
(Nsmf_PDUSession_ReleaseSMContext from the AMF), then step 3c; charging per TS 32.255 5.2.2.2.4
step 2ch-a.

**Decision.**
- `perform_n4_session_deletion`: PFCP Session Deletion Request (TS 29.244 7.5.6; header only,
  addressed by the UP F-SEID) to the session's UPF; reads the Cause and the final Usage Report
  (IE 79, UR-SEQN + Volume Measurement) from the response (7.5.7).
- ReleaseSMContext now does, in order: N4 Session Release (step 2, first so the final usage is
  known), Npcf_SMPolicyControl_Delete, Nchf_ConvergedCharging_Release, local removal. The CP-SEID
  session entry is removed too (it was never removed before).
- Charging Data Request [Termination] carries `triggers` = [{triggerType FINAL, triggerCategory
  IMMEDIATE_REPORT}] (TS 32.291: FINAL "on session level for normal release of session e.g., end
  of PDU session"; TS 32.255 table 5.2.1.4.1: immediate) and, when the UPF reported one, the final
  usage as `multipleUnitUsage` (rating group = the session's one rating group, localSequenceNumber =
  UR-SEQN, totalVolume). The CHF already debits from Release's usedUnitContainer (ADR-0297).
- A failed or unanswered N4 release is logged and the release continues; the AMF gets 204.

**Rejected alternatives.**
- *Refusing ReleaseSMContext when the UPF does not answer.* TS 23.502 3c answers the AMF; the UE
  and AMF contexts go regardless, and a refused release would leave the SMF context too.
- *Charging Release before the N4 release (the old order).* TS 32.255 places 2ch-a after step 2;
  the final usage would be lost.

**Disclosed.**
- Only the step 1c trigger (ReleaseSMContext) releases the N4 session. The UE-requested (1a),
  PCF-initiated (1b) and SMF-initiated (1e) releases do not exist in this SMF yet; they are the next
  increment (N1 PDU SESSION RELEASE COMMAND via N1N2MessageTransfer, N2 resource release).
- Step 2's "releases the IP address / Prefix(es)": this SMF allocates no UE IP address at all (a
  long-standing disclosed gap), so there is nothing to release.
- One UPF per session (NOTE 5's multi-UPF ordering does not arise).
- No retry of an unanswered Session Deletion; the UPF may keep the session.
- Small Data Rate Control / APN Rate Control status are not supported.

**Verified (2026-10-24).**
- Built: `smf`, `integration_tests` (0 failures). A later comment-only edit (a table number) was
  not rebuilt -- the build was stopped for low host memory while CI ran.
- `N28SyEndToEnd.SpendingLimitStatusChangeReachesTheSmfThatOwnsTheSession` (real NRF, CHF, UDR,
  UPF, PCF, SMF) extended: ReleaseSMContext answers 204, `smf_n4_session_deletion_total` = 1
  (moves only on the real UPF's RequestAccepted) and `smf_chf_charging_data_release_total` = 1.
  **Not run locally** (CI shares this host and was running); CI is the first run.
