-- =================================================================================================
-- AGREEMENT (subscriber_mgmt, TMF651) + ROAMING (roaming) -- lossless against the DTOs the
-- roaming-interconnect API accepts (ADR-0387)
-- =================================================================================================
-- An interconnect agreement is a TMF651 Agreement plus roaming specifics. 50-agreement.sql gives the
-- TMF651 part a normalized home (subscriber_mgmt.agreement*) and the roaming part its own table,
-- but measured against bss_sid::Agreement it: flattened AgreementItem's product / productOffering /
-- termOrCondition LISTS to one product and one offering per row (terms hung off the agreement);
-- had no home for associatedAgreement; kept completionDate (a TimePeriod) as a single date; dropped
-- agreementSpecification's href/description; kept no list order. roaming_cdr_file had no column for
-- the TAP3 payload, its format, or the agreement it belongs to -- the service's whole purpose.
-- Idempotent.
SET search_path TO subscriber_mgmt;

CREATE SEQUENCE IF NOT EXISTS agreement_id_seq;   -- one id space for every TMF651 agreement

ALTER TABLE agreement ADD COLUMN IF NOT EXISTS completion_date_end TIMESTAMPTZ;  -- completion_date = start
ALTER TABLE agreement ADD COLUMN IF NOT EXISTS agreement_specification_href TEXT;
ALTER TABLE agreement ADD COLUMN IF NOT EXISTS agreement_specification_description TEXT;

ALTER TABLE agreement_item ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;
-- The per-item lists (the single product_id / product_offering_id columns are superseded).
CREATE TABLE IF NOT EXISTS agreement_item_product (
    item_id  TEXT NOT NULL REFERENCES agreement_item(id) ON DELETE CASCADE,
    ordinal  INTEGER NOT NULL,
    ref_id   TEXT NOT NULL, href TEXT, name TEXT,
    PRIMARY KEY (item_id, ordinal)
);
CREATE TABLE IF NOT EXISTS agreement_item_offering (
    item_id  TEXT NOT NULL REFERENCES agreement_item(id) ON DELETE CASCADE,
    ordinal  INTEGER NOT NULL,
    ref_id   TEXT NOT NULL, href TEXT, name TEXT,
    PRIMARY KEY (item_id, ordinal)
);
ALTER TABLE agreement_term_or_condition ADD COLUMN IF NOT EXISTS item_id TEXT
    REFERENCES agreement_item(id) ON DELETE CASCADE;
ALTER TABLE agreement_term_or_condition ADD COLUMN IF NOT EXISTS term_id TEXT;
ALTER TABLE agreement_term_or_condition ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;
CREATE INDEX IF NOT EXISTS idx_agrterm_item ON agreement_term_or_condition (item_id);

CREATE TABLE IF NOT EXISTS agreement_associated (
    agreement_id TEXT NOT NULL REFERENCES agreement(id) ON DELETE CASCADE,
    ordinal      INTEGER NOT NULL,
    ref_id       TEXT NOT NULL, href TEXT, name TEXT,
    PRIMARY KEY (agreement_id, ordinal)
);
ALTER TABLE agreement_authorization  ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;
ALTER TABLE agreement_characteristic ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;
ALTER TABLE agreement_engaged_party  ADD COLUMN IF NOT EXISTS ordinal INTEGER NOT NULL DEFAULT 0;

SET search_path TO roaming;

-- interconnect_agreement.agreement_ref -> the TMF651 agreement (same id; ADR-0387).
DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_constraint WHERE conname = 'interconnect_agreement_ref_fk') THEN
        ALTER TABLE roaming.interconnect_agreement ADD CONSTRAINT interconnect_agreement_ref_fk
            FOREIGN KEY (agreement_ref) REFERENCES subscriber_mgmt.agreement(id);
    END IF;
END $$;
CREATE INDEX IF NOT EXISTS idx_interconnect_partner ON interconnect_agreement (partner_plmn);

ALTER TABLE roaming_cdr_file ADD COLUMN IF NOT EXISTS agreement_id TEXT
    REFERENCES interconnect_agreement(id);
ALTER TABLE roaming_cdr_file ADD COLUMN IF NOT EXISTS format TEXT NOT NULL DEFAULT 'STUB';
ALTER TABLE roaming_cdr_file ADD COLUMN IF NOT EXISTS raw_payload BYTEA;   -- TAP3 BER (libs/tap3-core)
ALTER TABLE roaming_cdr_file ADD COLUMN IF NOT EXISTS received_at TIMESTAMPTZ NOT NULL DEFAULT now();
ALTER TABLE roaming_cdr_file ADD COLUMN IF NOT EXISTS processed_at TIMESTAMPTZ;
CREATE INDEX IF NOT EXISTS idx_roamfile_agreement ON roaming_cdr_file (agreement_id);
CREATE SEQUENCE IF NOT EXISTS roaming_cdr_file_id_seq;

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
