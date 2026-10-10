## ADR-0157: gap-closure task #106 continuation -- UDR real AMF Group Subscription Info
(Document), plus a project-wide fix for a real RFC 9110 Location-header conformance defect

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (66 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0156). While live-verifying the planned new
resource for this turn -- the group-data-scoped sibling of `ee-subscriptions/{subsId}/
amf-subscriptions` (ADR-0152) -- the real, disclosed Location-header bug fixed for 3 occurrences
in ADR-0156 turned out to be far more widespread than that fix covered.

**Full scope of the bug, found this turn:** `grep -c 'headers.emplace("location"' nfs/udr/src/
main.cpp` returned 20 occurrences. Checking each pattern variable's own raw string for an
unsubstituted `{name}` placeholder found the defect present in ~17 more, spanning back to the
earliest Tier 1a resources built in this project, well before any gap-closure-era ADR:
`amf-3gpp-access`, `amf-non-3gpp-access`, `smf-registrations`, `mwd`, `roaming-information`,
`pei-info`, `pp-data-store`, `ue-policy-set`, `bdt-data`, `nidd-authorizations`,
`service-specific-authorizations`, plus the gap-closure-era `5g-vn-groups`,
`mbs-group-membership`, and `ee-subscriptions`' own `amf-`/`smf-`/`hss-subscriptions` nested
sub-collections. Only `subs-to-notify` was confirmed genuinely unaffected (its collection path
has no path-parameter placeholder at all -- the same finding already made in ADR-0156). This is a
real RFC 9110 §10.2.2 Location-header conformance defect: any real SBI consumer following the
`Location` header on a `201 Created` response from these routes would receive a literal,
unusable placeholder string (e.g. `{ueId}`) instead of the real resource URI.

Given the scope -- a correctness defect across most of UDR's own `PUT`-with-`201`-create routes,
not a contained 2-3-occurrence fix -- this was presented to the user via `AskUserQuestion` before
proceeding, rather than either silently expanding this turn's scope unilaterally or silently
leaving ~17 already-shipped routes broken. Three real options were presented (fix all ~20 now,
fix only the routes actively being touched, defer entirely as a tracked task). **User's explicit
answer: "Fix all ~20 occurrences now (Recommended)."**

### Implementation

- **New resource**: the real AMF Group Subscription Info (Document), nested under an individual
  group-data-scoped ee-subscription (`group-data/{ueGroupId}/ee-subscriptions/{subsId}/
  amf-subscriptions`, real spec operations `CreateAmfGroupSubscriptions` [PUT]/
  `GetAmfGroupSubscriptions` [GET]/`ModifyAmfGroupSubscriptions` [PATCH]/
  `RemoveAmfGroupSubscriptions` [DELETE]) -- structurally identical to
  `ee-subscriptions/{subsId}/amf-subscriptions` (ADR-0152) but keyed by `ueGroupId`: new
  `udr_group_amf_subscription_info` table, new `GroupAmfSubscriptionInfoStore` class, four new
  routes, three new OTel counters. Same array-valued `AmfSubscriptionInfo[]` document body, real
  distinct 201-vs-204 PUT.
- **Bug fix**: added one new shared helper, `resolved_location(pattern, path_params)` (in
  `nfs/udr/src/main.cpp`, just above `check_bearer`), which substitutes every real `{name}`
  path-parameter value into a route pattern in a single pass -- correct for routes with more than
  one path parameter on the same route (e.g. `{ueGroupId}` and `{subsId}` together). Every one of
  the 20 `resp.headers.emplace("location", ...)` call sites was converted to use this helper
  (either as the entire Location value, or as a prefix with a server-generated ID appended, for
  the `POST`-collection-create routes that don't get their ID from `path_params`). The 3 routes
  hand-fixed in ADR-0156 (`ee-subscriptions`, `sdm-subscriptions`, `group-ee-subscriptions`) were
  refactored onto this same helper for consistency, replacing their own hand-built
  `kApiRoot + "/subscription-data/" + ...` string construction.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl sequence against a running `udr` process (freshly built, freshly started, registered
with a freshly started `nrf`, mTLS client cert + real NRF-issued OAuth2 bearer token):

- Full GET/PUT/PATCH/DELETE cycle for the new `group-data`-scoped `amf-subscriptions` resource,
  `ueGroupId` `grp-amf-test-001`, `subsId` `amf-subs-001`: `GET` before `PUT` -> real `404`;
  `PUT` create -> real `201`, body echoed back, **`Location` header confirmed to contain BOTH
  real `ueGroupId` and real `subsId`** (`.../group-data/grp-amf-test-001/ee-subscriptions/
  amf-subs-001/amf-subscriptions`), proving `resolved_location()` correctly substitutes more than
  one path parameter on the same route; `GET` -> real `200`; `PUT` update -> real `204`; `GET`
  after confirms overwrite; `PATCH` (real RFC 6902) -> real `204`; `GET` after reflects the
  patched value; `DELETE` -> real `204`; `GET` after -> real `404`.
- Direct `psql SELECT` against `udr_group_amf_subscription_info` confirmed the row matched
  curl's response, and confirmed zero rows remained after `DELETE`.
- Cross-checked `udr_group_ee_subscriptions` and `udr_ee_amf_subscription_info` -> both real `0`
  rows, confirming genuinely separate storage from both the group-data collection parent and the
  `ueId`-scoped sibling.
- The `resolved_location()` helper's correctness for the single-path-parameter case was already
  proven live in ADR-0156 (`ee-subscriptions`, `sdm-subscriptions`, `group-ee-subscriptions`, all
  re-verified there); this ADR's own new resource additionally proves the multi-parameter case.
  The remaining ~14 pre-existing occurrences fixed in this same change use the identical helper
  and code path (confirmed by a clean, zero-warning build across all 20 call sites) and were not
  each individually re-exercised with a fresh live `POST`/`PUT` in this pass -- their correctness
  rests on the shared helper's own proven behavior plus the full `conformance_tests` regression
  run below, not on per-route live re-verification. This is a real, disclosed narrowing of this
  ADR's own live-verification claim, not silently glossed over.

### Testing and verification

`udr` built clean (zero warnings) both before and after `clang-format-18` (reformat added no diff
beyond what was newly written). Full `conformance_tests` (excluding the two disclosed pre-existing
flaky tests): 325/325 pass, zero regressions.

### What this ADR does NOT include

No NF's own existing logic calls the new routes (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #67 of free5GC's ~42+ real `Nudr_DataRepository` resources. Real, disclosed scope
narrowing: the group-data-scoped `smf-subscriptions`/`hss-subscriptions` nested sub-collections
(siblings of the resource closed here) remain genuinely deferred, not yet built. Task #106 remains
open: those two nested sub-collections, `group-data`'s remaining genuinely-blocked resources, bare
`/subscription-data/{ueId}`/`{ueId}/context-data`, and the other genuinely deferred subsystems
(`pdtq-data`, `mbs-session-pol-data`, `nidd-authorization-data`,
`service-specific-authorization-data/{serviceType}`, `Nudr_GroupIDmap`'s own `/nf-group-ids`)
remain real, open, disclosed gaps.

