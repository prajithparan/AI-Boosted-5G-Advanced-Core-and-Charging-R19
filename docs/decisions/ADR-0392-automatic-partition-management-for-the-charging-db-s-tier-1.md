## ADR-0392: automatic partition management for the charging DB's Tier-1 event tables (pg_partman)

**Date:** 2026-09-26. **Status:** accepted (project_tier1_scale_architecture, user-directed: heavy
tables must be partitioned/distributed from the start, no manual intervention as volume grows).

**The gap.** ADR-0384..0388's consolidation declared 5 event tables `PARTITION BY RANGE`
(`balance_mgmt.topup_balance/adjust_balance/reserve_balance` on `occurred_at`,
`chf_rating.rating_decision` on `decided_at`, `chf_rating.applied_customer_billing_rate` on
`rate_date`) but each has had only its one manually-created DEFAULT partition since -- every
top-up, adjustment, reservation and rating decision for every customer has been landing in a single
unbounded partition. At the Tier-1 target (300M+ CDRs/day, proportionally more charging events) that
partition would grow without bound and every per-customer/per-date query would scan it in full.

**Decision.** `pg_partman` (PostgreSQL License, OSI-approved), installed from the real PGDG apt
repository already used for PostgreSQL itself (`postgres:16`, Debian -- not `-alpine`, which has no
`.deb`/`.apk` build of the package), version-pinned to `5.5.0-1.pgdg13+1` (the real candidate this
was verified against, not "latest"). `deploy/docker/postgres-chf.Dockerfile` builds the image;
`deploy/db/charging/70-partition-management.sql` (idempotent, following this directory's own
`31/41/51/61-*.sql` convention) converts all 5 tables via `partman.create_parent()`, daily
partitions (`p_interval => '1 day'`), 7-partition premake, retention left unset. The background
worker (`pg_partman_bgw`, enabled via `shared_preload_libraries` + `pg_partman_bgw.*` GUCs on the
compose `postgres-chf` service's `command:`) then keeps creating future partitions automatically,
hourly, with zero manual intervention -- the actual requirement.

**Every fact about pg_partman's real behaviour below was read from the ACTUALLY INSTALLED 5.5.0
extension SQL and verified by running it against a real container, not assumed from general
knowledge of the project (pg_partman's own API has changed materially across major versions, and
this project's own #1 rule -- never invent a field/behaviour, verify against the real thing in
hand -- applies to third-party dependencies exactly as it does to 3GPP specs):**
- `p_interval` must be a native PostgreSQL interval string (`'1 day'`); the pre-5.0 keyword form
  (`'daily'`) is explicitly rejected by 5.x with its own exception message.
- `p_type` accepts only `'range'`/`'list'` in 5.x; the old `'native'` literal no longer exists
  (pg_partman dropped its own legacy non-native partitioning entirely).
- `create_parent()` REQUIRES the parent already be `PARTITION BY [RANGE|LIST]` -- exactly our
  existing schema, adopted with zero data migration, not converted.
- `p_default_table => true` (pg_partman's own default) would have **failed**: its default-partition
  branch does `CREATE TABLE IF NOT EXISTS <table>_default` (a no-op, ours already exists with that
  exact name) then an unconditional `ALTER TABLE ... ATTACH PARTITION ... DEFAULT` with no existence
  guard -- which errors against an already-attached DEFAULT partition. `p_default_table => false`
  was necessary, not a style choice, found by reading `create_partition()`'s body before calling it.
- `run_maintenance_proc()` is a `PROCEDURE` in 5.5.0 (`CALL`, not `SELECT`) -- caught by actually
  running the migration, not by reading the signature alone.
- There is no `update_part_config()` helper in 5.5.0 (older-version API this project might otherwise
  have assumed); `partman.part_config` is a plain table, updated directly.
- With `p_start_partition` left NULL against an empty (freshly adopted) partition set, `premake => 7`
  produced daily partitions from *both* 7 days before and 7 days after "now" (15 dated partitions),
  not "7 forward" as the parameter name alone suggests -- observed directly, not inferred.
- End-to-end verified against the real, built image: fresh container -> `init-domain-dbs.sh` ->
  `70-partition-management.sql` succeeds -> `pg_partman_bgw` initializes on the real final server
  start and stays up -> a real `INSERT` into `rating_decision` lands in `rating_decision_p20260926`
  (today's dated partition), not `DEFAULT`. Re-running the whole migration a second time is a no-op
  (idempotency guard on `part_config`).
- One disclosed, harmless artifact: the BGW's very first connection attempt, during the container's
  bootstrap-only server start (before `docker-entrypoint-initdb.d/*` runs), logs `FATAL: database
  "charging" does not exist` -- expected (that database is created by the init script that runs
  between the bootstrap start and the real final start) and self-resolving; the BGW succeeds from
  the real final start onward. A background worker failing does not crash the postmaster.

**Retention/archival: deliberately NOT set**, per ADR-0283's own precedent for
`CdrWriter::apply_retention` ("a retention window is an operator compliance decision -- regulatory
retention periods differ by jurisdiction -- and a default that silently deleted [records] after N
days would be this project choosing someone's compliance posture for them"). The same reasoning
applies verbatim to these financial/billing tables. `pg_partman` only CREATES partitions ahead here;
it never drops or detaches one. An operator with a real window sets it directly on
`partman.part_config` (documented inline in the SQL file, `retention_keep_table => true` so the
closed step is DETACH, matching ADR-0283's archive-first-then-remove ordering).

**CI.** GitHub Actions `services:` accepts only a pre-built `image:`, not a custom Dockerfile, so
both CI jobs install the identical pinned `postgresql-16-partman` package directly into the already-
running `postgres:16` service container (switched from `-alpine`, which has no `.deb`) before
applying the domain DDL. This validates `create_parent()`/`run_maintenance()` for real in CI.
**Disclosed:** CI does NOT exercise `pg_partman_bgw` -- `shared_preload_libraries` only takes effect
at server start, and the service container is already running by the time CI's steps execute;
restarting a GHA-managed service container mid-job is unsupported and risks the self-hosted runner
instability `project_ci_runner_shutdown_flakiness` already tracks. The BGW itself is exercised, real,
in the compose/production image (verified above) -- only its CI validation is narrowed to the SQL.

**Folded in while touching this same file/service block (ADR-0390's own scope, missed there):** the
`postgres-chf` service's vestigial `POSTGRES_DB: chf_rating` (compose and both CI jobs) is removed --
unused since ADR-0388 moved `chf_rating` into a schema inside `charging`; nothing creates or expects
a standalone `chf_rating` database any more (verified: no remaining reference outside this file/ADRs).

**Rejected.**
- *Build pg_partman from source with a hand-verified tarball checksum* -- this project already
  trusts the signed PGDG apt repository for PostgreSQL itself; verifying a third-party checksum by
  hand where a maintained, signed package exists is a self-imposed risk, not rigor.
- *A separate cron/sidecar container to call `run_maintenance()`* -- `pg_partman_bgw` runs inside
  the same PostgreSQL process with no extra moving part, is the extension's own supported mechanism,
  and needs no new component under the "self-hosted, no new moving parts" instinct.
- *Backfilling per-day partitions for the DEFAULT partition's existing history* -- a data-migration/
  retention decision for whoever owns that policy, not something this DDL should do silently; the
  DEFAULT partition keeps holding it, unchanged, with zero downtime and zero rewrite.

**Disclosed remaining gap.** `pg_partman_bgw`'s own health (is it actually still running hours/days
into a real deployment, not just at the cold-start verified here) has no monitoring/alerting yet --
a P4.12/observability follow-up, not blocking this increment.

