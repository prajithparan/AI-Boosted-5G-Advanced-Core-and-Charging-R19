## ADR-0128: gap-closure task #106 continuation -- UDR real V2X Subscription Data

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (40 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0127). Real, confirmed-by-YAML-read:
`TS29505_Subscription_Data.yaml`'s `/subscription-data/{ueId}/v2x-data` (real schema
`V2xSubscriptionData`, `TS29503_Nudm_SDM.yaml` -- `nrV2xServicesAuth`/`lteV2xServicesAuth`/
`nrUePc5Ambr`/`ltePc5Ambr`, every field optional) is genuinely `GET`-only (`QueryV2xData`) -- no
create/update operation exists at all, confirmed by direct read of the block between this path and
the next (`/subscription-data/{ueId}/prose-data`). Genuinely NOT part of the `provisioned-data`
group -- keyed by `ueId` alone, not the `(ueId, servingPlmnId)` composite key that group's own
sub-resources share, so this needs its own store/table rather than another column on
`udr_provisioned_data`. Same real "provisioned out-of-band, seeded at startup" shape already
established for `CoverageRestrictionDataStore`.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_v2x_data` table (`ue_id` PK, `data` JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `V2xDataStore` class (`seed`/`get`), same shape as
  `CoverageRestrictionDataStore`/`OdbDataStore`.
- `nfs/udr/src/main.cpp`: one new `GET` route at `/subscription-data/{ueId}/v2x-data` and one new
  OTel counter. Seeded for the same two real test SUPIs every other GET-only UDR resource seeds,
  with `nrV2xServicesAuth.vehicleUeAuth: "AUTHORIZED"` (a real enum value, this project's own
  representative test choice).

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl against a running `udr` process backed by a real PostgreSQL database: `GET` on the
seeded SUPI (`imsi-999700000000001`) -> real `200` with
`{"nrV2xServicesAuth":{"vehicleUeAuth":"AUTHORIZED"}}`; `GET` on an unseeded SUPI
(`imsi-999700000000099`) -> real `404`. Direct `psql` query against `udr_v2x_data` independently
confirmed both seeded rows match.

### Testing and verification

`udr` built clean. Full `conformance_tests`: unchanged pass count (same disclosed
manual-live-verification precedent already established for every GET-only seeded resource in this
series), zero regressions (325/325).

### What this ADR does NOT include

No NF's own existing logic calls this new route (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #41 of free5GC's ~42+ real `Nudr_DataRepository` resources; roughly 1 remains a real,
open, disclosed gap (docs/CAPABILITY_GAP_ANALYSIS.md) -- not counting the genuinely deferred
subsystems (`ee-subscriptions`/`sdm-subscriptions`, `subs-to-notify`, `pdtq-data`,
`mbs-session-pol-data`) and the not-yet-surveyed remainder (`prose-data`, `uc-data`,
`time-sync-data`, `group-data/*`, `nidd-authorization-data`, and others). Task #106 remains open
(not fully closed).

