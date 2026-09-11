-- bss/balance-management PostgreSQL schema.
--
-- Design per docs/DATA_MODEL.md's E6 (Balance Management/ABMF) persistence decision and
-- docs/DECISIONS.md's P4.3 ADR: real TMF654 header fields become real columns.
--
-- Disclosed deviation from docs/DATA_MODEL.md's original E6 sketch (Redis hot balance +
-- PostgreSQL ledger, two stores): this schema uses PostgreSQL ALONE as Bucket's authoritative
-- store. Reasoning: CHARGING_PROMPT.md's P4.3 explicitly requires "strong consistency on balance
-- mutation -- prove it under concurrent debit tests" -- a single-statement atomic
-- `UPDATE ... WHERE remaining_value >= $amount` already gives genuine, provable strong
-- consistency via PostgreSQL's own row-level locking and MVCC, with no risk of the two stores
-- (Redis hot value + PostgreSQL ledger) drifting out of sync under a crash between the two
-- writes. Adding a Redis hot-path cache on top would be a real, valid future optimization once
-- real throughput numbers justify it (nothing benchmarked yet, ADR-0049's own standing
-- disclosure) -- not adding speculative complexity now for a correctness property PostgreSQL
-- alone already provides.
--
-- Extended 2026-08-11 (user directive: "no compromise on data model", docs/DECISIONS.md ADR-0060):
-- full real TMF654 field fidelity, re-confirmed by re-fetching the real swagger directly. Real,
-- concrete bug found and fixed by this pass: `product` is `array<ProductRef>` in the real spec on
-- EVERY one of these resources (Bucket, TopupBalance, AdjustBalance, ReserveBalance), not a single
-- ref -- the old `product_id`/`product_name` scalar column pair could only ever represent one
-- product, silently wrong for the real multi-product case. Replaced with a `product` jsonb array
-- column, matching this project's own established convention for array fields (see
-- bss/product-catalog/schema.sql). Also fixes a real, live data-loss bug: `Bucket.logicalResource`/
-- `Bucket.relatedParty` were already modeled in `bss_sid::Bucket` (the C++ struct) but had NO
-- columns at all -- silently dropped on every write. New jsonb columns for the previously-missing
-- real fields: `logical_resource`, `related_party`, `channel`, `payment_method`, `requestor`,
-- `balance_topup`.

CREATE SEQUENCE IF NOT EXISTS bucket_id_seq;
CREATE SEQUENCE IF NOT EXISTS topup_balance_id_seq;
CREATE SEQUENCE IF NOT EXISTS adjust_balance_id_seq;
CREATE SEQUENCE IF NOT EXISTS reserve_balance_id_seq;

