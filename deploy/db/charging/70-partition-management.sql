-- =================================================================================================
-- AUTOMATIC PARTITION MANAGEMENT for the charging DB's time-partitioned event tables (ADR-0392)
-- =================================================================================================
-- project_tier1_scale_architecture (user-directed): heavy tables must be distributed/partitioned so
-- per-customer/per-date retrieval is a pruned, local scan, with NO manual intervention as volume
-- grows. 40-balance.sql / 60-rating.sql already declared 5 tables `PARTITION BY RANGE`, but each has
-- only its one manually-created DEFAULT partition -- every row of every customer's top-up, adjust,
-- reserve and rating-decision history has been landing in a single unbounded partition since
-- ADR-0385/0388. This file hands that to pg_partman (PostgreSQL License, OSI-approved;
-- deploy/docker/postgres-chf.Dockerfile installs the real Debian/PGDG package
-- postgresql-16-partman, pinned 5.5.0-1.pgdg13+1) so future partitions are created automatically,
-- ahead of when rows need them.
--
-- Every fact below about pg_partman's real behaviour (function signatures, valid p_type values,
-- p_default_table's interaction with an already-existing DEFAULT partition) was read from the
-- ACTUAL installed 5.5.0 extension SQL (pg_partman--5.5.0.sql), not assumed from memory of older
-- releases -- pg_partman's API has changed materially across major versions (see disclosures below).
--
-- Idempotent: `create_parent()` itself is not (a second call errors on the part_config PK), so
-- every call here is guarded on `part_config` already holding that parent, matching this directory's
-- existing convention (31/41/51/61-*.sql).
CREATE SCHEMA IF NOT EXISTS partman;
CREATE EXTENSION IF NOT EXISTS pg_partman SCHEMA partman;

