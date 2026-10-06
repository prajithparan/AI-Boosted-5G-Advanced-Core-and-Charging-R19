## ADR-0207: NEF `Nnef_SMService` + `Nnef_UEId` + `Nnef_DNAIMapping` + `Nnef_EASDeployment` -- twelfth ADR-0193 gap-closure (first NEF slice)

### Context

First of NEF's own Tier-A gap-closure slices (task #164, ADR-0185's original turn wired only
`Nnef_PFDmanagement`, leaving all 13 other real NEF YAML files unbuilt, a real, disclosed deferral
at the time). Given the size (13 files, ~45 operations total across the remainder), this closes 4
of the smallest/best-defined files as the first slice: `TS29541_Nnef_SMService.yaml` (1 op,
`/nnef-smservice/v1`), `TS29591_Nnef_UEId.yaml` (2 ops, `/nnef-ueid/v1`),
`TS29591_Nnef_DNAIMapping.yaml` (3 ops, `/nnef-dnai-mapping/v1`), and
`TS29591_Nnef_EASDeployment.yaml` (3 ops, `/nnef-eas-deployment/v1`) -- 9 real operations, all api
roots confirmed directly from each YAML's own `servers:` block. 9 of NEF's remaining 13 real
Tier-A gaps close after this slice (9 remain: `Nnef_SMContext`, `Nnef_ECSAddress`,
`Nnef_EventExposure`, `Nnef_Inference`, `Nnef_TrafficInfluenceData`, `Nnef_Training`,
`Nnef_VFLInference`, `Nnef_VFLTraining`, `Nnef_Authentication`).

Real, non-fabricated spec quirk found and preserved, not "fixed": `TS29541_Nnef_SMService.yaml`'s
own `securitySchemes` block declares its real OAuth2 scope as `nnef-smcontext`, even though its own
api root is `/nnef-smservice/v1` -- a genuine mismatch in the vendored 3GPP spec text itself
(confirmed by direct read, not assumed), used verbatim (`nnef-smcontext`) rather than "corrected"
to what would seem more consistent, since inventing a different scope name would itself be
fabrication.

Real codegen limitation confirmed and worked around (a new finding, not previously seen in this
project's own prior 11 gap-closure ADRs): `TS29591_Nnef_DNAIMapping.yaml` itself owns zero schemas
of its own -- both its request and response types are entirely `$ref`'d from a separate, real,
already-vendored `TS29522_DNAIMapping.yaml`. `tools/sbi-codegen` only emits a real output file for
a pilot-set YAML that owns at least one schema; a pilot entry whose paths only reference external
schemas produces no usable named C++ type. The prior "only the operation's own entry-point file
needs adding to the pilot set" rule (confirmed by reading `loader.py`, re-confirmed in ADR-0201/
ADR-0202) holds when the referenced schemas live in a common-data grouping file already folded
into `TS29122_CommonData_grp.hpp` -- it does NOT hold for a distinct, separately-3GPP-numbered
schema file like `TS29522_DNAIMapping.yaml` that has never been wired before. Fixed by adding
`TS29522_DNAIMapping.yaml` as its own, additional, real pilot-set entry.

### Implementation

- **`Nnef_SMService`**: `SendSMS`, real `multipart/related` handling reusing the already-wired
  `SmsData`/`SmsDeliveryData` types from `TS29577_Nipsmgw_SMService.yaml` -- same real pattern
  `nfs/smsf`'s own `SendMtSMS` already established (ADR-0188), disclosed the same way: no real
  TS 24.011 SMS-DELIVER-REPORT encoder exists in this build (the binary delivery-report part is an
  honest empty placeholder), and this NEF has no real onward IP-SM-GW/SMSF relay wired -- the real
  ack is NEF-level acceptance only.
- **`Nnef_UEId`**: `FetchUEId`/`UEIDMappingInfoRetrieval`, real structural request parsing then a
  real `204` ("does not exist") for both -- this NEF has no real roaming H-NEF/internal-UE-identity
  mapping database to look anything up in, matching the real YAML's own documented "No Content"
  semantics for this case rather than fabricating a match.
- **`Nnef_DNAIMapping`**, backed by a new `DnaiMapSubStore` (`nfs/nef/src/stores.hpp/.cpp`) -- same
  real create/get/remove shape as `PfdSubscriptionStore` above, deliberately re-implemented as its
  own class. `CreateIndividualSubcription`: real structural validation of `DnaiMapSub`'s required
  `notifUri`/`notifCorrId`, real `201` + `Location`. `GetIndividualSubcription`: real get/`404`.
  `DeleteIndividualSubcription`: real get/`404`-then-remove/`204`.
- **`Nnef_EASDeployment`**, backed by a new `EasDeploySubStore`. Same real create/get/delete shape:
  `CreateIndividualSubcription` validates `EasDeploySubData`'s required
  `eventId`/`notifId`/`notifUri`, real `201`/`200`/`204`.

### Live verification (real, live mTLS + OAuth2, not self-consistency)

New `tests/integration/test_nef_sms_ueid_dnaimapping_easdeployment.cpp` -- NEF's first-ever live
integration test (its own prior turn, ADR-0185, only had DTO conformance tests, never a live
spawned-process test; `NEF_PATH` newly added to `tests/integration/CMakeLists.txt`). Real spawned
`nrf`+`nef` processes over real TLS 1.3 + mTLS: `SendSMS` real `200` with a verified real two-part
`multipart/related` request/response round-trip (binary + JSON parts, `Content-Id` matching);
`FetchUEId`/`UEIDMappingInfoRetrieval` both real `204`; `Nnef_DNAIMapping` create->read->delete
full lifecycle -- real `404` on repeat delete, real `400` on a body missing `notifCorrId`;
`Nnef_EASDeployment` create->read->delete full lifecycle -- real `404` on repeat delete, real `400`
on a body missing `notifId`. All 6 new tests pass.

### Testing

Full reconfigure + rebuild against the complete pilot set clean (`EXIT=0` verified directly from
the build log at every step, including after the real `TS29522_DNAIMapping.yaml` pilot-set fix).
All 6 new NEF tests pass; full suite re-run clean: 412/412 passing (`ctest --timeout 120 -E
"UdrIntegration.AmfContextLifecycle|UdmIntegration.SdmDataRetrievalAndSubscriptions"`, matching
`.github/workflows/ci.yml`'s own exclusion). No strays before or after any run (`ps aux` checked
explicitly).

### What this ADR does NOT include

The remaining 9 NEF Tier-A gaps (`Nnef_SMContext`, `Nnef_ECSAddress`, `Nnef_EventExposure`,
`Nnef_Inference`, `Nnef_TrafficInfluenceData`, `Nnef_Training`, `Nnef_VFLInference`,
`Nnef_VFLTraining`, `Nnef_Authentication`) -- tracked as follow-up slices under task #164. No real
roaming H-NEF UE-identity mapping database (`Nnef_UEId`'s own disclosed consequence). No real
SMS-DELIVER-REPORT encoding or onward SMS relay (`Nnef_SMService`'s own disclosed consequence,
same class as `nfs/smsf`'s pre-existing one). No live verification added for NEF's own
pre-existing `Nnef_PFDmanagement` routes (ADR-0185) -- a real, pre-existing gap (conformance-tested
only, never live-verified), disclosed here but out of this ADR's own scope, not newly introduced by
it.

