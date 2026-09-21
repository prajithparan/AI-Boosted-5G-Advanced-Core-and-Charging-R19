-- =====================================================================================
-- CDR MEDIATION -- Doris (OLAP) data model. The revenue-assurance (RA) hub.
--
-- Purpose (user directive 2026-09-21, project_cdr_mediation_revenue_assurance):
--   collect CDRs from MANY sources -> normalize to ONE canonical CDR -> correlate/dedupe
--   -> RECONCILE the network-side record against the CHF-charged record to find revenue
--   leakage. Sources: 5G data (CHF, live -- nfs/chf `cdr` table), voice (MSC circuit-
--   switched + IMS VoLTE), SMS (SMSC legacy + SMSF 5G), 4G online charging (PGW/OCS Gy),
--   roaming (TAP3). This file holds the two Doris tables that carry the CDR volume
--   (Tier-1: 300M CDRs/day) -- MPP, date-partitioned, subscriber-bucketed.
--
-- Provenance / #1 rule: canonical fields are anchored on REAL, vendored codecs the repo
--   already owns -- CHF ASN.1 (nfs/chf/src/cdr_asn1.cpp, TS 32.298/32.291), TAP3
--   (libs/tap3-core), Diameter Gy (libs/diameter-core). MSC (TS 32.250) and SMS (TS
--   32.270) stage-3 record layouts are NOT held offline -> those sources are ingested via
--   a GENERIC staging format and disclosed as "provisioned, not live"; no CDR field
--   invented to fill the gap.
--
-- Doris mechanics carried over from the proven `cdr` table (nfs/chf/schema.doris.sql,
--   ADR-0348), NOT re-derived from portable SQL: UNIQUE KEY table -> partition column must
--   be a key column and lead in key order; BOOLEAN default must be a quoted literal
--   ("0"/"1"), `DEFAULT FALSE` is rejected by Doris; dynamic daily RANGE partitions with
--   create_history_partition so no write is rejected for an unanticipated date.
-- =====================================================================================

CREATE DATABASE IF NOT EXISTS mediation;

