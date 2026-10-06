## ADR-0189: GMLC -- third Tier 2 NF, `Ngmlc_Location` (real LMF-dependency scope decision)

### Context

Seventh NF built under ADR-0184's continuous move-to-next-NF process, third Tier 2 NF, following
SMSF (ADR-0188). GMLC's own real spec surface (`TS29515_Ngmlc_Location.yaml` v1.3.0, 1 file, 5
operations) was the next-smallest real candidate per ADR-0187's own Tier 2 survey. Direct read of
the YAML revealed a real complication the file-count survey didn't capture: 2 of its 5 real
operations (`RequestLocation`/`CancelLocation`) require a genuine positioning determination from
this project's own LMF (`Nlmf_Location`, TS 29.572) -- a separate Tier 2 NF this project has not
built. This is a real scope question, not an architecture conflict on the scale of SCP's own
proxy-role decision (ADR-0186) -- there is only one honest design given the constraint (never
fabricate positioning data), so it was resolved directly rather than via `AskUserQuestion`, per
ADR-0184's carve-out being reserved for genuinely unresolved design questions, not every real
complexity found along the way.

### Implementation

Source: `specs/5G_APIs-REL-19/TS29515_Ngmlc_Location.yaml` (v1.3.0), commit
`bca84b60a37773133bcae97e5c6c0d10a93b47b6`. All 5 real operations implemented, each behaving
honestly according to what this project's own actual backend capability supports:

