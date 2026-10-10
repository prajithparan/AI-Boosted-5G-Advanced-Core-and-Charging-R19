## ADR-0169: gap-closure task #106 continuation -- UDR real `group-data` `5g-vn-groups/pp-profile-data` + `mbs-group-membership/pp-profile-data`, this project's first genuinely keyless singleton resources

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (76 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0168). This ADR closes the `/pp-profile-data`
variants of `5g-vn-groups`/`mbs-group-membership` (`Query5GVnGroupPPData`/`Query5GMbsGroupPPData`,
`TS29505_Subscription_Data.yaml` lines 7908-7960/10080-10132) -- named in ADR-0167's/ADR-0168's
own "What this ADR does NOT include" as needing "a genuinely new store/schema."

Real, confirmed by direct read: both operations are real GET-only, real OPTIONAL `ext-group-ids`
array filter (`style: form, explode: false`) and real OPTIONAL `supported-features`. Response
schemas `Pp5gVnGroupProfileData`/`Pp5gMbsGroupProfileData` (`TS29505_Subscription_Data.yaml` lines
11136/11289) are, real and disclosed, **genuinely NOT per-group documents** -- unlike every other
`group-data` sub-resource closed in this series (all keyed by `ExtGroupId`), each of these two
schemas is a single, global document whose own internal `allowedMtcProviders`/`allowedMbsInfos`
field is itself a map keyed by ExtGroupId (or the literal `"ALL"`). This project's own prior
"non-per-UE" resources (`group_identifiers`, `routing_ids`, `coverage-restriction-data`,
`plmn-ue-policy-set`) are all still keyed by some real identifier -- this is the first genuinely
keyless singleton resource in the project, confirmed by direct read rather than assumed.

Confirmed by direct read that neither path-item block contains any create/update/delete
operation -- GET-only, same "no live provisioning path yet, seeded at startup" precedent as
`routing_ids`/`nf_group_ids`/`nidd_authorization_data`.

### Implementation

- `nfs/udr/schema.postgres.sql`: two new tables, `udr_5g_vn_group_pp_profile_data` and
  `udr_mbs_group_pp_profile_data`, each a fixed single-row table (`id INTEGER PRIMARY KEY DEFAULT
  1 CHECK (id = 1)`, `data JSONB`) -- modeling the real keyless singleton shape directly rather
  than inventing a key the spec doesn't have.
- `nfs/udr/src/stores.hpp`/`.cpp`: two new store classes, `FiveGVnGroupPpProfileDataStore` and
  `MbsGroupPpProfileDataStore`, each with a parameterless `seed(json)`/`get()` pair (no key
  argument at all, the first store class in this project with that shape).
- `nfs/udr/src/main.cpp`: store construction, real seed data (both seeded with an empty top-level
  JSON object -- every field on both response schemas is optional, so `{}` is a real, valid
  document, not a fabricated placeholder), two new OTel counters, two new `GET` routes.
- Real, disclosed scope choice: the optional `ext-group-ids`/`supported-features` filters are
  accepted but not honored -- there is nothing meaningful to filter against without inspecting the
  singleton's own internal `allowedMtcProviders`/`allowedMbsInfos` map, deferred as the same class
  of real, separate work as the bare collection's own `gpsis` filter (ADR-0167).
- Real `200`-always design: since both stores are seeded at startup and never deleted, `get()`
  always returns a value in practice; the `404` branch exists defensively (matching this
  project's own established pattern for every other seeded-singleton `get()` route) but is not
  reachable under normal operation.
- **Same critical route-ordering requirement as ADR-0168's `/internal` routes** (re-applied, not
  re-derived): `.../5g-vn-groups/pp-profile-data` and `.../mbs-group-membership/pp-profile-data`
  are both 4-path-segment literal routes, the same segment count as their own `{externalGroupId}`
  wildcard siblings -- both registered before those routes in `nfs/udr/src/main.cpp`, with the
  same explicit ordering-requirement comment. Verified live, not just reasoned about (see below).

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl against a running `udr` process (freshly built, freshly started, registered with a
freshly started `nrf`, mTLS client cert + real NRF-issued OAuth2 bearer token):

- `GET /5g-vn-groups/pp-profile-data` -> real `200 {}` (the seeded empty singleton).
- `GET /mbs-group-membership/pp-profile-data` -> real `200 {}`.
- `GET /5g-vn-groups/pp-profile-data` with an unused `ext-group-ids`/`supported-features` filter
  appended -> identical real `200 {}`, confirming the filters are accepted but not honored.
- **Critical negative check**: none of the above showed the individual-resource route's own
  `"No 5G VN Group for externalGroupId pp-profile-data"` error text, confirming the literal
  `/pp-profile-data` route is genuinely reached, not shadowed.
- `GET 5g-vn-groups/group-A` (individual resource) and `GET 5g-vn-groups/internal?...` (ADR-0168's
  own route) immediately after -> both still real `200` with their own correct results, confirming
  the three sibling routes under this prefix (`{externalGroupId}`, `/internal`, `/pp-profile-data`)
  do not interfere with each other.
- Direct `psql SELECT` against both `udr_5g_vn_group_pp_profile_data` and
  `udr_mbs_group_pp_profile_data` independently confirmed exactly one row each, `{}`, matching the
  curl responses exactly.

### Testing and verification

`udr` built clean (zero warnings) both before and after `clang-format-18` (reformat added no diff
beyond what was newly written). Full `conformance_tests` (excluding the two disclosed pre-existing
flaky tests): 331/331 pass, zero regressions -- same count as ADR-0168, since this ADR adds no new
automated test (same disclosed manual-live-verification precedent already established for
GET-only, seeded-at-startup resources).

### What this ADR does NOT include

No NF's own existing logic calls these new routes. This closes UDR resources #77 and #78 of
free5GC's ~42+ real `Nudr_DataRepository` resources (docs/CAPABILITY_GAP_ANALYSIS.md). This closes
out the entirety of `5g-vn-groups`/`mbs-group-membership`'s own real, in-scope resource set (bare
collection, individual document, `/internal`, `/pp-profile-data` -- all four now real and closed).
Real, disclosed, still-open work: `ext-group-ids`/`supported-features` filtering on these two
singletons (deferred, same class as `gpsis`); `policy-data`'s `mbs-session-pol-data` (deferred,
key-encoding ambiguity); `GetSSAuData` (deliberately deferred, ADR-0160); `Nudr_GroupIDmap`'s own
subscription-management family (ADR-0164); real webhook delivery for `subs-to-notify`/
`nf-group-ids/subscriptions`.

