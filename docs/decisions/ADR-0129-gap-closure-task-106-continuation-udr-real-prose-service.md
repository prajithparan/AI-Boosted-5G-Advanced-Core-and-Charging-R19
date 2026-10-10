## ADR-0129: gap-closure task #106 continuation -- UDR real ProSe Service Subscription Data

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (41 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0128). Real, confirmed-by-YAML-read:
`TS29505_Subscription_Data.yaml`'s `/subscription-data/{ueId}/prose-data` (real schema
`ProseSubscriptionData`, `TS29503_Nudm_SDM.yaml` -- `proseServiceAuth`/`nrUePc5Ambr`/
`proseAllowedPlmn`, every field optional) is genuinely `GET`-only -- no create/update operation
exists at all, confirmed by direct read of the block between this path and the next
(`/subscription-data/{ueId}/{servingPlmnId}/provisioned-data/lcs-bca-data`). Real, disclosed: the
spec's own `operationId` for this resource is literally `QueryPorseData` (a real typo in
`TS29505_Subscription_Data.yaml` itself -- "Porse" instead of "Prose") -- cited as-is in code
comments, not silently corrected, consistent with this project's own "never invent or fix spec
text" rule. Genuinely NOT part of the `provisioned-data` group -- keyed by `ueId` alone, same key
shape as `v2x-data` (ADR-0128), so backed by its own new store/table.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_prose_data` table (`ue_id` PK, `data` JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `ProseDataStore` class (`seed`/`get`), same shape as
  `V2xDataStore`/`CoverageRestrictionDataStore`.
- `nfs/udr/src/main.cpp`: one new `GET` route at `/subscription-data/{ueId}/prose-data` and one
  new OTel counter. Seeded for the same two real test SUPIs every other GET-only UDR resource
  seeds, with `proseServiceAuth.proseDirectDiscoveryAuth: "AUTHORIZED"` (a real enum value, this
  project's own representative test choice).

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl against a running `udr` process backed by a real PostgreSQL database: `GET` on the
seeded SUPI (`imsi-999700000000001`) -> real `200` with
`{"proseServiceAuth":{"proseDirectDiscoveryAuth":"AUTHORIZED"}}`; `GET` on an unseeded SUPI
(`imsi-999700000000099`) -> real `404`; cross-checked the sibling `v2x-data` resource (a separate
table) on the same UE is unaffected -> real `200` with the unchanged expected body. Direct `psql`
query against `udr_prose_data` independently confirmed both seeded rows match.

### Testing and verification

`udr` built clean. Full `conformance_tests`: unchanged pass count (same disclosed
manual-live-verification precedent already established for every GET-only seeded resource in this
series), zero regressions (325/325).

### What this ADR does NOT include

No NF's own existing logic calls this new route (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #42 of free5GC's ~42+ real `Nudr_DataRepository` resources -- UDR now matches or
exceeds free5GC's own real resource-type count on this specific comparison. This does **not** mean
task #106 is closed: the not-yet-surveyed remainder of `TS29505_Subscription_Data.yaml`
(`uc-data`, `time-sync-data`, `group-data/*`, `nidd-authorization-data`, and others) and the
genuinely deferred subsystems (`ee-subscriptions`/`sdm-subscriptions`, `subs-to-notify`,
`pdtq-data`, `mbs-session-pol-data`, `Nudr_GroupIDmap`'s own `/nf-group-ids`) remain real, open,
disclosed gaps. Task #106 remains open (not fully closed).

