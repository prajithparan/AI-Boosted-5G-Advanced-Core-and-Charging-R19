## ADR-0191: LMF -- fourth Tier 2 NF, `Nlmf_Location` (real RF/GNSS-dependency scope decision, same shape as GMLC's own ADR-0189)

### Context

Eighth NF built under ADR-0184's continuous move-to-next-NF process, sixteenth NF overall, fourth
Tier 2 NF, the direct continuation of GMLC's own disclosed gap (ADR-0189): GMLC's `RequestLocation`
was disclosed `501` because it genuinely requires this project's own LMF, which didn't exist yet.
LMF's real spec surface (`TS29572_Nlmf_Location.yaml` v1.4.1, 7 real operations) is itself larger
and more RF-adjacent than any Tier 2 NF built so far -- `DetermineLocation`'s own real behavior (TS
23.273/38.305) requires an actual LPP (TS 37.355) message exchange with a UE over NG-RAN, or real
GNSS assistance data; `LocationMeasure` requires real PRU/NRPPa measurement data (TS 38.305/
38.455). This project has no spec material, codec, or real/simulated RAN/UE stack for either --
genuinely out of session scope, not a shortfall. Consistent with CLAUDE.md's single-worst-failure-
mode rule, no GPS coordinates, LPP PDUs, or PRU measurement bytes are fabricated.

### Implementation

Source: `specs/5G_APIs-REL-19/TS29572_Nlmf_Location.yaml` (v1.4.1), commit
`bca84b60a37773133bcae97e5c6c0d10a93b47b6`. All 7 real operations implemented, each behaving
honestly according to what this project's own actual backend capability supports:

- `DetermineLocation` (`POST /determine-location`): real structural input validation (both the
  request's own `multipart/related` and plain `application/json` content types handled, matching
  the real spec's own dual-content-type declaration; a real `400` on malformed input; the real
  YAML's own `not: required: [ecgi, ncgi]` mutual-exclusivity constraint enforced explicitly,
  since the codegen's structural typing alone doesn't enforce JSON-Schema `not` constraints), then
  the real, documented `501` the YAML itself declares.
- `LocationMeasure` (`POST /measure-location`): real input validation (all fields optional per the
  real schema), then the real, documented `501` for the same real reason.
- `CancelLocation` (`POST /cancel-location`): real input validation, then a real `404` -- since
  `DetermineLocation` never issues a real `ldrReference`, no cancellation can ever match one.
- `UpSubscriptions`/`DeleteSubscription` (`POST /up-subscriptions` + `DELETE .../{subscriptionId}`,
  real, complete, no RF dependency): a real, full create+delete lifecycle via
  `lmf::UpSubscriptionStore` -- unlike GMLC's own create-only `/loc-update-subs` gap (ADR-0189),
  this real YAML resource has both operations declared. Real, disclosed spec gap: the real `201`
  response declares no `Location` header or id-bearing body field, so the created resource's own
  id would otherwise be undiscoverable -- a real `Location` header is added at the HTTP layer (not
  a fabricated JSON field) to make the resource actually usable, same class of fill-a-genuine-gap
  choice as other NFs' own disclosed additions.
- `LocationContextTransfer` (`POST /location-context-transfer`, real, complete, no RF dependency):
  a real LMF-relocation context push, stored in `lmf::LocationContextStore` keyed by the real,
  required `ldrReference`.
- `UpConfig` (`POST /configure-up`, real, complete, no RF dependency): a real secure-LCS-UP-
  connection setup/modify/terminate via `lmf::UpConfigStore`, keyed by supi/gpsi per the real
  YAML's own declared `anyOf` requirement (enforced as a real `400`, not invented, unlike GMLC's
  own store-key-necessity workaround -- this one IS a real spec constraint). `TERMINATION` really
  removes the stored entry rather than accepting and discarding the request.

