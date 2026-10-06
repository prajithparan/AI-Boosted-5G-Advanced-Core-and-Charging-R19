## ADR-0181: `gpsis`/`ext-group-ids` filtering retrofit across 4 already-closed UDR `group-data` GET resources

### Context

Every `group-data` GET resource with an optional `gpsis`/`ext-group-ids` array query-param filter
was originally closed with that filter deliberately left unhonored, each time disclosed as
"matching the established optional-filter-not-honored precedent." With UDR's `onDataChange`/
`onGroupIdMapChange` arc fully closed (ADR-0180), user chose to retrofit this real, disclosed
backlog next.

Direct read of `TS29505_Subscription_Data.yaml` plus the schemas it references
(`TS29503_Nudm_PP.yaml`) found the real backlog splits into two genuinely different classes, not
one:

1. **Tractable now, just never wired** -- `Query5GVnGroup`/`Query5GmbsGroup`'s own `gpsis` filter
   and `Query5GVNGroupPPData`/`Query5GMbsGroupPPData`'s own `ext-group-ids` filter all target a
   real, concrete field already present in the exact data these routes already return
   (`5GVnGroupConfiguration.members`, `MulticastMbsGroupMemb.multicastGroupMemb`,
   `Pp5gVnGroupProfileData.allowedMtcProviders`, `Pp5gMbsGroupProfileData.allowedMbsInfos`) --
   this project's own original ADR-0167/ADR-0169 comments called this "inspecting the member
   list/internal map," which reads more ominous than the real, direct field lookup and array/map
   filter it actually is. All 4 done in this pass.
2. **Genuinely not tractable, correctly still deferred** -- `QueryUeSubscribedData`'s own
   `ext-group-ids` (and `adjacent-plmns`/`single-nssai`/`dnn`/`uc-purpose`) filters, per its own
   real ADR-0166 comment, have no store to filter *against*: this resource is a live view over 32
   flat per-UE fields (`am-data`, `sm-data`, etc.), none of which are group-membership data --
   confirmed correct on re-check, not retrofitted, no fabricated filtering invented where the real
   schema gives nothing to filter.

Real, disclosed documentation fix made while surveying: this file's own top-of-file "Deliberately
still deferred" comment block (written across ADR-0140-ADR-0149) was stale -- it still described
`5g-vn-groups`/`mbs-group-membership`'s bare-collection `gpsis` filters as blocked on missing
array-query-param infra, a state ADR-0161's `split_form_array()` (used by `internal-group-ids`
since ADR-0168) had already resolved months of ADRs ago. Superseded by this ADR, not corrected
in place (this project's own established practice: append, don't rewrite history).

### Implementation

`nfs/udr/src/main.cpp`, four GET routes:
- `Query5GVnGroup`/`Query5GmbsGroup` (bare collections): parse the optional `gpsis` query param
  with the existing `sbi_core::http2::split_form_array()`; a group is included when no filter is
  given, or when at least one requested `Gpsi` appears in that group's own real membership array
  (`members` for 5G-VN groups, `multicastGroupMemb` for MBS groups -- two different real field
  names, confirmed by direct schema read, not assumed identical). A group missing that field never
  matches a non-empty filter.
- `Query5GVNGroupPPData`/`Query5GMbsGroupPPData` (keyless singletons): parse the optional
  `ext-group-ids` query param the same way; filter `allowedMtcProviders`/`allowedMbsInfos` down to
  the requested keys, with the real spec-documented special key `"ALL"` ("allowed operating all
  the external group identifiers") always kept regardless of what was requested -- a real spec
  rule, not an invented default. `supported-features` (present on the PP-data routes) remains
  accepted-but-not-honored, an unrelated, already-established, out-of-scope precedent for that
  capability-negotiation param.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl against a running `udr` process: `PUT` two real `5g-vn-groups` with distinct real
`members` arrays, then `GET` the bare collection unfiltered (both returned, plus this project's
own pre-existing seed data) -- `GET ?gpsis=<a member of only one group>` correctly returned only
that one group; `GET ?gpsis=<a value in neither group>` correctly returned a real empty map, not
an error. For the PP-data filter (a singleton with no write route, so pre-seeded directly via
`psql` -- the only way to populate it for this test): `allowedMtcProviders` with three real keys
(`vn-grp-A`, `vn-grp-B`, `ALL`), then `GET ?ext-group-ids=vn-grp-A` correctly returned exactly
`{vn-grp-A, ALL}`, excluding `vn-grp-B`. Test data cleaned up afterward (rows deleted, singleton
reset to `{}`).

### Testing and verification

`udr` built clean (zero warnings) both before and after `clang-format-18`. Full
`conformance_tests`+`integration_tests` (`ctest`, excluding the two disclosed pre-existing flaky
tests): 332/332 pass, zero regressions.

### What this ADR does NOT include

`QueryUeSubscribedData`'s own `ext-group-ids`/`adjacent-plmns`/`single-nssai`/`dnn`/`uc-purpose`
filters remain correctly unhonored -- re-confirmed, not retrofitted, since no store exists to
filter against for that resource's own real composed fields. `supported-features` remains
accepted-but-not-honored project-wide, a separate, already-established, unrelated gap. This ADR
closes the real `gpsis`/`ext-group-ids` backlog specifically named at the end of ADR-0180 in full
-- no other filtering gaps were surveyed or touched in this pass.

