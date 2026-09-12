-- ADR-0350: the CHF -> NWDAF feature store.
--
-- CLAUDE.md's data plane is "NFs emit events -> Kafka -> feature store -> training (Python
-- sidecar) -> ONNX artifact -> in-process C++ inference in AnLF". The feature store is the part
-- NWDAF actually consumes, and the part that does not exist: today the only way to reach charging
-- data is a C++ query API or raw SQL against a 2M-row CDR table, which means every consumer
-- re-derives the same aggregates and they drift.
--
-- WHY NOT STRAIGHT FROM THE CDR TABLE: a model, an analytics query and a bill run computing
-- "this subscriber's usage last week" from raw CDRs will each write their own SQL, and the first
-- time one of them forgets to filter `operation='Release'` or double-counts an Update, the
-- numbers disagree with no way to tell which is right. A materialised feature row is computed
-- once, by one piece of code, and every consumer reads the same number.
--
-- WHY NOT KAFKA YET, STATED PLAINLY: the mandated stack names Kafka or Redpanda as the event bus,
-- and this schema is NOT a substitute for it. Streaming matters for low-latency analytics -- an
-- anomaly you learn about a day later is a report, not a signal. This is the batch half, built
-- first because it is what training needs and because a broker JVM does not fit alongside Doris on
-- this hardware. The event interface is defined in analytics_features.hpp so a broker slots in
-- without reshaping the consumer.

CREATE TABLE IF NOT EXISTS subscriber_features (
    -- Partition first, as in the CDR table and for the same reason: every read is windowed.
    feature_date                   DATE         NOT NULL,
    subscriber_identifier          VARCHAR(128) NOT NULL,
    -- The window this row summarises. Daily is the grain NWDAF's mandated analytics need
    -- (load prediction, abnormal behaviour, slice SLA) and coarse enough that 50k subscribers
    -- produce 50k rows a day rather than millions.
    window_seconds                 INT          NOT NULL,

    -- Volume and shape of usage.
    session_count                  BIGINT       DEFAULT 0,
    total_used_octets              BIGINT       DEFAULT 0,
    max_session_octets             BIGINT       DEFAULT 0,
    avg_session_octets             DOUBLE       DEFAULT 0,
    -- Dispersion matters as much as the mean for anomaly detection: a subscriber whose usage is
    -- steady and one whose average is identical but wildly variable are different customers, and a
    -- feature set carrying only the mean cannot tell them apart.
    stddev_session_octets          DOUBLE       DEFAULT 0,

    -- Money. Revenue-per-user analytics and bill-shock detection both read these.
    total_spend                    DECIMAL(18,6) DEFAULT 0,
    spend_currency                 VARCHAR(8)    DEFAULT '',

    -- Product and service mix, as counts rather than a single label: a subscriber is rarely one
    -- product, and collapsing the mix to a dominant category destroys the signal that
    -- distinguishes a heavy video user from a heavy messaging user.
    pdu_session_cdrs               BIGINT       DEFAULT 0,
    sms_cdrs                       BIGINT       DEFAULT 0,
    mms_cdrs                       BIGINT       DEFAULT 0,
    voice_cdrs                     BIGINT       DEFAULT 0,
    content_cdrs                   BIGINT       DEFAULT 0,
    distinct_rating_groups         INT          DEFAULT 0,

    -- Network context. Slice SLA and roaming analytics read these directly.
    roaming_cdrs                   BIGINT       DEFAULT 0,
    distinct_serving_plmns         INT          DEFAULT 0,

    -- Quota behaviour, which is what the existing quota-sizing model predicts against.
    total_granted_octets           BIGINT       DEFAULT 0,
    grant_utilisation              DOUBLE       DEFAULT 0,  -- used / granted, 0 when nothing granted

    computed_at                    DATETIME     NOT NULL
)
UNIQUE KEY(feature_date, subscriber_identifier, window_seconds)
PARTITION BY RANGE(feature_date) ()
DISTRIBUTED BY HASH(subscriber_identifier) BUCKETS 16
PROPERTIES (
    "replication_num" = "1",
    "dynamic_partition.enable" = "true",
    "dynamic_partition.time_unit" = "DAY",
    -- Longer than the CDR window: features are small and a model wants more history than a
    -- billing query does.
    "dynamic_partition.start" = "-800",
    "dynamic_partition.end" = "3",
    "dynamic_partition.prefix" = "p",
    "dynamic_partition.buckets" = "16"
);
