## ADR-0164: gap-closure task #106 continuation -- UDR real GetNfGroupIDs, the second resource
genuinely unblocked by ADR-0161's array-parsing infra

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (71 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0162, plus one real distinct-API resource from
ADR-0120, `GetRoutingIDs`). This ADR closes `GetNfGroupIDs` (`/nf-group-ids`,
`TS29504_Nudr_GroupIDmap.yaml` lines 28-84) -- the same real `Nudr_GroupIDmap` service as
`GetRoutingIDs`, NOT `Nudr_DataRepository` -- confirming by direct read that it really was blocked
by exactly the `style: form, explode: false` array-query-param gap ADR-0161 closed, the same
resource ADR-0162's own "What this ADR does NOT include" section named as a real candidate.

Real, confirmed-by-YAML-read: real REQUIRED `nf-type` array query param (`style: form,
explode: false`, `minItems: 1`) and real REQUIRED `subscriberId` (`$ref:
'#/components/schemas/SubscriberId'`, a plain string). Response `200` schema is `type: object,
additionalProperties: NfGroupId, minProperties: 1` -- a real map `{NFType: NfGroupId}`
(`NfGroupId` itself, from `TS29571_CommonData.yaml`, is a plain `type: string`). A real `404` is
documented for the no-match case.

Also surveyed, by direct read, `/nf-group-ids/subscriptions` and
`/nf-group-ids/subscriptions/{subscriptionId}` -- a real, separate `POST`/`GET`/`PATCH`/`DELETE`
change-notification subscription family with a real `onGroupIdMapChange` webhook callback.
Confirmed this is genuinely more complex than the bare `GetNfGroupIDs` endpoint and, matching this
project's own one-resource-per-turn discipline and the `subs-to-notify` precedent of not building
real outbound webhook delivery, deliberately scoped this turn to `GetNfGroupIDs` alone, leaving
the subscription family as a separate, disclosed, deferred item.

Confirmed by direct read that NO create/update/delete operation exists anywhere in
`Nudr_GroupIDmap.yaml` for the actual `(subscriberId, nfType) -> groupId` mapping data itself --
the file's only other write path is the subscription-management `POST` above, which creates a
subscription to be *notified* of future changes, not a way to *set* the mapping. This applies the
same "GET-only, no write path exists in the spec, seeded at startup" precedent already established
by `group_identifiers` (ADR-0140), `coverage-restriction-data`, `plmn-ue-policy-set`, and
especially the sibling `routing_ids`/`GetRoutingIDs` (ADR-0120, the closest structural template --
same `Nudr_GroupIDmap` service, same store-class shape).

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_nf_group_ids` table, composite-keyed by
  `(subscriber_id, nf_type)` per the resource's own two real required query parameters.
- `nfs/udr/src/stores.hpp`/`.cpp`: new `NfGroupIdStore` class (`seed`/`get`), mirroring
  `RoutingIdStore`'s shape but storing a plain `std::string` rather than `nlohmann::json`, since
  `NfGroupId` is just a string per its own schema.
- `nfs/udr/src/main.cpp`: store construction, real seed data (this project's own two already-seeded
  test SUPIs, `imsi-999700000000001`/`002`, each mapped to a real `NFType` enum value from
  `TS29510_Nnrf_NFManagement.yaml`'s own `NFType` schema -- `AMF`/`SMF` -- with real, arbitrary
  representative group-ID strings, since `NfGroupId` has no documented format to match), one new
  OTel counter, and the new `GET /nf-group-ids` route under `kGroupIdMapApiRoot`
  (`/nudr-group-id-map/v1`), using `sbi_core::http2::split_form_array()` (ADR-0161) to parse the
  required `nf-type` array.
- Real, disclosed design choice: unlike the aggregate live-view resources
  (`ue-update-confirmation-data`/`QueryContextData`, ADR-0147/ADR-0161) which always return `200`
  since those resources have no independent existence and the spec doesn't document a meaningful
  `404`, `GetNfGroupIDs`'s own response schema literally requires `minProperties: 1` and a real
  `404` is documented -- so this route returns a real `404` when the composed result map is empty,
  honoring the spec literally rather than reusing the aggregate-resource `200`-always precedent.
  A partial result (some requested `nf-type`s present, others absent) still returns `200` with
  only the present entries, since `minProperties: 1` is satisfied by a non-empty partial map.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl against a running `udr` process (freshly built, freshly started, registered with a
freshly started `nrf`, mTLS client cert + real NRF-issued OAuth2 bearer token), at the real
`/nudr-group-id-map/v1/nf-group-ids` path:

- `GET` with both query parameters missing -> real `400`.
- `GET` with only `nf-type` present (`subscriberId` missing) -> real `400`.
- `GET` with the seeded pair (`nf-type=AMF&subscriberId=imsi-999700000000001`) -> real `200` with
  `{"AMF":"amf-group-01"}`.
- `GET` with two comma-separated `nf-type` values on the same seeded subscriber
  (`nf-type=AMF,SMF&subscriberId=imsi-999700000000001`) -> real `200` with
  `{"AMF":"amf-group-01","SMF":"smf-group-01"}`, confirming `split_form_array()`'s real comma-split
  behavior end-to-end through this route.
- `GET` with an unseeded `(subscriberId, nf-type)` combination
  (`nf-type=PCF&subscriberId=imsi-999700000000001`) -> real `404`.
- `GET` with a mixed request against the second seeded subscriber, one present `nf-type` (`AMF`)
  and one absent (`SMF`) (`nf-type=AMF,SMF&subscriberId=imsi-999700000000002`) -> real `200` with
  only `{"AMF":"amf-group-02"}`, confirming the partial-match-still-200 design choice above (not a
  `404`, since the composed map was non-empty).
- Direct `psql SELECT` against `udr_nf_group_ids` independently confirmed exactly the three seeded
  rows (`imsi-999700000000001`/`AMF`/`amf-group-01`, `imsi-999700000000001`/`SMF`/`smf-group-01`,
  `imsi-999700000000002`/`AMF`/`amf-group-02`), matching every curl response above.

### Testing and verification

`udr` built clean (zero warnings) both before and after `clang-format-18` (reformat added no diff
beyond what was newly written). Full `conformance_tests` (excluding the two disclosed pre-existing
flaky tests): 331/331 pass, zero regressions -- same count as ADR-0162, since this ADR adds no new
automated test (same disclosed manual-live-verification precedent already established for
GET-only, seeded-at-startup resources like `GetRoutingIDs`/ADR-0120).

### What this ADR does NOT include

No NF's own existing logic calls this new route (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). Does **not**
increment the "N of free5GC's ~42+ `Nudr_DataRepository` resources" count -- still 71, unchanged
from ADR-0162 -- since this is a real, distinct `Nudr_GroupIDmap` API resource, same non-increment
precedent as `GetRoutingIDs`/ADR-0120. `Nudr_GroupIDmap`'s own remaining resources
(`/nf-group-ids/subscriptions` and `/nf-group-ids/subscriptions/{subscriptionId}`, the real
change-notification subscription family with a real `onGroupIdMapChange` webhook callback) remain
deferred, surveyed but genuinely out of scope for this pass -- same disclosed gap class as
`subs-to-notify`'s own lack of real outbound webhook delivery. Task #106 remains open:
`nidd-authorization-data`, bare `/subscription-data/{ueId}`, and `group-data`'s own
`5g-vn-groups`/`mbs-group-membership` bare collection GETs remain real candidates genuinely
unblocked by ADR-0161's infra, not yet individually closed. `GetSSAuData` remains deliberately
deferred per ADR-0160.

