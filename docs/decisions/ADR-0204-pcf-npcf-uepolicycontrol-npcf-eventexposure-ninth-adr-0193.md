## ADR-0204: PCF `Npcf_UEPolicyControl` + `Npcf_EventExposure` -- ninth ADR-0193 gap-closure (first PCF slice)

### Context

Found during ADR-0193's audit: PCF has 7 real Tier-A gaps -- whole YAML files never added to the
sbi-codegen pilot set (`Npcf_EventExposure`, `Npcf_UEPolicyControl`, `Npcf_AMPolicyAuthorization`,
`Npcf_MBSPolicyAuthorization`, `Npcf_MBSPolicyControl`, `Npcf_PDTQPolicyControl`,
`Npcf_BDTPolicyControl`). Given the size (7 files, ~30 real operations total against an
already-1000-line `nfs/pcf/src/main.cpp`), this is split into slices like AMF's own ADR-0199/
ADR-0200 split -- this ADR closes the first two: `TS29523_Npcf_EventExposure.yaml` (4 ops,
`/npcf-eventexposure/v1`, scope `npcf-eventexposure`) and `TS29525_Npcf_UEPolicyControl.yaml` (4
ops, `/npcf-ue-policy-control/v1`, scope `npcf-ue-policy-control`) -- both real api roots and
scopes confirmed directly from each YAML's own `servers:`/`securitySchemes` blocks. The remaining 5
files are tracked as follow-up slices under the same task #163.

