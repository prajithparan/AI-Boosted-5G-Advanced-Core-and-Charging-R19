## ADR-0120: gap-closure task #106 continuation -- UDR real GetRoutingIDs (Nudr_GroupIDmap, a genuinely different Nudr API)

### Context

Continuing task #106's gap-closure. Both remaining real `Nudr_DataRepository` list-siblings
(`pdtq-data`, `mbs-session-pol-data`) are blocked on real, disclosed gaps (array-query-param
parsing and a deeply nested path-key encoding, respectively -- see ADR-0119). Surfaced this to the
user (this is a real scope decision, not a spec ambiguity resolvable by precedent) and got explicit
direction to implement `TS29504_Nudr_GroupIDmap.yaml`'s `GetRoutingIDs` resource
(`/routing-ids`) instead.

Real, disclosed, and important: this is a genuinely **different** real Nudr API from every other
resource closed in this series. `TS29504_Nudr_GroupIDmap.yaml` defines `Nudr_GroupIDmap` (real
server base path `/nudr-group-id-map/v1`, real OAuth2 scope `nudr-group-id-map`) -- a separate
service from `Nudr_DataRepository` (`/nudr-dr/v2`, scope `nudr-dr`) that every prior ADR in this
series has closed resources against. Per TS 29.504's own real structure, both APIs are hosted by
the same NF (UDR), so implementing this inside the existing `udr` binary is correct -- but it does
**NOT** count toward the "N of free5GC's ~42+ real `Nudr_DataRepository` resources" metric this
whole series has been tracking, since free5GC's own comparison baseline is specifically
`Nudr_DataRepository`. Confirmed by direct YAML read: `GetRoutingIDs` is real GET-only (no
`PUT`/`POST`/`PATCH`/`DELETE` exists for this resource at all), with two real required scalar
query parameters (`nf-type`: real `NFType` enum string, `nf-group-id`: real `NfGroupId` plain
string) -- no array-parsing or object-path-key ambiguity, genuinely clean.

Also surveyed while checking `Nudr_GroupIDmap` for other candidates: `/nf-group-ids` (`GetNfGroupIDs`)
requires a real required array query parameter (`nf-type`, `style: form, explode: false`) -- same
disclosed array-query-param parsing gap blocking `pdtq-data`, left deferred.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_routing_ids` table, composite-keyed
  (`nf_type`, `nf_group_id`) PK, `data` JSONB.
- `nfs/udr/src/stores.hpp`/`.cpp`: new `RoutingIdStore` class (`seed`/`get`), composite-key
  pattern matching `PpDataEntryStore`'s own precedent.
- `nfs/udr/src/main.cpp`: new `kGroupIdMapApiRoot` constant (`/nudr-group-id-map/v1`, distinct
  from `kApiRoot`), one new `GET` route at `/routing-ids` reading both required query parameters
  directly off `req.query_params` (real `400 ProblemDetails` if either is missing), one new OTel
  counter, and seed data for this project's own real, already-built `UDM` NF type paired with an
  arbitrary representative `nf-group-id` (`udm-group-1`).

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl against a running `udr` process backed by a real PostgreSQL database, at the real
`/nudr-group-id-map/v1/routing-ids` path (not `/nudr-dr/v2`): `GET` with both query parameters
missing -> real `400`; `GET` with only `nf-type` present -> real `400`; `GET` with the seeded pair
(`nf-type=UDM&nf-group-id=udm-group-1`) -> real `200` with `{"routingIndicators":["0001"]}`; `GET`
with an unseeded pair (`nf-type=SMF&nf-group-id=nonexistent`) -> real `404`. Direct `psql` query
against `udr_routing_ids` independently confirmed exactly one row, `(UDM, udm-group-1)`, matching
the seeded body.

### Testing and verification

`udr` built clean. Full `conformance_tests`: unchanged pass count (no new committed automated test
this pass, same disclosed manual-live-verification precedent already established), zero
regressions (325/325); `structural_conformance` passed, confirming the new distinct API root
didn't break the project's own schema-conformance tooling.

### What this ADR does NOT include

No NF's own existing logic calls this new route (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). Does **not**
increment the "N of free5GC's ~42+ `Nudr_DataRepository` resources" count -- still 34, unchanged
from ADR-0119 -- since this is a real, distinct Nudr API (see Context above). `Nudr_GroupIDmap`'s
own remaining resources (`/nf-group-ids`, `/nf-group-ids/subscriptions` collection and
subscription-lifecycle endpoints) remain deferred: the former on the same array-query-param
parsing gap, the latter genuinely out of scope for this pass (not surveyed in detail). Task #106
remains open (not fully closed).

