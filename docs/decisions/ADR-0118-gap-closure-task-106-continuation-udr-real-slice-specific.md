## ADR-0118: gap-closure task #106 continuation -- UDR real Slice-specific Policy Control Data

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (32 of free5GC's ~42+ real
TS 29.504 resources closed as of ADR-0117). Real, confirmed-by-YAML-read:
`TS29519_Policy_Data.yaml`'s `/policy-data/slice-control-data/{snssai}` (real schema
`SlicePolicyData` -- `mbrUl`/`mbrDl`/`remainMbrUl`/`remainMbrDl`/`suppFeat`, every field optional)
has a real `GET`+`PATCH`-only operation set: `ReadSlicePolicyControlData`,
`UpdateSlicePolicyControlData` (real `application/merge-patch+json`, RFC 7396, request body is the
narrower `SlicePolicyDataPatch` schema -- `remainMbrUl`/`remainMbrDl` only). Confirmed by direct
read: no `PUT`/`POST` create operation exists for this resource at all, so (same disclosed,
deliberate precedent already established for `AmPolicyDataStore`/`SmPolicyDataStore`)
`merge_patch` is upsert-capable.

Real, disclosed, genuinely different problem from any prior resource in this series: the YAML
types the `{snssai}` path parameter using the `Snssai` *object* schema (`$ref` to
`TS29571_CommonData.yaml#/components/schemas/Snssai`) with no documented string encoding for a
bare path segment -- checked, not assumed. Every other real 5G_APIs YAML use of `Snssai` as a
non-body parameter found while checking (e.g. `TS29510_Nnrf_NFDiscovery.yaml`'s `snssai`/
`additional-snssais` query parameters) wraps it in a real `content: application/json` parameter
instead of a plain `schema:`, which is the standard OpenAPI 3.0 idiom for complex-typed query
parameters -- this resource's path parameter has no equivalent wrapper, and path parameters can't
use the `content:` idiom at all. This project already faced and disclosed the identical underlying
question once before (ADR-0072, `PCF`'s own `snssai_map_key` used as a JSON map key, not a URI
segment): "no wire encoding is spec-mandated" for representing an `Snssai` as a plain string, and
chose `sst + '-' + sd` as its own deliberate, disclosed convention, explicitly not a claim of
interop with any other real implementation's own choice. Rather than re-litigating the same open
question a second time, this ADR reuses that same convention for the URI path segment.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_slice_control_data` table (`snssai` PK, `data` JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `SlicePolicyDataStore` class (`get`/`merge_patch`),
  byte-for-byte matching `AmPolicyDataStore`'s own upsert-capable `merge_patch` pattern.
- `nfs/udr/src/main.cpp`: two new routes (`GET`/`PATCH`) at
  `/policy-data/slice-control-data/{snssai}` and two new OTel counters.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl lifecycle against a running `udr` process backed by a real PostgreSQL database: `GET` on
an unseeded `snssai` (`1-000001`) -> real `404`; `PATCH`
(`application/merge-patch+json`) with `{"remainMbrUl":"100 Mbps"}` on that same key, no prior
create -> real `200` with the newly-originated document (confirming upsert-capable `PATCH`, no
`PUT`/`POST` needed); `GET` immediately after -> real `200` matching; `PATCH` again with
`{"remainMbrDl":"200 Mbps"}` -> real `200` with both fields present (`remainMbrUl` retained from
the first `PATCH`, `remainMbrDl` merged in); `GET` again -> real `200` confirming the merge.
Direct `psql` query against `udr_slice_control_data` independently confirmed the persisted
document (`{"remainMbrDl": "200 Mbps", "remainMbrUl": "100 Mbps"}`, key `1-000001`) matches the
API's final response exactly.

### Testing and verification

`udr` built clean. Full `conformance_tests`: unchanged pass count (no new committed automated test
this pass, same disclosed manual-live-verification precedent already established), zero
regressions (325/325).

### What this ADR does NOT include

No NF's own existing logic calls these new routes (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). The real
`snssai` path-segment string encoding is this project's own disclosed, not-spec-mandated choice
(see Context above) -- not a claim of interop with any other real implementation that might
encode it differently. This closes UDR resource #33 of free5GC's ~42+; roughly 9 remain a real,
open, disclosed gap (docs/CAPABILITY_GAP_ANALYSIS.md). Task #106 remains open (not fully closed).

