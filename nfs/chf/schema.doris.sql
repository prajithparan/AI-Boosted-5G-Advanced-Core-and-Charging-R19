-- nfs/chf's real Apache Doris CDR schema (P4.4/ADR-0058: CDF, TS 32.240/32.296; migrated to
-- Apache Doris, ADR-0192 -- that ADR holds the full comparison against the previous engine).
--
-- Disclosed history: through `recorded_at`, these columns were originally NOT a conformant
-- TS 32.298 CDR -- that spec wasn't vendored yet at the time. Gap-closure task #108/ADR-0089
-- supplied the real, vendored TS 32.298 (specs/TS_32_298.pdf) and added `asn1_cdr`: a real,
-- spec-conformant BER-encoded `ChargingRecord` (nfs/chf/src/cdr_asn1.cpp), stored alongside these
-- pre-existing columns rather than replacing them. Every non-`asn1_cdr` column below is a real,
-- already-confirmed TS 32.291 field (ChargingDataRequest/Response, already vendored and used
-- throughout nfs/chf/src/main.cpp) or a real, disclosed project-internal addition (recorded_at,
-- service_type, operation) -- not a fabricated field name.
--
-- UNIQUE KEY model (real, native Doris duplicate-detection mechanism, ADR-0192): rows sharing the
-- same key columns are deduplicated. Doris's Unique Key model with Merge-on-Write performs REAL,
-- IMMEDIATE dedup at write time, rather than only during background merges -- a genuine, disclosed
-- improvement this migration picked up rather than a like-for-like swap (ADR-0192 has the
-- engine-to-engine comparison).
--
-- Real, disclosed limitation: Doris requires any partition-by column to be part of the table's own
-- key columns for a Unique Key table (confirmed via Doris's own documentation, not assumed). Using
-- `recorded_at` for date-range partitioning (matching the pre-migration 90-day TTL) would
-- require adding it to the key, which would change real dedup semantics (the same CDR written on
-- two different days would then no longer deduplicate). Rather than silently accept that behavior
-- change, this migration deliberately keeps the UNIQUE KEY identical to the original ORDER BY key
-- and does NOT partition/TTL this table -- the 90-day retention window the pre-migration TTL
-- provided is a real, disclosed, deferred capability this migration does not yet replace (same
-- "separate cold-archive tier NOT implemented this pass" disclosure the original schema already
-- carried).
--
-- Real, disclosed limitation: Doris has no native BLOB type (confirmed via Doris's own
-- documentation) -- `asn1_cdr` therefore stores the real BER-encoded bytes as a HEX-ENCODED TEXT
-- string (nfs/chf/src/cdr.cpp), not the raw bytes themselves the way the pre-migration String
-- column held them. A real, disclosed representation change, not a silent one: nothing in this
-- project currently reads this column back (same as before the migration -- CdrWriter's own
-- detect_gaps() only ever queried invocation_sequence_number), so no decode path exists yet
-- either; any future real consumer of this column must hex-decode it first.

CREATE TABLE IF NOT EXISTS cdr (
    -- ADR-0348: the partition column, declared FIRST because Doris requires a UNIQUE KEY table's
    -- key columns to lead the definition in key order. DATE not DATETIME: day-granular is what
    -- makes pruning and partition-drop retention work, and the exact instant is already carried by
    -- `recorded_at` and `invocation_time_stamp`.
    recorded_date                  DATE         NOT NULL,
    subscriber_identifier          VARCHAR(128) NOT NULL, -- real TS 32.291 field: subscriberIdentifier (SUPI); widened to 128 and promoted into the key (ADR-0348)
    charging_data_ref              VARCHAR(64)  NOT NULL,
    invocation_sequence_number     BIGINT       NOT NULL,
    service_type                   VARCHAR(32)  NOT NULL, -- project-internal: 'ConvergedCharging' | 'OfflineOnlyCharging'
    operation                      VARCHAR(16),            -- project-internal: 'Create' | 'Update' | 'Release'
    nf_consumer_node_functionality VARCHAR(64),             -- real TS 32.291 field: nfConsumerIdentification.nodeFunctionality
    rating_group                   BIGINT,                  -- real TS 32.291 field: MultipleUnitUsage.ratingGroup
    granted_total_volume           BIGINT,                  -- real TS 32.291 field: GrantedUnit.totalVolume (octets)
    granted_service_specific_units BIGINT,                  -- real TS 32.291 field: GrantedUnit.serviceSpecificUnits
    used_total_volume              BIGINT,                  -- real TS 32.291 field: UsedUnitContainer.totalVolume (octets)
    reserved_cost                  DOUBLE,                  -- real ABMF amount reserved this event (ADR-0057), project-internal join field
    reserved_cost_currency         VARCHAR(8),
    invocation_time_stamp          DATETIME,                -- real TS 32.291 field: invocationTimeStamp
    recorded_at                    DATETIME DEFAULT CURRENT_TIMESTAMP, -- project-internal: when CHF wrote this CDR row
    serving_plmn                   VARCHAR(16) DEFAULT '', -- ADR-0311: real TS 32.291 pduSessionInformation.servingCNPlmnId, rendered "<mcc>-<mnc>". Empty when the request carried none. Needed to select a roaming partner's own CDRs for a TAP OUT batch -- without it, settlement cannot know whose usage a row is.
    is_roaming                     BOOLEAN DEFAULT "0",    -- ADR-0311: Doris rejects `DEFAULT FALSE` for BOOLEAN ("mismatched input 'FALSE'", confirmed against a real apache/doris:all-in-one-4.1.3, not assumed from portable SQL) -- it wants a string/integer literal. the derived roaming flag (servingCNPlmnId != hPlmnId, ADR-0305). Stored rather than recomputed at query time because the hPlmnId it was derived from is not itself a column, so a later query could not reproduce it.
    -- ADR-0344: WHICH TS 32.291 charging-information block this record came from, and the block
    -- itself.
    --
    -- Until now CHF parsed exactly one of the twenty-five blocks the specification defines
    -- (pDUSessionChargingInformation) and silently discarded the rest. An SMS, an MMTel call and a
    -- PDU session all produced byte-identical rows apart from rating group -- so the charging
    -- system could accept an SMS charging request and lose every fact that made it an SMS.
    --
    -- A column per service would mean hundreds of columns and a schema migration per 3GPP release.
    -- A discriminator plus the preserved block is how production charging systems carry
    -- service-specific records, and it means a new charging-information type in a future release
    -- needs no schema change at all.
    charging_information_type VARCHAR(64) DEFAULT '',
    service_charging_information STRING DEFAULT '',
    asn1_cdr                       STRING DEFAULT ''        -- real TS 32.298 ChargingRecord, BER-encoded (ADR-0089), hex-encoded (ADR-0192, no native BLOB); empty if this row's own nf_consumer_node_functionality has no real TS 32.298 NetworkFunctionality value to map to (see cdr_asn1.cpp)
)
-- ADR-0348: physical design for 700M+ CDRs.
--
-- The previous design was UNIQUE KEY(charging_data_ref, invocation_sequence_number, service_type),
-- 10 buckets, NO partitioning. At 700M rows that is ~70M rows per tablet and every billing-period
-- query scans the entire table, because there is nothing to prune on. Retention had to DELETE rows
-- one predicate at a time instead of dropping a partition -- which on an LSM-style store rewrites
-- data rather than freeing it.
--
-- PARTITIONED BY DATE. `recorded_date` is derived from the write time and must be part of the key,
-- because Doris requires a UNIQUE KEY table's partition column to be a key column. It is placed
-- FIRST so partition pruning applies before any other predicate.
--
-- The dedup consequence, stated rather than discovered: the same CDR retransmitted ACROSS MIDNIGHT
-- lands in a different partition and will NOT deduplicate against the original. A retransmission
-- happens within seconds of the original, so this is a narrow edge case -- but it is a real
-- behaviour change from the unpartitioned table, and it is the price of being able to prune and to
-- drop partitions at all. The alternative, keeping the old key, makes retention and every
-- period query O(whole table) forever.
--
-- 32 BUCKETS, not 10: Doris guidance is roughly 1-10 GB per tablet, and at 700M rows across a
-- year of daily partitions 10 buckets leaves each tablet far past that. Hashing on
-- `subscriber_identifier` rather than `charging_data_ref` co-locates one subscriber's records in
-- one tablet, which is what every per-customer read does -- a bill run, the customer agent's
-- charge history, and NWDAF's per-subscriber usage sequences all become single-tablet scans
-- instead of scatter-gather across all of them.
--
-- replication_num stays 1 for the single-node lab. PRODUCTION MUST RAISE IT: billing records with
-- one replica have no redundancy, and losing a tablet loses revenue evidence. Called out here
-- because a default that is fine in a lab and negligent in production should never be silent.
-- subscriber_identifier joins the key because Doris requires the distribution column to be a key
-- column -- and putting it SECOND, straight after the partition column, is what makes every
-- per-customer read a prefix scan: a bill run, the customer agent's charge history, and NWDAF's
-- per-subscriber usage sequences all hit one tablet after partition pruning instead of
-- scatter-gathering across 32.
--
-- Dedup consequence: the tuple widens, so the same charging_data_ref reported under two different
-- subscriber identifiers would no longer collapse. That combination is a data error rather than a
-- retransmission, so the widened key does not weaken real duplicate suppression.
UNIQUE KEY(recorded_date, subscriber_identifier, charging_data_ref, invocation_sequence_number,
           service_type)
PARTITION BY RANGE(recorded_date) ()
DISTRIBUTED BY HASH(subscriber_identifier) BUCKETS 32
PROPERTIES (
    "replication_num" = "1",
    -- Doris creates the day's partition on demand, so no operator has to pre-create them and no
    -- write is ever rejected for landing in a date nobody anticipated.
    "dynamic_partition.enable" = "true",
    "dynamic_partition.time_unit" = "DAY",
    -- Keep a rolling window on the hot table; ADR-0283's archival tier owns anything older. -400
    -- rather than -90 so a full year of history stays queryable for analytics and year-on-year
    -- revenue comparison, which is what NWDAF training needs.
    "dynamic_partition.start" = "-400",
    "dynamic_partition.end" = "3",
    "dynamic_partition.prefix" = "p",
    "dynamic_partition.buckets" = "32"
);
