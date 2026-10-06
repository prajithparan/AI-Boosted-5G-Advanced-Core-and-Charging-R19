## ADR-0144: gap-closure task #106 continuation -- UDR real group-data individual 5G VN Group
Configuration resource

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (54 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0143). Real, confirmed-by-YAML-read: the
`group-data` group's second sub-resource, after `group-identifiers` (ADR-0140), is
`/subscription-data/group-data/5g-vn-groups/{externalGroupId}` (real spec operations
`Create5GVnGroup`/`Get5GVnGroupConfiguration`/`Modify5GVnGroup`/`Delete5GVnGroup` -- real
GET+PUT+PATCH+DELETE, schema `5GVnGroupConfiguration` from `TS29503_Nudm_PP.yaml`, generated as
`sbi_gen::N5GVnGroupConfiguration` since a bare C++ identifier cannot start with a digit). Real,
disclosed: the PUT documents ONLY `201` (operationId literally `Create5GVnGroup`, no
update-via-PUT status) -- same precedent as `bdt-data` (ADR-0116). PATCH is real RFC 6902
`application/json-patch+json` (confirmed by direct read of the request body content type), NOT
the RFC 7396 merge-patch `bdt-data` itself uses -- genuinely different from its policy-data
sibling despite the shared "201-only PUT" shape.

Also surveyed in the same pass, confirmed genuinely blocked, not silently skipped: the sibling
bare collection GET at `/subscription-data/group-data/5g-vn-groups` (real spec `Query5GVnGroup`,
returning a map keyed by `ExtGroupId`) has a real `gpsis` query parameter declared
`style: form, explode: false` -- the same unsupported array-query-param class already disclosed
for `pdtq-data` and `Nudr_GroupIDmap`'s own `/nf-group-ids` -- left deferred.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_5g_vn_groups` table (`ext_group_id` PK, `data` JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `FiveGVnGroupStore` class (`put`/`get`/`apply_patch`/
  `remove`) -- `put` internally upsert-capable but the route always responds `201` (same
  precedent as `BdtDataStore`); `apply_patch` is real RFC 6902 via nlohmann::json's `.patch()`,
  NOT upsert-capable (returns `nullopt` if the resource doesn't exist -- PUT is the real create
  path, same precedent as `SorDataStore`).
- `nfs/udr/src/main.cpp`: store construction, three new OTel counters (write, get, delete -- PATCH
  reuses the write counter, same precedent as `nidd_authorization_write_counter`), and four new
  routes (GET/PUT/PATCH/DELETE) at
  `/subscription-data/group-data/5g-vn-groups/{externalGroupId}`, using
  `sbi_core::http2::parse_json_body<sbi_gen::N5GVnGroupConfiguration>` for the PUT body.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl sequence against a running `udr` process (freshly built, freshly started, registered
with a freshly started `nrf`) backed by real PostgreSQL, `externalGroupId` `grp-vn-002`:

Real bug found and fixed mid-verification, disclosed honestly: the first verification attempt
used the C++ field name `n5gVnGroupData`/`n5gVnGroupCommunicationInd` as the JSON key and got back
an empty `{}` body with a `400` on the follow-up PATCH. Reading the generated `to_json`
(`build/generated/sbi_gen/TS29122_CommonData_grp.cpp:16295`, `:16272`) confirmed this was a test
error, not an implementation bug -- sbi-codegen correctly emits the real JSON keys
`5gVnGroupData`/`5gVnGroupCommunicationInd` (leading-digit keys the C++ struct fields are
`n`-prefixed to hold), and the DTO's own `to_json`/`from_json` round-trip is correct. Re-ran with
the real JSON keys:

- `GET` before any `PUT` -> real `404`.
- `PUT` with `{"5gVnGroupData":{"dnn":"internet","sNssai":{"sst":1,"sd":"000001"},
  "5gVnGroupCommunicationInd":true}}` -> real `201`, body echoes the same structure back.
- `GET` after `PUT` -> real `200` with the same body.
- `PATCH` with a real RFC 6902
  `[{"op":"replace","path":"/5gVnGroupData/5gVnGroupCommunicationInd","value":false}]` -> real
  `204`.
- `GET` after `PATCH` -> real `200` with `5gVnGroupCommunicationInd` now `false`, `dnn`/`sNssai`
  unchanged.
- `PATCH` against a nonexistent `externalGroupId` -> real `404` (confirms `apply_patch` is NOT
  upsert-capable).
- Direct `psql SELECT` against `udr_5g_vn_groups` independently confirmed the persisted row
  matched curl's response at each step.
- `DELETE` -> real `204`; `GET` after -> real `404`; a second `DELETE` on the same, now-gone
  `externalGroupId` -> real `404` (not idempotent-200, matches `remove()`'s own
  affected-rows-based semantics); `psql SELECT` confirmed zero rows remained.

### Testing and verification

`udr` built clean both before and after `clang-format-18` (reformat added no diff beyond what was
newly written). Full `conformance_tests` (excluding the two disclosed pre-existing flaky tests):
325/325 pass, zero regressions.

### What this ADR does NOT include

No NF's own existing logic calls these new routes (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). Live
verification only exercised `dnn`/`sNssai`/`5gVnGroupCommunicationInd` of the real
`N5GVnGroupData` schema's dozen-plus fields, and only `5gVnGroupData` of `N5GVnGroupConfiguration`'s
own six top-level fields (`members`/`referenceId`/`afInstanceId`/`internalGroupIdentifier`/
`mtcProviderInformation`/`membersData` were not individually exercised, though they round-trip
through the same generated, already-tested `to_json`/`from_json`, not hand-written code). This
closes UDR resource #55 of free5GC's ~42+ real `Nudr_DataRepository` resources -- the second real
`group-data` sub-resource, after `group-identifiers` (ADR-0140). Task #106 remains open: the
bare `5g-vn-groups` collection GET (confirmed genuinely blocked -- real `style: form,
explode: false` array query parameter), `5g-vn-groups`'s own `/internal`/`/pp-profile-data`
variants, `mbs-group-membership`, `ee-profile-data`'s own group-keyed sibling, bare
`/subscription-data/{ueId}`, and the other genuinely deferred subsystems
(`ee-subscriptions`/`sdm-subscriptions`, `subs-to-notify`, `pdtq-data`, `mbs-session-pol-data`,
`nidd-authorization-data`, `service-specific-authorization-data/{serviceType}`,
`Nudr_GroupIDmap`'s own `/nf-group-ids`) remain real, open, disclosed gaps.