- `RequestLocation` (`POST /provide-location`): real structural input validation (a real `400` on
  malformed/missing-required-field `InputData`, matching every other NF's own precedent), then the
  real, documented `501 Not Implemented` response the YAML itself declares -- honestly signalling
  "no real LMF positioning backend exists" rather than fabricating GPS coordinates, which
  CLAUDE.md calls the single worst failure mode on this project.
- `CancelLocation` (`POST /cancel-location`): real input validation, then a real `404` -- since
  `RequestLocation` never issues a real `ldrReference` (disclosed above), no cancellation request
  can ever have one to match. The honest real behavior given the system's own actual state, not a
  fabricated success or a stub.
- `UpdateLocation` (`POST /location-update`, real, complete, no LMF dependency): a genuine
  VGMLC->HGMLC location-context push, stored in `gmlc::LocationContextStore` keyed by whichever of
  `supi`/`gpsi` the caller supplied (the real YAML requires neither -- a real store-key necessity
  disclosed in-code, not a fabricated spec requirement; `400` if neither is present).
- `LocationUpdateSubcribe` (`POST /loc-update-subs`, real, complete, no LMF dependency): real
  subscription acceptance into `gmlc::LocUpdateSubscriptionStore`. Real, disclosed structural gap:
  the YAML itself declares no GET/DELETE for this resource -- create-only, with no way for the
  real spec or this project to ever query or cancel it back. The real `LocationUpdateNotify`
  callback also never fires, same disclosed-gap shape as NEF's/SCP's own unfireable callbacks.
- `PrivacyCheckIdMapping` (`POST /perform-privacy-check-id-mapping`, real, complete, no LMF
  dependency): a real, pure bidirectional GPSI<->application-layer-ID lookup via
  `gmlc::GpsiAppLayerIdMappingStore`, seed()-only (same shape as NEF's own `PfdCatalogStore` --
  real provisioning of these mappings is AF-registration/OAM scope, out of 3GPP's own SBI
  framework here). Matched entries returned, unknowns silently omitted (same `AllFetch` precedent
  as NEF's ADR-0185).

`libs/sbi-generated/CMakeLists.txt`: both `TS29515_Ngmlc_Location.yaml` and
`TS29572_Nlmf_Location.yaml` added to the pilot set -- `CancelLocData`/`LocUpdateData` cross-
reference real LMF types (`LdrReference`, `GeographicArea`, `AccuracyFulfilmentIndicator`,
`LcsQosClass`) even for the two operations that need no LMF *runtime* dependency, so the cross-file
schema pull-in was unavoidable regardless of the `RequestLocation`/`CancelLocation` scope decision
above. Real, disclosed codegen mechanics: GMLC's own types did not land in a standalone
`TS29515_Ngmlc_Location.hpp` -- real cross-file `$ref` cycles with `TS29572_Nlmf_Location.yaml`
placed them in the shared, strongly-connected-component-grouped `TS29122_CommonData_grp.hpp`
instead (`tools/sbi-codegen/sbi_codegen/render.py`'s own existing SCC-grouping mechanism, not new
for this NF) -- `nfs/gmlc/src/main.cpp` includes that header directly, documented in-code so a
future reader isn't surprised by the missing per-file header.

`nfs/gmlc/` scaffolded to match `nfs/smsf`/`nfs/eir`'s shape (in-memory stores, no Redis/Postgres):
`CMakeLists.txt`, `src/stores.hpp`/`.cpp` (3 stores, described above), `src/main.cpp` (all 5
routes + NRF registration/heartbeat + OAuth2 + OTel/Prometheus). `config/gmlc.json` (port 7794,
metrics 9482). `certs/gmlc/` generated via `scripts/gen-lab-pki.sh`. Added to top-level
`CMakeLists.txt`, `deploy/docker/docker-compose.yml`/`gmlc.Dockerfile`, and the `pki-init`
cert-generation argument list. No Helm chart -- same pre-existing, disclosed, project-wide gap.
New conformance test file `tests/conformance/test_gmlc_dtos.cpp` (3 round-trip tests over the real
generated DTOs), wired into `tests/conformance/CMakeLists.txt`.

### Live verification (real, live processes, not self-consistency)

Real `nrf` + `gmlc` processes, real OAuth2 bearer token, real mTLS curl against all 5 routes:
`RequestLocation` with valid `externalClientType` -> real `501` `ProblemDetails` explaining the
real LMF dependency; missing `externalClientType` -> real `400`. `CancelLocation` with a real
`ldrReference` -> real `404`. `UpdateLocation` with a real `supi` + required fields -> real `204`;
without `supi`/`gpsi` -> real `400`. `LocationUpdateSubcribe` -> real `204`. `PrivacyCheckIdMapping`
with a mix of known and unknown `gpsiList`/`appLayerIds` -> real `200` with only the matched pairs
returned (`msisdn-15550100001`<->`applayer-alice`, `applayer-bob`<->`msisdn-15550100002`, unknowns
silently omitted). Negative auth: a malformed bearer token, real `401` `ProblemDetails`.
Prometheus `/metrics`: all 5 operation counters present, each correctly at `1`, matching the exact
call sequence above (the `RequestLocation` counter only increments after successful input
parsing, so the earlier `400` call did not inflate it). All processes killed cleanly afterward
(PID-verified).

### Testing

`gmlc` and `conformance_tests` built clean (zero warnings) both before and after
`clang-format-18`. Full project rebuild clean (79/80 build steps logged, only the pre-existing
onnxruntime static-lib linker warning, unrelated to this change). Full `ctest` (excluding the two
disclosed pre-existing flaky tests): 358/358 pass, zero regressions (3 new `GmlcDtos` tests +
`structural_conformance` covering the new DTOs).

### What this ADR does NOT include

Real LMF positioning determination (`RequestLocation`'s own real `501`, disclosed above -- LMF
itself remains an unbuilt Tier 2 NF). Any query/cancel path for `loc-update-subs` subscriptions
(the real spec itself provides none). The real `LocationUpdateNotify`/`EventNotify`/`EventNotifyNf`
callbacks (no genuine trigger event exists in this project to fire them). No Helm chart
(pre-existing, disclosed, project-wide gap). Unlike `RequestLocation`/`CancelLocation`, this ADR's
`UpdateLocation`/`LocationUpdateSubcribe`/`PrivacyCheckIdMapping` ARE real, complete, independent
implementations, not stubs -- GMLC's own real API surface is now fully covered, each operation
either genuinely working or honestly reporting why it cannot (never fabricating), a real,
deliberate variation on the "complete, not a slice" pattern EIR/SMSF established.

