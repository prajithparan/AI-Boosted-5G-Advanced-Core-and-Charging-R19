-- =================================================================================================
-- PARTY (party) + SUBSCRIBER MGMT (subscriber_mgmt) -- lossless against the TMF632 / project DTOs
-- the subscriber-management API accepts (ADR-0386)
-- =================================================================================================
-- Measured field by field against bss_sid::Individual / Organization (TMF632) and the project's
-- own Account / Subscriber DTOs, 10-party.sql + 20-subscriber.sql: stored birthDate/deathDate as
-- DATE (TMF632 carries a date-time); dropped the href of relatedParty and of the parent/child
-- organization refs; keyed TaxExemptionCertificate / TaxDefinition by their OPTIONAL TMF ids; kept no
-- list order; and had nowhere for the subscriber lifecycle history the service records.
-- Idempotent -- applies to a fresh DB and to one that already ran 10/20.
SET search_path TO party;

ALTER TABLE individual ALTER COLUMN birth_date TYPE TIMESTAMPTZ USING birth_date::timestamptz;
ALTER TABLE individual ALTER COLUMN death_date TYPE TIMESTAMPTZ USING death_date::timestamptz;

ALTER TABLE related_party ADD COLUMN IF NOT EXISTS href TEXT;
ALTER TABLE organization ADD COLUMN IF NOT EXISTS parent_organization_href TEXT;
ALTER TABLE organization_child_relationship ADD COLUMN IF NOT EXISTS child_organization_href TEXT;
-- TMF632 ids on these are optional: surrogate PK stays, the TMF id gets its own column.
ALTER TABLE tax_exemption_certificate ADD COLUMN IF NOT EXISTS cert_id TEXT;
ALTER TABLE tax_definition ADD COLUMN IF NOT EXISTS def_id TEXT;

-- List order for every TMF632 array.
ALTER TABLE contact_medium                  ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;
ALTER TABLE individual_identification       ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;
ALTER TABLE organization_identification     ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;
ALTER TABLE credit_profile                  ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;
ALTER TABLE external_reference              ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;
ALTER TABLE disability                      ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;
ALTER TABLE language_ability                ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;
ALTER TABLE skill                           ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;
ALTER TABLE other_name_individual           ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;
ALTER TABLE other_name_organization         ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;
ALTER TABLE party_characteristic            ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;
ALTER TABLE related_party                   ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tax_exemption_certificate       ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tax_definition                  ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;
ALTER TABLE organization_child_relationship ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;

-- Server-assigned ids for API-created parties (bss/provisioning uses its own "ind-" ids).
CREATE SEQUENCE IF NOT EXISTS individual_id_seq;
CREATE SEQUENCE IF NOT EXISTS organization_id_seq;

SET search_path TO subscriber_mgmt;
CREATE SEQUENCE IF NOT EXISTS account_id_seq;
CREATE SEQUENCE IF NOT EXISTS subscriber_id_seq;

-- The lifecycle history subscriber-management records on every status transition.
CREATE TABLE IF NOT EXISTS subscriber_lifecycle_event (
    id            BIGSERIAL PRIMARY KEY,
    subscriber_id TEXT NOT NULL REFERENCES subscriber(id),
    from_status   TEXT,
    to_status     TEXT NOT NULL,
    reason        TEXT,
    occurred_at   TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_sublc_subscriber ON subscriber_lifecycle_event (subscriber_id, occurred_at DESC);
CREATE INDEX IF NOT EXISTS idx_sublc_to_status  ON subscriber_lifecycle_event (to_status, occurred_at DESC);
