## ADR-0520: NEF AsSessionWithQoS: PCF event and termination notifications to the AF (SPEC-01, increment 2)

**Date:** 2026-10-23. **Status:** Proposed.

**Context.** Increment 2 of the procedure list the user approved in ADR-0519: TS 23.502 4.15.6.6
steps 7-8 (PCF `Npcf_PolicyAuthorization_Notify` -> NEF -> AF), SESSION_TERMINATION, removing the
invented `_npcfEventsNotification` body (REVIEW-5ae0ef1 SPEC-01) and correcting the ADR text that
called it TS 29.122-shaped (DOC-02). Two further defects found while reading: the NEF never set
`evSubsc.notifUri`, and its callback route was the bare notifUri, while TS 29.514's callbacks are
`{evSubsc/notifUri}/notify` (eventNotification) and `{notifUri}/terminate` (terminationRequest) --
so even a PCF that sent events could not have reached the route.

**Decision.**
- **Callbacks.** The create sets `evSubsc.notifUri` to the same NEF URI as `notifUri`
  (`/nnef-callback/v1/qos-notify/{afId}/{subscriptionId}`). Routes `.../notify` (body
  `EventsNotification`, parsed with the generated type) and `.../terminate` (`TerminationInfo`);
  the bare route is removed. Both answer 204, 404 for a deleted AF subscription, no OAuth2
  (TS 33.501 13.4.1.0, ADR-0499).
- **To the AF.** TS 29.122 `UserPlaneNotificationData`: `transaction` = the AF's own resource URI,
  `eventReports` from `nef::af_reports_for` (TS 29.522 4.4.9.2.2):
  SUCCESSFUL_/FAILED_RESOURCES_ALLOCATION -> same event; QOS_NOTIF -> one report per `qncReports`
  entry, `notifType` GUARANTEED / NOT_GUARANTEED / NOT_GUARANTEED_DL / _UL -> QOS_GUARANTEED /
  QOS_NOT_GUARANTEED / _DL / _UL, `altSerReqNotSuppInd` -> `altQosNotSuppInd`; USAGE_REPORT ->
  `accumulatedUsage` from `usgRep`; ACCESS_TYPE_CHANGE -> `ratType`; PLMN_CHG -> `plmnId`.
  `flowIds` = the `fNums` of media component 1, which ADR-0519 made equal to the AF's flowIds;
  omitted when the PCF names none (TS 29.122: the event then applies to all flows). A termination
  request is one SESSION_TERMINATION report (TS 29.122 4.4.13).
- Any other PCF event is logged and not reported; a notification with no reportable event sends
  nothing to the AF.

**Rejected alternatives.**
- *Keeping the raw PCF body alongside the TS 29.122 one.* Not in the schema; the review finding
  is exactly that.
- *Deleting the PCF app-session (and/or the AF subscription) on a termination request.* TS 29.514
  4.2.5.3 lets the consumer delete or update ("may"); TS 29.122 does not say the NEF removes the
  AF's resource. Left to the AF's DELETE, which already deletes the PCF app-session.
- *`ACCESS_TYPE_CHANGE` carrying `accessType`.* `UserPlaneEventReport` has no such member; it has
  `ratType`, which is what is sent.

**Disclosed.**
- **The project's PCF still emits no events or termination requests** (ADR-0519), so end to end
  with the real PCF nothing reaches the AF yet. The integration test plays the PCF's callbacks at
  the URIs the real PCF holds.
- `appliedQosRef` is never set: TS 29.522 says "the currently applied QoS reference if received",
  and no PCF field naming it was identified short of the alternative-QoS `altSerReq`, which is out of
  scope (no alternative QoS is sent, ADR-0519).
- QoS monitoring, PDV, L4S, BAT offset, rate-limit and multi-modal reports are not mapped.
- The TSCTSF path (`tsctsf:` sessions) is unchanged: the TSCTSF has its own notification and is out
  of scope, as in ADR-0519.
- AF delivery failure is logged only; no retry.

**Verified (2026-10-24).**
- `NefQosMapping.*` (tests/conformance/test_nef_qos_mapping.cpp, now 8 tests): adds the
  `evSubsc.notifUri` check, the PCF-event -> AF-report mapping (flowIds, altQosNotSuppInd,
  accumulatedUsage, ratType, plmnId, an unmapped APP_DETECTION) and QOS_NOTIF without qncReports.
  8/8 pass; full `conformance_tests` 571/571.
- `validate_structural_conformance.py`: the AF body passes against `UserPlaneNotificationData`;
  the same body without `transaction` and with `_npcfEventsNotification` fails on both (checked).
- `NefDirectPcf.ThePcfsEventsAndTerminationReachTheAfAsUserPlaneNotifications`
  (tests/integration/test_tsctsf_e2e.cpp; real NRF, PCF, NEF, an in-test AF): built, **not run
  locally** (CI shares this host and was running); left to CI.
- Built: `nef`, `conformance_tests`, `tsctsf_e2e_integration_tests`, `integration_tests` (0
  failures). The full ctest suite was not run.
