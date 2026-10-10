## ADR-0276: NSACF -- Network Slice Admission Control, both API files, with real reporting and EAC

**Date:** 2026-09-05
**Status:** Accepted

### What was built

This project's fifth Tier 2 NF (after 5G-EIR ADR-0187, SMSF ADR-0188, GMLC ADR-0189, LMF
ADR-0191). Sources: `TS29536_Nnsacf_NSAC.yaml` and `TS29536_Nnsacf_SliceEventExposure.yaml`, commit
`bca84b60a37773133bcae97e5c6c0d10a93b47b6`. All **8** real operations across **both** API files --
counted by direct read, not estimated:

| Service | Operation |
|---|---|
| `Nnsacf_NSAC` | `NumOfUEsUpdate`, `NumOfPDUsUpdate`, `LocalNumberUpdate`, `QuotaUpdate` |
| `Nnsacf_SliceEventExposure` | `CreateSubscription`, `PartialModifySubscription`, `CompleteModifySubscription`, `DeleteSubscription` |

Plus **both callbacks the YAML defines**, which is where most of the real work is:
`eventReport` (POST `SACEventReport` to the subscription's `eventNotifyUri`) and `eacNotification`
(POST `EacNotification` to the `eacNotificationUri` a requesting NF supplies).

### Admission is real, and the test is built to fail if it is not

Counts are held as **sets of identities** -- SUPI for UEs, `(SUPI, pduSessionId)` for sessions --
not as integers. TS 29.536's operations are idempotent by design: AMF re-sends INCREASE for a UE it
already registered (re-registration, restart, retry), and a plain counter would climb until the
slice looked full while holding nothing.

`NsacfIntegration.SliceMaximumIsEnforcedAndReleasedCapacityIsReusable` admits 10 UEs to a 10-UE
slice, re-sends one (must not consume quota), asserts the 11th is rejected with the spec's own
`EXCEED_MAX_UE_NUM`, then DECREASEs one and asserts the freed unit is reusable. A stub answering
`204` fails at the 11th; a counter without identities fails after the re-send.

### A conformance bug this NF shipped with in draft, found by re-reading the YAML

`CreateSubscription` returned **the request body echoed back**. The schema requires
`CreatedSACEventSubscription` -- `{subscription, subscriptionId, report?}`. Every CRUD test passed
against the wrong shape, because a test that only checks "201 with a body" cannot see it. Fixed,
and the test now asserts the real shape and the `immediateFlag` report inside it.

Recorded rather than quietly corrected: this was found only because the three "disclosed
limitations" in the first draft were challenged and re-checked against the YAML instead of being
accepted as scope decisions.

### Reporting: what the spec requires, and what is now built

`SACEvent` carries `eventType` (`NUM_OF_REGD_UES` / `NUM_OF_ESTD_PDU_SESSIONS`), `eventTrigger`
(`THRESHOLD` / `PERIODIC`), `eventFilter` (S-NSSAI list), `notifThreshold` (`SACInfo`, absolute
**or** percentage), `notificationPeriod`, `immediateFlag`, and `maxReports`. All of those now
drive behaviour:

- **THRESHOLD** subscriptions are evaluated after every admission change, against both the
  absolute (`numericValNumUes` / `numericValNumPduSess`) and percentage
  (`percValueNumUes` / `percValueNumPduSess`) forms.
- **PERIODIC** subscriptions are driven by a 1-second scan thread.
- **immediateFlag** produces the report inside the creation response, where the schema puts it.
- **maxReports** decrements `SACEventState.remainReports` and flips `active` to false at zero --
  the spec's own lifecycle, not a local invention.
- **PATCH re-parses the merged subscription** rather than storing merged JSON, because a patch can
  change the threshold, period or watched slices and the reporting loop reads the record. Storing
  the body alone would leave reports running against the pre-patch subscription: a silent
  divergence, avoided deliberately.

`NsacfIntegration.ThresholdReportAndEacNotificationAreReallyDelivered` receives both callbacks on a
real in-process TLS server (the pattern ADR-0179 established for UDR's webhooks) and asserts the
report's `eventType`, `eventFilter.sst`, `notifyCorrelationId` echo, and the `sliceStautsInfo`
count that triggered it.

### EAC: what the spec requires, and the one thing it does not standardise

TS 23.501 §5.15.11.1 has NSACF switch the serving AMF into Early Admission Control as a slice
approaches its maximum. The NSAC YAML's `eacNotification` callback carries `EacNotification`:
a map of S-NSSAI-as-string to `EACMode` (`ACTIVE`/`DEACTIVE`). Both are now implemented, and a
notification is sent only when the mode **changes**, not on every admission.

**What the spec does not define is WHEN.** "Approaching the maximum" is not a schema field
anywhere. So the activation point is `eac_activation_percent` in `config/nsacf.json` (80 by
default), and it is called out here and in the source as **local policy, not a 3GPP value** --
the one place where an implementation choice was unavoidable, named rather than dressed up.

`eacNotificationUri` is carried per-request in `UeACRequestData`, so NSACF can only notify a
consumer that has supplied one. Until an NF does, EAC state is tracked and logged with nowhere to
send -- a property of the spec's own design.

### Still open

- **Not wired.** Neither AMF nor SMF calls this NF yet: TS 23.502 has AMF invoke the UE-count
  service during Registration and SMF the PDU-count service during PDU Session Establishment. Both
  procedures run end to end here (ADR-0267), so the wiring is reachable and is the next increment.
  By this project's own full-coverage rule a server with no consumer is not done.
- **Subscriptions are in-memory and process-local**, like every other event-exposure service here.
- **`notificationPeriod` is honoured to within a second**, the scan thread's cadence.
- **No Helm chart**, matching every Tier 2 NF; charts exist only for the original seven Tier 1 NFs.

---

