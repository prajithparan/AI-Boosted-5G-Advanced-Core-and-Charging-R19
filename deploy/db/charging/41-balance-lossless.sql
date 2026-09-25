-- =================================================================================================
-- BALANCE MANAGEMENT (balance_mgmt) -- make the TMF654 write model LOSSLESS and SAFE (ADR-0385)
-- =================================================================================================
-- Measured field by field against the bss_sid TMF654 DTOs the balance-management API accepts and
-- returns, 40-balance.sql (a) cut amount precision from the old store's 6 decimals to 4, (b) left
-- bucket amounts NULLable -- `reserved_value_amount + x` on NULL is NULL, so the first reserve on a
-- provisioned bucket would have silently corrupted it, (c) omitted most TopupBalance / AdjustBalance
-- / ReserveBalance fields, (d) could only find a top-up/adjust/reserve by id with a scan of every
-- partition (their PK is (occurred_at, id)), and (e) had no index for the shared-bucket lookup the
-- CHF makes before every reserve.
--
-- Access-path decision (ADR-0385): the BUCKET -- the entity read and mutated on every charge -- is
-- normalized. The append-only, partitioned balance EVENTS (topup/adjust/reserve; one reserve per
-- rating decision, hundreds of millions a day at the Tier-1 target) keep their TMF654 reference lists
-- as JSONB columns on the event row: one INSERT per event, never a fan-out into child tables on the
-- hottest write path. Scalars stay columns.
--
-- Idempotent (IF NOT EXISTS / DROP ... IF EXISTS / re-runnable ALTERs) for fresh and existing DBs.
SET search_path TO balance_mgmt;

-- ---- Bucket: exact amounts, never NULL ----------------------------------------------------------
ALTER TABLE bucket ALTER COLUMN remaining_value_amount TYPE NUMERIC;
ALTER TABLE bucket ALTER COLUMN reserved_value_amount  TYPE NUMERIC;
UPDATE bucket SET remaining_value_amount = 0 WHERE remaining_value_amount IS NULL;
UPDATE bucket SET reserved_value_amount  = 0 WHERE reserved_value_amount  IS NULL;
ALTER TABLE bucket ALTER COLUMN remaining_value_amount SET DEFAULT 0;
ALTER TABLE bucket ALTER COLUMN reserved_value_amount  SET DEFAULT 0;
ALTER TABLE bucket ALTER COLUMN remaining_value_amount SET NOT NULL;
ALTER TABLE bucket ALTER COLUMN reserved_value_amount  SET NOT NULL;
-- PartyAccountRef carries href/description too.
ALTER TABLE bucket ADD COLUMN IF NOT EXISTS party_account_href TEXT;
ALTER TABLE bucket ADD COLUMN IF NOT EXISTS party_account_description TEXT;
-- Deferred until subscriber-management's own cut-over: accounts are still created in its old
-- per-service DB, so this FK would reject top-up auto-create for every account it manages.
ALTER TABLE bucket DROP CONSTRAINT IF EXISTS bucket_party_account_id_fkey;

ALTER TABLE bucket_logical_resource ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;
ALTER TABLE bucket_product ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;
ALTER TABLE bucket_related_party ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;
-- The CHF's shared-bucket lookup (GET /bucket?relatedParty.id=<SUPI>) runs before every reserve.
CREATE INDEX IF NOT EXISTS idx_bucketrp_party ON bucket_related_party (party_id);

-- ---- Balance events: every TMF654 field kept; lookup by id indexed --------------------------------
ALTER TABLE topup_balance ALTER COLUMN amount_value TYPE NUMERIC;
ALTER TABLE topup_balance ADD COLUMN IF NOT EXISTS href TEXT;
ALTER TABLE topup_balance ADD COLUMN IF NOT EXISTS description TEXT;
ALTER TABLE topup_balance ADD COLUMN IF NOT EXISTS party_account JSONB;      -- PartyAccountRef
ALTER TABLE topup_balance ADD COLUMN IF NOT EXISTS balance_topup JSONB;      -- RelatedTopupBalance
ALTER TABLE topup_balance ADD COLUMN IF NOT EXISTS channel JSONB;            -- ChannelRef
ALTER TABLE topup_balance ADD COLUMN IF NOT EXISTS payment_method JSONB;     -- PaymentMethodRef
ALTER TABLE topup_balance ADD COLUMN IF NOT EXISTS logical_resource JSONB;   -- LogicalResourceRef[]
ALTER TABLE topup_balance ADD COLUMN IF NOT EXISTS product JSONB;            -- ProductRef[]
ALTER TABLE topup_balance ADD COLUMN IF NOT EXISTS related_party JSONB;      -- RelatedParty[]
ALTER TABLE topup_balance ADD COLUMN IF NOT EXISTS requestor JSONB;          -- RelatedParty
ALTER TABLE topup_balance ADD COLUMN IF NOT EXISTS valid_for_start TIMESTAMPTZ;
ALTER TABLE topup_balance ADD COLUMN IF NOT EXISTS valid_for_end TIMESTAMPTZ;
CREATE INDEX IF NOT EXISTS idx_topup_id ON topup_balance (id);

