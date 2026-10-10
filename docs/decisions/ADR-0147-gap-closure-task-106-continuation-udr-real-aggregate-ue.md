## ADR-0147: gap-closure task #106 continuation -- UDR real aggregate UE Update Confirmation Data
resource

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (57 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0146, after the fourth `group-data`
sub-resource). With `group-data` now exhausted of unblocked candidates (both bare collection GETs
and their `/internal`/`/pp-profile-data` variants confirmed blocked on the real array-query-param
class), this pass surveyed `TS29505_Subscription_Data.yaml` more broadly rather than staying
inside `group-data`. Real, confirmed-by-YAML-read: `/subscription-data/{ueId}/
ue-update-confirmation-data` (bare, no trailing sub-resource segment, real spec operation
`QueryUeUpdConf`) is a real, distinct resource from its four individual children
(`subscribed-snssais`/`subscribed-cag`/`sor-data`/`upu-data`, all already closed ADR-0141 through
ADR-0143) -- an aggregate "get everything at once" view, schema `UeUpdConfData`: every field
optional (`sorData`/`upuData`/`nssaiAckData`/`cagAckData`). Genuinely unblocked: only the
already-established, not-honored `supported-features` query parameter, no array or complex-object
parameter of the class that has blocked every other bare-`{ueId}`-scoped aggregate this project
has surveyed so far (`context-data`'s own bare GET, `/subscription-data/{ueId}` itself).

Real, deliberate design decision, disclosed rather than silently assumed: this resource is
implemented as a live composition over the four existing sub-resource stores' own `get()` calls,
not a fifth, duplicate PostgreSQL table -- the real data already lives in
`udr_sor_data`/`udr_upu_data`/`udr_nssai_ack_data`/`udr_cag_ack_data`, and persisting a second copy
would risk the two going out of sync. Because every field is optional and the aggregate document
itself has no real create/update operation to key a "does it exist" flag off of, the route always
returns `200` (an empty object if none of the four sub-resources exist for that UE), rather than
inventing a rule for when the spec's documented `404` should fire.

### Implementation

- No new schema table -- reuses `udr_sor_data`/`udr_upu_data`/`udr_nssai_ack_data`/
  `udr_cag_ack_data` directly.
- No new store class -- the route handler itself calls each of the four already-existing stores'
  `get()` methods and composes a `UeUpdConfData`-shaped JSON object from whichever return a value.
- `nfs/udr/src/main.cpp`: one new OTel counter (get) and one new route (GET) at
  `/subscription-data/{ueId}/ue-update-confirmation-data`.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl sequence against a running `udr` process (freshly built, freshly started, registered
with a freshly started `nrf`) backed by real PostgreSQL, SUPI `imsi-999700000000009`:

- `GET` the aggregate before any of the four sub-resources exist -> real `200` with an empty JSON
  object (not a `404`, per this ADR's own disclosed design decision).
- `PUT` `sor-data` (`{"provisioningTime":"2026-08-21T21:00:00Z",
  "ueUpdateStatus":"ACKNOWLEDGEMENT_SUCCESSFUL"}`) -> real `204`.
- `PUT` `subscribed-cag` (`{"provisioningTime":"2026-08-21T21:01:00Z",
  "ueUpdateStatus":"WAITING_FOR_ACK"}`) -> real `204`.
- `GET` the aggregate again -> real `200` with `sorData`/`cagAckData` both present (matching the
  two PUT bodies exactly) and `upuData`/`nssaiAckData` genuinely absent (not present as `null`, not
  fabricated) -- confirms the partial-presence composition logic is real, not a fixed shape.
- Cross-checked the individual `sor-data` GET route is unaffected -> real `200` with the identical
  body.
- Direct `psql SELECT` against `udr_sor_data` and `udr_cag_ack_data` independently confirmed the
  exact persisted rows match what the aggregate GET composed.

### Testing and verification

`udr` built clean both before and after `clang-format-18` (reformat added no diff beyond what was
newly written). Full `conformance_tests` (excluding the two disclosed pre-existing flaky tests):
325/325 pass, zero regressions.

### What this ADR does NOT include

No NF's own existing logic calls this new route (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #58 of free5GC's ~42+ real `Nudr_DataRepository` resources. Task #106 remains open:
`group-data`'s remaining genuinely-blocked resources, bare `/subscription-data/{ueId}` and
`{ueId}/context-data` (both confirmed blocked on their own real array-query-param requirements),
and the other genuinely deferred subsystems (`ee-subscriptions`/`sdm-subscriptions`,
`subs-to-notify`, `pdtq-data`, `mbs-session-pol-data`, `nidd-authorization-data`,
`service-specific-authorization-data/{serviceType}`, `Nudr_GroupIDmap`'s own `/nf-group-ids`)
remain real, open, disclosed gaps.

