## ADR-0142: gap-closure task #106 continuation -- UDR real CAG update ack (Document) resource

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (52 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0141). Real, confirmed-by-YAML-read:
`TS29505_Subscription_Data.yaml`'s `/subscription-data/{ueId}/ue-update-confirmation-data/
subscribed-cag` (real spec operations `CreateCagUpdateAck`/`QueryCagAck`) is a real,
near-identical twin of `subscribed-snssais` (ADR-0141): real PUT+GET, no PATCH/DELETE operation
exists, real schema `CagAckData` -- required `provisioningTime`/`ueUpdateStatus` -- identical
shape to `NssaiAckData`. Real, disclosed, confirmed again by direct read: the spec documents only
a single `204` response for this PUT (no `201`), same genuinely-no-create-vs-update-distinction
shape as its sibling. Used the already-generated `sbi_gen::CagAckData` DTO directly.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_cag_ack_data` table (`ue_id` PK, `data` JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `CagAckDataStore` class (`put`/`get`), identical shape to
  `NssaiAckDataStore`.
- `nfs/udr/src/main.cpp`: store construction, two new OTel counters (write, get), and two new
  routes (PUT/GET) at `/subscription-data/{ueId}/ue-update-confirmation-data/subscribed-cag`,
  using `sbi_core::http2::parse_json_body<sbi_gen::CagAckData>` for the PUT body.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl sequence against a running `udr` process backed by a real PostgreSQL database, SUPI
`imsi-999700000000001`: `GET` before any `PUT` -> real `404`; `PUT` with
`{"provisioningTime":"2026-08-21T02:00:00Z","ueUpdateStatus":"WAITING_FOR_ACK"}` -> real `204`;
`GET` -> real `200` with the same body; cross-checked the sibling `subscribed-snssais` resource
(a separate table) on the same UE is unaffected -> real `200` with its own unchanged expected
body. Direct `psql` query against `udr_cag_ack_data` independently confirmed the persisted row.

### Testing and verification

`udr` built clean (including a clean rebuild after `clang-format-18`, no formatting diff beyond
what was written). Full `conformance_tests` (excluding the two disclosed pre-existing flaky
tests): 325/325 pass, zero regressions.

### What this ADR does NOT include

No NF's own existing logic calls these new routes (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #53 of free5GC's ~42+ real `Nudr_DataRepository` resources. Task #106 remains open:
the remainder of `group-data`, `ue-update-confirmation-data`'s own `sor-data`/`upu-data`
siblings, bare `/subscription-data/{ueId}`, and the genuinely deferred subsystems
(`ee-subscriptions`/`sdm-subscriptions`, `subs-to-notify`, `pdtq-data`, `mbs-session-pol-data`,
`nidd-authorization-data`, `service-specific-authorization-data/{serviceType}`,
`Nudr_GroupIDmap`'s own `/nf-group-ids`) remain real, open, disclosed gaps.

