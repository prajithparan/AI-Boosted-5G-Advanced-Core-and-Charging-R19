## ADR-0519: NEF AsSessionWithQoS: media component and PCF event subscription (SPEC-01, increment 1)

**Date:** 2026-10-23. **Status:** Proposed.

**Context.** REVIEW-5ae0ef1 SPEC-01 found the NEF's QoS notification to the AF in an invented
shape (`_npcfEventsNotification`). Reading the code behind it: the NEF's
`Npcf_PolicyAuthorization_Create` carried only `notifUri`, `suppFeat` (the AF's TS 29.122 feature
bits, a different list) and `dnn` -- no UE address, no media component, no `evSubsc` -- so the PCF
had no flows to authorise and no events to report, and an AF update (PUT/PATCH) never reached the
PCF. The user chose the full chain (2026-10-23) and approved this procedure list:
- increment 1 (this ADR): TS 23.502 4.15.6.6 steps 3 and 6, 4.15.6.6a steps 1-4;
- increment 2: steps 7-8 (PCF `EventsNotification` -> TS 29.122 `UserPlaneNotificationData`),
  SESSION_TERMINATION, removing `_npcfEventsNotification`, and the DOC-02 ADR text.
Out of scope: BSF discovery of the PCF (step 3; a configured PCF URL, as before), the TSCTSF
branch (3a-7b), alternative QoS and the R19 extended-QoS members.

**Decision:** `nfs/nef/src/qos_mapping.{hpp,cpp}`, used by create, PUT and PATCH.
- **Create (step 3).** `ueIpv4Addr`/`ueIpv6Addr`/`macAddr` -> `ueIpv4`/`ueIpv6`/`ueMac`;
  `dnn`; `snssai` -> `sliceInfo`; `exterAppId` -> `afAppId`; `suppFeat` "0" (the NEF negotiates
  no optional Npcf_PolicyAuthorization feature). One media component, `medCompN` 1, carrying
  `qosReference` and one media subcomponent per `flowInfo` entry: `fNum` = `flowId`, `fDescs` =
  `flowDescriptions`. Both sides encode flow descriptions per TS 29.214 5.3.8, and the TS 29.122
  flow id is what the AF's later reports name (`UserPlaneEventReport.flowIds`), so it is kept as
  the flow number. One media component is this NEF's choice: TS 29.122's request has no media
  component structure to map from.
- **Events (step 6, TS 29.522 4.4.9.2).** Always `SUCCESSFUL_RESOURCES_ALLOCATION` and
  `FAILED_RESOURCES_ALLOCATION` (step 6 "shall subscribe to notifications of Resource allocation
  status"). From the AF's `events`: `QOS_GUARANTEED`, `QOS_NOT_GUARANTEED`,
  `QOS_NOT_GUARANTEED_DL`/`_UL` -> `QOS_NOTIF`; `USAGE_REPORT`, `ACCESS_TYPE_CHANGE`, `PLMN_CHG`
  by name. `USAGE_REPORT` also when the AF gives `usageThreshold` (TS 29.122 4.4.13), which goes
  to `evSubsc.usgThres` (the same TS 29.122 type on both sides). `SESSION_TERMINATION` needs no
  subscription. Any other AF event is logged as not subscribed.
- **Update (4.15.6.6a).** PUT and PATCH send `Npcf_PolicyAuthorization_Update`: an
  `application/merge-patch+json` `AppSessionContextUpdateDataPatch` whose `ascReqData` is the
  RFC 7396 patch from the PCF state the old subscription asked for to the state the new one asks
  for (`merge_patch_between`), so a removed flow or QoS reference is `null` at the PCF. Nothing is
  sent when nothing changed. Sessions held by the TSCTSF (`tsctsf:` ids) are not updated (out of
  scope, as before).

**Rejected alternatives.**
- *One media component per flow.* TS 29.122 gives one `qosReference` for the whole request;
  splitting flows into components would repeat it and change the flow numbering the AF sees.
- *Sending the whole new state on update.* Merge patch keeps members it is not told to remove, so
  a dropped flow would stay authorised at the PCF.
- *Rejecting the AF request when the PCF fails.* TS 29.122 4.4.13 says the NEF shall not create or
  update the resource then; the NEF has always accepted and logged (disclosed in ADR-0315), and
  the NEF LI end-to-end test runs with the PCF unreachable. Changing that is its own decision,
  kept out of this increment and disclosed below.

**Disclosed.**
- **The PCF does not yet emit events.** `nfs/pcf` stores `evSubsc` but never sends an
  `EventsNotification` (no `evNotifs` anywhere in it), so with the project's own PCF no QoS event
  reaches the NEF even after both increments. Increment 2 is tested against a PCF stand-in; the
  PCF's event reporting (driven by the SMF's resource reports) is a separate gap, raised with the
  user.
- **PCF failure is not propagated** to the AF on create or update (above).
- Ethernet flows (`ethFlowInfo`), `sponsorInfo`, alternative QoS, QoS monitoring events and the R19
  extended-QoS members are not mapped; a request with them is accepted and they are not sent.
- No UE address in the request (neither IP nor MAC, and not the GPSI/TSCTSF path) still reaches
  the PCF, which then lacks the binding the schema requires (`oneOf` ueIpv4/ueIpv6/ueMac).
- Single media component (above).

**Verified (2026-10-23).**
- `NefQosMapping.*` (tests/conformance/test_nef_qos_mapping.cpp, 5 tests): event plan, create
  request, update patch (applying it to the old state gives the new), no-change update. 5/5 pass.
- `validate_structural_conformance.py`, extended to check map members (`additionalProperties`)
  and array items: the create body against `AppSessionContextReqData` and the update body against
  `AppSessionContextUpdateDataPatch` pass. It first failed on the update -- `MediaComponentRm` and
  `MediaSubComponentRm` require `medCompN`/`fNum` even when unchanged -- fixed by
  `pcf_update_patch`.
- `NefDirectPcf.TheAfsFlowsQosReferenceAndEventsReachThePcfAndFollowAnUpdate`
  (tests/integration/test_tsctsf_e2e.cpp; real NRF, PCF and NEF): the PCF's app-session holds the
  UE address, `suppFeat` "0", the media component (QoS reference, flows 1 and 2) and the four
  events; after an AF PATCH it holds flow 1 only and the new QoS reference. Passes; against the NEF
  built from `sbi-cause` before this change it fails (no `ueIpv4` at the PCF).
- Built: `nef`, `conformance_tests`, `tsctsf_e2e_integration_tests` (no failures). The full ctest
  suite was not run.
