## ADR-0237: UDM `Nudm_PP` -- the 11 ops ADR-0082 disclosed but did not implement

### Context

`TS29503_Nudm_PP.yaml` has 13 real operationIds. ADR-0082 closed 2 (`Get PP Data`/`Update`) and
explicitly disclosed 3 larger, undone resource groups: 5G VN Group CRUD, PP Data Entry CRUD, 5G
MBS Group CRUD -- 11 ops total (4+3+4), matching the "11 ops" figure this project's own docs have
carried since.

Checked UDR first per this project's own re-learned "check before assuming new work" lesson
(ADR-0233): all 3 real backing resources already exist, fully live -- `FiveGVnGroupStore`
(`5g-vn-groups/{externalGroupId}`, ADR-0144), `MbsGroupMembershipStore`
(`mbs-group-membership/{externalGroupId}`, ADR-0167), `PpDataStore` (`pp-data-store/{afInstanceId}`,
ADR-0109). No new UDR work needed.

Confirmed by direct read of both YAMLs, not assumed: `Create`/`Delete`/`Get` for all 3 groups match
UDR's own real response shapes exactly (PUT 201, DELETE 204/404, GET 200/404) -- real, direct
proxies. `Modify` (5G VN Group / 5G MBS Group only -- PP Data Entry has no Modify op) is genuinely
NOT a passthrough: Nudm_PP's real spec uses `application/merge-patch+json` (RFC 7396) for both, but
UDR's own PATCH for both resources is real RFC 6902 JSON Patch (`apply_patch`, confirmed in
`nfs/udr/src/main.cpp`) -- a real protocol mismatch, not assumed to align.

### Implementation

- New generic `proxy_to_udr(method, path, body)` helper in `nfs/udm/src/main.cpp`, relaying UDR's
  own status/JSON body verbatim (including its `ProblemDetails` error shape, already correct, no
  re-wrapping). Justified now (same "N call sites, one shared helper" precedent as
  `fetch_from_udr_no_plmn`, ADR-0233) since 9 of the 11 new routes share this identical
  direct-proxy shape.
- 9 direct-proxy routes: `PUT`/`DELETE`/`GET` for `pp-data-store/{afInstanceId}`,
  `5g-vn-groups/{extGroupId}`, `mbs-group-membership/{extGroupId}`.
- 2 `Modify` routes (`5g-vn-groups`/`mbs-group-membership` `PATCH`): a real GET+merge_patch
  (locally, `nlohmann::json::merge_patch`)+PUT round trip against UDR, giving the real Nudm_PP
  caller correct RFC 7396 semantics without UDR needing to change. Real `404` if the GET finds no
  existing group (matches spec's own existence-check expectation); real `204` on success.

### Live verification (real, live mTLS + OAuth2, not self-consistency)

New `tests/integration/test_udm_pp_gap_closure_237.cpp`, 2 tests. `PpDataEntryFullLifecycle`: real
404 before creation, real 201 on create, real 204 on re-PUT (update), real 200 with the exact
proxied `referenceId` on GET, real 204/404 on delete/re-delete.
`FiveGVnGroupAndFiveGMbsGroupFullLifecycle` (both groups, shared helper): real 404/201/200 create
lifecycle, real 204 Modify with the change genuinely persisted alongside the original data (verified
via a follow-up real GET), real 404 Modify against an unknown group, real 204/404 delete/re-delete.

Two real bugs caught and fixed during this ADR, both in the new test file, not the server code:
1. `PpDataEntry`'s real schema has no `afInstanceId` field (that's only a path param for this
   resource) -- the test's original body used it anyway; UDR silently dropped the unrecognized
   key on parse, and the test's own unguarded `.at("afInstanceId")` then threw an uncaught
   exception, reproducing this project's documented leaked-process pipe-hang pattern (the crashed
   test skipped its own `kill()`/`waitpid()` cleanup). Fixed by using a real `PpDataEntry` field
   (`referenceId`) and hardening the verification with `ASSERT_TRUE(contains(...))` guards before
   any `.at()`, matching this project's existing defensive-test convention.
2. `N5GVnGroupConfiguration`'s real WIRE key for its nested group data is `5gVnGroupData`
   (numeric-leading, matching `TS29503_Nudm_PP.yaml`'s real property name exactly) -- the C++
   struct field is `n5gVnGroupData` only because `5gVnGroupData` isn't a valid C++ identifier
   (codegen's own real, disclosed convention). The test initially used the C++ field name as the
   JSON key, which UDR silently ignored (same class of bug), isolated by testing UDR directly
   (bypassing UDM) with both keys before concluding this was a test bug, not a server bug in
   either UDM or UDR. Fixed in the test.

Both bugs left stale rows in the real, persistent local Postgres UDR database from crashed test
runs; found via `docker exec docker-postgres-udr-1 psql` and deleted before the next clean run --
no server-side data-hygiene issue, a normal artifact of manual dev-session debugging against a
persistent local DB (CI's own Postgres is a fresh per-run instance).

Passes.

### Testing

Full reconfigure + rebuild clean (`udm` only -- no UDR changes this ADR). New tests pass standalone
after the two test-file fixes above. No strays before/after any run (`ps aux` checked explicitly).
Full suite re-run clean at `-j1`.

### What this ADR does NOT include

This closes `Nudm_PP` in full (13/13 real ops: 2 from ADR-0082 + 11 here) and, with it, UDM's
entire Tier-B gap-closure backlog opened by `docs/CAPABILITY_GAP_ANALYSIS.md`'s original audit --
the only remaining disclosed UDM gap anywhere in this project is `Update SOR Info`
(`Nudm_SDM`, ADR-0234's own real CounterSoR/steering-list content gap).

