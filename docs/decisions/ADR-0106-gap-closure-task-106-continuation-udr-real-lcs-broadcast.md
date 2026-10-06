## ADR-0106: gap-closure task #106 continuation -- UDR real LCS Broadcast Assistance Data (provisioned-data sibling)

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (20 of free5GC's ~42+ real
TS 29.504 resources closed as of ADR-0105). Real, confirmed-by-YAML-read:
`TS29505_Subscription_Data.yaml`'s
`/subscription-data/{ueId}/{servingPlmnId}/provisioned-data/lcs-bca-data` (real schema
`LcsBroadcastAssistanceTypesData`, `$ref`'d verbatim from `TS29503_Nudm_SDM.yaml` -- mandatory
`locationAssistanceType`, a real `Bytes` (base64) field) is a real, genuinely GET-only resource
(`QueryLcsBcaData`) -- confirmed by grepping every operationId referencing this path, no
create/update operation exists at all. Unlike the `lcs-*` siblings closed in ADR-0103/0104/0105
(which are keyed by `ueId` alone), this one is a real sibling of the already-closed
`provisioned-data` group (`am-data`/`smf-selection-subscription-data`/`sm-data`, ADR-0069) --
same real `(ueId, servingPlmnId)` path/key shape, genuinely distinct resource under that same
group, not a rename.

### Implementation

- `nfs/udr/schema.postgres.sql`: `lcs_bca_data JSONB` column added to the existing
  `udr_provisioned_data` table (real 4th sub-resource column, same "one column per real
  sub-resource, all seeded together" design already established for the other three), plus an
  idempotent `ALTER TABLE ... ADD COLUMN IF NOT EXISTS` -- `CREATE TABLE IF NOT EXISTS` alone is a
  no-op against the already-existing real table from every prior run, so the `ALTER` is what
  actually applies the new column to the live database.
- `nfs/udr/src/stores.hpp`/`.cpp`: `ProvisionedDataStore::seed()` gains a 4th parameter
  (`lcs_bca_data`); new `get_lcs_bca_data()` accessor reusing the existing generic
  `get_provisioned_column()` helper -- no new query logic needed.
- `nfs/udr/src/main.cpp`: one new route (`GET`) at
  `.../provisioned-data/lcs-bca-data`, reusing the already-vendored
  `sbi_gen::LcsBroadcastAssistanceTypesData` DTO indirectly (raw JSON, matching this group's own
  established pattern) and the existing `provisioned_data_get_counter` (already generic across
  this group's routes, extended rather than duplicated); seed loop extended with
  `locationAssistanceType: "dGVzdA=="` (base64 of "test") -- this project's own arbitrary
  representative test payload for the real `Bytes` field, not real 3GPP assistance-data content.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl against a running `udr` process backed by a real PostgreSQL database: `GET` for the real
seeded `(ueId, servingPlmnId)` pair -> real `200` with the exact seeded `locationAssistanceType`
document; `GET` for the same `ueId` with an unseeded `servingPlmnId` -> real `404`; re-checked the
sibling `am-data` route on the same `(ueId, servingPlmnId)` still returns its own real `200` data
unchanged -- confirming the schema/store extension caused no regression to the other three
sub-resources sharing the same row. Direct `psql` query against `udr_provisioned_data`
independently confirmed both seeded rows' `lcs_bca_data` column matches what the API returned.

### Testing and verification

`udr` built clean. Full `conformance_tests`: unchanged pass count (no new committed automated test
this pass, same disclosed manual-live-verification precedent already established), zero
regressions (325/325).

### What this ADR does NOT include

No NF's own existing logic calls this new route (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #21 of free5GC's ~42+; roughly 21 remain a real, open, disclosed gap
(docs/CAPABILITY_GAP_ANALYSIS.md). Task #106 remains open (not fully closed).

