## ADR-0253: UDR `application-data` traffic-influence family (first ADR-0252 gap-closure)

### Context

ADR-0252's full-coverage audit found UDR's 23 unrouted paths -- the one large, previously
undisclosed API gap in the project. This closes the family that audit named as mattering most:
`/application-data/influenceData`, the traffic-influence data store a real NEF/AF path depends on,
with all 14 Nnef YAML files already built here.

### What was built

**4 paths, 9 operations**, taken from `TS29504_Nudr_DR.yaml` -- which `$ref`s them into
`TS29519_Application_Data.yaml`, where the real operations live (the `$ref` indirection is why
ADR-0252's first path-counting attempt under-reported UDR):

| Path | Methods |
|---|---|
| `/application-data/influenceData` | GET (collection) |
| `/application-data/influenceData/{influenceId}` | PUT, PATCH, DELETE |
| `/application-data/influenceData/subs-to-notify` | GET, POST |
| `/application-data/influenceData/subs-to-notify/{subscriptionId}` | GET, PUT, DELETE |

Backed by two new Postgres tables and two store classes -- deliberately separate, since
`TrafficInfluData` and `TrafficInfluSub` are genuinely distinct resources with distinct schemas,
following the same "real, distinct resource, not a rename" precedent `udr_amf_non3gpp_context`
established.

**Read from the YAML, not assumed from sibling resources:** PATCH here is RFC 7396 merge-patch
(`application/merge-patch+json`), where some other UDR resources are RFC 6902. PUT distinguishes
201 from 200. PATCH on a missing resource returns 404 rather than upserting -- unlike
`udr_am_policy_data`'s own deliberately upsert-capable patch.

### Disclosed scope limit: query-parameter filtering

The GET collection defines **8** query parameters (`influence-Ids`, `dnns`, `snssais`,
`internal-Group-Ids`, `internal-group-ids-Add`, `subscriber-categories`, `supis`, `supp-feat`).
Only `influence-Ids` is honoured -- the one filtering on a field this store keys on. The rest
filter on fields inside the stored document that this build does not index. An unhonoured filter
returns the **unfiltered** set; it does not silently pretend to have filtered. Stated at the route
itself and here, not left to be discovered.

### A flaw found in ADR-0252's own audit heuristic

Verifying the result exposed a real weakness worth recording. After this family landed, the audit
script reported UDR dropping 23 -> 11 unrouted, with "likely-dynamic" rising 8 -> 16. **That
improvement was not real.** The literal `application-data` now exists in the binary, so
still-unimplemented siblings (`pfds`, `bdtPolicyData`, `iptvConfigData`, `serviceParamData`,
`subs-to-notify`) began matching the all-segments-present test and were reclassified as probably
routed.

The tier-2 heuristic therefore gets **weaker as coverage grows**, because shared prefixes
accumulate. The reliable check is a full-literal match for the exact path, which was run for all
nine paths and is the basis for the honest figures: **4 closed, 19 UDR paths still unrouted.**
`docs/NF_API_COVERAGE_AUDIT.md` carries the same correction.

### Testing

UDR builds clean, clang-format clean, conformance 333/333, `ctest` 469/469. Not yet live-verified
against a running UDR with the new tables applied -- `schema.postgres.sql` gained two tables that
an existing database will not have until re-applied, which is the next step before these routes
are exercised for real.

