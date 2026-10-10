## ADR-0104: gap-closure task #106 continuation -- UDR real LCS Subscription Data

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (18 of free5GC's ~42+ real
TS 29.504 resources closed as of ADR-0103). Real, confirmed-by-YAML-read:
`TS29505_Subscription_Data.yaml`'s `/subscription-data/{ueId}/lcs-subscription-data` (real schema
`LcsSubscriptionData`, `$ref`'d verbatim from `TS29503_Nudm_SDM.yaml` -- optional
`configuredLmfId`, `pruInd`, `lpHapType`, `userPlanePosIndLmf`, every field optional) is a real,
genuinely GET-only resource (`QueryLcsSubscriptionData`) -- confirmed by grepping every
operationId referencing this path, no create/update operation exists at all, same real
"provisioned out-of-band, seeded at startup" shape already established for `LcsPrivacyDataStore`
(ADR-0103) and its own siblings.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_lcs_subscription_data` table (`ue_id` PK, `data` JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `LcsSubscriptionDataStore` class (`seed`/`get` only,
  matching `LcsPrivacyDataStore`'s own real GET-only shape).
- `nfs/udr/src/main.cpp`: one new route (`GET`) at `/subscription-data/{ueId}/lcs-subscription-data`,
  and a real seed loop for the same two real test SUPIs every other GET-only UDR resource in this
  project already seeds, populated with `{"pruInd":"NON_PRU","userPlanePosIndLmf":false}` --
  `NON_PRU` is a real enum value from `PruInd` (`TS29503_Nudm_SDM.yaml`), this project's own
  representative test choice; `userPlanePosIndLmf: false` matches the schema's own documented
  `default: false` -- and one new OTel get counter.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl against a running `udr` process backed by a real PostgreSQL database: `GET` for the real
seeded SUPI `imsi-999700000000001` -> real `200` with the exact seeded
`pruInd`/`userPlanePosIndLmf` document; `GET` for an unseeded SUPI -> real `404`. Direct `psql`
query against `udr_lcs_subscription_data` independently confirmed both seeded rows persisted
correctly across the fresh startup.

### Testing and verification

`udr` built clean. Full `conformance_tests`: unchanged pass count (no new committed automated test
this pass, same disclosed manual-live-verification precedent already established), zero
regressions.

### What this ADR does NOT include

No NF's own existing logic calls this new route (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure) --
`configuredLmfId`/`lpHapType` are left unpopulated in the seed data, a real, disclosed gap rather
than a guessed nested shape, same precedent as this resource's own siblings. This closes UDR
resource #19 of free5GC's ~42+; roughly 23 remain a real, open, disclosed gap
(docs/CAPABILITY_GAP_ANALYSIS.md). Task #106 remains open (not fully closed).