ALTER TABLE adjust_balance ALTER COLUMN amount_value TYPE NUMERIC;
ALTER TABLE adjust_balance ADD COLUMN IF NOT EXISTS href TEXT;
ALTER TABLE adjust_balance ADD COLUMN IF NOT EXISTS description TEXT;
ALTER TABLE adjust_balance ADD COLUMN IF NOT EXISTS adjust_type TEXT;
ALTER TABLE adjust_balance ADD COLUMN IF NOT EXISTS usage_type TEXT;
ALTER TABLE adjust_balance ADD COLUMN IF NOT EXISTS party_account JSONB;
ALTER TABLE adjust_balance ADD COLUMN IF NOT EXISTS channel JSONB;
ALTER TABLE adjust_balance ADD COLUMN IF NOT EXISTS logical_resource JSONB;
ALTER TABLE adjust_balance ADD COLUMN IF NOT EXISTS product JSONB;
ALTER TABLE adjust_balance ADD COLUMN IF NOT EXISTS related_party JSONB;
ALTER TABLE adjust_balance ADD COLUMN IF NOT EXISTS requestor JSONB;
ALTER TABLE adjust_balance ADD COLUMN IF NOT EXISTS valid_for_start TIMESTAMPTZ;
ALTER TABLE adjust_balance ADD COLUMN IF NOT EXISTS valid_for_end TIMESTAMPTZ;
CREATE INDEX IF NOT EXISTS idx_adjust_id ON adjust_balance (id);

ALTER TABLE reserve_balance ALTER COLUMN amount_value TYPE NUMERIC;
ALTER TABLE reserve_balance ADD COLUMN IF NOT EXISTS href TEXT;
ALTER TABLE reserve_balance ADD COLUMN IF NOT EXISTS description TEXT;
ALTER TABLE reserve_balance ADD COLUMN IF NOT EXISTS reason TEXT;
ALTER TABLE reserve_balance ADD COLUMN IF NOT EXISTS usage_type TEXT;
ALTER TABLE reserve_balance ADD COLUMN IF NOT EXISTS party_account JSONB;
ALTER TABLE reserve_balance ADD COLUMN IF NOT EXISTS channel JSONB;
ALTER TABLE reserve_balance ADD COLUMN IF NOT EXISTS logical_resource JSONB;
ALTER TABLE reserve_balance ADD COLUMN IF NOT EXISTS product JSONB;
ALTER TABLE reserve_balance ADD COLUMN IF NOT EXISTS related_party JSONB;
ALTER TABLE reserve_balance ADD COLUMN IF NOT EXISTS requestor JSONB;
ALTER TABLE reserve_balance ADD COLUMN IF NOT EXISTS valid_for_start TIMESTAMPTZ;
ALTER TABLE reserve_balance ADD COLUMN IF NOT EXISTS valid_for_end TIMESTAMPTZ;
CREATE INDEX IF NOT EXISTS idx_reserve_id ON reserve_balance (id);

-- ---- Server-assigned event ids + audit trail (ADR-0060 shape) -----------------------------------
CREATE SEQUENCE IF NOT EXISTS topup_balance_id_seq;
CREATE SEQUENCE IF NOT EXISTS adjust_balance_id_seq;
CREATE SEQUENCE IF NOT EXISTS reserve_balance_id_seq;
CREATE SEQUENCE IF NOT EXISTS audit_record_id_seq;
CREATE TABLE IF NOT EXISTS audit_record (
    id              TEXT PRIMARY KEY,
    entity_type     TEXT NOT NULL,
    entity_id       TEXT NOT NULL,
    action          TEXT NOT NULL,
    actor           TEXT NOT NULL,
    before_snapshot JSONB,
    after_snapshot  JSONB,
    ai_advisory_ref TEXT,
    recorded_at     TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_bal_audit_entity ON audit_record (entity_type, entity_id);
