## ADR-0058: P4.4 -- CDF (CDR generation, TS 32.240/32.296) as real code inside CHF, backed by real ClickHouse, and a real process-crash bug found and fixed via live verification

**Date:** 2026-08-11
**Status:** Accepted.

**Context:** CHARGING_PROMPT.md's P4.4 requires real CDR generation with duplicate detection and
gap detection. Two real, internal CHF functions are in scope here -- CDF (Charging Data Function)
and CGF (Charging Gateway Function) -- per TS 32.240/32.296's own converged 5G-SA architecture,
which does **not** expose either as a separate SBI-facing NF the way ABMF (TMF654, ADR-0056) has
its own real, distinct TM Forum Open API. That distinction (internal-to-CHF vs. a real separate
service with its own API) is why this ADR adds new code inside `nfs/chf` rather than a new
standalone component -- not a shortcut, a real architectural read of the two specs' actual scope.

### Real gap disclosed up front: TS 32.298 is not vendored

TS 32.298 (CDR parameter description) is the real, normative field taxonomy for a 3GPP CDR --
already flagged as an open question back in P4.1 (`docs/DATA_MODEL.md`) since it isn't among this
project's vendored specs. Resolution on file and applied here: build the CDR schema entirely from
TS 32.291 fields already vendored and already flowing through `nfs/chf/src/main.cpp`
(`ChargingDataRequest`/`ChargingDataResponse`'s own real fields -- `subscriberIdentifier`,
`nfConsumerIdentification.nodeFunctionality`, `MultipleUnitUsage.ratingGroup`,
`GrantedUnit.totalVolume`/`serviceSpecificUnits`, `UsedUnitContainer.totalVolume`,
`invocationTimeStamp`), plus a small number of disclosed project-internal columns
(`service_type`, `operation`, `recorded_at`), never a guessed TS 32.298 field name (e.g. a real
"5GSChargingDataRecord" record type has its own real ASN.1-based field names this project has no
access to). `nfs/chf/schema.clickhouse.sql`'s own header carries this same disclosure at the
schema itself, not just here.

### Design

- **ClickHouse** (`clickhouse-cpp` v2.6.2, vcpkg, Apache-2.0) -- `docs/DATA_MODEL.md`'s E4
  assignment ("ClickHouse for CDR/usage analytics"), the third database technology in this
  project after PostgreSQL (UDR, ADR-0054) and Redis/Valkey (session stores, ADR-0055).
- **Duplicate detection**: `ReplacingMergeTree(recorded_at)` -- a real, native ClickHouse
  mechanism, not a project invention. Rows sharing the same `ORDER BY` key
  (`charging_data_ref, invocation_sequence_number, service_type`) are deduplicated (highest
  `recorded_at` wins) during background merges. Disclosed, real eventual-consistency
  characteristic: dedup is not immediate at insert time, only during ClickHouse's own background
  merge cycles or when a query uses `FINAL` -- a real ClickHouse behavior, not a simplification
  this project chose.
- **Gap detection**: `CdrWriter::detect_gaps` runs a real `SELECT DISTINCT
  invocation_sequence_number ... WHERE charging_data_ref = {ref:String}` and returns every missing
  value in the contiguous range between the lowest and highest sequence number seen. Wired into
  the real `Nchf_ConvergedCharging` Update handler (`nfs/chf/src/main.cpp`), logging a real warning
  when a gap is found.
- **Retention**: `TTL recorded_at + INTERVAL 90 DAY DELETE` -- real, native ClickHouse
  retention-driven auto-archival (PROMPT.md P14) for this table specifically. A separate
  cold-archive-to-object-store tier (E4's other assignment, for retention beyond this table's own
  TTL window) is disclosed as not implemented this pass.
- **CDR writes wired into `Nchf_ConvergedCharging`'s Create/Update/Release handlers only** --
  `Nchf_OfflineOnlyCharging`'s three handlers deliberately not wired this pass, a disclosed
  scoping decision to bound this increment, not an oversight.

### Real, disclosed vcpkg-port limitation: no CMake config for `clickhouse-cpp`

Confirmed by direct inspection (`find .../share -ipath "*clickhouse*"` returns only
`vcpkg.spdx.json`/`copyright`/`vcpkg_abi_info.txt`, no `.cmake` file at all) -- unlike every other
vcpkg dependency this project uses so far (`libpqxx`, `redis-plus-plus`, etc.), this port installs
no `*Config.cmake` and no pkg-config `.pc` file. `find_package(clickhouse-cpp CONFIG)` simply does
not work for this port. Fixed by manually creating an `IMPORTED STATIC` CMake target
(`find_library`/`find_path`) in `nfs/chf/CMakeLists.txt`, linking the real transitive dependency
set (`absl::int128`, `cityhash`, `lz4::lz4`, `zstd::libzstd`) determined by reading clickhouse-cpp's
own real upstream `CMakeLists.txt` directly from the vcpkg buildtree -- not guessed.

