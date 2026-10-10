## ADR-0116: gap-closure task #106 continuation -- UDR real individual BDT Data (richest policy-data resource yet)

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (30 of free5GC's ~42+ real
TS 29.504 resources closed as of ADR-0115). Real, confirmed-by-YAML-read:
`TS29519_Policy_Data.yaml`'s `/policy-data/bdt-data/{bdtReferenceId}` (real schema `BdtData` --
`aspId`/`transPolicy`/`bdtRefId`/`nwAreaInfo`/`numOfUes`/`volPerUe`/`dnn`/`snssai`/`trafficDes`/
`bdtpStatus`/`warnNotifEnabled`, every field optional) has a real `GET`+`PUT`+`PATCH`+`DELETE`
operation set: `ReadIndividualBdtData`, `CreateIndividualBdtData`, `UpdateIndividualBdtData`
(real `application/merge-patch+json`, RFC 7396), `DeleteIndividualBdtData` -- the richest real
operation set of any UDR `policy-data` resource closed so far. Real, disclosed, genuinely
different from every other real PUT already closed: `CreateIndividualBdtData`'s own response set
documents **only** `201` -- no `200`/`204` update-via-PUT status at all (confirmed by direct read;
the operationId itself is literally "Create", not "CreateOrReplace" like `ue-policy-set`'s own).
Real, disclosed, genuinely different from every other real merge-patch PATCH already closed
(`am-data`, `ue-policy-set`): `UpdateIndividualBdtData`'s own response set documents a real `404`
-- confirmed this PATCH is **not** upsert-capable, unlike every prior merge-patch resource in this
project, because `PUT` is the real, sole create path for this resource.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_bdt_data` table (`bdt_ref_id` PK, `data` JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `BdtDataStore` class (`put`/`get`/`merge_patch`/`remove`).
  `put()` is internally upsert-capable (idempotent-safe for retries) but the real route always
  responds `201`, matching the real spec's own single documented status literally rather than
  inventing an undocumented `204`. `merge_patch()` returns `nullopt` (real `404`) if the resource
  doesn't already exist -- genuinely not upsert-capable, unlike `AmPolicyDataStore`/
  `UePolicySetStore`'s own `merge_patch()`.
- `nfs/udr/src/main.cpp`: four new routes (`GET`/`PUT`/`PATCH`/`DELETE`) at
  `/policy-data/bdt-data/{bdtReferenceId}` and four new OTel counters. Real sibling collection
  resource (`/policy-data/bdt-data`, optional `bdt-ref-ids` array query filter) remains deferred
  -- same real array-query-parameter parsing gap already disclosed for `shared-data`'s own list
  sibling (ADR-0110).

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl lifecycle against a running `udr` process backed by a real PostgreSQL database: `GET` on
an unseeded `bdtReferenceId` -> real `404`; `PATCH` on that same unseeded `bdtReferenceId` -> real
`404`, confirming `PATCH` is genuinely not upsert-capable for this resource (unlike every prior
merge-patch resource); `PUT` with `{"aspId":"asp1","numOfUes":10}` -> real `201`; `GET`
immediately after -> real `200`; `PUT` again with different values -> real `201` again (not
`204`), confirming the real spec's own single documented PUT status is honored literally; `PATCH`
(`application/merge-patch+json`) with `{"numOfUes":30}` -> real `200` with the merged document
(`aspId` unchanged from the second `PUT`, `numOfUes` updated); `GET` again -> real `200` confirming
the merge; `DELETE` -> real `204`; `GET` again -> real `404`. Direct `psql` query against
`udr_bdt_data` independently confirmed zero rows remained after the delete.

### Testing and verification

`udr` built clean. Full `conformance_tests`: unchanged pass count (no new committed automated test
this pass, same disclosed manual-live-verification precedent already established), zero
regressions (325/325).

### What this ADR does NOT include

No NF's own existing logic calls these new routes (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure); the real
sibling collection resource (`/policy-data/bdt-data`) remains deferred, needing array-query-param
parsing this project has no precedent for yet. This closes UDR resource #31 of free5GC's ~42+;
roughly 11 remain a real, open, disclosed gap (docs/CAPABILITY_GAP_ANALYSIS.md). Task #106 remains
open (not fully closed).

