## ADR-0136: gap-closure task #106 continuation -- UDR real Ranging and Sidelink Positioning Service Subscription Data

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (47 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0135). Real, confirmed-by-YAML-read:
`TS29505_Subscription_Data.yaml`'s `/subscription-data/{ueId}/ranging-slpos-data` (real spec
`operationId` `QueryRangingSlPosData`, real schema `RangingSlPosSubscriptionData` --
`TS29503_Nudm_SDM.yaml` -- `rangingSlPosAuth`/`rangingSlPosPlmn`/`rangingSlPosQos`, every
top-level field optional) is genuinely `GET`-only -- no create/update operation exists, confirmed
by direct read of the block between this path and its neighbors, no complex or required query
parameters (distinct from its own sibling `rangingsl-privacy-data`'s optional `fields` filter --
this resource doesn't even have that). Genuinely NOT part of the `provisioned-data` group -- keyed
by `ueId` alone, same key shape as `rangingsl-privacy-data` (ADR-0135), so backed by its own new
store/table.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_ranging_slpos_data` table (`ue_id` PK, `data` JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `RangingSlPosDataStore` class (`seed`/`get`), same shape
  as `RangingSlPrivacyDataStore`.
- `nfs/udr/src/main.cpp`: one new `GET` route at `/subscription-data/{ueId}/ranging-slpos-data`
  and one new OTel counter. Seeded for the same two real test SUPIs every other GET-only UDR
  resource seeds, with `rangingSlPosAuth.rgSlPosPc5Auth: "AUTHORIZED"` (a real enum value,
  `UeAuth` -- `TS29571_CommonData.yaml`), this project's own representative test choice.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl against a running `udr` process backed by a real PostgreSQL database: `GET` on the
seeded SUPI (`imsi-999700000000001`) -> real `200` with
`{"rangingSlPosAuth":{"rgSlPosPc5Auth":"AUTHORIZED"}}`; `GET` on an unseeded SUPI
(`imsi-999700000000099`) -> real `404`; cross-checked the sibling `rangingsl-privacy-data`
resource (a separate table) on the same UE is unaffected -> real `200` with the unchanged expected
body; second seeded SUPI (`imsi-999700000000002`) independently confirmed. Direct `psql` query
against `udr_ranging_slpos_data` independently confirmed both seeded rows match.

### Testing and verification

`udr` built clean (including a clean rebuild after `clang-format-18`, no formatting diff beyond
what was written). Full `conformance_tests` (excluding the two disclosed pre-existing flaky
tests): 325/325 pass, zero regressions.

### What this ADR does NOT include

No NF's own existing logic calls this new route (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #48 of free5GC's ~42+ real `Nudr_DataRepository` resources. Task #106 remains open:
the not-yet-surveyed remainder of `TS29505_Subscription_Data.yaml` (`group-data/*`, `5mbs-data`,
`service-specific-authorization-data/{serviceType}`,
`context-data/service-specific-authorizations/{serviceType}`, bare `/subscription-data/{ueId}`,
`ue-update-confirmation-data/subscribed-snssais`, `ue-update-confirmation-data/subscribed-cag`,
and others) and the genuinely deferred subsystems (`ee-subscriptions`/`sdm-subscriptions`,
`subs-to-notify`, `pdtq-data`, `mbs-session-pol-data`, `nidd-authorization-data`,
`Nudr_GroupIDmap`'s own `/nf-group-ids`) remain real, open, disclosed gaps.

