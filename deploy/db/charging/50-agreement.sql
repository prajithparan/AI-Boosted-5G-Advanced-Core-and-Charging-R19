-- ============================================================================
-- AGREEMENT ABE (TMF651, bss_sid/agreement.hpp) + Roaming/Interconnect.
-- Customer/enterprise agreements (contracts, SLAs) under subscriber_mgmt; interconnect agreements
-- + roaming CDR files under roaming (the roaming-interconnect NF scope).
-- ============================================================================
SET search_path TO subscriber_mgmt;

CREATE TABLE agreement (
    id                   TEXT PRIMARY KEY,
    href                 TEXT,
    agreement_type       TEXT,
    description          TEXT,
    document_number      INTEGER,
    initial_date         TIMESTAMPTZ,
    name                 TEXT,
    statement_of_intent  TEXT,
    status               TEXT,
    version              TEXT,
    agreement_period_start TIMESTAMPTZ, agreement_period_end TIMESTAMPTZ,
    completion_date      TIMESTAMPTZ,
    agreement_specification_id TEXT, agreement_specification_name TEXT,
    account_id           TEXT REFERENCES account(id),      -- the customer/account this contract binds
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(), updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_agreement_account ON agreement (account_id);
CREATE INDEX idx_agreement_status  ON agreement (status);

CREATE TABLE agreement_item (
    id             TEXT PRIMARY KEY,
    agreement_id   TEXT NOT NULL REFERENCES agreement(id) ON DELETE CASCADE,
    product_id     TEXT, product_offering_id TEXT
);
CREATE INDEX idx_agritem_agreement ON agreement_item (agreement_id);

CREATE TABLE agreement_term_or_condition (
    id             TEXT PRIMARY KEY,
    agreement_id   TEXT NOT NULL REFERENCES agreement(id) ON DELETE CASCADE,
    description    TEXT, valid_for_start TIMESTAMPTZ, valid_for_end TIMESTAMPTZ
);
CREATE INDEX idx_agrterm_agreement ON agreement_term_or_condition (agreement_id);

CREATE TABLE agreement_authorization (
    id             BIGSERIAL PRIMARY KEY,
    agreement_id   TEXT NOT NULL REFERENCES agreement(id) ON DELETE CASCADE,
    auth_date      TIMESTAMPTZ, signature_representation TEXT, state TEXT
);
CREATE INDEX idx_agrauth_agreement ON agreement_authorization (agreement_id);

CREATE TABLE agreement_characteristic (
    id             BIGSERIAL PRIMARY KEY,
    agreement_id   TEXT NOT NULL REFERENCES agreement(id) ON DELETE CASCADE,
    name TEXT NOT NULL, value_type TEXT, value JSONB
);
CREATE INDEX idx_agrchar_agreement ON agreement_characteristic (agreement_id);

CREATE TABLE agreement_engaged_party (
    id             BIGSERIAL PRIMARY KEY,
    agreement_id   TEXT NOT NULL REFERENCES agreement(id) ON DELETE CASCADE,
    party_id TEXT NOT NULL, href TEXT, name TEXT, role TEXT
);
CREATE INDEX idx_agrparty_agreement ON agreement_engaged_party (agreement_id);

-- ---- Roaming / Interconnect (roaming-interconnect NF) ----
SET search_path TO roaming;
CREATE TABLE interconnect_agreement (
    id             TEXT PRIMARY KEY,
    partner_plmn   TEXT,
    partner_name   TEXT,
    agreement_ref  TEXT,
    status         TEXT,
    valid_for_start TIMESTAMPTZ, valid_for_end TIMESTAMPTZ,
    terms          JSONB NOT NULL DEFAULT '{}',
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_icagreement_partner ON interconnect_agreement (partner_plmn);
CREATE INDEX idx_icagreement_status  ON interconnect_agreement (status);

CREATE TABLE roaming_cdr_file (
    id             TEXT PRIMARY KEY,
    file_name      TEXT,
    direction      TEXT,                                 -- inbound|outbound (TAP3)
    partner_plmn   TEXT,
    status         TEXT,
    record_count   BIGINT,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_roamfile_partner ON roaming_cdr_file (partner_plmn);
CREATE INDEX idx_roamfile_status  ON roaming_cdr_file (status);
