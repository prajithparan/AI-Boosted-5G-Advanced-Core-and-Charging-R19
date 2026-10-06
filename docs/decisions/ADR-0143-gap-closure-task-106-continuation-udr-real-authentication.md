## ADR-0143: gap-closure task #106 continuation -- UDR real Authentication SoR + Authentication
UPU (Document) resources

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (53 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0142). Real, confirmed-by-YAML-read:
`TS29505_Subscription_Data.yaml`'s `ue-update-confirmation-data/sor-data` (real spec operations
`CreateAuthenticationSoR`/`QueryAuthSoR`/`UpdateAuthenticationSoR`) and `.../upu-data`
(`CreateAuthenticationUPU`/`QueryAuthUPU`) are the two remaining sibling resources of this group
after `subscribed-snssais` (ADR-0141) and `subscribed-cag` (ADR-0142) -- both closed together in
this ADR since they share the same `UeUpdateStatus`-based schema shape and were read from the
YAML side by side.

Real, disclosed asymmetry found on direct read, not assumed from the sibling pattern: `sor-data`
has a real PUT+GET **+PATCH** (`UpdateAuthenticationSoR`, real RFC 6902
`application/json-patch+json`, responses documenting both `204` and an optional `200` with a
`PatchResult` body -- this project's own established precedent, first set for
`nidd-authorizations`, is to always return the simpler `204` rather than build the `PatchResult`
report body, followed here too) -- genuinely richer than either ack resource closed so far.
`upu-data` has only PUT+GET, no PATCH/DELETE at all -- confirmed by reading past its `200`
response block to the end of the resource definition, no third operation follows. Both PUTs
document only a single `204` response (no `201`), the same no-create-vs-update-distinction shape
as `subscribed-snssais`/`subscribed-cag`. Real schemas `SorData` (required
`provisioningTime`/`ueUpdateStatus`; optional `sorXmacIue`/`sorMacIue`/`meSupportOfSorCmci`/
`meSupportOfSorSnpnSi`/`meSupportOfSorSnpnSiLs`) and `UpuData` (required
`provisioningTime`/`ueUpdateStatus`; optional `upuXmacIue`/`upuMacIue`/`meSupportUHP`) were both
already generated (`sbi_gen::SorData`, `sbi_gen::UpuData_Subscription_Data` -- the latter suffixed
because a second, unrelated `UpuData` also exists under `TS29509_Nausf_UPUProtection.yaml`'s own
namespace) -- confirmed field-for-field against the real YAML before use, used directly, no DTO
hand-written.

### Implementation

- `nfs/udr/schema.postgres.sql`: two new tables, `udr_sor_data` and `udr_upu_data` (both `ue_id`
  PK, `data` JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `SorDataStore` class (`put`/`get`/`apply_patch` -- the
  latter NOT upsert-capable, returns `nullopt` if `ue_id` doesn't exist yet, same precedent as
  `NiddAuthorizationInfoStore::apply_patch`, since a real PUT-based create path already exists for
  this resource) and new `UpuDataStore` class (`put`/`get` only, identical shape to
  `NssaiAckDataStore`/`CagAckDataStore`).
- `nfs/udr/src/main.cpp`: store construction, five new OTel counters (sor-data write/get,
  upu-data write/get -- PATCH reuses the write counter, same precedent as
  `nidd_authorization_write_counter`), and five new routes (PUT/GET/PATCH at
  `.../ue-update-confirmation-data/sor-data`, PUT/GET at `.../upu-data`), using
  `sbi_core::http2::parse_json_body<sbi_gen::SorData>` and
  `parse_json_body<sbi_gen::UpuData_Subscription_Data>` for the two PUT bodies.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl sequence against a running `udr` process (freshly built, freshly started, registered
with a freshly started `nrf`) backed by real PostgreSQL:
- `sor-data`, SUPI `imsi-999700000000001`: `GET` before any `PUT` -> real `404`; `PUT` with
  `{"provisioningTime":"2026-08-21T20:00:00Z","ueUpdateStatus":"ACKNOWLEDGEMENT_SUCCESSFUL",
  "meSupportOfSorCmci":true}` -> real `204`; `GET` -> real `200` with the same body; `PATCH` with
  a real RFC 6902 `[{"op":"replace","path":"/meSupportOfSorCmci","value":false}]` -> real `204`;
  `GET` again -> real `200` with `meSupportOfSorCmci` now `false`, `provisioningTime`/
  `ueUpdateStatus` unchanged.
- `upu-data`, SUPI `imsi-999700000000002`: `GET` before any `PUT` -> real `404`; `PUT` with
  `{"provisioningTime":"2026-08-21T20:01:00Z","ueUpdateStatus":"ACKNOWLEDGEMENT_SUCCESSFUL",
  "meSupportUHP":true}` -> real `204`; `GET` -> real `200` with the same body; a second, idempotent
  `PUT` with a different `provisioningTime` and no `meSupportUHP` -> real `204`; `GET` -> real `200`
  confirming a genuine overwrite (new `provisioningTime`, `meSupportUHP` gone).
- Direct `psql SELECT` against both `udr_sor_data` and `udr_upu_data` independently confirmed the
  exact persisted rows match what curl returned.

### Testing and verification

`udr` built clean both before and after `clang-format-18` (reformat added no diff beyond what was
newly written). Full `conformance_tests` (excluding the two disclosed pre-existing flaky tests):
325/325 pass, zero regressions.

### What this ADR does NOT include

No NF's own existing logic calls these new routes (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). `sor-data`'s
real PATCH always returns `204`, never the optional `200`-with-`PatchResult` variant the spec also
documents (established project precedent, not a new simplification). This closes all four
`ue-update-confirmation-data` sub-resources this project has surveyed to date (`subscribed-snssais`,
`subscribed-cag`, `sor-data`, `upu-data`) and closes UDR resource #54 of free5GC's ~42+ real
`Nudr_DataRepository` resources. Task #106 remains open: the remainder of `group-data`, bare
`/subscription-data/{ueId}`, and the genuinely deferred subsystems (`ee-subscriptions`/
`sdm-subscriptions`, `subs-to-notify`, `pdtq-data`, `mbs-session-pol-data`,
`nidd-authorization-data`, `service-specific-authorization-data/{serviceType}`,
`Nudr_GroupIDmap`'s own `/nf-group-ids`) remain real, open, disclosed gaps.

