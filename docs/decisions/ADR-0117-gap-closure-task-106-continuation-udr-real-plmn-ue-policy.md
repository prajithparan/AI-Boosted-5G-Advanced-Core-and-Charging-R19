## ADR-0117: gap-closure task #106 continuation -- UDR real PLMN UE Policy Set

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (31 of free5GC's ~42+ real
TS 29.504 resources closed as of ADR-0116). Real, confirmed-by-YAML-read: `TS29519_Policy_Data.yaml`'s
`/policy-data/plmns/{plmnId}/ue-policy-set` (`ReadPlmnUePolicySet`) is a real `GET`-only resource
-- no create/update operation exists for it at all, confirmed by grepping every operationId
referencing this path. It reuses the same real `UePolicySet` schema (`praInfos`/`subscCats`/
`uePolicySections`, every field optional) already backing `udr_ue_policy_set`'s own per-UE
resource (ADR-0113), but is genuinely a distinct resource: keyed by `plmnId` (`VarPlmnId`,
TS29505_Subscription_Data.yaml -- mcc+mnc concatenated), not `ueId` -- an H-PLMN-scoped default
policy set, not a per-subscriber one. Same real "provisioned out-of-band, seeded at startup" shape
already established for `CoverageRestrictionDataStore` and every other real GET-only resource
closed this series.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_plmn_ue_policy_set` table (`plmn_id` PK, `data` JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `PlmnUePolicySetStore` class (`seed`/`get`), same shape as
  `CoverageRestrictionDataStore`.
- `nfs/udr/src/main.cpp`: one new `GET` route at `/policy-data/plmns/{plmnId}/ue-policy-set` and
  one new OTel counter. Seeded once (genuinely not per-UE) for this project's own real lab PLMN
  ("99970", mcc=999/mnc=70, ADR-0016) with a representative `subscCats: ["cat1"]` body -- not
  fabricated spec content, `subscCats` is a real optional untyped-enum field.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl against a running `udr` process backed by a real PostgreSQL database: `GET` on the
seeded PLMN `99970` -> real `200` with `{"subscCats":["cat1"]}`; `GET` on an unseeded PLMN
(`00101`) -> real `404`. Direct `psql` query against `udr_plmn_ue_policy_set` independently
confirmed exactly one row, `plmn_id = '99970'`, matching the seeded body.

### Testing and verification

`udr` built clean. Full `conformance_tests`: unchanged pass count (same disclosed
manual-live-verification precedent already established for every GET-only seeded resource in
this series), zero regressions (325/325).

### What this ADR does NOT include

No NF's own existing logic calls this new route (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #32 of free5GC's ~42+; roughly 10 remain a real, open, disclosed gap
(docs/CAPABILITY_GAP_ANALYSIS.md). Task #106 remains open (not fully closed).

