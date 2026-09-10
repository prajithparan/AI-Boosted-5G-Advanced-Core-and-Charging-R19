-- bss/subscriber-management PostgreSQL schema.
--
-- P4.1/P4.7's E1 (Subscriber Management) and E10 (Master Model -> Consumer|Enterprise) persistence,
-- per docs/DATA_MODEL.md's own schema sketches. Real, disclosed scoping decision (2026-08-11,
-- docs/DECISIONS.md ADR-0060): this turn built the schema and a real PostgreSQL-backed store
-- library (src/store.hpp/.cpp), proven with real live verification -- same rigor as E2/E6 -- and
-- deliberately deferred the HTTP/REST service (main.cpp) to P4.7 itself.
--
-- UPDATE (P4.7, docs/DECISIONS.md ADR-0066): that deferred service now exists (src/main.cpp) --
-- real TMF632 Individual/Organization Create/Get/List over `/tmf-api/party/v4/`, plus
-- project-internal Account/Subscriber Create/Get/List over `/bss-api/subscriberManagement/v1/`.
-- See src/main.cpp's own header for the full real disclosure.
--
-- `subscriber`/`account` themselves are project-internal linking tables (same "not itself a SID
-- business entity" category as E3's ChargingSession) -- DATA_MODEL.md's own E1/E10 sections say so
-- explicitly ("Schema sketch (proposed for this project -- not itself a spec-mandated shape; the
-- identifiers and identity fields inside it are grounded in real sources as cited)"). The REAL TM
-- Forum resources these tables reference are `party_individual` (TMF632 Individual, full real
-- field set per libs/bss-sid/include/bss_sid/party.hpp) and `party_organization` (TMF632
-- Organization, same file) -- both genuinely new, real, persisted TMF632 resources this project
-- has never stored before this turn.

CREATE SEQUENCE IF NOT EXISTS party_individual_id_seq;
CREATE SEQUENCE IF NOT EXISTS party_organization_id_seq;
CREATE SEQUENCE IF NOT EXISTS account_id_seq;
CREATE SEQUENCE IF NOT EXISTS subscriber_id_seq;

-- Real TMF632 Individual. Header scalar fields become real columns (matching this project's own
-- established E2/E6 convention); every array/nested field (contactMedium, individualIdentification,
-- partyCharacteristic, relatedParty, ...) is a single `jsonb` column, since none of them are
-- independently queried by this project's own real use case yet (unlike E2's prodSpecCharValueUse,
-- which the rating engine actually reads).
CREATE TABLE IF NOT EXISTS party_individual (
    id                          TEXT PRIMARY KEY,
    href                        TEXT,
    aristocratic_title          TEXT,
    birth_date                  TEXT,
    country_of_birth            TEXT,
    death_date                  TEXT,
    family_name                 TEXT,
    family_name_prefix          TEXT,
    formatted_name               TEXT,
    full_name                   TEXT,
    gender                      TEXT,
    generation                  TEXT,
    given_name                  TEXT,
    legal_name                  TEXT,
    location                    TEXT,
    marital_status               TEXT,
    middle_name                  TEXT,
    nationality                  TEXT,
    place_of_birth               TEXT,
    preferred_given_name          TEXT,
    title                       TEXT,
    contact_medium               JSONB NOT NULL DEFAULT '[]',
    credit_rating                 JSONB NOT NULL DEFAULT '[]',
    disability                    JSONB NOT NULL DEFAULT '[]',
    external_reference            JSONB NOT NULL DEFAULT '[]',
    individual_identification      JSONB NOT NULL DEFAULT '[]',
    language_ability               JSONB NOT NULL DEFAULT '[]',
    other_name                    JSONB NOT NULL DEFAULT '[]',
    party_characteristic           JSONB NOT NULL DEFAULT '[]',
    related_party                  JSONB NOT NULL DEFAULT '[]',
    skill                        JSONB NOT NULL DEFAULT '[]',
    status                      TEXT,
    tax_exemption_certificate      JSONB NOT NULL DEFAULT '[]',
    created_at                   TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at                   TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Real TMF632 Organization -- the ENTERPRISE account hierarchy's real mechanism (docs/DATA_MODEL.md
-- E10's own resolution: organizationParentRelationship/organizationChildRelationship, confirmed
-- against the real swagger, not the earlier unconfirmed partyRelationship guess).
CREATE TABLE IF NOT EXISTS party_organization (
    id                                  TEXT PRIMARY KEY,
    href                                TEXT,
    is_head_office                      BOOLEAN,
    is_legal_entity                     BOOLEAN,
    name                                TEXT,
    name_type                           TEXT,
    organization_type                   TEXT,
    trading_name                        TEXT,
    contact_medium                      JSONB NOT NULL DEFAULT '[]',
    credit_rating                        JSONB NOT NULL DEFAULT '[]',
    exists_during_from                   TEXT,
    exists_during_to                     TEXT,
    external_reference                   JSONB NOT NULL DEFAULT '[]',
    organization_child_relationship        JSONB NOT NULL DEFAULT '[]',
    organization_identification            JSONB NOT NULL DEFAULT '[]',
    organization_parent_relationship        JSONB,
    other_name                           JSONB NOT NULL DEFAULT '[]',
    party_characteristic                  JSONB NOT NULL DEFAULT '[]',
    related_party                         JSONB NOT NULL DEFAULT '[]',
    status                              TEXT,
    tax_exemption_certificate              JSONB NOT NULL DEFAULT '[]',
    created_at                          TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at                          TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- E10 Account (MASTER -- CONSUMER and ENTERPRISE are both instances of this, not separate tables).
-- Project-internal, per docs/DATA_MODEL.md's own disclosure -- the real TM Forum resource for the
-- ENTERPRISE branch specifically is party_organization above (linked via organization_id when
-- account_kind = 'ENTERPRISE'); for CONSUMER, organization_id is always NULL.
CREATE TABLE IF NOT EXISTS account (
    id                     TEXT PRIMARY KEY,
    account_kind           TEXT NOT NULL,                 -- CONSUMER | ENTERPRISE
    parent_account_id      TEXT REFERENCES account(id),   -- self-referential, arbitrary depth
    organization_id        TEXT REFERENCES party_organization(id), -- ENTERPRISE only
    billing_mode           TEXT,                           -- INDIVIDUAL | SPLIT
    cost_center             TEXT,
    contract_sla_id          TEXT,                          -- FK -> ProductOffering.serviceLevelAgreement,
                                                            -- not a DB-level FK (SLARef lives inside
                                                            -- product_offering's own jsonb, E2) --
                                                            -- see store.hpp's own comment
    provisioning_mode        TEXT,                           -- INDIVIDUAL | BULK
    created_at              TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- E1 Subscriber. Project-internal linking table between a real SUPI (TS 23.501, already the one
-- field wired end-to-end in this codebase, ADR-0045) and its real TMF632 Individual record.
CREATE TABLE IF NOT EXISTS subscriber (
    id                     TEXT PRIMARY KEY,
    supi                   TEXT NOT NULL UNIQUE,
    individual_id           TEXT REFERENCES party_individual(id),
    msisdn                  TEXT,
    account_id              TEXT REFERENCES account(id),
    charging_mode            TEXT,                          -- PREPAID | POSTPAID
    bill_cycle_day            INTEGER,                        -- 1-28
    service_preferences       JSONB NOT NULL DEFAULT '{}',
    created_at               TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at               TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- ADR-0335: subscription lifecycle, because churn is currently unobservable.
--
-- The `subscriber` table above records `created_at` and `updated_at` and nothing else about a
-- subscriber's life. A subscriber who leaves is either still present or deleted, and in neither
-- case is there a record that they churned, when, or why. That makes churn-propensity modelling
-- impossible -- not hard, impossible: there is no label to train against.
--
-- This is therefore NOT a model and NOT an agent. It is the data-collection prerequisite that has
-- to run for a while before either is possible, which is exactly why it is being started now
-- rather than discovered later when someone asks for churn prediction and finds a year of history
-- that was never recorded.

ALTER TABLE subscriber ADD COLUMN IF NOT EXISTS status TEXT NOT NULL DEFAULT 'active';

CREATE TABLE IF NOT EXISTS subscriber_lifecycle_event (
    id             BIGSERIAL PRIMARY KEY,
    subscriber_id  TEXT NOT NULL REFERENCES subscriber(id),
    -- Both sides of the transition. Knowing a subscriber is 'terminated' is far less useful than
    -- knowing they went suspended -> terminated rather than active -> terminated: the first is a
    -- collections outcome, the second is a customer walking away, and a churn model that cannot
    -- tell them apart learns the wrong thing.
    from_status    TEXT,
    to_status      TEXT NOT NULL,
    -- Free text on purpose. No specification enumerates churn reasons and inventing a closed set
    -- here would force every future reason into a category chosen before anyone had seen the data.
    reason         TEXT,
    occurred_at    TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS subscriber_lifecycle_subscriber_idx
    ON subscriber_lifecycle_event (subscriber_id, occurred_at DESC);
-- The query a churn trainer actually runs: "who terminated, and when".
CREATE INDEX IF NOT EXISTS subscriber_lifecycle_to_status_idx
    ON subscriber_lifecycle_event (to_status, occurred_at DESC);