-- -------------------------------------------------------------------------------------
-- mediation_cdr -- the canonical, unified CDR from ALL sources.
--
-- TWO DISTINCT IDENTITIES, kept in separate columns (the classic mediation error is to
-- conflate them):
--   * DEDUP identity  = (source_system, source_record_id) within a usage day. This is what
--     the UNIQUE KEY enforces -- a retransmitted MSC file or a replayed CHF CDR collapses
--     to one row.
--   * CORRELATION identity = `correlation_key`, a DERIVED value column (subscriber + service
--     class + time bucket + call/charging ref). Recon groups by this ACROSS sources to line
--     a network record up against its CHF-charged twin. Same column for both and you could
--     neither dedupe a retransmission nor correlate MSC<->CHF.
--
-- PARTITIONED ON EVENT/USAGE TIME (`event_date`), NOT ingest time. A late-arriving voice/SMS
-- file for yesterday's calls must land in yesterday's partition so it partition-aligns with
-- the CHF CDRs it reconciles against. `ingested_at` separately records when mediation loaded
-- the row; the two are deliberately different columns.
--
-- `subscriber_key` is the RESOLVED common subscriber identity (SUPI where the identity
-- resolver -- Valkey chg:sub cache over subscriber_mgmt.resource -- maps the raw network id;
-- otherwise the raw id itself, with subscriber_id_type recording which). It is the
-- distribution column, HASH ... BUCKETS 32, matching `cdr` so one subscriber's records
-- co-locate and a future colocate group can make the recon join local instead of a shuffle.
-- -------------------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS mediation.mediation_cdr (
    -- KEY columns (lead the definition, in key order; event_date first for partition pruning).
    event_date              DATE          NOT NULL COMMENT "usage/event date (NOT ingest date) -- the partition & alignment key",
    subscriber_key          VARCHAR(128)  NOT NULL COMMENT "resolved common subscriber identity (SUPI where resolvable, else raw id); distribution column",
    source_system           VARCHAR(32)   NOT NULL COMMENT "CHF_5G_DATA|MSC_VOICE|IMS_VOICE|SMSC_SMS|SMSF_SMS|PGW_GY_4G|TAP3_ROAM",
    source_record_id        VARCHAR(160)  NOT NULL COMMENT "dedup identity within the source (e.g. charging_data_ref+seq, or file+sequence)",

    -- VALUE columns.
    service_class           VARCHAR(16)   NOT NULL COMMENT "DATA|VOICE|SMS",
    correlation_key         VARCHAR(224)  NOT NULL DEFAULT "" COMMENT "DERIVED cross-source correlation identity (subscriber|service|time-bucket|ref)",
    raw_subscriber_id       VARCHAR(128)  NOT NULL DEFAULT "" COMMENT "identity as received on the source record (SUPI/IMSI/MSISDN)",
    subscriber_id_type      VARCHAR(16)   NOT NULL DEFAULT "" COMMENT "SUPI|IMSI|MSISDN -- what raw_subscriber_id is",
    other_party             VARCHAR(64)   DEFAULT "" COMMENT "called/calling party for VOICE/SMS; empty for DATA",
    event_time              DATETIME      COMMENT "exact usage start / charging invocation instant",
    event_end_time          DATETIME      COMMENT "usage end (voice release); NULL for instantaneous events",
    duration_seconds        BIGINT        DEFAULT 0 COMMENT "call duration (VOICE)",
    volume_uplink_bytes     BIGINT        DEFAULT 0 COMMENT "uplink octets (DATA)",
    volume_downlink_bytes   BIGINT        DEFAULT 0 COMMENT "downlink octets (DATA)",
    volume_total_bytes      BIGINT        DEFAULT 0 COMMENT "total octets (DATA) -- TS 32.291 UsedUnit.totalVolume",
    event_count             BIGINT        DEFAULT 0 COMMENT "event/message count (SMS)",
    rating_group            BIGINT        DEFAULT 0 COMMENT "TS 32.291 MultipleUnitUsage.ratingGroup",
    network_amount          DOUBLE        DEFAULT 0 COMMENT "amount the SOURCE record itself carried (network-rated), if any",
    network_currency        VARCHAR(8)    DEFAULT "" COMMENT "currency of network_amount",
    charged_amount          DOUBLE        DEFAULT 0 COMMENT "CHF-charged amount when this row IS the CHF record (source_system=CHF_5G_DATA)",
    charged_currency        VARCHAR(8)    DEFAULT "" COMMENT "currency of charged_amount",
    charging_id             VARCHAR(96)   DEFAULT "" COMMENT "charging_data_ref / call reference / Gy Session-Id -- the cross-system linking ref",
    serving_plmn            VARCHAR(16)   DEFAULT "" COMMENT "TS 32.291 servingCNPlmnId rendered <mcc>-<mnc>",
    is_roaming              BOOLEAN       DEFAULT "0" COMMENT "derived roaming flag (Doris needs a quoted literal, not FALSE)",
    source_file             VARCHAR(256)  DEFAULT "" COMMENT "provenance: the ingest file this row came from",
    ingested_at             DATETIME      DEFAULT CURRENT_TIMESTAMP COMMENT "when mediation loaded this row (distinct from event_date)"
)
ENGINE=OLAP
UNIQUE KEY(event_date, subscriber_key, source_system, source_record_id)
COMMENT "Canonical multi-source CDR -- the RA correlation input (nfs/chf `cdr` is the 5G-data source of this)"
PARTITION BY RANGE(event_date) ()
DISTRIBUTED BY HASH(subscriber_key) BUCKETS 32
PROPERTIES (
    "replication_num" = "1",
    "dynamic_partition.enable" = "true",
    "dynamic_partition.time_unit" = "DAY",
    "dynamic_partition.start" = "-400",
    "dynamic_partition.create_history_partition" = "true",
    "dynamic_partition.end" = "3",
    "dynamic_partition.prefix" = "p",
    "dynamic_partition.buckets" = "32"
);