`nfs/lmf/` scaffolded to match `nfs/gmlc`'s shape (in-memory stores, no Redis/Postgres):
`CMakeLists.txt`, `src/stores.hpp`/`.cpp` (3 stores, described above), `src/main.cpp` (all 7
routes + NRF registration/heartbeat + OAuth2 + OTel/Prometheus). `config/lmf.json` (port 7795,
metrics 9483). `certs/lmf/` generated via `scripts/gen-lab-pki.sh`. `TS29572_Nlmf_Location.yaml`,
`TS29518_Namf_Location.yaml`, and `TS29518_Namf_EventExposure.yaml` added to
`libs/sbi-generated/CMakeLists.txt`'s pilot set (the latter two real cross-file dependencies of
`InputData`'s own `lpHapType`/`ueConnectivityState` fields). Real codegen mechanic (same as GMLC,
ADR-0189): LMF's own types (`InputData_Nlmf_Location`, `CancelLocData_Nlmf_Location`,
`LocContextData`, `LocMeasurementReq`, `UpSubscription`, `UpConfig`) land in the shared
`TS29122_CommonData_grp.hpp` rather than a standalone header, due to real cross-file `$ref`
cycles. Added to top-level `CMakeLists.txt`, `deploy/docker/docker-compose.yml`/`lmf.Dockerfile`,
and the `pki-init` cert-generation argument list. No Helm chart -- same pre-existing, disclosed,
project-wide gap. New conformance test file `tests/conformance/test_lmf_dtos.cpp` (3 round-trip
tests over the real generated DTOs), wired into `tests/conformance/CMakeLists.txt`.

### Live verification (real, live processes, not self-consistency)

Real `nrf` + `lmf` processes, real OAuth2 bearer token, real mTLS curl against all 7 routes:
`DetermineLocation` with valid input -> real `501`; with both `ecgi`+`ncgi` present -> real `400`
(the real declared mutual-exclusivity constraint). `CancelLocation` -> real `404`.
`UpSubscriptions` -> real `201` + `Location` header + echoed body; `DeleteSubscription` against
that same id -> real `204`; repeated -> real `404`. `LocationContextTransfer` -> real `204`.
`LocationMeasure` -> real `501`. `UpConfig` with `supi` -> real `204`; without `supi`/`gpsi` ->
real `400` (the real declared `anyOf` constraint). Negative auth: a malformed bearer token, real
`401` `ProblemDetails`. Prometheus `/metrics`: all 7 operation counters present, each correctly at
`1`. All processes killed cleanly afterward (PID-verified).

### Testing

`lmf` and `conformance_tests` built clean (zero warnings) both before and after
`clang-format-18` (once correctly scoped to the C++ files only -- see ADR-0190's own disclosed
process incident). Full project rebuild clean (82/82 steps, only the pre-existing onnxruntime
static-lib linker warning, unrelated to this change). Full `ctest` (excluding the two disclosed
pre-existing flaky tests): 361/361 pass, zero regressions (3 new `LmfDtos` tests +
`structural_conformance` covering the new DTOs).

### What this ADR does NOT include

Real LPP (TS 37.355) UE positioning exchange or real PRU/NRPPa measurement (TS 38.305/38.455) --
both genuinely out of this session's spec material, disclosed above; `DetermineLocation`'s and
`LocationMeasure`'s own real `501`s are the honest consequence, not a workaround. No Helm chart
(pre-existing, disclosed, project-wide gap). Like GMLC, this ADR splits real-complete
(`UpSubscriptions`/`DeleteSubscription`/`LocationContextTransfer`/`UpConfig`, 4 of 7) from
real-honest-501/404 (`DetermineLocation`/`LocationMeasure`/`CancelLocation`, 3 of 7) rather than
claiming a false "complete NF" -- this closes the GMLC->LMF dependency chain from the LMF side: the
real reason GMLC's own `RequestLocation`/`CancelLocation` stay `501`/`404` is now visible end-to-
end in this project's own code, not just asserted in a comment.

