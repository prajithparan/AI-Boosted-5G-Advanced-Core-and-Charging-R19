## ADR-0151: gap-closure task #106 continuation -- UDR real SDM Subscriptions collection +
individual document resource, correcting ADR-0122's second bundled deferral

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (60 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0149, before this session's CHF-focused ADR-0150
detour). ADR-0122 originally bundled `ee-subscriptions`/`sdm-subscriptions` together as blanket
"genuinely deeply-nested" and deferred both without an individual read of either. ADR-0148 (this
session) corrected `ee-subscriptions` on direct read; this ADR performs the same correction for
`sdm-subscriptions`, its real, structurally-parallel sibling.

Real, confirmed-by-YAML-read: `/subscription-data/{ueId}/context-data/sdm-subscriptions` (real
spec operations `Querysdmsubscriptions`/`CreateSdmSubscriptions`, a collection GET+POST, real
path-scoped by `ueId`, no query-param blockers) and
`/subscription-data/{ueId}/context-data/sdm-subscriptions/{subsId}` (real spec operations
`QuerysdmSubscription`/`Updatesdmsubscriptions`/`ModifysdmSubscription`/`RemovesdmSubscriptions`
-- the individual document's GET+PUT+PATCH+DELETE) are structurally identical to
`ee-subscriptions` in every real, load-bearing way: `subsId` server-generated (confirmed by the
same real `Location` header format,
`.../context-data/sdm-subscriptions/{subsId}`, and the absence of `subsId` as a request field on
`POST`); `Updatesdmsubscriptions` (PUT) genuinely update-only, never create (real spec `404`,
"update of non-existing resource is rejected", identical wording to `ee-subscriptions`'s own);
`ModifysdmSubscription` (PATCH) real RFC 6902. Schema `SdmSubscription` -- required `nfInstanceId`
+ `callbackReference` (Uri) + `monitoredResourceUris` (array of Uri), plus a real dozen-plus
optional fields (already generated as `sbi_gen::SdmSubscription`, checked field-for-field against
the real YAML before use).

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_sdm_subscriptions` table (`ue_id`, `subs_id` composite
  PK, `data` JSONB) -- identical shape to `udr_ee_subscriptions`.
- `nfs/udr/src/stores.hpp`/`.cpp`: new `SdmSubscriptionsStore` class (`create`/`get`/`list`/
  `update`/`apply_patch`/`remove`), structurally identical to `EeSubscriptionsStore`.
- `nfs/udr/src/main.cpp`: store construction, five new OTel counters (create, list, get, write
  [PUT+PATCH share it], delete), and six new routes: `GET`/`POST` on the collection path, `GET`/
  `PUT`/`PATCH`/`DELETE` on the individual path. `subsId` generated via
  `sbi_core::generate_uuid_v4()`; `Location` header follows the same established
  append-to-collection-path convention as `ee-subscriptions`/`smf-registrations`.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl sequence against a running `udr` process (freshly built, freshly started, registered
with a freshly started `nrf`) backed by real PostgreSQL, SUPI `imsi-999700000000015`:

- `GET` the collection before any `POST` -> real `200` with an empty array.
- `POST` with `{"nfInstanceId":"<uuid>","callbackReference":"https://amf.example.com/notify",
  "monitoredResourceUris":["https://udr.example.com/subscription-data/.../am-data"]}` -> real
  `201`, a real, freshly-generated UUID v4 `subsId` in `Location`, body echoed back.
- `GET` the individual resource by that `subsId` -> real `200` with the same body; `GET` the
  collection -> real `200` with a one-element array.
- `PUT` an update to the same `subsId` -> real `204`; `GET` after -> real `200` reflecting the
  new `callbackReference`/`monitoredResourceUris`.
- `PUT` to a nonexistent `subsId` -> real `404`, confirming update-only semantics.
- `PATCH` with a real RFC 6902 `[{"op":"replace","path":"/callbackReference","value":"..."}]` ->
  real `204`; `GET` after -> real `200` reflecting the patched value.
- Direct `psql SELECT` against `udr_sdm_subscriptions` independently confirmed the single row
  matched curl's response exactly at this point.
- Cross-checked the sibling `ee-subscriptions` collection for the same UE -> real `200` with an
  empty array, confirming the two subscription tables are genuinely independent, not accidentally
  sharing storage.
- `DELETE` -> real `204`; individual `GET` after -> real `404`; collection `GET` after -> real
  `200` with an empty array again; `psql SELECT` confirmed zero rows remained.

### Testing and verification

`udr` built clean (zero warnings) both before and after `clang-format-18` (reformat added no diff
beyond what was newly written). Full `conformance_tests` (excluding the two disclosed pre-existing
flaky tests): 325/325 pass, zero regressions.

### What this ADR does NOT include

No NF's own existing logic calls these new routes (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). Real,
disclosed scope narrowing, same shape as `ee-subscriptions`: only the collection + individual
document are implemented -- the deeper `hss-sdm-subscriptions` nested sub-collection under each
`subsId` (a real, separate resource with its own path) remains genuinely deferred, not built.
This closes UDR resource #61 of free5GC's ~42+ real `Nudr_DataRepository` resources -- completing
the correction of ADR-0122's original bundled deferral for both `ee-subscriptions` and
`sdm-subscriptions`. Task #106 remains open: `group-data`'s remaining genuinely-blocked
resources, bare `/subscription-data/{ueId}`/`{ueId}/context-data`, `ee-subscriptions`'s/
`sdm-subscriptions`'s own nested sub-collections, and the other genuinely deferred subsystems
(`pdtq-data`, `mbs-session-pol-data`, `nidd-authorization-data`,
`service-specific-authorization-data/{serviceType}`, `Nudr_GroupIDmap`'s own `/nf-group-ids`)
remain real, open, disclosed gaps.

