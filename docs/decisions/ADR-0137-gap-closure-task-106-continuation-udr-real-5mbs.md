## ADR-0137: gap-closure task #106 continuation -- UDR real 5MBS Subscription Data (Document)

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (48 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0136). Real, confirmed-by-YAML-read:
`TS29505_Subscription_Data.yaml`'s `/subscription-data/{ueId}/5mbs-data` (real spec `operationId`
`Query5mbsData`, real schema `MbsSubscriptionData` -- `TS29503_Nudm_SDM.yaml` --
`mbsAllowed`/`mbsSessionIdList`/`ueMbsAssistanceInfo`, every field optional) is genuinely
`GET`-only -- no create/update operation exists, confirmed by direct read of the block between
this path and its neighbors, no complex or required query parameters. Genuinely NOT part of the
`provisioned-data` group -- keyed by `ueId` alone, same key shape as `ranging-slpos-data`
(ADR-0136), so backed by its own new store/table.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_5mbs_data` table (`ue_id` PK, `data` JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `MbsDataStore` class (`seed`/`get`), same shape as
  `RangingSlPosDataStore`.
- `nfs/udr/src/main.cpp`: one new `GET` route at `/subscription-data/{ueId}/5mbs-data` and one
  new OTel counter. Seeded for the same two real test SUPIs every other GET-only UDR resource
  seeds, with `mbsAllowed: true` (a real, simple boolean field), this project's own representative
  test choice.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl against a running `udr` process backed by a real PostgreSQL database: `GET` on the
seeded SUPI (`imsi-999700000000001`) -> real `200` with `{"mbsAllowed":true}`; `GET` on an
unseeded SUPI (`imsi-999700000000099`) -> real `404`; cross-checked the sibling
`ranging-slpos-data` resource (a separate table) on the same UE is unaffected -> real `200` with
the unchanged expected body; second seeded SUPI (`imsi-999700000000002`) independently confirmed.
Direct `psql` query against `udr_5mbs_data` independently confirmed both seeded rows match.

### Testing and verification

`udr` built clean (including a clean rebuild after `clang-format-18`, no formatting diff beyond
what was written). Full `conformance_tests` (excluding the two disclosed pre-existing flaky
tests): 325/325 pass, zero regressions.

### What this ADR does NOT include

No NF's own existing logic calls this new route (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #49 of free5GC's ~42+ real `Nudr_DataRepository` resources. Task #106 remains open:
the not-yet-surveyed remainder of `TS29505_Subscription_Data.yaml` (`group-data/*`,
`service-specific-authorization-data/{serviceType}`,
`context-data/service-specific-authorizations/{serviceType}`, bare `/subscription-data/{ueId}`,
`ue-update-confirmation-data/subscribed-snssais`, `ue-update-confirmation-data/subscribed-cag`,
and others) and the genuinely deferred subsystems (`ee-subscriptions`/`sdm-subscriptions`,
`subs-to-notify`, `pdtq-data`, `mbs-session-pol-data`, `nidd-authorization-data`,
`Nudr_GroupIDmap`'s own `/nf-group-ids`) remain real, open, disclosed gaps.

