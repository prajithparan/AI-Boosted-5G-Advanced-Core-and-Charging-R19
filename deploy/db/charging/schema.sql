-- =====================================================================================
-- CHARGING / BSS domain database -- consolidated, normalized (3NF), Tier-1 scale.
-- =====================================================================================
-- ONE domain DB, schema per NF (project_db_per_domain_rule). Normalized for ultra performance
-- (project_tier1_scale_architecture, 10M customers / 300M CDR-day): every queryable/relational
-- attribute is a real indexed column with proper FKs; multi-valued *queried* attributes are child
-- tables (contact_medium); only rarely-queried TMF sub-arrays stay JSONB. Master entities are
-- indexed btree (NOT hash-partitioned -- 10M rows is unremarkable for indexed Postgres); only the
-- time-series tables (rating_decision, balance transactions) are RANGE-partitioned by date.
--
-- SID PROVENANCE (#1 rule): attributes anchored on the TM Forum Open API resources vendored/used in
-- this repo (docs/CHARGING_MAPPING.md) -- Party TMF632/629, Customer TMF629, BillingAccount,
-- Product TMF620/637, Service TMF633/638, Resource TMF639, Balance TMF654. The GB922 SID model
-- document is NOT held offline, so SID ABE attributes are realized via those TMF resources; any SID
-- attribute with no TMF-resource home is FLAGGED, never invented. Rarely-queried nested TMF
-- structures are JSONB (ADR-0053 discipline).
-- =====================================================================================

CREATE SCHEMA IF NOT EXISTS party;
CREATE SCHEMA IF NOT EXISTS product_catalog;
CREATE SCHEMA IF NOT EXISTS subscriber_mgmt;
CREATE SCHEMA IF NOT EXISTS balance_mgmt;
CREATE SCHEMA IF NOT EXISTS chf_rating;
CREATE SCHEMA IF NOT EXISTS roaming;

-- ============================ PARTY ABE (TMF632/629) ============================
CREATE TABLE party.party_individual (
    id                 TEXT PRIMARY KEY,
    href               TEXT,
    given_name         TEXT,
    family_name        TEXT,
    full_name          TEXT,
    formatted_name     TEXT,
    gender             TEXT,
    birth_date         DATE,
    marital_status     TEXT,
    nationality        TEXT,
    status             TEXT NOT NULL DEFAULT 'initialized',   -- TMF PartyStatusType
    -- rarely-queried TMF sub-arrays (retrieved with the resource, not filtered): kept JSONB
    ext               JSONB NOT NULL DEFAULT '{}',            -- individualIdentification, credit_rating, disability, skill, taxExemption, languageAbility, externalReference, partyCharacteristic
    created_at         TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at         TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_individual_family_name ON party.party_individual (family_name);
CREATE INDEX idx_individual_status      ON party.party_individual (status);

CREATE TABLE party.party_organization (
    id                 TEXT PRIMARY KEY,
    href               TEXT,
    name               TEXT,
    trading_name       TEXT,
    is_head_office     BOOLEAN,
    organization_type  TEXT,
    status             TEXT NOT NULL DEFAULT 'initialized',
    ext                JSONB NOT NULL DEFAULT '{}',
    created_at         TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at         TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_org_name ON party.party_organization (name);

-- ContactMedium normalized (TMF632 ContactMedium) -- digital-channel login/lookup by phone/email.
CREATE TABLE party.contact_medium (
    id                 TEXT PRIMARY KEY,
    individual_id      TEXT REFERENCES party.party_individual(id) ON DELETE CASCADE,
    organization_id    TEXT REFERENCES party.party_organization(id) ON DELETE CASCADE,
    medium_type        TEXT NOT NULL,                          -- email|phone|sms|postal
    contact_value      TEXT NOT NULL,                          -- normalized email / E.164 phone
    is_preferred       BOOLEAN NOT NULL DEFAULT false,
    valid_from         TIMESTAMPTZ,
    valid_to           TIMESTAMPTZ,
    CHECK (individual_id IS NOT NULL OR organization_id IS NOT NULL)
);
-- one active identity per (type,value): stops duplicate-identity at 10M scale (advisor)
CREATE UNIQUE INDEX uq_contact_type_value ON party.contact_medium (medium_type, contact_value)
    WHERE valid_to IS NULL;
CREATE INDEX idx_contact_individual ON party.contact_medium (individual_id);
CREATE INDEX idx_contact_org        ON party.contact_medium (organization_id);

-- ============================ CUSTOMER ABE (TMF629) ============================
CREATE TABLE party.customer (
    id                 TEXT PRIMARY KEY,
    href               TEXT,
    name               TEXT,
    engaged_individual_id   TEXT REFERENCES party.party_individual(id),
    engaged_organization_id TEXT REFERENCES party.party_organization(id),
    status             TEXT NOT NULL DEFAULT 'active',
    created_at         TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at         TIMESTAMPTZ NOT NULL DEFAULT now(),
    CHECK (engaged_individual_id IS NOT NULL OR engaged_organization_id IS NOT NULL)
);
CREATE INDEX idx_customer_individual ON party.customer (engaged_individual_id);
CREATE INDEX idx_customer_org        ON party.customer (engaged_organization_id);

-- ============================ BILLING ACCOUNT ABE ============================
CREATE TABLE party.account (
    id                 TEXT PRIMARY KEY,
    customer_id        TEXT NOT NULL REFERENCES party.customer(id),
    account_kind       TEXT NOT NULL,                          -- CONSUMER|ENTERPRISE
    parent_account_id  TEXT REFERENCES party.account(id),      -- enterprise hierarchy
    organization_id    TEXT REFERENCES party.party_organization(id),
    billing_mode       TEXT,                                   -- INDIVIDUAL|SPLIT
    cost_center        TEXT,
    contract_sla_id    TEXT,
    provisioning_mode  TEXT,                                   -- INDIVIDUAL|BULK
    status             TEXT NOT NULL DEFAULT 'active',
    created_at         TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_account_customer ON party.account (customer_id);
CREATE INDEX idx_account_parent   ON party.account (parent_account_id);
CREATE INDEX idx_account_kind     ON party.account (account_kind);

-- ============================ PRODUCT ABE (TMF620/637) ============================
CREATE TABLE product_catalog.product_specification (
    id                 TEXT PRIMARY KEY,
    href               TEXT,
    name               TEXT,
    lifecycle_status   TEXT,
    ext                JSONB NOT NULL DEFAULT '{}',            -- characteristics, relationships (read with the resource)
    created_at         TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at         TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_prodspec_name ON product_catalog.product_specification (name);

CREATE TABLE product_catalog.product_offering (
    id                 TEXT PRIMARY KEY,
    href               TEXT,
    name               TEXT NOT NULL,
    lifecycle_status   TEXT,
    is_bundle          BOOLEAN NOT NULL DEFAULT false,
    product_specification_id TEXT REFERENCES product_catalog.product_specification(id),
    segment            TEXT,                                   -- CONSUMER|ENTERPRISE applicability
    ext                JSONB NOT NULL DEFAULT '{}',
    created_at         TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at         TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_offering_name   ON product_catalog.product_offering (name);
CREATE INDEX idx_offering_status ON product_catalog.product_offering (lifecycle_status);
CREATE INDEX idx_offering_spec   ON product_catalog.product_offering (product_specification_id);

CREATE TABLE product_catalog.product_offering_price (
    id                 TEXT PRIMARY KEY,
    href               TEXT,
    name               TEXT,
    product_offering_id TEXT REFERENCES product_catalog.product_offering(id),
    price_type         TEXT,                                   -- recurring|oneTime|usage
    amount             NUMERIC(18,4),
    currency           TEXT,
    unit_of_measure    TEXT,
    ext                JSONB NOT NULL DEFAULT '{}',
    created_at         TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_price_offering ON product_catalog.product_offering_price (product_offering_id);

-- ============================ SERVICE ABE (the line/CFS, TMF633/638) ============================
CREATE TABLE subscriber_mgmt.subscriber (
    id                 TEXT PRIMARY KEY,
    account_id         TEXT NOT NULL REFERENCES party.account(id),
    individual_id      TEXT REFERENCES party.party_individual(id),
    service_spec       TEXT,                                   -- CFS spec, e.g. 'mobile-line'
    charging_mode      TEXT NOT NULL,                          -- PREPAID|POSTPAID
    bill_cycle_day     INTEGER CHECK (bill_cycle_day BETWEEN 1 AND 28),
    status             TEXT NOT NULL DEFAULT 'pendingActive',  -- pendingActive|active|suspended|terminated
    service_preferences JSONB NOT NULL DEFAULT '{}',
    created_at         TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at         TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_subscriber_account ON subscriber_mgmt.subscriber (account_id);
CREATE INDEX idx_subscriber_status  ON subscriber_mgmt.subscriber (status);

-- ============================ RESOURCE ABE (SUPI/IMSI/MSISDN/SIM, TMF639) ============================
-- Own table (not columns on subscriber): MSISDN/SIM change over time; digital-channel "find by
-- MSISDN/SUPI" goes through here. Index (type,value); one ACTIVE value per type.
CREATE TABLE subscriber_mgmt.resource (
    id                 TEXT PRIMARY KEY,
    subscriber_id      TEXT NOT NULL REFERENCES subscriber_mgmt.subscriber(id),
    resource_type      TEXT NOT NULL,                          -- SUPI|IMSI|MSISDN|SIM_ICCID
    resource_value     TEXT NOT NULL,
    status             TEXT NOT NULL DEFAULT 'active',         -- active|released
    valid_from         TIMESTAMPTZ NOT NULL DEFAULT now(),
    valid_to           TIMESTAMPTZ,
    ext                JSONB NOT NULL DEFAULT '{}'
);
CREATE INDEX idx_resource_subscriber ON subscriber_mgmt.resource (subscriber_id);
CREATE INDEX idx_resource_lookup     ON subscriber_mgmt.resource (resource_type, resource_value);
CREATE UNIQUE INDEX uq_resource_active ON subscriber_mgmt.resource (resource_type, resource_value)
    WHERE status = 'active';

-- The purchased product per subscriber (Product inventory, TMF637).
CREATE TABLE subscriber_mgmt.product_subscription (
    id                 TEXT PRIMARY KEY,
    subscriber_id      TEXT NOT NULL REFERENCES subscriber_mgmt.subscriber(id),
    account_id         TEXT NOT NULL REFERENCES party.account(id),
    product_offering_id TEXT NOT NULL REFERENCES product_catalog.product_offering(id),
    status             TEXT NOT NULL DEFAULT 'active',
    start_date         TIMESTAMPTZ NOT NULL DEFAULT now(),
    end_date           TIMESTAMPTZ,
    characteristics    JSONB NOT NULL DEFAULT '{}',            -- S-NSSAI, DNN, AMBR, rating group...
    created_at         TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_prodsub_subscriber ON subscriber_mgmt.product_subscription (subscriber_id);
CREATE INDEX idx_prodsub_account    ON subscriber_mgmt.product_subscription (account_id);
CREATE INDEX idx_prodsub_offering   ON subscriber_mgmt.product_subscription (product_offering_id);

-- ============================ BALANCE ABE (TMF654) ============================
CREATE TABLE balance_mgmt.bucket (
    id                 TEXT PRIMARY KEY,
    subscriber_id      TEXT REFERENCES subscriber_mgmt.subscriber(id),
    account_id         TEXT REFERENCES party.account(id),      -- shared/pooled buckets sit at the account
    bucket_type        TEXT NOT NULL,                          -- MONETARY|DATA|VOICE|SMS
    unit_of_measure    TEXT,
    amount             NUMERIC(20,4) NOT NULL DEFAULT 0,
    reserved           NUMERIC(20,4) NOT NULL DEFAULT 0,
    is_shared          BOOLEAN NOT NULL DEFAULT false,
    status             TEXT NOT NULL DEFAULT 'active',
    valid_from         TIMESTAMPTZ,
    valid_to           TIMESTAMPTZ,
    created_at         TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at         TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_bucket_subscriber ON balance_mgmt.bucket (subscriber_id);
CREATE INDEX idx_bucket_account    ON balance_mgmt.bucket (account_id);
CREATE INDEX idx_bucket_shared     ON balance_mgmt.bucket (is_shared) WHERE is_shared;

-- Balance transactions = time-series -> RANGE-partitioned by month (heavy at Tier-1 scale).
CREATE TABLE balance_mgmt.balance_transaction (
    id                 TEXT NOT NULL,
    bucket_id          TEXT NOT NULL,
    txn_type           TEXT NOT NULL,                          -- TOPUP|ADJUST|RESERVE|DEBIT|RELEASE
    amount             NUMERIC(20,4) NOT NULL,
    charging_data_ref  TEXT,                                   -- ties a reserve/debit to the CDR session
    occurred_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    ext                JSONB NOT NULL DEFAULT '{}',
    PRIMARY KEY (occurred_at, id)
) PARTITION BY RANGE (occurred_at);
CREATE TABLE balance_mgmt.balance_transaction_default PARTITION OF balance_mgmt.balance_transaction DEFAULT;
CREATE INDEX idx_baltxn_bucket ON balance_mgmt.balance_transaction (bucket_id, occurred_at);

-- ============================ CHF RATING (time-series) ============================
CREATE TABLE chf_rating.rating_decision (
    id                 TEXT NOT NULL,
    charging_data_ref  TEXT NOT NULL,
    subscriber_identifier TEXT NOT NULL,                       -- SUPI, for per-customer inquiry
    rating_group       BIGINT,
    decided_at         TIMESTAMPTZ NOT NULL DEFAULT now(),
    rated_units        BIGINT,
    monetary_amount    NUMERIC(18,4),
    currency           TEXT,
    input_snapshot     JSONB NOT NULL DEFAULT '{}',
    PRIMARY KEY (decided_at, id)
) PARTITION BY RANGE (decided_at);
CREATE TABLE chf_rating.rating_decision_default PARTITION OF chf_rating.rating_decision DEFAULT;
CREATE INDEX idx_rating_subscriber ON chf_rating.rating_decision (subscriber_identifier, decided_at);
CREATE INDEX idx_rating_ref        ON chf_rating.rating_decision (charging_data_ref);

CREATE TABLE chf_rating.audit_record (
    id                 BIGSERIAL PRIMARY KEY,
    at                 TIMESTAMPTZ NOT NULL DEFAULT now(),
    actor              TEXT,
    action             TEXT,
    detail             JSONB NOT NULL DEFAULT '{}'
);

-- ============================ ROAMING (read-by-id, move-under-schema) ============================
CREATE TABLE roaming.interconnect_agreement (
    id                 TEXT PRIMARY KEY,
    partner_plmn       TEXT,
    status             TEXT,
    ext                JSONB NOT NULL DEFAULT '{}',
    created_at         TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_agreement_partner ON roaming.interconnect_agreement (partner_plmn);

CREATE TABLE roaming.roaming_cdr_file (
    id                 TEXT PRIMARY KEY,
    file_name          TEXT,
    status             TEXT,
    ext                JSONB NOT NULL DEFAULT '{}',
    created_at         TIMESTAMPTZ NOT NULL DEFAULT now()
);
