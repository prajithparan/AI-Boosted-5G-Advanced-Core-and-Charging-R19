## ADR-0125: gap-closure task #106 continuation -- UDR real SMS Management Subscription Data

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (37 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0123). Real, confirmed-by-YAML-read:
`TS29505_Subscription_Data.yaml`'s
`/subscription-data/{ueId}/{servingPlmnId}/provisioned-data/sms-mng-data` (real schema
`SmsManagementSubscriptionData`, `TS29503_Nudm_SDM.yaml` -- `supportedFeatures`/
`mtSmsSubscribed`/`mtSmsBarringAll`/`mtSmsBarringRoaming`/`moSmsSubscribed`, every field optional)
is genuinely `GET`-only (`QuerySmsMngData`) -- no create/update operation exists at all, same real
reasoning already established for every other `provisioned-data` sub-resource. Same real
`(ueId, servingPlmnId)` composite key as `am-data`/`smf-selection-subscription-data`/`sm-data`/
`lcs-bca-data`. Same real "genuinely part of the same resource group" shape ADR-0106 already used
to add `lcs_bca_data` as a new column on the existing `udr_provisioned_data` table rather than a
new table -- `sms_mng_data` follows that exact precedent.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `sms_mng_data JSONB` column on `udr_provisioned_data`, plus
  the matching `ALTER TABLE ... ADD COLUMN IF NOT EXISTS` for existing databases (`CREATE TABLE IF
  NOT EXISTS` alone is a no-op against an already-existing table, same real reasoning ADR-0106
  already documented).
- `nfs/udr/src/stores.hpp`/`.cpp`: `ProvisionedDataStore::seed()` gains a 7th parameter
  (`sms_mng_data`); new `get_sms_mng_data()` reuses the existing `get_provisioned_column()`
  helper, same as `get_lcs_bca_data()`.
- `nfs/udr/src/main.cpp`: one new `GET` route at `.../provisioned-data/sms-mng-data`, reusing the
  existing shared `provisioned_data_get_counter` (same pattern as every other sibling route in
  this group). Seed data extended with `mtSmsSubscribed: true`, a real optional boolean field,
  this project's own representative test choice.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl against a running `udr` process backed by a real PostgreSQL database: `GET` on the
seeded (SUPI, PLMN) pair (`imsi-999700000000001`, `99970`) -> real `200` with
`{"mtSmsSubscribed":true}`; `GET` on the same SUPI with an unseeded PLMN (`00101`) -> real `404`;
cross-checked the sibling `am-data` column on the same row is unaffected -> real `200` with the
expected, unchanged `nssai` body. Direct `psql` query against `udr_provisioned_data` independently
confirmed both seeded rows' `sms_mng_data` column matches the API's response.

### Testing and verification

`udr` built clean. Full `conformance_tests`: unchanged pass count (same disclosed
manual-live-verification precedent already established for every GET-only seeded resource in this
series), zero regressions (325/325).

### What this ADR does NOT include

No NF's own existing logic calls this new route (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #38 of free5GC's ~42+ real `Nudr_DataRepository` resources; roughly 4 remain a real,
open, disclosed gap (docs/CAPABILITY_GAP_ANALYSIS.md). The real sibling
`.../provisioned-data/sms-data` resource (schema `SmsSubscriptionData`) was found during the same
survey and is a strong next candidate (same shape, same column-on-existing-table precedent) but is
deliberately left for its own turn. Task #106 remains open (not fully closed).

