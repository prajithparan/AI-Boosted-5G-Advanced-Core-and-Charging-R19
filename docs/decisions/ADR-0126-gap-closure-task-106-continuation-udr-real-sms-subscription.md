## ADR-0126: gap-closure task #106 continuation -- UDR real SMS Subscription Data

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (38 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0125). Real, confirmed-by-YAML-read:
`TS29505_Subscription_Data.yaml`'s
`/subscription-data/{ueId}/{servingPlmnId}/provisioned-data/sms-data` (real schema
`SmsSubscriptionData`, `TS29503_Nudm_SDM.yaml` -- `smsSubscribed`/`sharedSmsSubsDataId`/
`supportedFeatures`, every field optional) is genuinely `GET`-only (`QuerySmsData`) -- no
create/update operation exists at all, same real reasoning already established for every other
`provisioned-data` sub-resource. Same real `(ueId, servingPlmnId)` composite key as the other
five sub-resources already in this table. Genuinely distinct real resource from
`sms-mng-data` (ADR-0125) -- separate operationId, separate schema (`SmsSubscriptionData` vs
`SmsManagementSubscriptionData`), not a rename or duplicate; confirmed by live side-by-side
verification below. Same real "genuinely part of the same resource group" shape ADR-0106
established for `lcs_bca_data` -- `sms_data` follows that exact precedent as a 6th column.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `sms_data JSONB` column on `udr_provisioned_data`, plus the
  matching `ALTER TABLE ... ADD COLUMN IF NOT EXISTS` for existing databases.
- `nfs/udr/src/stores.hpp`/`.cpp`: `ProvisionedDataStore::seed()` gains an 8th parameter
  (`sms_data`); new `get_sms_data()` reuses the existing `get_provisioned_column()` helper, same
  as `get_sms_mng_data()`/`get_lcs_bca_data()`.
- `nfs/udr/src/main.cpp`: one new `GET` route at `.../provisioned-data/sms-data`, reusing the
  existing shared `provisioned_data_get_counter`. Seed data extended with
  `smsSubscribed: true`, a real optional boolean field, this project's own representative test
  choice.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl against a running `udr` process backed by a real PostgreSQL database: `GET` on the
seeded (SUPI, PLMN) pair -> real `200` with `{"smsSubscribed":true}`; `GET` on the same SUPI with
an unseeded PLMN -> real `404`; cross-checked the sibling `sms-mng-data` resource on the same row
independently returns its own distinct `{"mtSmsSubscribed":true}` body, confirming the two
resources are genuinely separate, not aliases of each other. Direct `psql` query against
`udr_provisioned_data` independently confirmed both seeded rows' `sms_data` and `sms_mng_data`
columns hold their own distinct values.

### Testing and verification

`udr` built clean. Full `conformance_tests`: unchanged pass count (same disclosed
manual-live-verification precedent already established for every GET-only seeded resource in this
series), zero regressions (325/325). Also ran the complete suite with no exclusions this pass (at
explicit user request) -- see the standalone note in this same session's README/DECISIONS history
for the known-flaky-test result under that condition.

### What this ADR does NOT include

No NF's own existing logic calls this new route (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #39 of free5GC's ~42+ real `Nudr_DataRepository` resources; roughly 3 remain a real,
open, disclosed gap (docs/CAPABILITY_GAP_ANALYSIS.md). Task #106 remains open (not fully closed).