-- -------------------------------------------------------------------------------------
-- mediation_recon_result -- the RA DELIVERABLE: one row per correlation reconciled, with the
-- match verdict and the delta.
--
-- BIDIRECTIONAL: MISSING_IN_NETWORK comes from an anti-join FROM the CHF side, so a valid row
-- may have a null network ref; MISSING_IN_CHF has a null CHF ref. Neither ref is NOT NULL
-- (Doris VARCHAR "" is the null-equivalent here; emptiness is meaningful, not a bug).
--
-- status:
--   MATCHED           -- network usage and CHF charge agree within tolerance
--   MISSING_IN_CHF    -- network usage with NO CHF charge  = REVENUE LEAKAGE
--   MISSING_IN_NETWORK-- CHF charge with NO network record = over-charge / phantom charge
--   AMOUNT_MISMATCH   -- both present, amounts differ beyond tolerance
--   DUPLICATE         -- >1 network record for one correlation (double-count risk)
-- -------------------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS mediation.mediation_recon_result (
    recon_date          DATE          NOT NULL COMMENT "usage date reconciled (partition key)",
    subscriber_key      VARCHAR(128)  NOT NULL COMMENT "resolved subscriber identity (distribution column, colocates with mediation_cdr)",
    correlation_key     VARCHAR(224)  NOT NULL COMMENT "the correlation identity reconciled",
    run_id              VARCHAR(64)   NOT NULL COMMENT "ties to Postgres mediation.recon_run.run_id",

    service_class       VARCHAR(16)   NOT NULL DEFAULT "" COMMENT "DATA|VOICE|SMS",
    status              VARCHAR(24)   NOT NULL DEFAULT "" COMMENT "MATCHED|MISSING_IN_CHF|MISSING_IN_NETWORK|AMOUNT_MISMATCH|DUPLICATE",
    network_source      VARCHAR(32)   DEFAULT "" COMMENT "which network source; empty => MISSING_IN_NETWORK",
    network_record_id   VARCHAR(160)  DEFAULT "" COMMENT "network-side source_record_id; empty => MISSING_IN_NETWORK",
    chf_charging_ref    VARCHAR(96)   DEFAULT "" COMMENT "CHF charging_data_ref; empty => MISSING_IN_CHF",
    network_amount      DOUBLE        DEFAULT 0 COMMENT "network-rated amount for this correlation",
    chf_amount          DOUBLE        DEFAULT 0 COMMENT "CHF-charged amount for this correlation",
    amount_delta        DOUBLE        DEFAULT 0 COMMENT "network_amount - chf_amount (the per-correlation leakage)",
    network_usage       BIGINT        DEFAULT 0 COMMENT "network usage (bytes or seconds or count, per service_class)",
    chf_usage           BIGINT        DEFAULT 0 COMMENT "CHF-side usage for the same measure",
    usage_delta         BIGINT        DEFAULT 0 COMMENT "network_usage - chf_usage",
    currency            VARCHAR(8)    DEFAULT "" COMMENT "currency of the amounts",
    dup_count           INT           DEFAULT 0 COMMENT "network record count for this correlation (>1 => DUPLICATE)",
    created_at          DATETIME      DEFAULT CURRENT_TIMESTAMP COMMENT "when this verdict was written"
)
ENGINE=OLAP
UNIQUE KEY(recon_date, subscriber_key, correlation_key, run_id)
COMMENT "Reconciliation verdicts -- the revenue-assurance output"
PARTITION BY RANGE(recon_date) ()
DISTRIBUTED BY HASH(subscriber_key) BUCKETS 32
PROPERTIES (
    "replication_num" = "1",
    "dynamic_partition.enable" = "true",
    "dynamic_partition.time_unit" = "DAY",
    "dynamic_partition.start" = "-400",
    "dynamic_partition.create_history_partition" = "true",
    "dynamic_partition.end" = "3",
    "dynamic_partition.prefix" = "p",
    "dynamic_partition.buckets" = "32"
);
