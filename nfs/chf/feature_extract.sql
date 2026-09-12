-- ADR-0350: CHF CDRs -> NWDAF subscriber features, for one day.
--
-- Computed ONCE here rather than re-derived by each consumer, so a model, a revenue query and a
-- bill run cannot disagree about the same subscriber's usage.
--
-- Two correctness rules this query exists to enforce in one place:
--
--   1. USAGE comes from rows carrying it (the Updates), not from Release records -- a Release CDR
--      is written with rating_group and used_total_volume both NULL, so summing all operations
--      would count sessions with no usage and dilute every average.
--   2. SESSION COUNT comes from DISTINCT charging_data_ref, not from row count -- one session
--      produces a Create, several Updates and a Release, and counting rows would inflate a
--      three-update session into five "sessions".
--
-- Both are mistakes a consumer writing their own SQL makes silently, and neither shows up as an
-- error: the numbers are simply wrong and plausible.
INSERT INTO chf_features.subscriber_features
SELECT
    :feature_date                                        AS feature_date,
    subscriber_identifier,
    86400                                                AS window_seconds,
    COUNT(DISTINCT charging_data_ref)                    AS session_count,
    SUM(COALESCE(used_total_volume, 0))                  AS total_used_octets,
    MAX(COALESCE(used_total_volume, 0))                  AS max_session_octets,
    AVG(COALESCE(used_total_volume, 0))                  AS avg_session_octets,
    STDDEV(COALESCE(used_total_volume, 0))               AS stddev_session_octets,
    SUM(COALESCE(reserved_cost, 0))                      AS total_spend,
    MAX(COALESCE(reserved_cost_currency, ''))            AS spend_currency,
    SUM(CASE WHEN charging_information_type = 'PDUSession' THEN 1 ELSE 0 END) AS pdu_session_cdrs,
    SUM(CASE WHEN charging_information_type = 'SMS'  THEN 1 ELSE 0 END)       AS sms_cdrs,
    SUM(CASE WHEN charging_information_type = 'MMS'  THEN 1 ELSE 0 END)       AS mms_cdrs,
    SUM(CASE WHEN charging_information_type IN ('MMTel','IMS') THEN 1 ELSE 0 END) AS voice_cdrs,
    -- Content classes are distinct rating groups (ADR-0342), which is how 5G binds a service data
    -- flow to a charge; they carry no separate charging-information block.
    SUM(CASE WHEN rating_group IN (8, 9, 10) THEN 1 ELSE 0 END)              AS content_cdrs,
    COUNT(DISTINCT rating_group)                         AS distinct_rating_groups,
    SUM(CASE WHEN is_roaming THEN 1 ELSE 0 END)          AS roaming_cdrs,
    COUNT(DISTINCT serving_plmn)                         AS distinct_serving_plmns,
    SUM(COALESCE(granted_total_volume, 0))               AS total_granted_octets,
    CASE WHEN SUM(COALESCE(granted_total_volume, 0)) > 0
         THEN SUM(COALESCE(used_total_volume, 0)) / SUM(COALESCE(granted_total_volume, 0))
         ELSE 0 END                                      AS grant_utilisation,
    NOW()                                                AS computed_at
FROM chf_prod.cdr
WHERE recorded_date = :feature_date
GROUP BY subscriber_identifier;
