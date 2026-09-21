-- ============================================================================
-- RATING / BILLING (chf_rating) -- internal rating audit + TMF678 AppliedCustomerBillingRate +
-- CustomerBill (bss_sid/rating.hpp). The per-usage tables are time-series -> RANGE-partitioned by
-- date, indexed for per-subscriber / per-account retrieval (digital-channel bill history).
-- ============================================================================
SET search_path TO chf_rating;

-- Internal rating decision (per rated usage) -- heavy time-series.
CREATE TABLE rating_decision (
    id                    TEXT NOT NULL,
    charging_data_ref     TEXT NOT NULL,
    subscriber_identifier TEXT NOT NULL,                 -- SUPI, for per-customer inquiry
    rating_group          BIGINT,
    decided_at            TIMESTAMPTZ NOT NULL DEFAULT now(),
    rated_units           BIGINT,
    monetary_amount       NUMERIC(18,4),
    currency              TEXT,
    input_snapshot        JSONB NOT NULL DEFAULT '{}',
    PRIMARY KEY (decided_at, id)
) PARTITION BY RANGE (decided_at);
CREATE TABLE rating_decision_default PARTITION OF rating_decision DEFAULT;
CREATE INDEX idx_rating_subscriber ON rating_decision (subscriber_identifier, decided_at);
CREATE INDEX idx_rating_ref        ON rating_decision (charging_data_ref);

-- TMF678 AppliedCustomerBillingRate -- one per billable usage/charge; heavy time-series.
CREATE TABLE applied_customer_billing_rate (
    id                    TEXT NOT NULL,
    href                  TEXT,
    rate_date             TIMESTAMPTZ NOT NULL DEFAULT now(),
    description           TEXT,
    is_billed             BOOLEAN,
    name                  TEXT,
    rate_type             TEXT,
    bill_id               TEXT,
    billing_account_id    TEXT REFERENCES subscriber_mgmt.account(id),
    product_id            TEXT,
    tax_excluded_unit     TEXT, tax_excluded_amount NUMERIC(18,4),
    tax_included_unit     TEXT, tax_included_amount NUMERIC(18,4),
    period_coverage_start TIMESTAMPTZ, period_coverage_end TIMESTAMPTZ,
    PRIMARY KEY (rate_date, id)
) PARTITION BY RANGE (rate_date);
CREATE TABLE applied_customer_billing_rate_default PARTITION OF applied_customer_billing_rate DEFAULT;
CREATE INDEX idx_acbr_account ON applied_customer_billing_rate (billing_account_id, rate_date);
CREATE INDEX idx_acbr_bill    ON applied_customer_billing_rate (bill_id);
CREATE INDEX idx_acbr_billed  ON applied_customer_billing_rate (is_billed) WHERE is_billed = false;

CREATE TABLE applied_billing_tax_rate (
    id             BIGSERIAL PRIMARY KEY,
    acbr_id        TEXT NOT NULL,                          -- ref to applied_customer_billing_rate.id
    tax_category   TEXT, tax_rate DOUBLE PRECISION, tax_amount_unit TEXT, tax_amount_value NUMERIC(18,4)
);
CREATE INDEX idx_abtr_acbr ON applied_billing_tax_rate (acbr_id);

-- TMF678 CustomerBill -- the bill; master (indexed by account + date).
CREATE TABLE customer_bill (
    id                 TEXT PRIMARY KEY,
    href               TEXT,
    bill_date          TIMESTAMPTZ,
    bill_no            TEXT,
    category           TEXT,
    last_update        TIMESTAMPTZ,
    next_bill_date     TIMESTAMPTZ,
    payment_due_date   TIMESTAMPTZ,
    run_type           TEXT,
    state              TEXT,
    amount_due_unit    TEXT, amount_due_amount NUMERIC(18,4),
    remaining_unit     TEXT, remaining_amount  NUMERIC(18,4),
    tax_excluded_unit  TEXT, tax_excluded_amount NUMERIC(18,4),
    tax_included_unit  TEXT, tax_included_amount NUMERIC(18,4),
    billing_account_id TEXT REFERENCES subscriber_mgmt.account(id),
    billing_period_start TIMESTAMPTZ, billing_period_end TIMESTAMPTZ,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_bill_account ON customer_bill (billing_account_id, bill_date);
CREATE INDEX idx_bill_state   ON customer_bill (state);
CREATE INDEX idx_bill_no      ON customer_bill (bill_no);

CREATE TABLE audit_record (
    id     BIGSERIAL PRIMARY KEY,
    at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    actor  TEXT, action TEXT, detail JSONB NOT NULL DEFAULT '{}'
);