-- The balance resource itself. remaining_value/reserved_value are the authoritative, strongly-
-- consistent values every mutation below acts on atomically.
CREATE TABLE IF NOT EXISTS bucket (
    id                     TEXT PRIMARY KEY,
    href                   TEXT,
    confirmation_date      TEXT,
    description            TEXT,
    is_shared              BOOLEAN,
    name                   TEXT,
    remaining_value_name   TEXT,
    requested_date         TEXT,
    party_account_id       TEXT,
    party_account_name     TEXT,
    product                JSONB NOT NULL DEFAULT '[]',
    logical_resource       JSONB NOT NULL DEFAULT '[]',
    related_party          JSONB NOT NULL DEFAULT '[]',
    remaining_value_unit   TEXT,
    remaining_value        NUMERIC(18, 6) NOT NULL DEFAULT 0,
    reserved_value_unit    TEXT,
    reserved_value         NUMERIC(18, 6) NOT NULL DEFAULT 0,
    status                 TEXT NOT NULL DEFAULT 'active',
    usage_type             TEXT,
    valid_from             TEXT,
    valid_to               TEXT,
    created_at             TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at             TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Real, durable audit ledger for every TopupBalance/AdjustBalance/ReserveBalance action -- entity
-- E8 (Security)'s "full audit trail on every balance... mutation" requirement, and P4.3's own
-- "every rating decision emits an audit record sufficient to reconstruct the charge".

CREATE TABLE IF NOT EXISTS topup_balance (
    id                  TEXT PRIMARY KEY,
    href                TEXT,
    confirmation_date   TEXT,
    description         TEXT,
    is_auto_topup       BOOLEAN,
    number_of_periods   INTEGER,
    reason              TEXT,
    requested_date      TEXT,
    voucher             TEXT,
    bucket_id           TEXT NOT NULL,
    amount              NUMERIC(18, 6) NOT NULL,
    amount_units        TEXT,
    balance_topup       JSONB,
    channel             JSONB,
    logical_resource    JSONB NOT NULL DEFAULT '[]',
    party_account_id    TEXT,
    payment_method      JSONB,
    product              JSONB NOT NULL DEFAULT '[]',
    recurring_period    TEXT,
    related_party       JSONB NOT NULL DEFAULT '[]',
    requestor           JSONB,
    status              TEXT NOT NULL,
    usage_type          TEXT,
    valid_from          TEXT,
    valid_to            TEXT,
    created_at          TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS adjust_balance (
    id                  TEXT PRIMARY KEY,
    href                TEXT,
    confirmation_date   TEXT,
    description         TEXT,
    reason              TEXT,
    requested_date      TEXT,
    adjust_type         TEXT,
    bucket_id           TEXT NOT NULL,
    amount              NUMERIC(18, 6) NOT NULL,
    amount_units        TEXT,
    channel             JSONB,
    logical_resource    JSONB NOT NULL DEFAULT '[]',
    party_account_id    TEXT,
    product              JSONB NOT NULL DEFAULT '[]',
    related_party       JSONB NOT NULL DEFAULT '[]',
    requestor           JSONB,
    status              TEXT NOT NULL,
    usage_type          TEXT,
    valid_from          TEXT,
    valid_to            TEXT,
    created_at          TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS reserve_balance (
    id                  TEXT PRIMARY KEY,
    href                TEXT,
    confirmation_date   TEXT,
    description         TEXT,
    reason              TEXT,
    requested_date      TEXT,
    bucket_id           TEXT NOT NULL,
    amount              NUMERIC(18, 6) NOT NULL,
    amount_units        TEXT,
    channel             JSONB,
    logical_resource    JSONB NOT NULL DEFAULT '[]',
    party_account_id    TEXT,
    product              JSONB NOT NULL DEFAULT '[]',
    related_party       JSONB NOT NULL DEFAULT '[]',
    requestor           JSONB,
    status              TEXT NOT NULL,
    usage_type          TEXT,
    valid_from          TEXT,
    valid_to            TEXT,
    created_at          TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- P4.5/ADR-0060 (E8, Security): real audit trail, docs/DATA_MODEL.md's own explicit requirement
-- ("full audit trail on every balance... mutation"). Same real, local-per-service architectural
-- resolution as bss/product-catalog's own audit_record table (see that file's own header for the
-- full disclosure) -- balance-management owns its own database, so its audit trail lives here,
-- same transaction as the real balance mutation it records.
CREATE SEQUENCE IF NOT EXISTS audit_record_id_seq;

CREATE TABLE IF NOT EXISTS audit_record (
    id                TEXT PRIMARY KEY,
    entity_type       TEXT NOT NULL,   -- BUCKET, TOPUP_BALANCE, ADJUST_BALANCE, RESERVE_BALANCE
    entity_id         TEXT NOT NULL,
    action            TEXT NOT NULL,   -- e.g. "balance.topup", "balance.adjust", "balance.reserve"
    actor             TEXT NOT NULL,
    before_snapshot   JSONB,
    after_snapshot    JSONB,
    ai_advisory_ref   TEXT,
    recorded_at       TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- ADR-0347: the index the hot path needs.
--
-- CHF resolves a subscriber's bucket on EVERY charging request via
-- `GET /bucket?relatedParty.id=<supi>`, which runs `related_party @> '[{"id": ...}]'::jsonb`. With
-- only the primary key present that was a SEQUENTIAL SCAN: measured at 6,495 rows discarded and
-- 2.278 ms per reservation, pegging one Postgres core and capping the entire charging pipeline at
-- ~60 CDRs/sec. With this index the same query is a bitmap index scan at 0.058 ms -- a ~39x
-- improvement that took end-to-end throughput from 60 to 131 CDRs/sec.
--
-- It is O(n) in bucket count without the index, so the cost grows with the subscriber base: at a
-- few thousand buckets it is a slow query, at a few million it is an outage. jsonb_path_ops is the
-- narrower, faster operator class and is sufficient because every lookup is a containment (@>)
-- test, never a key-existence one.
CREATE INDEX IF NOT EXISTS bucket_related_party_gin
    ON bucket USING GIN (related_party jsonb_path_ops);

-- Shared-bucket resolution also filters on these two. A partial index keeps it small: the vast
-- majority of lookups want an active bucket, and indexing the inactive ones would cost writes
-- without serving a read.
CREATE INDEX IF NOT EXISTS bucket_shared_active_idx
    ON bucket (is_shared) WHERE status = 'active' OR status IS NULL;