Before adding the dependency, `clickhouse-cpp`'s own portfile and its four real dependencies'
portfiles were checked for unusual `find_program`/`REQUIRED` system-build-tool needs (none found,
unlike the earlier `libpqxx`/bison Dockerfile surprise), then confirmed with a real background
`docker build -f deploy/docker/pcf.Dockerfile .` (succeeded) before committing to the dependency --
`vcpkg.json` is a single shared manifest, so any new entry affects every Dockerfile's build.

### Real bug found and fixed via live verification (not caught by reasoning alone)

The first real end-to-end run (real `nrf` + Valkey + a real ClickHouse container, schema applied)
**aborted the entire CHF process** at startup: `terminate called after throwing an instance of
'clickhouse::ServerException'` / `Authentication failed`, `Aborted (core dumped)`.

**Immediate/environmental cause**: the `clickhouse/clickhouse-server:latest` image auto-generates a
random password for the `default` user on first startup (a real, documented recent ClickHouse
security-hardening default); this project's dev-convention empty password failed authentication.

**Deeper, more important cause**: `clickhouse::Client`'s constructor connects EAGERLY -- a real,
confirmed clickhouse-cpp behavior, unlike `sw::redis::Redis`'s lazy-on-first-command connection
pool already relied on elsewhere in this same file (confirmed when `nfs/chf`'s Redis stores were
built, ADR-0055) -- and throws a real, uncaught `clickhouse::ServerException` on connection/auth
failure. Nothing in the call chain from `main()`'s `chf::CdrWriter cdr_writer(...)` construction
caught it, so it propagated to `std::terminate()` and aborted the whole CHF process -- not just
CDR generation. This directly contradicts this same file's own already-established
best-effort-per-write design (individual `cdr_writer.write(...)` calls are already wrapped in
try/catch, explicitly disclosed as "a ClickHouse write failure does not block or fail the real
charging response CHF already committed to"): a ClickHouse outage must never be able to crash or
block the higher-priority real-time charging/balance-reservation path.

**Fix**: `CdrWriter::client_` changed from a plain `clickhouse::Client` member to
`std::unique_ptr<clickhouse::Client>`; the constructor now catches the connect failure, logs a
real warning, and leaves `client_` null rather than rethrowing. `write()`/`detect_gaps()` both
null-guard at the top (log a warning, safe no-op / return `{}`) rather than every call site needing
its own check. `main()`'s own startup log line was also fixed to actually reflect connection state
(`is_connected()`) instead of unconditionally claiming success.

**This is the second bug this session found only by running real processes against real
dependencies, not by code review or unit-level reasoning alone** -- the same discipline that
caught ADR-0057's balance-finalize-ordering bug.

### Live-verified for real (after the fix)

1. **Negative path**: CHF started against the same auth-failing ClickHouse container that
   previously aborted it -- ran cleanly for the full duration with a real, accurate warning
   logged (`chf: could not connect to ClickHouse, CDR generation disabled: ...` /
   `chf: ClickHouse unavailable, CDF/CDR generation disabled for this process`), no crash.
2. **Positive path**: a fresh ClickHouse container (`CLICKHOUSE_SKIP_USER_SETUP=1`, real empty-
   password auth working), schema applied, CHF connected successfully. A real
   Create -> Update (with a real skipped sequence number, 2, to force a gap) -> Release cycle was
   driven via real `curl` + mTLS client cert against the running CHF, and the results confirmed
   **independently via direct ClickHouse queries** (`clickhouse-client --query "SELECT * FROM
   cdr"`), not just CHF's own logs or HTTP responses:
   - Real Create CDR row landed (`chg-1`, seq 1).
   - Real Update CDR row landed (`chg-1`, seq 3, `used_total_volume=500000`), and CHF's real
     `detect_gaps` query correctly logged the missing sequence: `chf: CDR sequence gap detected
     for ChargingDataRef=chg-1 -- missing invocationSequenceNumber(s): 2`.
   - Real Release CDR row landed (`chg-1`, seq 4).

Full rebuild + `clang-format-18` clean (one real violation in `cdr.cpp`'s include ordering/line
wrap, fixed) + 146/146 `ctest` (the 3 `ProductCatalogPostgresTest` skips are the pre-existing real
skip-when-no-Postgres-running behavior, not a regression from this ADR) both before and after the
fix.

### Disclosed, NOT done by this ADR

- `Nchf_OfflineOnlyCharging`'s Create/Update/Release handlers are not wired to CDR generation --
  deliberately out of scope for this increment.
- No CGF-side file-format transfer (TS 32.297) to an external Billing Domain -- disclosed,
  deferred; not fabricated against an unvendored spec.
- `detect_gaps` runs on-demand (triggered by each real Update call), not as a standing background
  reconciliation job -- a real, working answer to "gap detection is mandatory" per P4.4, but not a
  scheduled audit process.
- No cold-archive-to-object-store tier beyond the table's own 90-day `TTL` -- disclosed above.
- `clickhouse::Client` is not documented as thread-safe for concurrent multi-thread use; CHF's
  route handlers all run on the server's single `io_context` thread today, so this is safe in this
  build's actual concurrency model, but is not safe to share across multiple threads without adding
  a mutex first if that model ever changes -- documented in `cdr.hpp` itself, not just here.