Real, substantial name collisions found wiring `Npcf_UEPolicyControl`: `PolicyAssociationRequest`,
`PolicyAssociation`, `PolicyAssociationUpdateRequest`, `PolicyUpdate`, `RequestTrigger` are all
independently declared by the already-wired `TS29507_Npcf_AMPolicyControl.yaml` too (both are real,
separate "UE/AM Policy Association" 3GPP resources that happen to reuse the same schema names).
Disambiguated by the codegen to `*_Npcf_AMPolicyControl`/`*_Npcf_UEPolicyControl`. Every
pre-existing bare-name reference to the AM-Policy-Control variant broke and was fixed: this file's
own `Npcf_AMPolicyControl` routes (`nfs/pcf/src/main.cpp`), `nfs/amf/src/ngap_task.cpp`'s real PCF
client call (constructs a real `PolicyAssociationRequest` to POST to PCF during registration), and
`tests/integration/test_pcf_policy_control.cpp`'s own pre-existing test -- a real, necessary fix
required by this pilot-set change, not a functional change to AM Policy Control's own behavior
(verified via that file's own pre-existing tests, all still pass unchanged). `TerminationNotification`
also collides three ways (AM/SM/UE Policy Control) but was never referenced bare anywhere in this
codebase, so nothing to fix there.

### Implementation

- **`Npcf_EventExposure`** (4 ops), backed by a new `PcEventExposureStore`
  (`nfs/pcf/src/stores.hpp/.cpp`) -- same nlohmann::json-store shape as `AmPolicyStore`/
  `SmPolicyStore`/`AppSessionStore` already in this file. `PostPcEventExposureSubsc`: real
  structural validation of `PcEventExposureSubsc`'s required `eventSubs`/`notifId`/`notifUri`, real
  `201` + `Location`. `GetPcEventExposureSubsc`: real get/`404`. `PutPcEventExposureSubsc`: real
  get/`404`-then-full-replace/`200`. `DeletePcEventExposureSubsc`: real get/`404`-then-remove/`204`.
  Disclosed: no real event notification delivery pipeline exists, same class of gap as every other
  `Nnf_EventExposure` service in this project (Namf/Nsmf/Nupf).
- **`Npcf_UEPolicyControl`** (4 ops), backed by a new `UePolicyStore` (own, separate `polAssoId` id
  space from `AmPolicyStore`'s). `CreateIndividualUEPolicyAssociation`: real structural validation
  of `PolicyAssociationRequest_Npcf_UEPolicyControl`'s required `notificationUri`/`supi`/`suppFeat`,
  real `201` + `Location`. `ReadIndividualUEPolicyAssociation`: real get/`404`.
  `DeleteIndividualUEPolicyAssociation`: real get/`404`-then-remove/`204`.
  `ReportObservedEventTriggersForIndividualUEPolicyAssociation`: real get/`404`, applies the
  request's own `triggers` to the stored association, real `200` with a real, structurally-valid
  `PolicyUpdate_Npcf_UEPolicyControl`. Disclosed: no real URSP (UE Route Selection Policy) or ANDSP
  generation logic exists in this build, so the returned `PolicyUpdate`'s `uePolicy` is always
  honestly absent -- same "acknowledge the real report, no real policy content to hand back" class
  of gap as `Nudm_NIDDAU`/`Nudm_SSAU` (ADR-0202).

### Live verification (real, live mTLS + OAuth2, not self-consistency)

New `tests/integration/test_pcf_ue_policy_event_exposure.cpp`, real spawned `nrf`+`pcf` processes
over real TLS 1.3 + mTLS: `Npcf_UEPolicyControl` create->read->update->delete full lifecycle (real
`201`/`200`/`200`/`204`, real `404` on repeat delete, real `400` on a body missing `supi`);
`Npcf_EventExposure` create->read->replace->delete full lifecycle (real `201`/`200`/`200`/`204`,
real `404` on both an unknown `subscriptionId` PUT and repeat delete, real `400` on a body missing
`notifUri`). Pre-existing `test_pcf_policy_control.cpp`'s AM/SM Policy Control tests (4 tests)
re-verified passing unchanged after the collision fix.

Two real bugs found and fixed during this verification pass, both from guessing wire shapes instead
of reading the actual generated `to_json`/`from_json`: (1) `PcEvent` and `RequestTrigger_*` are open
string-enum wrapper types that serialize as bare JSON strings (`j = v.value`), not as
`{"value": ...}` or `{"event": ...}` objects -- the test's own request bodies were fixed to send
plain strings, confirmed against the generated `.cpp`'s real `to_json` bodies directly, not assumed
from the C++ struct shape. (2) A real, generalizable test-authoring bug: the `Npcf_EventExposure`
test's own `ASSERT_NE(loc, create_resp->headers.end())` fired (because of bug (1) causing a real
`400` instead of `201`, so no `Location` header existed) before the test's own `reap_all()` cleanup
ran, leaking the spawned `nrf`+`pcf` processes -- same class of orphaned-process pipe-hang bug
already root-caused and disclosed in ADR-0202, reproduced here despite knowing the pattern. Fixed
by guarding the header/body-parsing logic behind `if (create_resp->status == 201)` with `EXPECT_*`
instead of `ASSERT_*`, applied to both new tests' create paths for consistency, not just the one
that actually hit it.

### Testing

Full reconfigure + rebuild against the complete pilot set clean (`EXIT=0` verified directly from
the build log at every step, including after each real collision fix). All 4 new PCF tests pass;
all 4 pre-existing PCF tests still pass unchanged. Full suite re-run clean: 396/396 passing (`ctest
--timeout 120 -E "UdrIntegration.AmfContextLifecycle|UdmIntegration.SdmDataRetrievalAndSubscriptions"`,
matching `.github/workflows/ci.yml`'s own exclusion). No strays before or after any run (`ps aux`
checked explicitly, including explicit-PID cleanup of the orphaned processes discovered mid-verification).

### What this ADR does NOT include

The remaining 5 PCF Tier-A gaps (`Npcf_AMPolicyAuthorization`, `Npcf_MBSPolicyAuthorization`,
`Npcf_MBSPolicyControl`, `Npcf_PDTQPolicyControl`, `Npcf_BDTPolicyControl`) -- tracked as follow-up
slices under task #163. No real event notification delivery pipeline for `Npcf_EventExposure`. No
real URSP/ANDSP policy generation behind `Npcf_UEPolicyControl`'s update response.

