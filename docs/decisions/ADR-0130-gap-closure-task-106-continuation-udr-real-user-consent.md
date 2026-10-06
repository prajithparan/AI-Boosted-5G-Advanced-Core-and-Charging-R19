## ADR-0130: gap-closure task #106 continuation -- UDR real User Consent Subscription Data

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (42 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0129, at or past free5GC's own comparison
baseline). Real, confirmed-by-YAML-read: `TS29505_Subscription_Data.yaml`'s
`/subscription-data/{ueId}/uc-data` (real spec `operationId` `QueryUserConsentData`, real schema
`UcSubscriptionData` -- `TS29503_Nudm_SDM.yaml` -- a single optional `userConsentPerPurposeList`
map keyed by `UcPurpose` (real enum: `ANALYTICS`/`MODEL_TRAINING`/`NW_CAP_EXPOSURE`/
`EDGEAPP_UE_LOCATION`/`ENERGY`) to `UserConsent` (real enum: `CONSENT_NOT_GIVEN`/`CONSENT_GIVEN`),
no `required` fields at all) is genuinely `GET`-only -- no create/update operation exists,
confirmed by direct read of the block between this path and its neighbors. Genuinely NOT part of
the `provisioned-data` group -- keyed by `ueId` alone, same key shape as `prose-data` (ADR-0129),
so backed by its own new store/table.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_uc_data` table (`ue_id` PK, `data` JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `UcDataStore` class (`seed`/`get`), same shape as
  `ProseDataStore`/`V2xDataStore`.
- `nfs/udr/src/main.cpp`: one new `GET` route at `/subscription-data/{ueId}/uc-data` and one new
  OTel counter. Seeded for the same two real test SUPIs every other GET-only UDR resource seeds,
  with `userConsentPerPurposeList.ANALYTICS: "CONSENT_GIVEN"` (real enum values, this project's
  own representative test choice).

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl against a running `udr` process backed by a real PostgreSQL database: `GET` on the
seeded SUPI (`imsi-999700000000001`) -> real `200` with
`{"userConsentPerPurposeList":{"ANALYTICS":"CONSENT_GIVEN"}}`; `GET` on an unseeded SUPI
(`imsi-999700000000099`) -> real `404`; cross-checked the sibling `prose-data` resource (a
separate table) on the same UE is unaffected -> real `200` with the unchanged expected body;
second seeded SUPI (`imsi-999700000000002`) independently confirmed. Direct `psql` query against
`udr_uc_data` independently confirmed both seeded rows match.

### Testing and verification

`udr` built clean (including a clean rebuild after `clang-format-18`, no formatting diff beyond
what was written). Full `conformance_tests` (excluding the two disclosed pre-existing flaky
tests): 325/325 pass, zero regressions.

### What this ADR does NOT include

No NF's own existing logic calls this new route (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #43 of free5GC's ~42+ real `Nudr_DataRepository` resources -- past the free5GC
comparison baseline, which is now firmly a background metric rather than a meaningful completion
signal. Task #106 remains open: the not-yet-surveyed remainder of
`TS29505_Subscription_Data.yaml` (`time-sync-data`, `group-data/*`, `nidd-authorization-data`, and
others) and the genuinely deferred subsystems (`ee-subscriptions`/`sdm-subscriptions`,
`subs-to-notify`, `pdtq-data`, `mbs-session-pol-data`, `Nudr_GroupIDmap`'s own `/nf-group-ids`)
remain real, open, disclosed gaps.

