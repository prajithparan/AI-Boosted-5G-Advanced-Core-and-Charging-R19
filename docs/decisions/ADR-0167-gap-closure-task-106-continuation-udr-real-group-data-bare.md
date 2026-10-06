## ADR-0167: gap-closure task #106 continuation -- UDR real `group-data` bare `5g-vn-groups`/`mbs-group-membership` collection GETs, closing the last real candidate this series' array-parsing infra unblocked

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (72 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0166). This ADR closes the bare
`/subscription-data/group-data/5g-vn-groups` and `/subscription-data/group-data/
mbs-group-membership` collection resources (`Query5GVnGroup`/`Query5GmbsGroup`,
`TS29505_Subscription_Data.yaml` lines 6876-6925/9766-9815) -- the last real candidates named
across ADR-0161/ADR-0162/ADR-0164/ADR-0165/ADR-0166's own "What this ADR does NOT include"
sections as genuinely unblocked by this series' array-parsing infra.

Real, confirmed by direct read: both operations are structurally identical twins (same shape as
their own already-closed individual-resource siblings, `5g-vn-groups/{externalGroupId}`/
`mbs-group-membership/{externalGroupId}`, ADR-0144/ADR-0145). Real OPTIONAL `gpsis` array
query-param filter (`style: form, explode: false`); real `200` response is a map `{ExtGroupId:
5GVnGroupConfiguration}` / `{ExtGroupId: MulticastMbsGroupMemb}` -- confirmed, by direct read of
the `5GVnGroupConfiguration`/`MulticastMbsGroupMemb` schemas (`TS29503_Nudm_PP.yaml`), that these
are member-list documents (`members`/`multicastGroupMemb`, both arrays of `Gpsi`), so honoring
`gpsis` would require inspecting each stored group's own member list -- a real, separate,
deliberately deferred piece of work (matching the established "optional filter not honored"
precedent, same reasoning already used for `ee-subscriptions`' `event-types`/`nf-identifiers` and
`pdtq-data`'s `pdtq-ref-ids`), not attempted here.

### Implementation

- No schema or table changes -- both routes compose exclusively from the already-existing
  `udr_5g_vn_groups`/`udr_mbs_group_membership` tables (ADR-0144/ADR-0145), the same tables their
  own individual-resource PUT/GET/PATCH/DELETE routes already read and write.
- `nfs/udr/src/stores.hpp`/`.cpp`: new `list_all()` method on `FiveGVnGroupStore` and
  `MbsGroupMembershipStore` (`SELECT ext_group_id, data FROM ...`, returning
  `std::vector<std::pair<std::string, nlohmann::json>>` -- a map shape, not an array, matching the
  real response schema).
- `nfs/udr/src/main.cpp`: two new OTel counters, two new `GET` routes (`.../5g-vn-groups` and
  `.../mbs-group-membership`), inserted immediately before their own individual-resource route
  blocks. Real, disclosed route-ordering note (same discipline as ADR-0166): the router's
  exact-segment-count matching means these 3-segment bare patterns cannot shadow the existing
  4-segment `{externalGroupId}`/`internal`/`pp-profile-data` routes under the same prefix --
  confirmed by direct read before implementing.
- Real, disclosed `200`-always design, **distinct** from the aggregate live-view resources'
  own `200`-always reasoning (`QueryContextData`/`ue-update-confirmation-data`): this is a literal
  listing of every persisted row in a real table, the same shape as `pdtq-data`'s own collection
  GET (ADR-0162, `200 []` on a fresh install) -- not a composed view with no independent
  existence. An empty map on a fresh install is the real, correct answer, not a deviation.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl against a running `udr` process (freshly built, freshly started, registered with a
freshly started `nrf`, mTLS client cert + real NRF-issued OAuth2 bearer token):

- `GET` both collections on a fresh install -> real `200 {}`.
- Real `PUT` to `5g-vn-groups/group-A` with `{"members":["msisdn-15555550001",
  "msisdn-15555550002"]}` and to `5g-vn-groups/group-B` with `{}` (every field on
  `5GVnGroupConfiguration` is optional, so an empty body is a real, valid document) -> both real
  `201`.
- `GET` the `5g-vn-groups` collection -> real `200` with `{"group-A":{"members":[...]},
  "group-B":{}}`, matching both PUTs exactly.
- `GET` the same collection with an unused `gpsis` filter appended -> identical real `200` result,
  confirming the filter is genuinely accepted but not honored.
- Real `PUT` to `mbs-group-membership/mbs-group-X` with the real required
  `{"multicastGroupMemb":["msisdn-15555550003"]}` -> real `201`; `GET` the `mbs-group-membership`
  collection -> real `200` with exactly that one entry.
- `GET` `5g-vn-groups/group-A` (the individual resource) immediately after -> still real `200`
  with the correct single document, confirming the new bare-collection route did not shadow the
  existing `{externalGroupId}` route.
- Direct `psql SELECT` against both `udr_5g_vn_groups` and `udr_mbs_group_membership`
  independently confirmed every row matched its curl response exactly.

### Testing and verification

`udr` built clean (zero warnings -- one real `-Wsign-conversion` warning on `result.size()` was
caught and fixed with the established `static_cast<std::size_t>(...)` idiom, matching
`PdtqDataStore::list()`'s own precedent, before this ADR's own final build) both before and after
`clang-format-18` (reformat added no diff beyond what was newly written). Full `conformance_tests`
(excluding the two disclosed pre-existing flaky tests): 331/331 pass, zero regressions -- same
count as ADR-0166, since this ADR adds no new automated test (same disclosed
manual-live-verification precedent already established for bare-collection-GET resources).

### What this ADR does NOT include

No NF's own existing logic calls these new routes. This closes UDR resources #73 and #74 of
free5GC's ~42+ real `Nudr_DataRepository` resources (docs/CAPABILITY_GAP_ANALYSIS.md). Task #106's
own array-parsing-infra-unblocked candidate list (ADR-0161's own "What this ADR does NOT include")
is now fully closed: `pdtq-data` (ADR-0162), `GetNfGroupIDs` (ADR-0164), `GetNiddAuData`
(ADR-0165), bare `QueryUeSubscribedData` (ADR-0166), and these two resources are all done. Real,
disclosed, still-open work: `gpsis` filtering on both of these collections (deferred, as above);
`5g-vn-groups`/`mbs-group-membership`'s own `/internal`/`/pp-profile-data` variants (same real
class, not surveyed); `policy-data`'s `mbs-session-pol-data` (deferred per its own earlier
disclosed key-encoding ambiguity); `GetSSAuData` (deliberately deferred, ADR-0160);
`Nudr_GroupIDmap`'s own subscription-management family (ADR-0164); real webhook delivery for
`subs-to-notify` and `nf-group-ids/subscriptions`.