-- One partition per day: matches the Doris CDR/feature-store convention already established
-- (deploy/db/doris/analytics-rollups.sql, nfs/chf/schema*.doris.sql) for the same Tier-1 event
-- volume, and keeps each customer's per-day digital-channel query (billing history, recent charges)
-- a single-partition scan. `p_interval` MUST be a native PostgreSQL interval string ('1 day') --
-- pg_partman 5.x raises on the pre-5.0 keyword form ('daily'), a real, verified behavioural break
-- from older pg_partman versions, not a stylistic choice.
--
-- p_default_table => false: our tables' DEFAULT partition was created (and named identically,
-- '<table>_default') by 40-balance.sql/60-rating.sql at cut-over time -- see those files' own
-- headers. create_parent()'s own default-table branch does `ALTER TABLE ... ATTACH PARTITION ...
-- DEFAULT` unconditionally when p_default_table is true, with no existence guard on the ATTACH
-- (only the CREATE TABLE step is IF NOT EXISTS) -- verified by reading create_partition()'s body --
-- so calling it with the default true against our already-DEFAULT-partitioned tables would fail on
-- that ATTACH. Passing false skips the whole block; our existing DEFAULT partition keeps holding
-- whatever falls outside the managed range (there was no PDF/PII purge to justify pre-partman
-- data movement, so old rows simply stay there -- no downtime, no rewrite).
--
-- p_premake => 7: on a fresh, empty partition set with p_start_partition left NULL, pg_partman
-- creates p_premake daily partitions on BOTH sides of today (verified empirically against this
-- exact migration: today +/- 7 days, 15 dated partitions + the pre-existing default) -- more than
-- the "future only" reading its name suggests, and harmless: extra past-dated empty partitions cost
-- nothing, and a week of partitions always exists ahead of the write path either way (pg_partman
-- default premake is 4; a wider margin is cheap and this is Tier-1 production data, so "ran out of
-- partition to insert into" must not be reachable by any plausible maintenance-run gap).
--
-- p_start_partition => NULL (the default): pg_partman starts creating partitions from the current
-- time, not from each table's oldest row -- deliberate, since the DEFAULT partition already holds
-- everything older and backfilling per-day partitions for that history is a data-migration decision
-- for whoever owns that retention/archival policy, not something this DDL should do silently.
--
-- p_jobmon => false: pg_jobmon (a separate, older pg_partman companion extension for logging its own
-- maintenance runs) is not installed; passing false is a no-op either way (create_parent only acts
-- on jobmon if that extension's schema is found), made explicit rather than left to an accepted
-- default whose meaning depends on what else happens to be installed.
--
-- retention left unset (NULL) on every table below: per ADR-0283's own precedent for
-- CdrWriter::apply_retention, "a retention window is an operator compliance decision -- regulatory
-- retention periods differ by jurisdiction -- and a default that silently deleted [billing/charging
-- records] after N days would be this project choosing someone's compliance posture for them." The
-- same reasoning applies verbatim to top-ups, adjustments, reserves and rating decisions: these are
-- financial/billing records. NULL retention means pg_partman only ever CREATES partitions ahead;
-- it never drops or detaches one. Unlike older pg_partman releases, 5.5.0 has no separate
-- `update_part_config()` helper -- `partman.part_config` is a plain table, read fresh by every
-- `run_maintenance()`/`run_maintenance_proc()` call (verified: no such function exists in the
-- installed extension), so an operator who has decided a real window sets one directly:
-- `UPDATE partman.part_config SET retention = '<interval>', retention_keep_table = true WHERE
-- parent_table = '<schema>.<table>';` (keep_table => true so the closed step is DETACH, matching
-- ADR-0283's archive-first ordering, not a same-step DROP).
DO $$
DECLARE
    v_tables text[] := ARRAY[
        'balance_mgmt.topup_balance:occurred_at',
        'balance_mgmt.adjust_balance:occurred_at',
        'balance_mgmt.reserve_balance:occurred_at',
        'chf_rating.rating_decision:decided_at',
        'chf_rating.applied_customer_billing_rate:rate_date'
    ];
    v_entry   text;
    v_table   text;
    v_control text;
BEGIN
    FOREACH v_entry IN ARRAY v_tables LOOP
        v_table   := split_part(v_entry, ':', 1);
        v_control := split_part(v_entry, ':', 2);
        IF NOT EXISTS (SELECT 1 FROM partman.part_config WHERE parent_table = v_table) THEN
            PERFORM partman.create_parent(
                p_parent_table    => v_table,
                p_control         => v_control,
                p_interval        => '1 day',
                p_type            => 'range',   -- the only value pg_partman 5.x accepts (verified:
                                                 -- check_partition_type() allows only 'range'/'list';
                                                 -- the old 'native' literal no longer exists)
                p_premake         => 7,
                p_default_table   => false,     -- see header: our DEFAULT partition already exists
                p_jobmon          => false
            );
        END IF;
    END LOOP;
END $$;

-- Create this run's premade partitions immediately (rather than waiting for the first scheduled
-- maintenance tick) so a fresh deployment is already ahead of the write path. The background worker
-- (pg_partman_bgw, deploy/docker/postgres-chf.Dockerfile + this service's own command-line GUCs in
-- deploy/docker/docker-compose.yml) takes over from here in a persistent deployment; CI has no
-- persistent server process between test runs, so this manual call is also what proves the
-- partitioning logic itself works there (disclosed in ADR-0392: CI validates the SQL, not the BGW).
CALL partman.run_maintenance_proc();

-- Fail loudly, per this project's fail-closed convention, if maintenance did not actually produce
-- a managed partition set for every table above (rather than silently leaving a table on its
-- DEFAULT-only state and discovering that at Tier-1 volume months later).
DO $$
DECLARE
    v_table text;
    v_count int;
BEGIN
    FOREACH v_table IN ARRAY ARRAY[
        'balance_mgmt.topup_balance', 'balance_mgmt.adjust_balance', 'balance_mgmt.reserve_balance',
        'chf_rating.rating_decision', 'chf_rating.applied_customer_billing_rate'
    ] LOOP
        SELECT count(*) INTO v_count
        FROM pg_inherits i
        JOIN pg_class c ON c.oid = i.inhparent
        JOIN pg_namespace n ON n.oid = c.relnamespace
        WHERE n.nspname || '.' || c.relname = v_table;
        -- >= 8: the pre-existing DEFAULT partition plus at least the 7 premade partitions.
        IF v_count < 8 THEN
            RAISE EXCEPTION 'partition management (ADR-0392) did not produce the expected managed '
                'partition set for %: found % child partitions, expected >= 8 (1 default + 7 '
                'premade). pg_partman extension present? part_config row present? '
                'run_maintenance_proc() error swallowed?', v_table, v_count;
        END IF;
    END LOOP;
END $$;
