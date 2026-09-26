-- =================================================================================================
-- Re-establish bucket -> account referential integrity (ADR-0386)
-- =================================================================================================
-- 41-balance-lossless.sql dropped balance_mgmt.bucket.party_account_id -> subscriber_mgmt.account
-- because accounts were still created in subscriber-management's old per-service DB (ADR-0385).
-- With subscriber-management now on this DB (ADR-0386) the FK comes back. NOT VALID: enforced for
-- every new/updated row; pre-existing lab rows are not retro-checked (validate once they are cleaned:
-- ALTER TABLE balance_mgmt.bucket VALIDATE CONSTRAINT bucket_party_account_fk).
DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_constraint WHERE conname = 'bucket_party_account_fk') THEN
        ALTER TABLE balance_mgmt.bucket ADD CONSTRAINT bucket_party_account_fk
            FOREIGN KEY (party_account_id) REFERENCES subscriber_mgmt.account(id) NOT VALID;
    END IF;
END $$;
