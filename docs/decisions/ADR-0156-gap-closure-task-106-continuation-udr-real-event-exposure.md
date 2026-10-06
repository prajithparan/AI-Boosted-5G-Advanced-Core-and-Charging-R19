## ADR-0156: gap-closure task #106 continuation -- UDR real Event Exposure Group Subscriptions
(group-data-scoped) collection + individual document, plus a real Location-header bug fix in two
pre-existing siblings

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (65 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0155). This ADR surveys and closes
`group-data`'s own parallel `ee-subscriptions/{subsId}/...` tree, previously flagged as "not yet
surveyed" in ADR-0152/ADR-0153/ADR-0155's own disclosure sections.

Real, confirmed-by-YAML-read: `/subscription-data/group-data/{ueGroupId}/ee-subscriptions` (real
spec operations `QueryEeGroupSubscriptions` [GET]/`CreateEeGroupSubscriptions` [POST]) and
`/subscription-data/group-data/{ueGroupId}/ee-subscriptions/{subsId}` (real spec operations
`QueryEeGroupSubscription` [GET]/`UpdateEeGroupSubscriptions` [PUT]/`ModifyEeGroupSubscription`
[PATCH]/`RemoveEeGroupSubscriptions` [DELETE]) is the group-data-scoped sibling of
`ee-subscriptions` (ADR-0148), structurally identical but keyed by `ueGroupId` instead of `ueId`:
same `EeSubscription` schema, PUT genuinely update-only (real spec 404 text "update of
non-existing resource is rejected"). The individual GET response schema has the exact same real
`items:` (without `type: array`) authoring artifact already found and resolved in ADR-0148's own
`QueryeeSubscription` -- treated identically here: returns a single `EeSubscription` object, not
an array.

**Real, disclosed bug found live-verifying this new resource, before considering it done:** the
`POST` create handler's `Location` header was built from the raw route-registration pattern
string (e.g. `.../subscription-data/group-data/{ueGroupId}/ee-subscriptions/<subsId>`) rather
than the actual request path -- the literal, unsubstituted `{ueGroupId}` placeholder text was
being returned to the client instead of the real group ID. Checked whether this was new-code-only
or pre-existing: it is pre-existing. `ee-subscriptions`' own `CreateEeSubscriptions` handler
(ADR-0148) and `sdm-subscriptions`' own `CreateSdmSubscriptions` handler (ADR-0151) build their
`Location` headers the exact same way and have the exact same bug -- confirmed by live curl
against both already-shipped resources before writing the fix. `subs-to-notify` (ADR-0149) was
checked and confirmed NOT affected: its collection path has no path-parameter placeholder at all.
Fixed all three occurrences in this same ADR (a small, mechanical, unambiguous correctness fix --
substituting the real path-parameter value into a directly-constructed URI string instead of
concatenating the raw pattern) rather than shipping a fourth resource with a known-broken
`Location` header while leaving two already-merged ones broken. Both pre-existing fixes were
re-verified live against a freshly rebuilt `udr` before this ADR was written.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_group_ee_subscriptions` table (`ue_group_id`,
  `subs_id` composite PK, `data` JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `GroupEeSubscriptionsStore` class (`create`/`get`/`list`/
  `update`/`apply_patch`/`remove`), structurally identical to `EeSubscriptionsStore` (ADR-0148)
  but parameterized by `ue_group_id`.
- `nfs/udr/src/main.cpp`: store construction, five new OTel counters (create, list, get, write
  [PUT+PATCH share it], delete), and six new routes (GET+POST on the collection,
  GET+PUT+PATCH+DELETE on the individual document) at `.../group-data/{ueGroupId}/
  ee-subscriptions`. `subsId` is server-generated (real UUID v4).
- Bug fix: `ee_subscriptions`' (line ~4104), `sdm_subscriptions`' (line ~4269), and this new
  resource's own POST-create handlers now build the `Location` header by directly concatenating
  `kApiRoot` + the real path-parameter value(s) + the resource segment + `subsId`, instead of
  concatenating the raw `..._collection_path_pattern` string (which still contains the
  unsubstituted `{ueId}`/`{ueGroupId}` route-registration placeholder).

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl sequence against a running `udr` process (freshly built, freshly started, registered
with a freshly started `nrf`, mTLS client cert + real NRF-issued OAuth2 bearer token):

- `GET` collection for a fresh `ueGroupId` `grp-test-001` -> real `200 []`.
- `POST` with `{"callbackReference":"...","monitoringConfigurations":{...}}` -> real `201`, body
  echoed back, **`Location` header confirmed to contain the real `grp-test-001`**, not
  `{ueGroupId}` (the bug found and fixed above).
- `GET` individual document by the returned `subsId` -> real `200` with a single `EeSubscription`
  object (not wrapped in an array), confirming the `items:`-artifact handling matches ADR-0148's
  own precedent.
- `PUT` update -> real `204`; `GET` after confirms a genuine overwrite.
- `PATCH` (real RFC 6902) -> real `204`; `GET` after reflects the patched value.
- `PUT` against a nonexistent `subsId` -> real `404` (update-only PUT, matching ADR-0148).
- Direct `psql SELECT` against `udr_group_ee_subscriptions` independently confirmed all three
  test rows (one per `POST`/re-tested-bug run) matched curl's responses, keyed by `ue_group_id`.
- Cross-checked `udr_ee_subscriptions` -> confirmed genuinely separate storage (the only row
  present was this ADR's own re-verification test on that sibling resource, not group data).
- **Re-verified the `Location`-header fix on both pre-existing siblings**: a fresh `POST` to
  `ee-subscriptions` (`imsi-999700000000099`) returned a `Location` header with the real UE ID;
  a fresh `POST` to `sdm-subscriptions` (`imsi-999700000000098`, using its own required
  `monitoredResourceUris` field, confirmed from the generated `SdmSubscription` struct) likewise
  returned a `Location` header with the real UE ID.
- `DELETE` on all three group-data test rows -> real `204` each; `psql SELECT` confirmed zero
  `udr_group_ee_subscriptions` rows remained. All sibling test rows (`ee_subscriptions`,
  `sdm_subscriptions`) cleaned up directly via `psql DELETE` (they were verified via GET/Location
  header only, not the resource's own DELETE route, since the point was the header, not the
  DELETE path).

### Testing and verification

`udr` built clean (zero warnings) both before and after `clang-format-18` (reformat added no diff
beyond what was newly written). Full `conformance_tests` (excluding the two disclosed pre-existing
flaky tests): 325/325 pass, zero regressions.

### What this ADR does NOT include

No NF's own existing logic calls these new routes (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #66 of free5GC's ~42+ real `Nudr_DataRepository` resources. Real, disclosed scope
narrowing kept from ADR-0148's own precedent: only the collection + individual document are
implemented here -- the deeper group-data-scoped `amf-subscriptions`/`smf-subscriptions`/
`hss-subscriptions` nested sub-collections under each `subsId` (confirmed to exist at
`group-data/{ueGroupId}/ee-subscriptions/{subsId}/amf-subscriptions` etc. in the YAML during this
survey) remain genuinely deferred, not yet built. Task #106 remains open: those three nested
sub-collections, `group-data`'s remaining genuinely-blocked resources, bare
`/subscription-data/{ueId}`/`{ueId}/context-data`, and the other genuinely deferred subsystems
(`pdtq-data`, `mbs-session-pol-data`, `nidd-authorization-data`,
`service-specific-authorization-data/{serviceType}`, `Nudr_GroupIDmap`'s own `/nf-group-ids`)
remain real, open, disclosed gaps.

