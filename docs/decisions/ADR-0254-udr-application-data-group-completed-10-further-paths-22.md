## ADR-0254: UDR `application-data` group completed -- 10 further paths, 22 operations

### What was built

The remaining `application-data` resources, completing the group ADR-0253 started:

| Resource | Paths | Methods | Schema |
|---|---|---|---|
| pfds | 2 | GET coll; GET/PUT/DELETE item | `PfdDataForAppExt` |
| bdtPolicyData | 2 | GET coll; GET/PUT/PATCH/DELETE item | `BdtPolicyData_Application_Data` |
| iptvConfigData | 2 | GET coll; GET/PUT/PATCH/DELETE item | `IptvConfigData` |
| serviceParamData | 2 | GET coll; GET/PUT/PATCH/DELETE item | `ServiceParameterData` |
| subs-to-notify | 2 | GET/POST coll; GET/PUT/DELETE item | `ApplicationDataSubs` |

**All 14 `application-data` paths are now routed. UDR: 23 -> 9 unrouted.**

### Read from the YAML per path, not generalised from one sibling

- **`pfds` has no PATCH** while the other three document resources do. That asymmetry is the real
  spec's, and was taken from the YAML per path rather than assumed uniform across the group.
- **`BdtPolicyData` is generated as `BdtPolicyData_Application_Data`** -- the plain name collides
  with `Npcf_BDTPolicyControl`'s own `BdtPolicyData`, so the codegen disambiguates by source file.
  Checked before writing rather than discovered at link time.
- PATCH content type is `application/merge-patch+json` (RFC 7396) for the three that have it.
- `subs-to-notify` POST assigns the id server-side via `generate_uuid_v4()`, matching this file's
  existing server-assigned-id convention.

### Five tables, one store class -- and why that is not the shape the project rejected

Each resource gets its **own table**: they are genuinely distinct real resources, and the
"real, distinct resource, not a rename" precedent (`udr_amf_non3gpp_context`) holds. But all five
share an identical CRUD shape over an opaque JSON document keyed by one id, so they share one
`ApplicationDataDocStore` parameterised by table and id column rather than five near-identical
classes. Distinctness is enforced in the schema, not by duplicated C++. This is deliberately not
the rejected "one table with a type discriminator" shape.

`table_`/`id_column_` are interpolated into SQL, so it is worth being explicit: they are fixed
literals chosen at construction in `main.cpp` and are never request-derived. Values are always
bound as parameters.

### Verification

All 10 paths confirmed by **exact-literal match** against the built binary -- deliberately not the
tier-2 heuristic, which ADR-0253 showed becomes unreliable as shared prefixes accumulate. Build
clean, clang-format clean, conformance 333/333, `ctest` 469/469.

**Not live-verified**: `schema.postgres.sql` gained five more tables, so an existing UDR database
needs the schema re-applied before these routes work against it. Same disclosure as ADR-0253 and
still outstanding for both.

