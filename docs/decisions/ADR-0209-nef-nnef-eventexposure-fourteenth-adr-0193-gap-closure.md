## ADR-0209: NEF `Nnef_EventExposure` -- fourteenth ADR-0193 gap-closure (third NEF slice); real `tools/sbi-codegen` allOf-narrowing fix; real project-wide common-data group rename

### Context

Third of NEF's own Tier-A gap-closure slices (task #164): `TS29591_Nnef_EventExposure.yaml`
(4 ops: Create/Get/Replace(PUT)/Delete subscription, `/nnef-eventexposure/v1`, scope
`nnef-eventexposure`). 8 of NEF's remaining 13 real Tier-A gaps close after this slice (5 remain:
`Nnef_Inference`, `Nnef_TrafficInfluenceData`, `Nnef_Training`, `Nnef_VFLInference`,
`Nnef_VFLTraining`).

Its own `NefEventExposureSubsc.eventNotifs` field transitively `$ref`s `NefEventNotification`,
whose ~20 real optional array fields pull in schemas from two additional, real, previously-unwired
files -- `TS29517_Naf_EventExposure.yaml` and `TS26512_EventExposure.yaml` -- added as their own
pilot-set entries (same "distinct schema file needs its own entry" rule as ADR-0207).

**Real `tools/sbi-codegen` limitation found and fixed** (a new class, not previously seen):
configuring with `TS26512_EventExposure.yaml` in the pilot set hit generator's own existing
allOf-conflict safety check (`schema_to_ir.py`, ADR-0190) with a hard failure:
`QoEMetricsCollection: allOf members disagree on field 'records'`. Read the real source directly
(not guessed): `TS26512_EventExposure.yaml`'s own `BaseEventCollection` (an intentionally abstract
base type several concrete collection schemas `allOf`) declares `records: array, items: {}` --
deliberately unconstrained, "type of record is defined by concrete data type" per its own
description -- and each concrete subtype (`QoEMetricsCollection` and, by the same real pattern,
its siblings `ConsumptionReportingUnitsCollection`, `NetworkAssistanceInvocationsCollection`,
`DynamicPolicyInvocationsCollection`, `MediaStreamingAccessesCollection`) narrows it via its own
`allOf` member to a specific item type (e.g. `array of QoEMetricsEvent`). This is a real,
standard OpenAPI/JSON-Schema "abstract base narrowed by a concrete subtype" composition pattern,
not a genuine field conflict -- ADR-0190's existing check couldn't distinguish it from an actual
disagreement and correctly refused to guess. Fixed properly in the shared generator (not routed
around): `tools/sbi-codegen/sbi_codegen/schema_to_ir.py` gained a new `_is_opaque_type_ref` helper
(true for a TypeRef that resolves, through any array nesting, to the `nlohmann::json` placeholder
`_property_type_ref` already emits for an unconstrained `items: {}`) and the allOf-merge conflict
branch now checks: if `required`/`nullable` still agree and exactly one side of a disagreeing pair
is the opaque placeholder, keep the non-opaque (narrower) side instead of raising. A genuine
conflict (neither side opaque, or `required`/`nullable` differ) still raises exactly as before --
this narrows the existing safety net, it does not remove it. Verified directly: re-running
`generate.py` against the full pilot set produces `QoEMetricsCollection.records` as
`std::vector<QoEMetricsEvent>` (the correctly-narrowed real type), not `nlohmann::json`, and the
generator's own reported type/file counts moved from 2665/92 to 2762/94 with no new errors.

