## ADR-0108: gap-closure task #106 continuation -- UDR real Parameter Provision profile Data (Document)

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (22 of free5GC's ~42+ real
TS 29.504 resources closed as of ADR-0107). Real, confirmed-by-YAML-read:
`TS29505_Subscription_Data.yaml`'s `/subscription-data/{ueId}/pp-profile-data` (real schema
`PpProfileData` -- an `allowedMtcProviders` map keyed by `PpDataType` or the real, documented
special key `"ALL"`, every field optional) is a real, genuinely GET-only resource (`QueryPPData`)
-- confirmed by grepping every operationId referencing this path, no create/update operation
exists at all, same real "provisioned out-of-band, seeded at startup" shape already established
for the other GET-only UDR resources (ADR-0102/0103/0104/0105).

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_pp_profile_data` table (`ue_id` PK, `data` JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `PpProfileDataStore` class (`seed`/`get` only, matching the
  established GET-only shape).
- `nfs/udr/src/main.cpp`: one new route (`GET`) at `/subscription-data/{ueId}/pp-profile-data`,
  and a real seed loop for the same two real test SUPIs every other GET-only UDR resource in this
  project already seeds, populated with `{"allowedMtcProviders":{"ALL":[{"afId":"af1"}]}}` --
  `"ALL"` is the real, documented special key from `PpProfileData`'s own description text (not
  fabricated), `afId: "af1"` is this project's own arbitrary representative test value -- and one
  new OTel get counter.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl against a running `udr` process backed by a real PostgreSQL database: `GET` for the real
seeded SUPI `imsi-999700000000001` -> real `200` with the exact seeded `allowedMtcProviders`
document; `GET` for an unseeded SUPI -> real `404`. Direct `psql` query against
`udr_pp_profile_data` independently confirmed both seeded rows persisted correctly across the
fresh startup.

### Testing and verification

`udr` built clean. Full `conformance_tests`: unchanged pass count (no new committed automated test
this pass, same disclosed manual-live-verification precedent already established), zero
regressions (325/325).

### What this ADR does NOT include

No NF's own existing logic calls this new route (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure) --
`pp-data-store` (a real, separate `{afInstanceId}`-keyed sibling resource) remains open, deferred
to its own scoped turn. This closes UDR resource #23 of free5GC's ~42+; roughly 19 remain a real,
open, disclosed gap (docs/CAPABILITY_GAP_ANALYSIS.md). Task #106 remains open (not fully closed).

