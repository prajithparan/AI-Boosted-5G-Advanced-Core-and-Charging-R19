## ADR-0141: gap-closure task #106 continuation -- UDR real NSSAI update ack (Document) resource

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (51 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0140). Real, confirmed-by-YAML-read: surveyed
three real candidates in the same pass:

- Bare `/subscription-data/{ueId}` (real spec `operationId` `QueryUeSubscribedData`) --
  genuinely blocked, not attempted: real query parameters include `dataset-names` (a real
  `style: form, explode: false` array) and `single-nssai` (a real `content: application/json`
  complex-object parameter) -- both real gap classes already disclosed as blocking other
  resources (array-query-param parsing for `pdtq-data`/`GetSharedData`; complex-object
  query-param parsing for `nidd-authorization-data`). This resource genuinely combines both.
- `/subscription-data/{ueId}/ue-update-confirmation-data/subscribed-snssais` (real operations
  `CreateOrUpdateNssaiAck`/`QueryNssaiAck` -- real PUT+GET, no complex query parameters at all)
  -- this is the one implemented by this ADR. Real schema `NssaiAckData` requires
  `provisioningTime` (`DateTime`) and `ueUpdateStatus` (real `UeUpdateStatus` enum:
  `NOT_SENT`/`SENT_NO_ACK_REQUIRED`/`WAITING_FOR_ACK`/`ACK_RECEIVED`/`NEGATIVE_ACK_RECEIVED`).
  Real, disclosed: unlike every other PUT resource this project has closed, the spec documents
  only a single `204` response for this PUT (no `201` at all) -- confirmed by direct read, not
  assumed -- so there is genuinely no create-vs-update distinction for this resource, and `put()`
  correctly returns `void`, not a `bool`. This is the first real
  `ue-update-confirmation-data` sub-resource closed; its siblings (`sor-data`, `upu-data`,
  `subscribed-cag`) remain genuinely deferred, not dropped. Used the already-generated
  `sbi_gen::NssaiAckData` DTO directly, no hand-written DTO.
- `.../subscribed-cag` (a real, near-identical twin resource using `CagAckData`, confirmed
  matching shape) -- deliberately left for its own turn, one resource per turn.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_nssai_ack_data` table (`ue_id` PK, `data` JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `NssaiAckDataStore` class (`put`/`get`) -- `put()` is
  `void`, matching the spec's own real single-response-code PUT shape.
- `nfs/udr/src/main.cpp`: store construction, two new OTel counters (write, get), and two new
  routes (PUT/GET) at `/subscription-data/{ueId}/ue-update-confirmation-data/subscribed-snssais`,
  using `sbi_core::http2::parse_json_body<sbi_gen::NssaiAckData>` for the PUT body.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl sequence against a running `udr` process backed by a real PostgreSQL database, SUPI
`imsi-999700000000001`: `GET` before any `PUT` -> real `404`; `PUT` with
`{"provisioningTime":"2026-08-21T00:00:00Z","ueUpdateStatus":"ACK_RECEIVED"}` -> real `204`;
`GET` -> real `200` with the same body; `PUT` again with a different `ueUpdateStatus`
(`NEGATIVE_ACK_RECEIVED`) -> real `204` (not `201`, confirming no create-vs-update distinction
exists); `GET` -> real `200` reflecting the updated value. Direct `psql` query against
`udr_nssai_ack_data` independently confirmed the persisted, updated row.

### Testing and verification

`udr` built clean (including a clean rebuild after `clang-format-18`, no formatting diff beyond
what was written). Full `conformance_tests` (excluding the two disclosed pre-existing flaky
tests): 325/325 pass, zero regressions.

### What this ADR does NOT include

No NF's own existing logic calls these new routes (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #52 of free5GC's ~42+ real `Nudr_DataRepository` resources. Bare
`/subscription-data/{ueId}` is now confirmed genuinely blocked (surveyed, not silently skipped) on
the combined array-query-param and complex-object-query-param gaps disclosed above. Task #106
remains open: the remainder of `group-data`, `ue-update-confirmation-data`'s own `sor-data`/
`upu-data`/`subscribed-cag` siblings, bare `/subscription-data/{ueId}` (now confirmed blocked),
and the genuinely deferred subsystems (`ee-subscriptions`/`sdm-subscriptions`, `subs-to-notify`,
`pdtq-data`, `mbs-session-pol-data`, `nidd-authorization-data`,
`service-specific-authorization-data/{serviceType}`, `Nudr_GroupIDmap`'s own `/nf-group-ids`)
remain real, open, disclosed gaps.

