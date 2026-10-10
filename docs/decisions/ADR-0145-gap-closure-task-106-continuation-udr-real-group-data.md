## ADR-0145: gap-closure task #106 continuation -- UDR real group-data individual 5G MBS Group
Membership resource

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (55 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0144). Real, confirmed-by-YAML-read: the
`group-data` group's third sub-resource,
`/subscription-data/group-data/mbs-group-membership/{externalGroupId}` (real spec operations
`Create5GmbsGroup`/`GetMulticastMbsGroupMemb`/`Modify5GmbsGroup`/`Delete5GmbsGroup`, schema
`MulticastMbsGroupMemb` -- required `multicastGroupMemb` (array of `Gpsi`), plus optional
`afInstanceId`/`internalGroupIdentifier`), is structurally an exact twin of
`5g-vn-groups/{externalGroupId}` (ADR-0144): real GET+PUT+PATCH+DELETE, PUT documents ONLY `201`
(operationId literally `Create5GmbsGroup`), PATCH is real RFC 6902
`application/json-patch+json`, NOT upsert-capable. Confirmed via the generated `to_json`
(`build/generated/sbi_gen/TS29122_CommonData_grp.cpp:16395`) that this schema has no
leading-digit-key renaming issue (unlike `5GVnGroupConfiguration`'s own `5gVnGroupData`) -- the
C++ struct field names match the real JSON keys exactly, checked proactively this time given
ADR-0144's own disclosed test mistake.

Also surveyed in the same pass, confirmed genuinely blocked, not silently skipped: the sibling
bare collection GET at `/subscription-data/group-data/mbs-group-membership` (real spec
`Query5GmbsGroup`) has the identical real `gpsis` query parameter declared
`style: form, explode: false` already disclosed for `5g-vn-groups`'s own `Query5GVnGroup`
sibling and `pdtq-data`/`nf-group-ids` -- left deferred.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_mbs_group_membership` table (`ext_group_id` PK, `data`
  JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `MbsGroupMembershipStore` class (`put`/`get`/
  `apply_patch`/`remove`), structurally identical to `FiveGVnGroupStore`.
- `nfs/udr/src/main.cpp`: store construction, three new OTel counters (write, get, delete -- PATCH
  reuses the write counter), and four new routes (GET/PUT/PATCH/DELETE) at
  `/subscription-data/group-data/mbs-group-membership/{externalGroupId}`, using
  `sbi_core::http2::parse_json_body<sbi_gen::MulticastMbsGroupMemb>` for the PUT body.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl sequence against a running `udr` process (freshly built, freshly started, registered
with a freshly started `nrf`) backed by real PostgreSQL, `externalGroupId` `grp-mbs-001`:

- `GET` before any `PUT` -> real `404`.
- `PUT` with `{"multicastGroupMemb":["gpsi-msisdn-1234567890","gpsi-msisdn-1234567891"],
  "afInstanceId":"af-001"}` -> real `201`, body echoes the same structure back (correct JSON keys
  on the first attempt this time).
- `GET` after `PUT` -> real `200` with the same body.
- `PATCH` with a real RFC 6902 `[{"op":"add","path":"/multicastGroupMemb/-",
  "value":"gpsi-msisdn-1234567892"}]` -> real `204`.
- `GET` after `PATCH` -> real `200` with all three members present, `afInstanceId` unchanged.
- `PATCH` against a nonexistent `externalGroupId` -> real `404` (confirms `apply_patch` is NOT
  upsert-capable).
- Direct `psql SELECT` against `udr_mbs_group_membership` independently confirmed the persisted
  row matched curl's response.
- `DELETE` -> real `204`; `GET` after -> real `404`; `psql SELECT` confirmed zero rows remained.
- Cross-checked table isolation: `GET` on the sibling `5g-vn-groups/{externalGroupId}` resource
  using the same `grp-mbs-001` identifier -> real `404` (never created there, confirms the two
  `group-data` sub-resources are genuinely separate tables/keyspaces, not accidentally sharing
  storage).

### Testing and verification

`udr` built clean both before and after `clang-format-18` (reformat added no diff beyond what was
newly written). Full `conformance_tests` (excluding the two disclosed pre-existing flaky tests):
325/325 pass, zero regressions.

### What this ADR does NOT include

No NF's own existing logic calls these new routes (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #56 of free5GC's ~42+ real `Nudr_DataRepository` resources -- the third real
`group-data` sub-resource, after `group-identifiers` (ADR-0140) and
`5g-vn-groups/{externalGroupId}` (ADR-0144). Task #106 remains open: both bare collection GETs
(`5g-vn-groups`'s `Query5GVnGroup` and `mbs-group-membership`'s own `Query5GmbsGroup`, both
confirmed genuinely blocked on the same array-query-param class), `5g-vn-groups`'s/
`mbs-group-membership`'s own `/internal`/`/pp-profile-data` variants, `ee-profile-data`'s own
group-keyed sibling, bare `/subscription-data/{ueId}`, and the other genuinely deferred
subsystems (`ee-subscriptions`/`sdm-subscriptions`, `subs-to-notify`, `pdtq-data`,
`mbs-session-pol-data`, `nidd-authorization-data`,
`service-specific-authorization-data/{serviceType}`, `Nudr_GroupIDmap`'s own `/nf-group-ids`)
remain real, open, disclosed gaps.

