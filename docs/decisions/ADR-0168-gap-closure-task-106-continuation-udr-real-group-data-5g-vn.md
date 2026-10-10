## ADR-0168: gap-closure task #106 continuation -- UDR real `group-data` `5g-vn-groups/internal` + `mbs-group-membership/internal`, and a real router-ordering hazard found and fixed before it could ship a bug

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (74 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0167). This ADR closes the `/internal` variants
of `5g-vn-groups`/`mbs-group-membership` (`Query5GVnGroupInternal`/`Query5GMbsGroupInternal`,
`TS29505_Subscription_Data.yaml` lines 7860-7902/10032-10074) -- named in ADR-0167's own "What
this ADR does NOT include" as "same real class, not surveyed."

Real, confirmed by direct read: both operations are real GET-only, real REQUIRED
`internal-group-ids` array query param (`style: form, explode: false`), response the same real
map `{ExtGroupId: 5GVnGroupConfiguration}` / `{ExtGroupId: MulticastMbsGroupMemb}` shape as their
own bare-collection siblings (ADR-0167). Unlike the bare collection's own `gpsis` filter (which
targets each group's *member list*, deferred as real, separate work), this filter targets each
group's own optional `internalGroupIdentifier` scalar field (confirmed present on both
`5GVnGroupConfiguration` and `MulticastMbsGroupMemb`, `TS29503_Nudm_PP.yaml`) -- real and
tractable to implement directly against the existing `list_all()` (ADR-0167), no new store method
needed.

**Real router-ordering hazard found and fixed before implementing, not after a bug shipped**:
`.../5g-vn-groups/internal` is a 4-path-segment URL, the exact same segment count as the
already-registered `.../5g-vn-groups/{externalGroupId}` GET route (ADR-0144). Direct read of
`libs/sbi-core/src/http2_server.cpp`'s own `try_match`/route-dispatch loop confirmed this router
has **no literal-vs-wildcard priority** -- routes are tried in registration order, first match
wins, and a `{externalGroupId}` wildcard segment matches the literal string `"internal"` just as
readily as any real group ID. Registering the new `/internal` route *after* the existing
`{externalGroupId}` GET route (the natural, "append near related code" instinct that every prior
ADR in this series followed) would have permanently shadowed it -- every real request to
`/internal` would have been silently misrouted to the individual-resource handler instead,
looking up a nonexistent group named "internal" and returning a confusing `404`. Fixed by
registering both new `/internal` GET routes *before* their own `{externalGroupId}` GET route
registrations in `nfs/udr/src/main.cpp`, with an explicit `CRITICAL ROUTE-ORDERING REQUIREMENT`
comment at each site so a future edit doesn't reintroduce the hazard. This is the first time this
project's own route-registration order has mattered for correctness (every prior route addition
in this series was either a distinct segment count or a distinct HTTP method) -- worth a permanent
comment, not just a one-time fix.

### Implementation

- No schema or table changes -- both routes compose exclusively from the already-existing
  `udr_5g_vn_groups`/`udr_mbs_group_membership` tables via `list_all()` (ADR-0167).
- `nfs/udr/src/main.cpp`: two new OTel counters, two new `GET` routes
  (`.../5g-vn-groups/internal` and `.../mbs-group-membership/internal`), each filtering
  `list_all()`'s own results by whether the stored document's own `internalGroupIdentifier` field
  (when present) is a member of the requested `internal-group-ids` set.
- Real, disclosed `404`-on-no-match design, **distinct** from the bare collection's own
  `200`-always literal-listing design (ADR-0167): this is a genuine query-by-identifier (the
  caller supplies specific IDs and expects matching data), matching `GetNfGroupIDs`'s own
  real-`404` precedent (ADR-0164), not `pdtq-data`'s own always-`200` collection-GET precedent.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl against a running `udr` process (freshly built, freshly started, registered with a
freshly started `nrf`, mTLS client cert + real NRF-issued OAuth2 bearer token):

- Real `PUT` to `5g-vn-groups/group-C` with a real, pattern-valid `internalGroupIdentifier`
  (`1a2b3c4d-999-70-a1b2c3`, confirmed against `GroupId`'s own real regex) alongside `members` ->
  real `201`.
- `GET /5g-vn-groups/internal?internal-group-ids=1a2b3c4d-999-70-a1b2c3` -> real `200` with
  exactly `{"group-C": {...}}` (the two other seeded groups, `group-A`/`group-B`, both lacking an
  `internalGroupIdentifier`, correctly excluded).
- `GET /5g-vn-groups/internal` with the required param missing -> real `400`.
- `GET /5g-vn-groups/internal` with a well-formed but non-matching `internal-group-ids` value ->
  real `404`.
- **Critical negative check**: none of the above three responses showed the individual-resource
  route's own `"No 5G VN Group for externalGroupId internal"` error text, confirming the literal
  `/internal` route is genuinely reached and not shadowed by the wildcard route -- the specific
  failure mode the route-ordering fix above was designed to prevent, verified live, not just
  reasoned about.
- Identical sequence repeated for `mbs-group-membership/internal` with `mbs-group-Y` and a real
  `internalGroupIdentifier` (`deadbeef-100-05-cafe01`) -- same real `200`/`400` results.
- `GET mbs-group-membership/mbs-group-Y` (the individual resource) immediately after -> still real
  `200` with the correct single document, confirming the new route didn't shadow anything else
  either.
- Direct `psql SELECT` against both `udr_5g_vn_groups` and `udr_mbs_group_membership`
  independently confirmed every row (including the two new `internalGroupIdentifier`-bearing ones)
  matched its curl response exactly.

### Testing and verification

`udr` built clean (zero warnings) both before and after `clang-format-18` (reformat added no diff
beyond what was newly written). Full `conformance_tests` (excluding the two disclosed pre-existing
flaky tests): 331/331 pass, zero regressions -- same count as ADR-0167, since this ADR adds no new
automated test (same disclosed manual-live-verification precedent already established for
GET-only, existing-store-backed resources) -- though the route-ordering hazard found here is a
real, disclosed argument for a future dedicated router unit test asserting literal segments beat
wildcards regardless of registration order, left as a separate, deliberate improvement not
attempted in this pass (fixing the two live occurrences via explicit ordering + comments was
judged sufficient for now).

### What this ADR does NOT include

No NF's own existing logic calls these new routes. This closes UDR resources #75 and #76 of
free5GC's ~42+ real `Nudr_DataRepository` resources (docs/CAPABILITY_GAP_ANALYSIS.md). Real,
disclosed, still-open work: the router's own literal-vs-wildcard ambiguity is fixed at these two
call sites by ordering, not fixed at the router level itself (a real, larger change -- adding
literal-segment-priority matching to `try_match` -- deliberately not attempted here, since every
existing route continues to work correctly under registration-order semantics and a router-level
change risks a wider blast radius for a narrow, already-mitigated problem); `5g-vn-groups`/
`mbs-group-membership`'s own `/pp-profile-data` variants (need a genuinely new store/schema,
`Pp5gVnGroupProfileData`, not surveyed in this pass); `gpsis` filtering on the bare collections
(ADR-0167); `policy-data`'s `mbs-session-pol-data` (deferred, key-encoding ambiguity);
`GetSSAuData` (deliberately deferred, ADR-0160); `Nudr_GroupIDmap`'s own subscription-management
family (ADR-0164); real webhook delivery for `subs-to-notify`/`nf-group-ids/subscriptions`.

