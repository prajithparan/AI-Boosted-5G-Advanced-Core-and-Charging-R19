## ADR-0115: gap-closure task #106 continuation -- UDR real Sponsor Connectivity Data (second non-per-UE resource)

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (29 of free5GC's ~42+ real
TS 29.504 resources closed as of ADR-0114). Real, confirmed-by-YAML-read:
`TS29519_Policy_Data.yaml`'s `/policy-data/sponsor-connectivity-data/{sponsorId}` (real schema
`SponsorConnectivityData` -- mandatory `aspIds`, optional `suppFeat`) is a real, genuinely
GET-only resource (`ReadSponsorConnectivityData`) -- confirmed by direct read, no other operation
exists for this path. Real, structurally notable: this is the **second UDR resource in this
project genuinely not keyed per-UE** (after `shared-data`, ADR-0110) -- keyed by `sponsorId`
alone, the real 3GPP concept (TS 23.503) of sponsored-data-connectivity policy shared across a
sponsor's own application service providers.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_sponsor_connectivity_data` table (`sponsor_id` PK,
  `data` JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `SponsorConnectivityDataStore` class (`seed`/`get` only,
  keyed by `sponsor_id` instead of `ue_id`, matching `SharedDataStore`'s own non-per-UE shape).
- `nfs/udr/src/main.cpp`: one new route (`GET`) at
  `/policy-data/sponsor-connectivity-data/{sponsorId}`, and a real, single (not looped-per-UE)
  seed call: `{"aspIds":["asp1"]}` for `sponsorId` `"sponsor1"` -- both this project's own
  arbitrary representative test values -- and one new OTel get counter.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl against a running `udr` process backed by a real PostgreSQL database: `GET` for the real
seeded `sponsorId` `sponsor1` -> real `200` with the exact seeded `aspIds` document; `GET` for an
unseeded `sponsorId` -> real `404`. Direct `psql` query against `udr_sponsor_connectivity_data`
independently confirmed the single seeded row persisted correctly.

### Testing and verification

`udr` built clean. Full `conformance_tests`: unchanged pass count (no new committed automated test
this pass, same disclosed manual-live-verification precedent already established), zero
regressions (325/325).

### What this ADR does NOT include

No NF's own existing logic calls this new route (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). Real,
disclosed simplification: the real spec also documents a distinct `204` ("resource found but no
data available") separate from `404` ("not found at all") for this resource -- this project's
simple existence-based store model (like every other GET-only UDR resource so far) only
distinguishes `200`-with-data vs `404`-not-provisioned, not the finer real "provisioned but empty"
case. This closes UDR resource #30 of free5GC's ~42+; roughly 12 remain a real, open, disclosed
gap (docs/CAPABILITY_GAP_ANALYSIS.md). Task #106 remains open (not fully closed).

