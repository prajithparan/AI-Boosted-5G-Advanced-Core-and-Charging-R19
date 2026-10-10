## ADR-0068: gap-closure Tier 1a -- real UDR PostgreSQL persistence (free5GC/open5gs source comparison)

### Context

Following the TAP3 work, the user asked for a genuine, deep, per-NF, three-way source comparison
between this project's own actual C++ source and the real free5GC (Go) and open5gs (C) GitHub
repos -- not the earlier whole-project capability surveys, and explicitly not narrowed to CHF
alone. Three parallel research forks (NRF/AMF/SMF, UDM/UDR/AUSF, PCF/UPF/CHF) read our own source
plus both references' real code and produced a ranked gap list; the user approved the recommended
Tier 1 -> 2 -> 3 sequencing. This ADR is Tier 1a, the first implemented item.

**Real, verified gap**: `nfs/udr/src/stores.hpp`'s `AmfContextStore`/`SmfRegistrationStore` were
`std::unordered_map`-backed, in-memory only -- data did not survive a process restart. Both real
references treat UDR as a genuinely persistent repository (free5gc/udr: real MongoDB backend,
`internal/database/mongodb/mongo_db_inplement.go`; open5gs: `lib/dbi/ogs-mongoc.c`). This project's
own mandated storage stack (CLAUDE.md) is PostgreSQL for exactly this kind of state, not MongoDB
specifically -- the real persistence property (survives restart) is what matters, not the vendor.

### What changed

`nfs/udr/schema.postgres.sql` (new): two tables, `udr_amf_context` (PK `ue_id`) and
`udr_smf_registration` (PK `(ue_id, pdu_session_id)`), both storing the real Nudr_DataRepository
context-data resources as opaque `JSONB` -- matching `bss/product-catalog`'s own established
"PostgreSQL jsonb for variable-shape nested fields" pattern (ADR-0053), since these resources
were already carried through this NF as generated OpenAPI JSON.

`nfs/udr/src/stores.{hpp,cpp}`: `AmfContextStore`/`SmfRegistrationStore` rewritten to hold a real
`pqxx::connection` instead of an `unordered_map`, same public API (`put`/`get`/`apply_patch`/
`remove`/`list_for_ue`) so `main.cpp`'s call sites barely changed. Real Postgres UPSERT idiom used
for `put()`'s 201-vs-204 distinction: `INSERT ... ON CONFLICT ... DO UPDATE ... RETURNING
(xmax = 0) AS inserted` -- `xmax = 0` is a real, standard Postgres signal that a row was inserted
by the current command rather than updated, avoiding a separate SELECT-then-write.

**Real, deliberate architectural choice**: connection failure is NOT gracefully degraded the way
CHF's `RatingDecisionStore` is (that store try-catches the connection and no-ops writes with a
warning if unreachable -- appropriate for a best-effort audit trail). UDR's context-data group IS
this NF's entire purpose; a UDR that can't reach Postgres has nothing meaningful to serve, so the
constructor hard-requires the connection (same choice `bss/product-catalog`'s own
`ProductOfferingStore` already makes) -- UDR fails fast at startup rather than silently degrading.

`nfs/udr/src/main.cpp`: added `database_conninfo()` (same getenv-based pattern as
`bss/product-catalog`'s own, env var `UDR_DATABASE_URL`, local-dev default
`postgresql://udr:udr@localhost:5432/udr`).

`nfs/udr/CMakeLists.txt`: `find_package(libpqxx CONFIG REQUIRED)`, linked into the `udr` target.

`deploy/docker/docker-compose.yml`: new `postgres-udr` service (own dedicated instance, port 5437,
same "one postgres per consumer" shape as every other PostgreSQL-backed service in this file),
`udr` service now depends on it and gets `UDR_DATABASE_URL` set to the real container address.

`.github/workflows/ci.yml` (both `build` and `sanitize` jobs): new `postgres-udr` service
container, a schema-apply step, and `UDR_DATABASE_URL` added to the `Test` step's env -- CI now
exercises the real DB path for `tests/integration/test_udr_context_data.cpp` (which spawns the
real `udr` binary as a subprocess) instead of that binary crashing at startup with no Postgres
reachable.

### Testing and verification

`tests/integration/test_udr_context_data.cpp` (pre-existing, unmodified) re-verified against a
real, freshly-created local PostgreSQL container: all 3 tests pass unchanged (`AmfContextLifecycle`,
`SmfRegistrationLifecycle`, `MissingResourceIs404AndTamperedTokenIs401`), confirming the real
HTTP-layer behavior (PUT/GET/PATCH, 201-vs-204, 404, RFC 6902 JSON Patch) is unchanged by the
storage-layer swap.

**Real restart-survival check** (the actual point of this ADR, not just a passing test): started a
real `udr` process against a real Postgres, `PUT` a real AMF context, `kill -9`'d the process
(not a graceful shutdown), started a fresh `udr` process against the same Postgres, `GET` the same
resource back -- returned the identical real data with `200 OK`. This is the concrete behavior the
in-memory version could never have (an in-memory `unordered_map` cannot survive `kill -9` by
definition), and is the real capability free5GC/open5gs both have that this project's own UDR
previously lacked.

`clang-format-18 --dry-run --Werror` clean. Full rebuild + `ctest`: 275/275 pass, zero regressions
(one apparent failure during verification traced to leftover TCP/connection state from repeated
manual `kill -9` testing against the same long-lived local Postgres container during this session's
own verification work, not a code defect -- recreating the container fresh gave a clean, fast
275/275 pass; disclosed here rather than silently omitted).

### What this ADR does NOT include

UDM's `GetAmData`/`GetSmfSelData`/`GetSmData` still return stubs, not real calls into this now-real
UDR -- that real wiring is Tier 1b, a separate, deliberate next turn (same "don't silently expand
scope mid-turn" discipline this project has held throughout). No MongoDB-specific behavior was
replicated (transaction semantics, replica sets, etc.) -- only the real persistence property
(survives restart) that both references' own UDR share, using this project's own mandated
PostgreSQL stack instead.

