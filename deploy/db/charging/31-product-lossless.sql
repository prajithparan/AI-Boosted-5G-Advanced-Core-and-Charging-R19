-- =================================================================================================
-- PRODUCT CATALOG (product_catalog) -- make the normalized TMF620 write model LOSSLESS (ADR-0384)
-- =================================================================================================
-- 30-product.sql normalized the TMF620 resources into relational tables but, measured field by field
-- against the bss_sid DTOs the TMF620 API accepts and returns, it dropped data the JSONB store kept:
-- the href/name/version of most references, all but one AgreementRef, the order of every list, the
-- TMF ids of relationships/bundles, and sub-cent price precision (NUMERIC(18,4)). It also keyed
-- ProductSpecificationCharacteristic and ProductSpecificationCharacteristicValueUse by their TMF ids,
-- which TMF620 scopes to the owning entity -- two prices both carrying a "ratingGroup" value-use with
-- the same id would collide.
--
-- The API contract (TMF620) is fixed; this schema is the project's own. So the schema is extended,
-- never the API narrowed. Every statement is idempotent (IF NOT EXISTS / ADD COLUMN IF NOT EXISTS /
-- DROP ... IF EXISTS) so it applies to a fresh database and to one that already ran 30-product.sql.
-- Every child collection gains `ordinal` so lists round-trip in the order they were posted.
SET search_path TO product_catalog;

-- Server-assigned resource ids (TMF620: the server owns identity on POST to a collection).
CREATE SEQUENCE IF NOT EXISTS product_offering_id_seq;
CREATE SEQUENCE IF NOT EXISTS product_offering_price_id_seq;
CREATE SEQUENCE IF NOT EXISTS product_specification_id_seq;

-- ---- ProductOffering: reference details kept alongside the FK/id columns ----------------------
ALTER TABLE product_offering ADD COLUMN IF NOT EXISTS product_specification_href TEXT;
ALTER TABLE product_offering ADD COLUMN IF NOT EXISTS product_specification_name TEXT;
ALTER TABLE product_offering ADD COLUMN IF NOT EXISTS product_specification_version TEXT;
ALTER TABLE product_offering ADD COLUMN IF NOT EXISTS product_specification_target_schema TEXT;
ALTER TABLE product_offering ADD COLUMN IF NOT EXISTS service_level_agreement_href TEXT;
ALTER TABLE product_offering ADD COLUMN IF NOT EXISTS service_level_agreement_name TEXT;
ALTER TABLE product_offering ADD COLUMN IF NOT EXISTS resource_candidate_href TEXT;
ALTER TABLE product_offering ADD COLUMN IF NOT EXISTS resource_candidate_name TEXT;
ALTER TABLE product_offering ADD COLUMN IF NOT EXISTS resource_candidate_version TEXT;
ALTER TABLE product_offering ADD COLUMN IF NOT EXISTS service_candidate_href TEXT;
ALTER TABLE product_offering ADD COLUMN IF NOT EXISTS service_candidate_name TEXT;
ALTER TABLE product_offering ADD COLUMN IF NOT EXISTS service_candidate_version TEXT;
-- AgreementRef is a LIST in TMF620; the single agreement_id column is superseded by offering_agreement.
ALTER TABLE product_offering DROP COLUMN IF EXISTS agreement_id;

ALTER TABLE offering_price_ref ADD COLUMN IF NOT EXISTS href TEXT;
ALTER TABLE offering_price_ref ADD COLUMN IF NOT EXISTS name TEXT;
ALTER TABLE offering_price_ref ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;
ALTER TABLE offering_category ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;
ALTER TABLE offering_channel ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;
ALTER TABLE offering_market_segment ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;

CREATE TABLE IF NOT EXISTS offering_agreement (
    offering_id TEXT NOT NULL REFERENCES product_offering(id) ON DELETE CASCADE,
    ordinal     INTEGER NOT NULL,
    ref_id      TEXT NOT NULL,          -- AgreementRef.id (TMF651 Agreement; may live in another catalog)
    href        TEXT,
    name        TEXT,
    PRIMARY KEY (offering_id, ordinal)
);
CREATE INDEX IF NOT EXISTS idx_offagr_ref ON offering_agreement (ref_id);

