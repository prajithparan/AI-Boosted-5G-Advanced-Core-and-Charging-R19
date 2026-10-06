## ADR-0255: UDR `exposure-data` -- structured data for exposure, 4 paths, 10 operations

### What was built

`/exposure-data/*` per `TS29519_Exposure_Data.yaml`, which `TS29504_Nudr_DR.yaml` `$ref`s -- the
same indirection ADR-0253 documented and the reason ADR-0252's first pass under-counted UDR.

| Path | Methods | Schema |
|---|---|---|
| `/exposure-data/{ueId}/access-and-mobility-data` | PUT, GET, PATCH, DELETE | `AccessAndMobilityData` |
| `/exposure-data/{ueId}/session-management-data/{pduSessionId}` | PUT, GET, DELETE | `PduSessionManagementData` |
| `/exposure-data/subs-to-notify` | POST | `ExposureDataSubscription` |
| `/exposure-data/subs-to-notify/{subId}` | PUT, DELETE | `ExposureDataSubscription` |

`TS29519_Exposure_Data.yaml` was added to the codegen pilot list this turn, so the three DTOs are
generated, not hand-written. **UDR: 9 -> 5 unrouted.**

### Three real asymmetries, taken per path rather than generalised across the family

The discipline ADR-0254 applied to `pfds` having no PATCH matters more here, because this family
looks superficially like `application-data`'s and is not:

- **`session-management-data` has no PATCH**; its `access-and-mobility-data` sibling does.
- **`subs-to-notify` is POST-only as a collection** -- there is no GET list operation, unlike
  `application-data`'s own `subs-to-notify` (ADR-0254), which has both.
- **`subs-to-notify/{subId}` has no GET** -- PUT and DELETE only, again unlike
  `application-data`'s.

None of the three missing operations is registered here. Copying the sibling family's shape would
have produced four routes the spec does not define.

### PUT on the individual subscription is modify-only

Its real response set is 200/204 plus 404, with **no 201**. An unknown `subId` is therefore a 404,
not a create. This is deliberately different from ADR-0254's `application-data` `subs-to-notify`
PUT, which upserts: each was read from its own path's responses rather than carried across. The
pre-existing upsert behaviour on the application-data route is left as it is and noted here rather
than silently changed under an unrelated ADR.

PATCH on `access-and-mobility-data` is RFC 7396 merge-patch (from that path's own
`application/merge-patch+json` request content type) and documents **only 204** for success -- no
200-with-body alternative -- so it returns 204 and does not upsert on a missing resource.

PUT on both data resources distinguishes 201 (created) from 200 (updated, representation
returned). The spec also allows 204 for an update; returning 200 with the representation is the
same choice ADR-0253/0254 made, kept consistent.

### `ApplicationDataDocStore` renamed to `KeyedJsonDocStore`

`access-and-mobility-data` and `subs-to-notify` are single-key opaque JSON documents with exactly
the CRUD shape ADR-0254's store already had, so they reuse it rather than getting near-identical
copies. The class was no longer specific to `application-data`, so it is renamed; behaviour is
unchanged and each resource still gets its **own table**. `list()` exists on the class but is
deliberately **not** routed for either new resource, because neither has a GET-collection
operation in the spec.

`session-management-data` is keyed by **two** path parameters, so it gets its own
`ExposureSessionManagementDataStore` with a composite primary key -- the single-key store cannot
express it.

`pduSessionId` is persisted as TEXT, the raw path segment, rather than parsed to the integer the
`PduSessionId` schema defines. Stated rather than quietly decided: normalising it would mean
choosing whether `07` and `7` are the same resource, which the spec does not say.

### Verification

All 4 paths confirmed by **exact-literal `grep -F`** against the built binary, not ADR-0252's
tier-2 heuristic (which ADR-0253 showed weakens as shared prefixes accumulate -- and
`exposure-data` is exactly such a shared prefix).

Build clean, clang-format clean, conformance 333/333, `ctest` 474/474 -- of which **3 are new
here**: `tests/integration/test_udr_exposure_data.cpp` drives real `nrf` + `udr` OS processes over
TLS 1.3 + mTLS HTTP/2 with a real signed OAuth2 token against real PostgreSQL, and asserts the
behaviours this ADR claims rather than only that the routes exist -- 201-vs-200 on PUT, RFC 7396
merge semantics (an untouched field survives the patch), PATCH returning 204 and 404-not-upsert,
two PDU sessions under one UE staying separate resources, PATCH **not** being routed on
session-management-data, and the individual subscription's PUT returning 404 on an unknown subId.

### The live-verification gap ADR-0253 and ADR-0254 disclosed is now closed

Both previous ADRs shipped with "not live-verified: an existing UDR database needs the schema
re-applied". `nfs/udr/schema.postgres.sql` has now been applied to the lab database, creating the
tables ADR-0253, ADR-0254 and this ADR added, and the new integration tests exercise the
exposure-data routes against it end to end. The schema file is fully additive (`CREATE TABLE IF
NOT EXISTS` / `ADD COLUMN IF NOT EXISTS`, no `DROP`/`TRUNCATE`), so re-applying it is safe on a
populated database.

**Honest limit on what that proves**: the live run covers this ADR's own routes. ADR-0253's and
ADR-0254's routes now have their tables present and their `KeyedJsonDocStore` code path exercised
by the shared store, but their specific routes still have no integration test of their own. The
schema half of their disclosure is closed; the route half is narrowed, not closed.

### A pre-existing test defect surfaced while doing this (not caused by it)

`UdrIntegration.AmfContextLifecycle` fails on any second run against a persistent database. It
PUTs `imsi-999700000000001` expecting 201, and the resource has **no DELETE operation in the
spec** (correctly noted in this file's own header), so the test cannot clean up after itself and
the next run sees 204. It passes exactly once per database reset, which is why CI -- with a fresh
container each run -- has never shown it. Confirmed unrelated to this change: the diff touches no
AMF-context code, and tests 1-68 do not create the row. Left as-is rather than rewritten under an
unrelated ADR; recorded here so it is not rediscovered as a mystery.

### Disclosed gaps

- **No notification delivery.** `ExposureDataSubscription` carries a `notificationUri` and
  `monitoredResourceUris`, and the spec defines an `ExposureDataChangeNotification` callback.
  Subscriptions are stored and returned correctly, but nothing here pushes a notification when the
  underlying data changes. Same disclosed gap class as `application-data`'s own `subs-to-notify`
  (ADR-0254) and the `influenceData` subscriptions (ADR-0253) -- consistent, and still a real gap.
- `immRep`/`immReports` (immediate reporting on subscription creation) are stored as supplied and
  not acted upon, which follows from the above.

