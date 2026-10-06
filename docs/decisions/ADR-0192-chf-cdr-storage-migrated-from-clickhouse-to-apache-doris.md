## ADR-0192: CHF CDR storage migrated from ClickHouse to Apache Doris

### Context

User-directed, mandatory migration ("migrate to Apache Doris ASAP"), preceded by a strict standing
decision: every datastore in this project must be self-hosted open-source in deployment
descriptors, never a cloud/managed variant. The trigger was a real, cited concern -- ClickHouse's
own "open-core drift" since ClickHouse Cloud launched (SharedMergeTree, lightweight `UPDATE`, and
S3-backed RBAC moved cloud-only, per Altinity's own public analysis of the OSS/Cloud split) -- vs.
Apache Doris's genuine Apache Software Foundation governance and native MySQL wire-protocol
compatibility, a better fit for this project's C/C++-only-new-code standing rule than ClickHouse's
own native client story.

### Decision and implementation

**Client library, and a real correction mid-migration.** First choice was
`mariadb-connector-cpp` (real, working vcpkg port, zero actual Java/JVM dependency, LGPL-2.1). The
user rejected it anyway, explicit and strict: "NO more JDBC please... NO JAVA NO JAVA" -- the
rejection was for the library's own JDBC-mirroring class-naming convention
(`sql::Driver`/`Connection`/`PreparedStatement`/`ResultSet`/`DriverManager::getConnection`), not
for any real Java dependency (it has none). Pivoted to `libmariadb`, the plain C client API
(`MYSQL*`, `mysql_real_connect`, `mysql_real_query`, `mysql_real_escape_string`,
`mysql_store_result`/`mysql_fetch_row`), wrapped in a project-owned RAII class
(`nfs/chf/src/cdr.hpp`/`cdr.cpp`) with no JDBC-shaped naming anywhere. Real vcpkg target:
`unofficial::libmariadb`.

**Schema** (`nfs/chf/schema.doris.sql`, replaces the deleted `schema.clickhouse.sql`): `UNIQUE
KEY(charging_data_ref, invocation_sequence_number, service_type)`, `DISTRIBUTED BY
HASH(charging_data_ref) BUCKETS 10`, `PROPERTIES ("replication_num" = "1")` (single-BE lab
deployment). Real technical differences from the ClickHouse-era schema, each disclosed rather than
silently absorbed:
- **Dedup model improved, not just swapped.** Doris's UNIQUE KEY model dedupes immediately via
  Merge-on-Write; the ClickHouse schema it replaces used `ReplacingMergeTree`, which only dedupes
  on background merge or an explicit `FINAL` query -- a real correctness improvement, not a
  lateral move.
- **No native BLOB type.** Doris's `asn1_cdr` column is `STRING`; the real TS 32.298 ASN.1 BER-
  encoded CDR bytes (`cdr_asn1.cpp`'s own encoder, ADR-0089) are hex-encoded before storage. No
  existing consumer reads this column back, matching the pre-existing ClickHouse-era state.
- **No partition/TTL.** A Unique-Key table's partition columns must be a subset of its own key
  columns; adding a time-based partition would force `invocation_time_stamp` (or similar) into the
  key, changing real dedup semantics. Deliberately not added -- disclosed, not silently dropped.

**Deployment**: `docker-compose.yml`'s `clickhouse:` service replaced with `doris:`
(`apache/doris:all-in-one-4.1.3`, FE HTTP 8030 / FE MySQL 9030 / BE HTTP 8040, healthcheck
`curl -sf http://localhost:8030/api/health`), plus a new one-shot `doris-schema-init:` service
(`mariadb:11` image, `depends_on: doris: condition: service_healthy`) that creates the `chf_cdr`
database and applies `schema.doris.sql` via the real `mysql` CLI -- matching this project's own
existing `pki-init` one-shot-init precedent, since Doris's own official image has no
auto-init-on-first-boot convention. `.github/workflows/ci.yml`'s `build` job updated identically
(both `doris:` service + schema-apply step); the `sanitize` job's own pre-existing shape (never
applied a schema or used CHF-datastore env vars) was preserved, not "fixed" into something it
never was.

**Training sidecar** (`nfs/chf/training/`): `train_quota_sizing.py`'s `fetch_real_examples()`
rewritten from `clickhouse_connect` to `pymysql` (Doris's real MySQL wire protocol), argparse
flags renamed (`--doris-host` etc., default port corrected 8123->9030),
`requirements.txt`'s `clickhouse-connect` -> `pymysql`, `README.md` updated to match.

**Dead code removed, not just migrated.** `charging_engine.cpp` and `diameter_server.cpp` both had
`try/catch` around `cdr_writer.write(cdr)` -- necessary for the old ClickHouse client (whose
`clickhouse::ServerException` could throw mid-insert) but genuinely vestigial now:
`CdrWriter::write()` catches every real Doris error surface internally via `mysql_real_query`'s
own return code and never throws. Removed, with an ADR-0192 comment explaining why, not a silent
deletion.

**Escaped plain-SQL INSERT, not prepared statements** -- a deliberate choice, disclosed in
`cdr.hpp`'s own header comment: more idiomatic for an OLAP engine like Doris than high-frequency
single-row prepared statements (which are more OLTP-oriented). `mysql_real_escape_string` used for
every string value to prevent SQL injection.

**Governing-doc consistency** (CLAUDE.md's own "if CLAUDE.md and PROMPT.md disagree, that's a
bug" rule): `CLAUDE.md`, `PROMPT.md`, `CHARGING_PROMPT.md` (2 references), and
`docs/DATA_MODEL.md` (4 references, including the ReplacingMergeTree->Unique-Key correction) all
updated to cite Apache Doris and this ADR instead of ClickHouse.

### Real bugs found and fixed during this migration (all via live verification, not guessed)

1. **`nfs/chf/src/cdr_asn1.cpp` missing `#include <array>`.** Pre-existing, latent -- `std::array`
   was used (return type of `encode_timestamp`) without the header, previously masked by a
   transitive include that disappeared when the dependency chain changed from
   `mariadb-connector-cpp` to `libmariadb`. Fixed directly; not related to the SQL client swap
   itself, but only surfaced by it.
2. **`tests/conformance/CMakeLists.txt` never wired to `unofficial-libmariadb`.** `test_cdr_asn1.cpp`
   compiles `cdr_asn1.cpp` directly, which transitively includes `cdr.hpp`'s `<mysql.h>` even
   though the test never constructs a real `CdrWriter` -- the initial full rebuild reported
   success only because it was piped through `tail`, which swallowed ninja's real nonzero exit
   code. Caught by rerunning with the exit code captured directly (no pipe), not by trusting the
   first "success."
3. **Real SSL-handshake bug, found and root-caused via live verification against a real
   `apache/doris:all-in-one-4.1.3` container**, not guessed: `CdrWriter`'s connection failed with
   "SSL is required, but the server does not support it" even after explicitly disabling
   `MYSQL_OPT_SSL_ENFORCE`. Root cause, confirmed by reading libmariadb's own vendored source
   (`plugins/auth/my_auth.c`): `MYSQL_OPT_SSL_VERIFY_SERVER_CERT` defaults to requiring
   verification, and that alone -- independent of `SSL_ENFORCE` -- forces `use_ssl=1` during the
   auth handshake. Fixed by also explicitly disabling `MYSQL_OPT_SSL_VERIFY_SERVER_CERT`. Real,
   disclosed scope: this project's actual Doris deployment has no TLS configured on its FE MySQL
   port, matching the plaintext posture already accepted for this project's other backend
   datastore links (PostgreSQL, Redis/Valkey) -- the real SBI mTLS discipline (TS 33.501) applies
   to inter-NF traffic, not this backend link. Tracked as real debt alongside ADR-0009's existing
   TLS gaps, not a new, separately-hidden one.

### Live verification (real, live processes and a real Doris container, not self-consistency)

Real `apache/doris:all-in-one-4.1.3` container started fresh, health-checked
(`/api/health` -> `online_backend_num: 1`), `schema.doris.sql` applied via the container's own
bundled `mysql` client. Real `nrf` + `chf` processes started, `chf` connecting successfully
("chf: connected to Doris (CDF)"). Real `POST /nchf-convergedcharging/v3/chargingdata` (Create)
over mTLS with a real `MultipleUnitUsage` (ratingGroup 1) -> real `201`,
`ChargingDataRef=chg-21`. Queried Doris directly (not through CHF) via the container's own `mysql`
client: the real row landed with `service_type=ConvergedCharging`, `operation=Create`, the real
`subscriber_identifier`, and a populated, correctly hex-encoded `asn1_cdr` column (`bf81486e8002...`,
a real TS 32.298 BER-encoded CHF-CDR, matching `cdr_asn1.cpp`'s own encoder). All processes killed
by explicit PID afterward, not `pkill -f`.

### Testing

Full project rebuild clean (0 warnings beyond the pre-existing, unrelated onnxruntime static-lib
linker note). Full `ctest` (excluding the two known-flaky integration tests,
`UdrIntegration.AmfContextLifecycle`/`UdmIntegration.SdmDataRetrievalAndSubscriptions`): 363/363
pass.

### What this ADR does NOT include

Real TLS on Doris's own FE MySQL port (disclosed above, tracked alongside ADR-0009). A dedicated
live exercise of `CdrWriter::detect_gaps()`'s own SELECT-based logic beyond the direct-query
confirmation already performed above. Update/Release-triggered CDR writes were not separately
live-exercised this pass -- they share the exact same `write_converged_charging_cdr` ->
`cdr_writer.write()` code path already proven live via Create, so this is a real, disclosed scope
narrowing, not an unverified claim.

### Follow-up (2026-08-31): naming scrub finished, and one real stale-venv breakage found

User-directed ("fix the chf clickhouse to doris completely"). Two prior passes had already
removed the live operator-facing `"CDR write to ClickHouse failed"` log line and the two dead
`chf_clickhouse_options` config-key citations (commit `6a6b763`); that commit deliberately KEPT
five remaining ClickHouse mentions in `cdr.hpp`/`charging_engine.cpp`/`diameter_server.cpp` as
"history, not drift". That judgement is now reversed by explicit user instruction: the predecessor
engine is named nowhere outside this ADR and the traceability record, and each of those five sites
cites ADR-0192 instead, so the *why* is still one hop away rather than deleted. Living design docs
(`CLAUDE.md`, `PROMPT.md`, `CHARGING_PROMPT.md`, `docs/DATA_MODEL.md`) likewise now name Doris and
defer the engine comparison to this ADR. Historical ADR text and `docs/TRACEABILITY.md`'s own
per-ADR sections are deliberately NOT rewritten -- rewriting a record of what was true at the time
would be falsifying history, not cleaning up drift.

**One real, non-cosmetic finding, not assumed from the file contents**: `nfs/chf/training/.venv`
(gitignored, but the real interpreter this project's own training sidecar runs under) still had
`clickhouse-connect 1.7.1` installed and did NOT have `pymysql` at all -- `requirements.txt` was
migrated in ADR-0192, but the already-materialised venv never was. `train_quota_sizing.py`'s
`fetch_real_examples()` imports `pymysql` lazily (inside the function, not at module scope), so
the breakage was invisible until the real Doris fetch path ran. Confirmed by direct import
(`ModuleNotFoundError: No module named 'pymysql'`), then fixed in place: `clickhouse-connect`
uninstalled, `pymysql` installed, both re-verified by import.

**Also corrected in the same pass, unrelated to the datastore**: an in-flight uncommitted edit had
deleted `CLAUDE.md`'s TMF633/638/639/651/654/688 extension rationale -- real, asked-and-approved
content with nothing to do with this migration. Restored rather than committed as collateral.

#### Live verification of the training sidecar's real fetch path (user-directed, not inferred)

An import check proves `pymysql` is installed; it does NOT prove the query path works against a
real Doris. Both branches were therefore exercised for real against a live
`apache/doris:all-in-one-4.1.3` (`chf-test-doris`), not simulated:

1. **Empty-table branch**: script run with the `cdr` table holding only ADR-0192's own `chg-21`
   row (no usage-bearing rows) -- the real `pymysql` connect + SELECT executed and returned 0
   usable examples, correctly falling back with `data_source=synthetic_bootstrap`, exactly as the
   `MIN_REAL_EXAMPLES` guard documents.
2. **Real-data branch**: real `nrf` + `chf` started (`chf: connected to Doris (CDF)`, real NRF
   registration HTTP 201), then real `Nchf_ConvergedCharging` traffic over mTLS with the `smf`
   client cert -- 3 Creates (`chg-22`/`chg-23`/`chg-24`) plus 27 Updates carrying real
   `multipleUnitUsage[].usedUnitContainer[].totalVolume`, all HTTP 200, zero CDR-write failures in
   CHF's own log. Doris then held 31 rows / 27 usage-bearing (confirmed by direct `mysql` query,
   not through CHF). Re-run: `training on 24 REAL CDR-derived examples`,
   `data_source=real_cdr, n_examples=24`, real ONNX artifact written, real MLflow run
   `acb61ab7cabb4b95a18a6ab8ba139260`. The `pymysql` -> Doris fetch path is now proven end-to-end,
   not asserted from a successful import. Both lab processes killed by explicit PID afterward.

**Real pre-existing bug found by this verification, NOT introduced by the migration and NOT fixed
here** (flagged rather than silently absorbed): `charging_engine.cpp`'s CDR population sets
`cdr.invocation_time_stamp = system_clock::now()`, discarding the consumer-supplied TS 32.291
`invocationTimeStamp` the request actually carried. Confirmed live: requests sent with
`2026-08-31T14:MM:0NZ` landed in Doris as the wall-clock write time (`19:39:37`/`19:39:38`), many
rows sharing one second. Two real consequences: the stored CDR misreports a real 3GPP field
(`recorded_at` already exists for write time, so this is duplication, not a substitute), and
`train_quota_sizing.py`'s own `inter_invocation_interval_sec` feature collapses to ~0 for rows
written in the same second, degrading a real model input. Out of scope for a naming/migration
cleanup -- needs its own change and its own ADR.