-- Relationship/bundle/term rows: the PK stays a generated surrogate; the TMF id is its own column.
ALTER TABLE offering_relationship ADD COLUMN IF NOT EXISTS rel_id TEXT;
ALTER TABLE offering_relationship ADD COLUMN IF NOT EXISTS href TEXT;
ALTER TABLE offering_relationship ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;
ALTER TABLE bundled_offering ADD COLUMN IF NOT EXISTS href TEXT;
ALTER TABLE bundled_offering ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;
ALTER TABLE offering_term ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;

-- ---- ProductSpecificationCharacteristicValueUse: surrogate PK, TMF id kept separately ----------
ALTER TABLE prod_spec_char_value_use ADD COLUMN IF NOT EXISTS use_id TEXT;
ALTER TABLE prod_spec_char_value_use ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;
ALTER TABLE prod_spec_char_value_use ADD COLUMN IF NOT EXISTS product_specification_href TEXT;
ALTER TABLE prod_spec_char_value_use ADD COLUMN IF NOT EXISTS product_specification_name TEXT;
ALTER TABLE prod_spec_char_value_use ADD COLUMN IF NOT EXISTS product_specification_version TEXT;
ALTER TABLE prod_spec_char_value_use ADD COLUMN IF NOT EXISTS product_specification_target_schema TEXT;
CREATE INDEX IF NOT EXISTS idx_pscvu_use_id ON prod_spec_char_value_use (use_id);
ALTER TABLE char_value_specification ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;

-- ---- ProductOfferingPrice ----------------------------------------------------------------------
-- Money.value is a JSON number in TMF620; per-octet usage prices routinely need more than 4 decimals.
ALTER TABLE product_offering_price ALTER COLUMN price_value TYPE NUMERIC;
ALTER TABLE price_relationship ADD COLUMN IF NOT EXISTS rel_id TEXT;
ALTER TABLE price_relationship ADD COLUMN IF NOT EXISTS href TEXT;
ALTER TABLE price_relationship ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;
ALTER TABLE price_tax_item ADD COLUMN IF NOT EXISTS tax_id TEXT;
ALTER TABLE price_tax_item ADD COLUMN IF NOT EXISTS href TEXT;
ALTER TABLE price_tax_item ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;
ALTER TABLE price_tax_item ALTER COLUMN tax_amount_value TYPE NUMERIC;
-- TaxItem is normalized into price_tax_item; the JSONB copy on the parent is not used.
ALTER TABLE product_offering_price DROP COLUMN IF EXISTS tax;

-- ---- ProductSpecification ----------------------------------------------------------------------
ALTER TABLE product_spec_characteristic ADD COLUMN IF NOT EXISTS char_id TEXT;
ALTER TABLE product_spec_characteristic ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;
CREATE INDEX IF NOT EXISTS idx_pspecchar_char_id ON product_spec_characteristic (char_id);
ALTER TABLE spec_relationship ADD COLUMN IF NOT EXISTS rel_id TEXT;
ALTER TABLE spec_relationship ADD COLUMN IF NOT EXISTS href TEXT;
ALTER TABLE spec_relationship ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;
ALTER TABLE bundled_specification ADD COLUMN IF NOT EXISTS href TEXT;
ALTER TABLE bundled_specification ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;
ALTER TABLE bundled_specification ALTER COLUMN bundled_id DROP NOT NULL;   -- id optional in TMF620
ALTER TABLE spec_related_party ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;
ALTER TABLE spec_candidate_ref ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;
ALTER TABLE spec_candidate_ref ALTER COLUMN ref_id DROP NOT NULL;          -- id optional in TMF620

-- ---- Audit trail (ADR-0060), same shape the per-service DB had, now in this schema --------------
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
CREATE INDEX IF NOT EXISTS idx_pc_audit_entity ON audit_record (entity_type, entity_id);
