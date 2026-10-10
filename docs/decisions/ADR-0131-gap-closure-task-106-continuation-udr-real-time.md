## ADR-0131: gap-closure task #106 continuation -- UDR real Time Synchronization Subscription Data

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (43 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0130, past free5GC's own comparison baseline).
Real, confirmed-by-YAML-read: `TS29505_Subscription_Data.yaml`'s
`/subscription-data/{ueId}/time-sync-data` (real spec `operationId`
`QueryTimeSyncSubscriptionData`, real schema `TimeSyncSubscriptionData` --
`TS29503_Nudm_SDM.yaml`) is genuinely `GET`-only -- no create/update operation exists, confirmed
by direct read of the block between this path and its neighbors. Genuinely NOT part of the
`provisioned-data` group -- keyed by `ueId` alone, same key shape as `uc-data` (ADR-0130), so
backed by its own new store/table. Real, disclosed difference from the last several GET-only
resources closed: `TimeSyncSubscriptionData` has real `required` fields --
`afReqAuthorizations` (itself a real `oneOf` requiring either `gptpAllowedInfoList` or
`astiAllowedInfo`) and `serviceIds` (an array of `TimeSyncServiceId`, each requiring a `reference`
string) -- so the seed data is a minimal but genuinely spec-valid body, not an all-optional
placeholder.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_time_sync_data` table (`ue_id` PK, `data` JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `TimeSyncDataStore` class (`seed`/`get`), same shape as
  `UcDataStore`/`ProseDataStore`.
- `nfs/udr/src/main.cpp`: one new `GET` route at `/subscription-data/{ueId}/time-sync-data` and
  one new OTel counter. Seeded for the same two real test SUPIs every other GET-only UDR resource
  seeds, with a minimal real-shaped body: `afReqAuthorizations.gptpAllowedInfoList` containing one
  `GptpAllowedInfo` (`dnn: "internet"`, `gptpAllowed: true`) and `serviceIds` containing one
  `TimeSyncServiceId` (`reference: "ts-service-1"`) -- this project's own representative test
  choice, satisfying every real `required` constraint.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl against a running `udr` process backed by a real PostgreSQL database: `GET` on the
seeded SUPI (`imsi-999700000000001`) -> real `200` with
`{"afReqAuthorizations":{"gptpAllowedInfoList":[{"dnn":"internet","gptpAllowed":true}]},"serviceIds":[{"reference":"ts-service-1"}]}`;
`GET` on an unseeded SUPI (`imsi-999700000000099`) -> real `404`; cross-checked the sibling
`uc-data` resource (a separate table) on the same UE is unaffected -> real `200` with the
unchanged expected body; second seeded SUPI (`imsi-999700000000002`) independently confirmed.
Direct `psql` query against `udr_time_sync_data` independently confirmed both seeded rows match.

### Testing and verification

`udr` built clean (including a clean rebuild after `clang-format-18`, no formatting diff beyond
what was written). Full `conformance_tests` (excluding the two disclosed pre-existing flaky
tests): 325/325 pass, zero regressions.

### What this ADR does NOT include

No NF's own existing logic calls this new route (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #44 of free5GC's ~42+ real `Nudr_DataRepository` resources. Task #106 remains open:
the not-yet-surveyed remainder of `TS29505_Subscription_Data.yaml` (`group-data/*`,
`nidd-authorization-data`, `a2x-data`, `rangingsl-privacy-data`, `ranging-slpos-data`,
`5mbs-data`, `service-specific-authorization-data/{serviceType}`,
`context-data/service-specific-authorizations/{serviceType}`, `context-data/location`, bare
`/subscription-data/{ueId}`, `ue-update-confirmation-data/subscribed-snssais`,
`ue-update-confirmation-data/subscribed-cag`, and others) and the genuinely deferred subsystems
(`ee-subscriptions`/`sdm-subscriptions`, `subs-to-notify`, `pdtq-data`, `mbs-session-pol-data`,
`Nudr_GroupIDmap`'s own `/nf-group-ids`) remain real, open, disclosed gaps.

