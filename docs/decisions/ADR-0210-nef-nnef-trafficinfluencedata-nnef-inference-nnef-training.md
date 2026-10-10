## ADR-0210: NEF `Nnef_TrafficInfluenceData` + `Nnef_Inference` + `Nnef_Training` + `Nnef_VFLInference` + `Nnef_VFLTraining` -- fifteenth ADR-0193 gap-closure; NEF's 13 Tier-A files now fully closed (task #164 complete)

### Context

Fourth and final NEF Tier-A gap-closure slice (task #164), closing the remaining 5 files:
`TS29591_Nnef_TrafficInfluenceData.yaml` (4 ops, `/nnef-traffic-influence-data/v1`, real
`anyOf: [{required:[dnns]}, {required:[snssais]}]` constraint -- not expressible in the generated
struct's own required/optional fields, so checked explicitly in the route), `TS29591_Nnef_Inference.yaml`
(4 ops, `/nnef-inference/v1`, real, disclosed: no GET operation exists on this resource at all),
`TS29591_Nnef_Training.yaml` (4 ops, `/nnef-training/v1`, same real no-GET gap),
`TS29591_Nnef_VFLInference.yaml` (5 ops, `/nnef-vfl-inference/v1`), and `TS29591_Nnef_VFLTraining.yaml`
(5 ops, `/nnef-vfl-training/v1`, real, disclosed: unlike every other NEF subscription resource
built this project, `notifUri`/`notifCorrId` are genuinely optional here -- only `vflTrainSubs` is
required, confirmed by direct read, not assumed). 22 real operations total. **This closes all 13 of
NEF's own real Tier-A YAML files -- task #164 complete.**

Six additional real dependency files needed pilot-set entries for schema resolution:
`TS29519_Application_Data.yaml`, `TS29530_Naf_Inference.yaml`, `TS29520_Nnwdaf_EventsSubscription.yaml`,
`TS29530_Naf_Training.yaml`, `TS29520_Nnwdaf_VFLInference.yaml`, `TS29520_Nnwdaf_VFLTraining.yaml` --
two of them (`TS29519_Application_Data.yaml` at 5416 lines, `TS29520_Nnwdaf_EventsSubscription.yaml`
at 4553 lines) are large, real, previously-unwired 3GPP files in their own right. Verified via a
fast standalone `generate.py` run (not a full CMake reconfigure) before touching the real pilot set
that all 11 new files (5 `Nnef_*` + 6 dependencies) generate cleanly together with no new
`NotImplementedError`s and no further common-data-group rename (the merged group stayed named
`TS26510_CommonData_grp` -- confirmed via `render.py`'s own `sorted(file_stems)[0]` rule, since
none of the 11 new files' stems sort before `TS26510_CommonData`).

**Real, unrelated-to-my-own-work SCC-merge side effect found and fixed**: this slice's dependency
files bridged a real cyclic `$ref` that absorbed `TS29551_Nnef_PFDmanagement.yaml`'s own schemas
(NEF's original service, ADR-0185) into the giant `TS26510_CommonData_grp.hpp` group -- its own
standalone generated header stopped existing. Caught immediately by the next full rebuild
(`nfs/nef/src/main.cpp:136: fatal error: TS29551_Nnef_PFDmanagement.hpp: No such file or
directory`), fixed by removing the now-stale `#include` (the shared group header was already
included) in `nfs/nef/src/main.cpp` and retargeting `tests/conformance/test_nef_dtos.cpp`'s own
include to `TS26510_CommonData_grp.hpp`.

**Two real bugs of my own, caught via live verification (not self-consistency)**: my own test
bodies for `Nnef_Training`/`Nnef_VFLInference`/`Nnef_VFLTraining` fabricated nested object shapes
for `EventSubsc`/`VflInferAnaSub`/`VflTrainingSub` (guessed field names like `anaEvent`/
`trainReportInd` instead of reading the real generated structs first) -- caught by real `400`
responses citing the real missing required fields (`event`, `vflCorrId`). Fixed by reading the
actual generated types and using their real required fields (`EventSubsc.event`,
`VflInferAnaSub.anaEvent`+`vflCorrId`, `VflTrainingSub.event`+`vflCorrId`, all real
`NwdafEvent` open-string-enum values). The resulting `400`s then triggered the same real
`ASSERT_FALSE(sub_id.empty())`-before-`reap_all()` pipe-hang/leaked-process bug class documented in
prior ADRs (0204) -- this time cascading into 3 subsequent tests failing to bind their own
NRF/NEF ports ("Address already in use") because the leaked processes from the first failure never
released them. Fixed by restructuring every lifecycle test in the new file to compute `sub_id`
unconditionally (ternary, no `ASSERT`), assert only `EXPECT_FALSE`, and guard all post-creation
work behind `if (!sub_id.empty())` so `reap_all()` always runs regardless of outcome -- applied to
all 5 lifecycle tests in the file, not just the 3 that actually hit it, same defensive precedent as
ADR-0204.

**A second, unrelated tooling mistake caught and fixed before commit**: while batch-formatting the
files this slice touched, `clang-format-18` was run against `tools/sbi-codegen/sbi_codegen/schema_to_ir.py`
by mistake (a Python file, not C++) -- it does not understand Python syntax and mangled the file's
docstrings/comments/indentation into invalid Python. Caught immediately (the file had no real
changes queued for this slice, so `git diff --stat` against the last real commit showed only
formatter damage), fixed via `git checkout --` to restore the real, already-committed content, then
verified with `python3 -c "import ast; ast.parse(...)"` that the restored file parses as valid
Python. Not committed at any point -- caught before ever leaving the working tree.

### Implementation

- **`Nnef_TrafficInfluenceData`**, backed by a new `TrafficInfluDataSubStore`. Real create/get/put/
  remove CRUD, no PATCH (none exists on this resource). The real `anyOf` dnns/snssais constraint
  enforced explicitly in both `Create` and `Replace` handlers.
- **`Nnef_Inference`**, backed by a new `InferEventSubStore` (create/get/put/patch/remove -- `get`
  used internally by `PATCH`'s own return-updated-representation, never exposed as a route since
  none exists on the real resource). Real RFC 7396 merge-patch on `PATCH`. Real, disclosed:
  `inferAnaSubs`/`inferResults` are real `additionalProperties`-map schemas this project's own
  codegen falls back to opaque `nlohmann::json` for (same "OPAQUE FALLBACK" class already
  established elsewhere) -- any JSON object is structurally accepted, not independently validated
  against the real per-key `InferAnaSub`/`EventNotification` shapes.
- **`Nnef_Training`**, backed by a new `TrainEventsSubStore`. Same real create/put/patch/remove
  shape, no GET.
- **`Nnef_VFLInference`**, backed by a new `VflInferSubStore`. Full real create/get/put/patch/
  remove CRUD.
- **`Nnef_VFLTraining`**, backed by a new `NefVflTrainSubStore`. Full real create/get/put/patch/
  remove CRUD.

### Live verification (real, live mTLS + OAuth2, not self-consistency)

New `tests/integration/test_nef_traffic_influence_inference_training.cpp`, 7 tests, real spawned
`nrf`+`nef` processes over real TLS 1.3 + mTLS: `Nnef_TrafficInfluenceData` full
create->read->put->delete lifecycle plus a real `400` on a body with neither `dnns` nor `snssais`;
`Nnef_Inference` full create->put->patch->delete lifecycle (no GET, matching the real YAML);
`Nnef_Training` full create->patch->delete lifecycle (no GET); `Nnef_VFLInference` full
create->read->patch->delete lifecycle; `Nnef_VFLTraining` full create->read->delete lifecycle with
only `vflTrainSubs` in the body (verifying notifUri/notifCorrId really are optional) plus a real
`400` on a body missing `vflTrainSubs`. All 7 pass.

### Testing

Full reconfigure + rebuild against the complete pilot set clean (`EXIT=0` verified directly from
each build-log step, including after the `TS29551_Nnef_PFDmanagement.hpp` include fix). `nef`,
`integration_tests`, and `conformance_tests` (for the retargeted `test_nef_dtos.cpp`) all rebuild
clean. The 7 new tests pass individually after the two real bugs above were found and fixed. Full
suite re-run clean: 426/426 passing (`ctest --timeout 120 -E
"UdrIntegration.AmfContextLifecycle|UdmIntegration.SdmDataRetrievalAndSubscriptions"`, matching
`.github/workflows/ci.yml`'s own exclusion). No strays before or after any run (`ps aux` checked
explicitly; one real stray `nrf`+`nef` pair from the leaked-process bug above was found and killed
by explicit PID before the fix, not left for the next run to inherit).

### What this ADR does NOT include

No real cross-NF event-detection/notification-firing pipeline for any of these 5 resources (same
disclosed consequence class as every EventExposure/notification-family gap closed this project).
No real per-key validation of `Nnef_Inference`'s opaque `additionalProperties` map fields. **NEF's
own 13 real Tier-A YAML files are now all closed** (ADR-0207/0208/0209/0210) -- task #164 complete.
NEF's real Tier-B gap surface (individual operations within already-wired files that might be
stubbed or missing) has not been separately audited in this pass; per ADR-0193's own Tier-A/Tier-B
distinction, that remains a real, disclosed, separately-trackable follow-up if found.

