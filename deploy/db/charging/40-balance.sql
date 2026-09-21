-- ============================================================================
-- BALANCE ABE -- faithful to TMF654 Prepay Balance v4 (bss_sid/balance.hpp).
-- Bucket + AccumulatedBalance are master (indexed); TopupBalance/AdjustBalance/ReserveBalance are
-- ACTION/transaction resources -> RANGE-partitioned by date (Tier-1 time-series). Refs inlined as
-- id columns; array refs (logicalResource, product, relatedParty) as child tables. Find-balance-by-
-- subscriber goes via bucket_logical_resource (the SUPI) or partyAccount (the account).
-- ============================================================================
SET search_path TO balance_mgmt;

-- Bucket (the balance resource) -- master.
CREATE TABLE bucket (
    id                  TEXT PRIMARY KEY,
    href                TEXT,
    name                TEXT,
    description         TEXT,
    is_shared           BOOLEAN,
    remaining_value_name TEXT,
    confirmation_date   TIMESTAMPTZ,
    requested_date      TIMESTAMPTZ,
    -- partyAccount (single ref, inline) -> the account (subscriber_mgmt.account)
    party_account_id    TEXT REFERENCES subscriber_mgmt.account(id),
    party_account_name  TEXT,
    party_account_status TEXT,
    remaining_value_unit TEXT, remaining_value_amount NUMERIC(20,4),  -- Money
    reserved_value_unit  TEXT, reserved_value_amount  NUMERIC(20,4),
    status              TEXT,                          -- active|suspended|expired
    usage_type          TEXT,                          -- monetary|voice|data|sms|other
    valid_for_start TIMESTAMPTZ, valid_for_end TIMESTAMPTZ,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(), updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_bucket_account ON bucket (party_account_id);
CREATE INDEX idx_bucket_shared  ON bucket (is_shared) WHERE is_shared;
CREATE INDEX idx_bucket_status  ON bucket (status);

-- Bucket array refs.
CREATE TABLE bucket_logical_resource (
    bucket_id TEXT NOT NULL REFERENCES bucket(id) ON DELETE CASCADE,
    resource_id TEXT NOT NULL, href TEXT, name TEXT,   -- the SUPI/resource this bucket serves
    PRIMARY KEY (bucket_id, resource_id)
);
CREATE INDEX idx_bucketlr_resource ON bucket_logical_resource (resource_id);  -- find balance by SUPI
CREATE TABLE bucket_product (
    bucket_id TEXT NOT NULL REFERENCES bucket(id) ON DELETE CASCADE,
    product_id TEXT NOT NULL, href TEXT, name TEXT, PRIMARY KEY (bucket_id, product_id)
);
CREATE TABLE bucket_related_party (
    id BIGSERIAL PRIMARY KEY, bucket_id TEXT NOT NULL REFERENCES bucket(id) ON DELETE CASCADE,
    party_id TEXT NOT NULL, href TEXT, name TEXT, role TEXT
);

-- AccumulatedBalance -- aggregate of a party account's buckets.
CREATE TABLE accumulated_balance (
    id                 TEXT PRIMARY KEY, href TEXT, description TEXT, name TEXT,
    bucket_id          TEXT REFERENCES bucket(id),
    party_account_id   TEXT REFERENCES subscriber_mgmt.account(id),
    total_balance_unit TEXT, total_balance_amount NUMERIC(20,4),
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_accbal_account ON accumulated_balance (party_account_id);

-- TopupBalance (action/transaction) -- partitioned by date.
CREATE TABLE topup_balance (
    id                 TEXT NOT NULL,
    bucket_id          TEXT,
    party_account_id   TEXT,
    is_auto_topup      BOOLEAN,
    number_of_periods  INTEGER,
    reason             TEXT,
    voucher            TEXT,
    amount_value       NUMERIC(20,4), amount_units TEXT,   -- Quantity
    channel_id         TEXT, payment_method_id TEXT,
    recurring_period   TEXT,                                -- weekly|fortnightly|monthly
    status             TEXT,                                -- created|failed|cancelled|completed
    usage_type         TEXT,
    confirmation_date  TIMESTAMPTZ, requested_date TIMESTAMPTZ,
    occurred_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (occurred_at, id)
) PARTITION BY RANGE (occurred_at);
CREATE TABLE topup_balance_default PARTITION OF topup_balance DEFAULT;
CREATE INDEX idx_topup_bucket ON topup_balance (bucket_id, occurred_at);

-- AdjustBalance (action/transaction) -- partitioned by date.
CREATE TABLE adjust_balance (
    id                 TEXT NOT NULL,
    bucket_id          TEXT,
    party_account_id   TEXT,
    reason             TEXT,
    amount_value       NUMERIC(20,4), amount_units TEXT,
    status             TEXT,
    confirmation_date  TIMESTAMPTZ, requested_date TIMESTAMPTZ,
    occurred_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (occurred_at, id)
) PARTITION BY RANGE (occurred_at);
CREATE TABLE adjust_balance_default PARTITION OF adjust_balance DEFAULT;
CREATE INDEX idx_adjust_bucket ON adjust_balance (bucket_id, occurred_at);

-- ReserveBalance (action/transaction) -- ties a reservation to the CDR session; partitioned by date.
CREATE TABLE reserve_balance (
    id                 TEXT NOT NULL,
    bucket_id          TEXT,
    party_account_id   TEXT,
    charging_data_ref  TEXT,                                -- the CDR session this reserves against
    amount_value       NUMERIC(20,4), amount_units TEXT,
    status             TEXT,
    confirmation_date  TIMESTAMPTZ, requested_date TIMESTAMPTZ,
    occurred_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (occurred_at, id)
) PARTITION BY RANGE (occurred_at);
CREATE TABLE reserve_balance_default PARTITION OF reserve_balance DEFAULT;
CREATE INDEX idx_reserve_bucket ON reserve_balance (bucket_id, occurred_at);
CREATE INDEX idx_reserve_ref    ON reserve_balance (charging_data_ref);
