## ADR-0133: gap-closure task #106 continuation -- UDR real UE's Location Information (Document)

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (44 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0131). Real, confirmed-by-YAML-read: surveyed two
candidates from the not-yet-surveyed remainder in the same pass:

- `/subscription-data/{ueId}/nidd-authorization-data` (real spec `operationId` `GetNiddAuData`) --
  genuinely blocked, not attempted: its real, required query parameters include `single-nssai`,
  which the spec defines with `content: application/json` (a JSON-encoded object passed as a query
  string value), a real complex-object query-parameter shape this project has no existing
  precedent for parsing -- the same class of gap already disclosed as blocking `pdtq-data`.
- `/subscription-data/{ueId}/context-data/location` (real spec `operationId` `QueryUeLocation`,
  real schema `LocationInfo` -- `TS29503_Nudm_UECM.yaml`) -- genuinely `GET`-only, no required or
  complex query parameters, no create/update operation exists at all. This is the one implemented
  by this ADR. Real schema requires a non-empty `registrationLocationInfoList` array, each entry
  requiring `amfInstanceId` (a real UUID-format `NfInstanceId`) and `accessTypeList` (real
  `AccessType` enum, `TS29571_CommonData.yaml`). Genuinely NOT part of the `provisioned-data`
  group -- keyed by `ueId` alone, same key shape as `time-sync-data` (ADR-0131), so backed by its
  own new store/table.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_location_data` table (`ue_id` PK, `data` JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `LocationDataStore` class (`seed`/`get`), same shape as
  `TimeSyncDataStore`/`UcDataStore`.
- `nfs/udr/src/main.cpp`: one new `GET` route at `/subscription-data/{ueId}/context-data/location`
  and one new OTel counter. Seeded for the same two real test SUPIs every other GET-only UDR
  resource seeds, with a synthetic test AMF instance ID
  (`00000000-0000-4000-8000-00000000a001`) and `3GPP_ACCESS` (a real enum value,
  `TS29571_CommonData.yaml`), this project's own representative test choice.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl against a running `udr` process backed by a real PostgreSQL database: `GET` on the
seeded SUPI (`imsi-999700000000001`) -> real `200` with
`{"registrationLocationInfoList":[{"accessTypeList":["3GPP_ACCESS"],"amfInstanceId":"00000000-0000-4000-8000-00000000a001"}]}`;
`GET` on an unseeded SUPI (`imsi-999700000000099`) -> real `404`; cross-checked the sibling
`time-sync-data` resource (a separate table) on the same UE is unaffected -> real `200` with the
unchanged expected body; second seeded SUPI (`imsi-999700000000002`) independently confirmed.
Direct `psql` query against `udr_location_data` independently confirmed both seeded rows match.

### Testing and verification

`udr` built clean (including a clean rebuild after `clang-format-18`, no formatting diff beyond
what was written). Full `conformance_tests` (excluding the two disclosed pre-existing flaky
tests): 325/325 pass, zero regressions.

### What this ADR does NOT include

No NF's own existing logic calls this new route (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #45 of free5GC's ~42+ real `Nudr_DataRepository` resources. `nidd-authorization-data`
remains a real, disclosed gap blocked on complex-object query-parameter parsing (see Context
above) -- not silently skipped, explicitly flagged for a future turn once that parsing precedent
exists. Task #106 remains open: the not-yet-surveyed remainder of `TS29505_Subscription_Data.yaml`
(`group-data/*`, `a2x-data`, `rangingsl-privacy-data`, `ranging-slpos-data`, `5mbs-data`,
`service-specific-authorization-data/{serviceType}`,
`context-data/service-specific-authorizations/{serviceType}`, bare `/subscription-data/{ueId}`,
`ue-update-confirmation-data/subscribed-snssais`, `ue-update-confirmation-data/subscribed-cag`,
and others) and the genuinely deferred subsystems (`ee-subscriptions`/`sdm-subscriptions`,
`subs-to-notify`, `pdtq-data`, `mbs-session-pol-data`, `nidd-authorization-data`,
`Nudr_GroupIDmap`'s own `/nf-group-ids`) remain real, open, disclosed gaps.