**Real, large, disclosed consequence of the above (found immediately on the next real build, not
overlooked)**: pulling `TS26512_EventExposure.yaml`/`TS29517_Naf_EventExposure.yaml` in bridged a
real cyclic `$ref` dependency into this project's own pre-existing giant common-data SCC group
(previously anchored, and named, at `TS29122_CommonData.yaml` -- confirmed by reading
`render.py`'s own group-naming rule, `group_name = sorted(file_stems)[0] + "_grp"`, fully
deterministic, not build-order-dependent). Because `TS26510_CommonData.yaml` (itself pulled in
transitively by `TS26512_EventExposure.yaml`) now sorts alphabetically before `TS29122_CommonData`,
the merged group's own generated filename changed project-wide: `TS29122_CommonData_grp.hpp` ->
`TS26510_CommonData_grp.hpp`. This broke the `#include` in every one of the 21 real source files
across `nfs/`/`tests/` that referenced the old filename directly (`amf`, `smf`, `udr`, `chf`
(3 files), `ausf`, `nssf`, `lmf`, `pcf`, `upf`, `udm`, `gmlc`, plus 10 conformance/integration test
files), caught immediately by the very next full rebuild (`nfs/amf/src/main.cpp:100: fatal error:
TS29122_CommonData_grp.hpp: No such file or directory`) rather than silently left broken. Fixed as
a pure, mechanical, real rename (same real generated type content, only the file it lives in
changed name) across all 21 `#include` lines plus 15 further comment-only mentions of the old
filename (kept accurate rather than left stale) -- `sed`, not hand-edited, to guarantee every
occurrence was caught consistently; confirmed via a final `grep -rl` returning zero remaining
`TS29122_CommonData_grp` references anywhere in `nfs/`/`tests/`/`bss/`. This is the same real class
of consequence as ADR-0208's cross-NF type-name collisions (adding a NEF-only YAML file can affect
other NFs' already-shipping code), one level larger in scope (a shared generated *file*, not one
*type*) -- fully disclosed, not discovered in review.

### Implementation

`Nnef_EventExposure`, backed by a new `NefEventExposureSubStore` (`nfs/nef/src/stores.hpp/.cpp`) --
same real create/get/put/remove shape as `PfdSubscriptionStore`, no PATCH (this real resource has
none). `CreateIndividualSubcription`: real structural validation of `NefEventExposureSubsc`'s
required `eventsSubs`/`notifId`/`notifUri`, real `201` + `Location`.
`GetIndividualSubcription`: real get/`404`. `ReplaceIndividualSubcription` (PUT): real full-replace,
`200` + body/`404`. `DeleteIndividualSubcription`: real get/`404`-then-remove/`204`. Same real,
disclosed non-scope as every other NEF EventExposure-family gap closed this project (`Namf_
EventExposure` ADR-0199, `Nsmf_EventExposure` ADR-0201, `Npcf_EventExposure` ADR-0204): the
subscription resource itself is real, live CRUD, but no notification is ever actually fired -- no
real cross-NF event-detection pipeline exists to trigger one, a structural gap disclosed plainly
rather than built as dead infrastructure with no real caller.

### Live verification (real, live mTLS + OAuth2, not self-consistency)

New `tests/integration/test_nef_event_exposure.cpp`, 2 tests, real spawned `nrf`+`nef` processes
over real TLS 1.3 + mTLS: full create->read->put->delete lifecycle (real `201`/`200`/`200`(PUT,
`notifId` change verified)/`204`, real `404` on repeat delete), and a real `400` on a create body
missing `notifUri`. Both pass.

### Testing

Full reconfigure + rebuild against the complete pilot set clean (`EXIT=0` verified directly from
each build-log step, including after the `schema_to_ir.py` fix and again after the 21-file
`#include` rename). The 2 new NEF tests pass individually. Full suite re-run clean: 419/419 passing
(`ctest --timeout 120 -E
"UdrIntegration.AmfContextLifecycle|UdmIntegration.SdmDataRetrievalAndSubscriptions"`, matching
`.github/workflows/ci.yml`'s own exclusion) -- confirms the project-wide common-data group rename
did not silently break any NF this run didn't individually re-verify by name. No strays before or
after any run (`ps aux` checked explicitly).

### What this ADR does NOT include

The remaining 5 NEF Tier-A gaps (`Nnef_Inference`, `Nnef_TrafficInfluenceData`, `Nnef_Training`,
`Nnef_VFLInference`, `Nnef_VFLTraining`) -- tracked as follow-up slices under task #164. No real
cross-NF event-detection/notification-firing pipeline (`Nnef_EventExposure`'s own disclosed
consequence, same class as every other EventExposure-family gap closed this project). The
`_is_opaque_type_ref` codegen fix is intentionally narrow (opaque-vs-concrete narrowing only, both
sides' `required`/`nullable` must still agree) -- it does not attempt to handle every conceivable
allOf-narrowing shape (e.g. a concrete-vs-concrete type change, or a `required`/`nullable`
narrowing), which would still correctly raise `NotImplementedError` and need its own, separately
evaluated fix if a future real YAML needs it.

