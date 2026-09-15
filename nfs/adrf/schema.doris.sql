-- ADRF Data Store (ADR-0367; storage engine chosen in ADR-0359: the collected data and analytics
-- the ADRF keeps ARE the analytics repository, so they live in Apache Doris next to the CDRs and
-- the feature store, queryable in place by the MTLF's training side).
--
-- One row per stored NadrfDataStoreRecord (TS 29.575 5.1.6.2.2). The record itself is kept whole
-- as JSON -- the ADRF is a repository, it does not reinterpret what it stores; the columns beside
-- it are exactly what the service operations select and delete on:
--   store_trans_id   RetrievalRequest by store-trans-id, Delete by storeTransId (4.2.2.5, 4.2.2.9.2)
--   spec_fp          "the same data or analytics" -- the fingerprint of the record's dataSub /
--                    anaSub with notification-target fields removed (the DCCF's definition,
--                    ADR-0366, reused so a retrieval subscription's anaSub/dataSub finds the records
--                    a storage subscription with the same spec produced)
--   data_set_id      RetrievalRequest / RetrievalSubscribe / Delete by dataSetId (EnhDataMgmt)
--   collected_at     the notification's own timeStamp, else receipt time -- what timePeriod filters
--   expires_at       storage lifetime (storeHandl.lifetime bounded by operator policy); the reaper
--                    alerts delNotifUri before deleting (4.2.2.8.3) and can defer on retrievalInd
--
-- UNIQUE KEY on store_trans_id (merge-on-write, the Doris default since 2.1): a DELETE or UPDATE by
-- any column is a plain statement, no partition juggling. Lifetimes are seconds-to-days, not the
-- CHF's month-scale CDR retention, so no partition-drop scheme here.
CREATE TABLE IF NOT EXISTS data_store_records (
    store_trans_id     VARCHAR(64)   NOT NULL,
    kind               VARCHAR(16)   NOT NULL,  -- 'analytics' | 'data'
    origin             VARCHAR(16)   NOT NULL,  -- 'request' (StorageRequest) | 'subscription'
    spec_fp            VARCHAR(32)   NOT NULL,
    data_set_id        VARCHAR(128),
    collected_at       DATETIME      NOT NULL,
    stored_at          DATETIME      NOT NULL,
    expires_at         DATETIME      NOT NULL,
    del_notif_uri      VARCHAR(2048),
    del_notif_corr_id  VARCHAR(256),
    alert_sent         BOOLEAN       NOT NULL,
    record             JSON          NOT NULL   -- the NadrfDataStoreRecord as stored
)
UNIQUE KEY(store_trans_id)
DISTRIBUTED BY HASH(store_trans_id) BUCKETS 4
PROPERTIES (
    "replication_num" = "1",
    "enable_unique_key_merge_on_write" = "true"
);
