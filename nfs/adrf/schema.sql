-- ADRF ML Model Store (ADR-0367; engine per ADR-0359: PostgreSQL for the model index -- and, here,
-- the model bytes themselves, so N ADRF replicas serve the same files with no shared filesystem).
--
-- TS 29.575 5.2: an "Individual ADRF ML Model Store Record" (storeTransId) groups one or more ML
-- models, each with a modelUniqueId that is unique across the ADRF (4.3.2.4.3 deletes by it
-- alone). The owner (nfInstanceId / nfSetId of the MTLF that stored it) and allowConsumerList
-- decide who may retrieve (4.3.2.3.2: RETRIEVAL_ML_MODEL_NOT_ALLOWED otherwise).
CREATE TABLE IF NOT EXISTS ml_store_records (
    store_trans_id        TEXT PRIMARY KEY,
    owner_nf_instance_id  TEXT,
    owner_nf_set_id       TEXT,
    created_at            TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at            TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS ml_models (
    model_unique_id       BIGINT PRIMARY KEY,
    store_trans_id        TEXT NOT NULL REFERENCES ml_store_records(store_trans_id) ON DELETE CASCADE,
    source_addr           JSONB,             -- MLModelAddr the model was downloaded from; NULL when inline
    allow_consumers       JSONB,             -- array(AllowedConsumer) as received; NULL = owner only
    storage_size          BIGINT NOT NULL,   -- octets actually stored (mlStorageSize on retrieval)
    model_bytes           BYTEA  NOT NULL,
    stored_at             TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS ml_models_store_trans_id ON ml_models (store_trans_id);
