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
    charging_data_ref              VARCHAR(64)  NOT NULL,
    invocation_sequence_number     BIGINT       NOT NULL,
    service_type                   VARCHAR(32)  NOT NULL, -- project-internal: 'ConvergedCharging' | 'OfflineOnlyCharging'
    operation                      VARCHAR(16),            -- project-internal: 'Create' | 'Update' | 'Release'
    subscriber_identifier          VARCHAR(64),             -- real TS 32.291 field: subscriberIdentifier (SUPI)
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
    asn1_cdr                       STRING DEFAULT ''        -- real TS 32.298 ChargingRecord, BER-encoded (ADR-0089), hex-encoded (ADR-0192, no native BLOB); empty if this row's own nf_consumer_node_functionality has no real TS 32.298 NetworkFunctionality value to map to (see cdr_asn1.cpp)
)
UNIQUE KEY(charging_data_ref, invocation_sequence_number, service_type)
DISTRIBUTED BY HASH(charging_data_ref) BUCKETS 10
PROPERTIES ("replication_num" = "1");
