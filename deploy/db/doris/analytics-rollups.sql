-- Doris analytics rollups (charging_analytics db) -- the READ-OPTIMIZED / cross-customer aggregate
-- layer. Per the performance strategy (project_tier1_scale_architecture): per-customer reads stay in
-- Postgres (bounded, indexed); heavy cross-customer + time-series aggregates live here in Doris MPP,
-- date-partitioned + subscriber-bucketed so digital-channel "my monthly usage/bill" and operator
-- analytics are pruned prefix scans. AGGREGATE KEY tables pre-aggregate at ingest.
-- Source: docs/DATA_MODEL.md E4 (Usage/CDR) + E6 (Balance). cdr + subscriber_features already exist.
CREATE DATABASE IF NOT EXISTS charging_analytics;

-- Per-subscriber per-day usage (digital-channel "my usage"): pre-summed at ingest.
CREATE TABLE IF NOT EXISTS charging_analytics.subscriber_usage_daily (
    usage_date            DATE          NOT NULL,
    subscriber_identifier VARCHAR(128)  NOT NULL,
    rating_group          BIGINT        NOT NULL,
    total_octets          BIGINT        SUM DEFAULT "0",
    session_count         BIGINT        SUM DEFAULT "0",
    total_duration_sec    BIGINT        SUM DEFAULT "0"
) AGGREGATE KEY(usage_date, subscriber_identifier, rating_group)
PARTITION BY RANGE(usage_date) ()
DISTRIBUTED BY HASH(subscriber_identifier) BUCKETS 32
PROPERTIES ("replication_num"="1","dynamic_partition.enable"="true","dynamic_partition.time_unit"="DAY",
            "dynamic_partition.start"="-400","dynamic_partition.end"="3","dynamic_partition.prefix"="p",
            "dynamic_partition.buckets"="32");

-- Per-subscriber per-month rated amount (digital-channel "my bill" + revenue analytics).
CREATE TABLE IF NOT EXISTS charging_analytics.subscriber_revenue_monthly (
    bill_month            DATE          NOT NULL,
    subscriber_identifier VARCHAR(128)  NOT NULL,
    rating_group          BIGINT        NOT NULL,
    currency              VARCHAR(8)     REPLACE DEFAULT "",
    monetary_amount       DECIMAL(20,4)  SUM DEFAULT "0",
    rated_units           BIGINT         SUM DEFAULT "0"
) AGGREGATE KEY(bill_month, subscriber_identifier, rating_group)
PARTITION BY RANGE(bill_month) ()
DISTRIBUTED BY HASH(subscriber_identifier) BUCKETS 32
PROPERTIES ("replication_num"="1","dynamic_partition.enable"="true","dynamic_partition.time_unit"="MONTH",
            "dynamic_partition.start"="-36","dynamic_partition.end"="2","dynamic_partition.prefix"="p",
            "dynamic_partition.buckets"="32");

-- Per-slice per-day usage (operator + NWDAF SERVICE_EXPERIENCE analytics; generic S-NSSAI).
CREATE TABLE IF NOT EXISTS charging_analytics.slice_usage_daily (
    usage_date   DATE          NOT NULL,
    snssai       VARCHAR(64)   NOT NULL,          -- verbatim SST[+SD], any std/custom slice
    total_octets BIGINT        SUM DEFAULT "0",
    session_count BIGINT       SUM DEFAULT "0",
    subscriber_count BIGINT    SUM DEFAULT "0"
) AGGREGATE KEY(usage_date, snssai)
PARTITION BY RANGE(usage_date) ()
DISTRIBUTED BY HASH(snssai) BUCKETS 8
PROPERTIES ("replication_num"="1","dynamic_partition.enable"="true","dynamic_partition.time_unit"="DAY",
            "dynamic_partition.start"="-400","dynamic_partition.end"="3","dynamic_partition.prefix"="p",
            "dynamic_partition.buckets"="8");

-- Per-day revenue by rating group (finance/operator dashboard).
CREATE TABLE IF NOT EXISTS charging_analytics.revenue_daily (
    revenue_date  DATE          NOT NULL,
    rating_group  BIGINT        NOT NULL,
    currency      VARCHAR(8)    NOT NULL,
    monetary_amount DECIMAL(20,4) SUM DEFAULT "0"
) AGGREGATE KEY(revenue_date, rating_group, currency)
PARTITION BY RANGE(revenue_date) ()
DISTRIBUTED BY HASH(rating_group) BUCKETS 4
PROPERTIES ("replication_num"="1","dynamic_partition.enable"="true","dynamic_partition.time_unit"="DAY",
            "dynamic_partition.start"="-400","dynamic_partition.end"="3","dynamic_partition.prefix"="p",
            "dynamic_partition.buckets"="4");
