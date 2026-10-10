## ADR-0112: gap-closure task #106 continuation -- UDR real Event Exposure Data (Document)

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (26 of free5GC's ~42+ real
TS 29.504 resources closed as of ADR-0111). Real, confirmed-by-YAML-read:
`TS29505_Subscription_Data.yaml`'s `/subscription-data/{ueId}/ee-profile-data` (real schema
`EeProfileData` -- optional `restrictedEventTypes` (array of real `EventType` enum values,
`TS29503_Nudm_EE.yaml`), `allowedMtcProvider`, `iwkEpcRestricted`, every field optional) is a
real, genuinely GET-only resource (`QueryEEData`) -- confirmed by grepping every operationId
referencing this exact path (only one), no create/update operation exists at all, same real
"provisioned out-of-band, seeded at startup" shape already established for the other GET-only UDR
resources. Real, distinct from this project's own UDM-side `Nudm_EE` work (task #105) -- this is
the real Nudr_DataRepository backing document, not the UDM service surface.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_ee_profile_data` table (`ue_id` PK, `data` JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `EeProfileDataStore` class (`seed`/`get` only, matching the
  established GET-only shape).
- `nfs/udr/src/main.cpp`: one new route (`GET`) at `/subscription-data/{ueId}/ee-profile-data`,
  and a real seed loop for the same two real test SUPIs every other GET-only UDR resource in this
  project already seeds, populated with `{"restrictedEventTypes":["LOSS_OF_CONNECTIVITY"]}` --
  `LOSS_OF_CONNECTIVITY` is a real enum value from `EventType` (`TS29503_Nudm_EE.yaml`), this
  project's own representative test choice -- and one new OTel get counter.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl against a running `udr` process backed by a real PostgreSQL database: `GET` for the real
seeded SUPI `imsi-999700000000001` -> real `200` with the exact seeded `restrictedEventTypes`
document; `GET` for an unseeded SUPI -> real `404`. Direct `psql` query against
`udr_ee_profile_data` independently confirmed both seeded rows persisted correctly across the
fresh startup.

### Testing and verification

`udr` built clean. Full `conformance_tests`: unchanged pass count (no new committed automated test
this pass, same disclosed manual-live-verification precedent already established), zero
regressions (325/325).

### What this ADR does NOT include

No NF's own existing logic calls this new route (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure) --
`allowedMtcProvider`/`iwkEpcRestricted` are left unpopulated in the seed data, a real, disclosed
gap rather than a guessed nested shape, same precedent as this resource's own siblings;
`ee-profile-data`'s own real group-keyed sibling (`/subscription-data/group-data/{ueGroupId}/
ee-profile-data`) remains open, deferred to its own scoped turn. This closes UDR resource #27 of
free5GC's ~42+; roughly 15 remain a real, open, disclosed gap
(docs/CAPABILITY_GAP_ANALYSIS.md). Task #106 remains open (not fully closed).

