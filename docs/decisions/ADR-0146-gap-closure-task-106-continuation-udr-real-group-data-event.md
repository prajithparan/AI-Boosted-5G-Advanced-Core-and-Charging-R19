## ADR-0146: gap-closure task #106 continuation -- UDR real group-data Event Exposure Data for a
group resource

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (56 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0145, after the third `group-data` sub-resource).
Real, confirmed-by-YAML-read: `/subscription-data/group-data/{ueGroupId}/ee-profile-data` (real
spec operation `QueryGroupEEData`, schema `EeGroupProfileData` -- every field optional:
`restrictedEventTypes`/`allowedMtcProvider`/`supportedFeatures`/`iwkEpcRestricted`/`extGroupId`/
`hssGroupId`) is a real, genuinely distinct sibling of the already-closed per-UE
`{ueId}/ee-profile-data` resource -- same resource name, different real path, different keying.
Real GET-only, no create/update operation exists in the spec at all. Keyed by `ueGroupId` (real
schema `VarUeGroupId`, a plain string constrained by the pattern
`^(extgroupid-[^@]+@[^@]+|anyUE)$` -- confirmed via direct YAML read, no encoding ambiguity, unlike
the still-deferred `mbs-session-pol-data`'s own `MbsSessPolDataId`). With every field on
`EeGroupProfileData` optional, an empty JSON object is a genuine, real, schema-valid response for
the seeded "anyUE" test case, not a fabricated placeholder.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_group_ee_profile_data` table (`ue_group_id` PK, `data`
  JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `GroupEeProfileDataStore` class (`seed`/`get`), same
  "GET-only, seed at startup" shape as `SponsorConnectivityDataStore`.
- `nfs/udr/src/main.cpp`: store construction, one seed call for `"anyUE"` with an empty object
  body, one new OTel counter (get), and one new route (GET) at
  `/subscription-data/group-data/{ueGroupId}/ee-profile-data`.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl sequence against a running `udr` process (freshly built, freshly started, registered
with a freshly started `nrf`) backed by real PostgreSQL: `GET` on the seeded `anyUE` group ->
real `200` with an empty JSON object body (matches the real, all-optional schema); `GET` on an
unseeded `extgroupid-nope@example.com` group -> real `404`. Direct `psql SELECT` against
`udr_group_ee_profile_data` independently confirmed the single seeded row (`ue_group_id = 'anyUE'`,
`data = '{}'`).

### Testing and verification

`udr` built clean both before and after `clang-format-18` (reformat added no diff beyond what was
newly written). Full `conformance_tests` (excluding the two disclosed pre-existing flaky tests):
325/325 pass, zero regressions.

### What this ADR does NOT include

No NF's own existing logic calls this new route (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). Only the
degenerate empty-object case of `EeGroupProfileData` was live-verified -- none of its six optional
fields were individually exercised, though they round-trip through the same generated,
already-tested `to_json`/`from_json`, not hand-written code. This closes UDR resource #57 of
free5GC's ~42+ real `Nudr_DataRepository` resources -- the fourth real `group-data` sub-resource,
after `group-identifiers` (ADR-0140), `5g-vn-groups/{externalGroupId}` (ADR-0144), and
`mbs-group-membership/{externalGroupId}` (ADR-0145). Task #106 remains open: both bare collection
GETs (`Query5GVnGroup`/`Query5GmbsGroup`, confirmed genuinely blocked on the same array-query-param
class), `5g-vn-groups`'s/`mbs-group-membership`'s own `/internal`/`/pp-profile-data` variants (same
real blocked class), bare `/subscription-data/{ueId}`, and the other genuinely deferred subsystems
(`ee-subscriptions`/`sdm-subscriptions`, `subs-to-notify`, `pdtq-data`, `mbs-session-pol-data`,
`nidd-authorization-data`, `service-specific-authorization-data/{serviceType}`,
`Nudr_GroupIDmap`'s own `/nf-group-ids`) remain real, open, disclosed gaps.

