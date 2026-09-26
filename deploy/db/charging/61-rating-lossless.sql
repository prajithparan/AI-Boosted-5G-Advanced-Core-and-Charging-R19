-- =================================================================================================
-- CHF RATING (chf_rating) -- lossless against the CHF's RatingDecisionRecord (ADR-0388)
-- =================================================================================================
-- 60-rating.sql partitions rating decisions by time and makes charging_data_ref / subscriber
-- identifier first-class (per-customer inquiry at Tier-1 volume), and gives TMF678
-- AppliedCustomerBillingRate its own table. Measured against what the CHF records per decision it
-- dropped the tariff that fired (tariff id + pinned version), the rule id, the AI advisory, and cut
-- amounts to 4 decimals (the old store kept 6; per-octet rating needs more). Idempotent.
SET search_path TO chf_rating;

ALTER TABLE rating_decision ADD COLUMN IF NOT EXISTS tariff_id TEXT;          -- ProductOfferingPrice.id
ALTER TABLE rating_decision ADD COLUMN IF NOT EXISTS tariff_version TEXT;     -- pinned price version
ALTER TABLE rating_decision ADD COLUMN IF NOT EXISTS rule_fired_id TEXT;
ALTER TABLE rating_decision ADD COLUMN IF NOT EXISTS ai_advisory JSONB;       -- AiQuotaSizer advisory
ALTER TABLE rating_decision ALTER COLUMN monetary_amount TYPE NUMERIC;
CREATE INDEX IF NOT EXISTS idx_rating_tariff ON rating_decision (tariff_id, decided_at);
CREATE INDEX IF NOT EXISTS idx_rating_id ON rating_decision (id);   -- PK is (decided_at, id)
CREATE SEQUENCE IF NOT EXISTS rating_decision_id_seq;

ALTER TABLE applied_customer_billing_rate ALTER COLUMN tax_excluded_amount TYPE NUMERIC;
ALTER TABLE applied_customer_billing_rate ALTER COLUMN tax_included_amount TYPE NUMERIC;
CREATE INDEX IF NOT EXISTS idx_acbr_id ON applied_customer_billing_rate (id);
