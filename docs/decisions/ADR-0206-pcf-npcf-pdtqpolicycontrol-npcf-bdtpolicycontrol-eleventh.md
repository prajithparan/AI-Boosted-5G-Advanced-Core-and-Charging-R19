## ADR-0206: PCF `Npcf_PDTQPolicyControl` + `Npcf_BDTPolicyControl` -- eleventh ADR-0193 gap-closure (third and final PCF slice)

### Context

Third and final PCF Tier-A gap-closure slice (task #163, ADR-0204 and ADR-0205 were the first two)
-- closes `TS29543_Npcf_PDTQPolicyControl.yaml` (4 ops, `/npcf-pdtq-policy-control/v1`, scope
`npcf-pdtq-policy-control`) and `TS29554_Npcf_BDTPolicyControl.yaml` (4 ops,
`/npcf-bdtpolicycontrol/v1`, scope `npcf-bdtpolicycontrol`) -- both real api roots and scopes
confirmed directly from each YAML's own `servers:`/`securitySchemes` blocks. This closes all 7 of
PCF's own real Tier-A gaps found by ADR-0193's audit; task #163 is now fully closed. No new name
collisions found wiring either file (confirmed by a clean reconfigure+rebuild with real `EXIT=0`
before any route code was written).

### Implementation

- **`Npcf_PDTQPolicyControl`** (4 ops), backed by a new `PdtqPolicyStore`
  (`nfs/pcf/src/stores.hpp/.cpp`) -- same `nlohmann::json`-store shape as this file's other stores.
  `CreatePDTQPolicy`: real structural validation of `PdtqPolicyData`'s required
  `aspId`/`desTimeInts`/`numOfUes`, real `201` + `Location`. `GetIndPDTQPolicy`: real get/`404`.
  `ModifyIndPDTQPolicy`: real merge-patch application of `PdtqPolicyPatchData`'s optional fields
  (`notifUri`/`selPdtqPolicyId`/`warnNotifReq`) onto the stored policy, real `200`.
  `DeleteIndPDTQPolicy`: real get/`404`-then-remove/`204`.
- **`Npcf_BDTPolicyControl`** (4 ops), backed by a new `BdtPolicyStore`. `CreateBDTPolicy`: real
  structural validation of `BdtReqData`'s required `aspId`/`desTimeInt`/`numOfUes`/`volPerUe`,
  wrapped into a real `BdtPolicy{bdtReqData}`, real `201` + `Location`. Disclosed: no real BDT
  decision engine exists in this build, so the created resource's own `bdtPolData` (the set of
  transfer policies the PCF offers) is honestly absent -- `BdtPolicyData` itself requires a real
  `bdtRefId` and at least one real `TransferPolicy`, neither of which this build can produce, so
  omitting it (a real, optional field on `BdtPolicy`) is the honest choice over fabricating one.
  `GetBDTPolicy`: real get/`404`. `UpdateBDTPolicy`: real structural validation of
  `PatchBdtPolicy`; if the request tries to select a transfer policy (`bdtPolData.selTransPolicyId`
  present), a real, disclosed `400` -- there is nothing valid to select since none were ever
  offered; otherwise real merge-patch application of `bdtReqData`'s optional fields
  (`warnNotifReq`/`energyInd`/`notifUri`) onto the stored resource, real `200`.
  `DeleteBDTPolicy`: real get/`404`-then-remove/`204`.

### Live verification (real, live mTLS + OAuth2, not self-consistency)

New `tests/integration/test_pcf_pdtq_bdt_policy_control.cpp`, real spawned `nrf`+`pcf` processes
over real TLS 1.3 + mTLS: `Npcf_PDTQPolicyControl` create->read->update->delete full lifecycle --
real `404` on repeat delete, real `400` on a body missing `numOfUes`; `Npcf_BDTPolicyControl`
create->read->update->delete full lifecycle -- real `201` with a verified-absent `bdtPolData`, real
`200` on a real `bdtReqData` merge-patch field, real `400` on an attempted transfer-policy
selection, real `404` on repeat delete, real `400` on a body missing `volPerUe`. All 4 new tests
pass; all 10 pre-existing PCF tests (across `test_pcf_policy_control.cpp`,
`test_pcf_ue_policy_event_exposure.cpp`, and `test_pcf_am_mbs_policy_auth_control.cpp`)
re-verified passing unchanged.

### Testing

Full reconfigure + rebuild against the complete pilot set clean (`EXIT=0` verified directly from
the build log at every step). All 4 new PCF tests pass; full suite re-run clean: 406/406 passing
(`ctest --timeout 120 -E
"UdrIntegration.AmfContextLifecycle|UdmIntegration.SdmDataRetrievalAndSubscriptions"`, matching
`.github/workflows/ci.yml`'s own exclusion). No strays before or after any run (`ps aux` checked
explicitly).

### What this ADR does NOT include

No real BDT decision engine anywhere in this build (`Npcf_BDTPolicyControl`'s own real,
disclosed consequence: no `bdtPolData` is ever offered, and transfer-policy selection is always
rejected). No real event notification delivery for either `Npcf_PDTQPolicyControl`'s or
`Npcf_BDTPolicyControl`'s own callback shapes (`PDTQNotification`/`BdtNotification`), same
disclosed class of gap as every other proactive/callback flow in this project. Task #163 (all 7
PCF Tier-A gaps) is now fully closed; no further PCF slices remain.

