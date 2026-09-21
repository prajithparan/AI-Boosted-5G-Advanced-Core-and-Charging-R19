-- ============================================================================
-- PARTY ABE -- faithful to TMF632 Party Management v4.0.0 (as modeled in bss_sid/party.hpp).
-- Individual + Organization, every scalar a column, every array a normalized child table.
-- Extendable: party_characteristic (SID Characteristic) is the name/value extension point.
-- ============================================================================
SET search_path TO party;

-- ---- Individual (TMF632 Individual): all real scalar fields ----
CREATE TABLE individual (
    id                   TEXT PRIMARY KEY,
    href                 TEXT,
    aristocratic_title   TEXT,
    birth_date           DATE,
    country_of_birth     TEXT,
    death_date           DATE,
    family_name          TEXT,
    family_name_prefix   TEXT,
    formatted_name       TEXT,
    full_name            TEXT,
    gender               TEXT,
    generation           TEXT,
    given_name           TEXT,
    legal_name           TEXT,
    location             TEXT,
    marital_status       TEXT,
    middle_name          TEXT,
    nationality          TEXT,
    place_of_birth       TEXT,
    preferred_given_name TEXT,
    title                TEXT,
    status               TEXT,                       -- IndividualStateType: initialized|validated|deceased
    created_at           TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at           TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_individual_family_name ON individual (family_name);
CREATE INDEX idx_individual_full_name   ON individual (full_name);
CREATE INDEX idx_individual_status      ON individual (status);

-- ---- Organization (TMF632 Organization): scalars + single parent relationship ----
CREATE TABLE organization (
    id                        TEXT PRIMARY KEY,
    href                      TEXT,
    is_head_office            BOOLEAN,
    is_legal_entity           BOOLEAN,
    name                      TEXT,
    name_type                 TEXT,
    organization_type         TEXT,
    trading_name              TEXT,
    exists_during_start       TIMESTAMPTZ,
    exists_during_end         TIMESTAMPTZ,
    status                    TEXT,                  -- OrganizationStateType
    -- organizationParentRelationship is a SINGLE ref (TMF632 quirk) -> inline columns
    parent_relationship_type  TEXT,
    parent_organization_id    TEXT REFERENCES organization(id),
    parent_organization_name  TEXT,
    created_at                TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at                TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_org_name   ON organization (name);
CREATE INDEX idx_org_parent ON organization (parent_organization_id);

-- ---- ContactMedium + MediumCharacteristic (flattened: 1 characteristic per medium) ----
CREATE TABLE contact_medium (
    id                 TEXT PRIMARY KEY,
    individual_id      TEXT REFERENCES individual(id)   ON DELETE CASCADE,
    organization_id    TEXT REFERENCES organization(id) ON DELETE CASCADE,
    medium_type        TEXT,                            -- email|phone|postalAddress|...
    preferred          BOOLEAN,
    valid_for_start    TIMESTAMPTZ,
    valid_for_end      TIMESTAMPTZ,
    -- MediumCharacteristic:
    city               TEXT,
    contact_type       TEXT,
    country            TEXT,
    email_address      TEXT,
    fax_number         TEXT,
    phone_number       TEXT,
    post_code          TEXT,
    social_network_id  TEXT,
    state_or_province  TEXT,
    street1            TEXT,
    street2            TEXT,
    CHECK (individual_id IS NOT NULL OR organization_id IS NOT NULL)
);
CREATE INDEX idx_contact_individual ON contact_medium (individual_id);
CREATE INDEX idx_contact_org        ON contact_medium (organization_id);
-- digital-channel login/lookup + duplicate-identity guard (advisor):
CREATE INDEX idx_contact_email ON contact_medium (email_address) WHERE email_address IS NOT NULL;
CREATE INDEX idx_contact_phone ON contact_medium (phone_number)  WHERE phone_number  IS NOT NULL;

-- ---- IndividualIdentification (SUPI lives here: identification_type='SUPI') ----
CREATE TABLE individual_identification (
    id                  TEXT PRIMARY KEY,
    individual_id       TEXT NOT NULL REFERENCES individual(id) ON DELETE CASCADE,
    identification_type TEXT,
    identification_id   TEXT,
    issuing_authority   TEXT,
    issuing_date        TIMESTAMPTZ,
    attachment          JSONB,                          -- AttachmentRefOrValue (opaque)
    valid_for_start     TIMESTAMPTZ,
    valid_for_end       TIMESTAMPTZ
);
CREATE INDEX idx_indid_individual ON individual_identification (individual_id);
-- SUPI / external-id lookup path (per CHARGING_MAPPING SUPI->IndividualIdentification):
CREATE INDEX idx_indid_type_value ON individual_identification (identification_type, identification_id);

CREATE TABLE organization_identification (
    id                  TEXT PRIMARY KEY,
    organization_id     TEXT NOT NULL REFERENCES organization(id) ON DELETE CASCADE,
    identification_type TEXT, identification_id TEXT, issuing_authority TEXT,
    issuing_date TIMESTAMPTZ, attachment JSONB, valid_for_start TIMESTAMPTZ, valid_for_end TIMESTAMPTZ
);
CREATE INDEX idx_orgid_org ON organization_identification (organization_id);

-- ---- Credit profile (both party types) ----
CREATE TABLE credit_profile (
    id                 TEXT PRIMARY KEY,
    individual_id      TEXT REFERENCES individual(id)   ON DELETE CASCADE,
    organization_id    TEXT REFERENCES organization(id) ON DELETE CASCADE,
    credit_agency_name TEXT, credit_agency_type TEXT, rating_reference TEXT, rating_score INTEGER,
    valid_for_start TIMESTAMPTZ, valid_for_end TIMESTAMPTZ,
    CHECK (individual_id IS NOT NULL OR organization_id IS NOT NULL)
);
CREATE INDEX idx_credit_individual ON credit_profile (individual_id);
CREATE INDEX idx_credit_org        ON credit_profile (organization_id);

-- ---- External reference (both) ----
CREATE TABLE external_reference (
    id               TEXT PRIMARY KEY,
    individual_id    TEXT REFERENCES individual(id)   ON DELETE CASCADE,
    organization_id  TEXT REFERENCES organization(id) ON DELETE CASCADE,
    external_reference_type TEXT, name TEXT,
    CHECK (individual_id IS NOT NULL OR organization_id IS NOT NULL)
);
CREATE INDEX idx_extref_individual ON external_reference (individual_id);
CREATE INDEX idx_extref_org        ON external_reference (organization_id);

-- ---- Disability, LanguageAbility, Skill (Individual only) ----
CREATE TABLE disability (
    id TEXT PRIMARY KEY,
    individual_id TEXT NOT NULL REFERENCES individual(id) ON DELETE CASCADE,
    disability_code TEXT, disability_name TEXT, valid_for_start TIMESTAMPTZ, valid_for_end TIMESTAMPTZ
);
CREATE INDEX idx_disability_individual ON disability (individual_id);

CREATE TABLE language_ability (
    id TEXT PRIMARY KEY,
    individual_id TEXT NOT NULL REFERENCES individual(id) ON DELETE CASCADE,
    is_favourite_language BOOLEAN, language_code TEXT, language_name TEXT,
    listening_proficiency TEXT, reading_proficiency TEXT, speaking_proficiency TEXT,
    writing_proficiency TEXT, valid_for_start TIMESTAMPTZ, valid_for_end TIMESTAMPTZ
);
CREATE INDEX idx_langability_individual ON language_ability (individual_id);

CREATE TABLE skill (
    id TEXT PRIMARY KEY,
    individual_id TEXT NOT NULL REFERENCES individual(id) ON DELETE CASCADE,
    comment TEXT, evaluated_level TEXT, skill_code TEXT, skill_name TEXT,
    valid_for_start TIMESTAMPTZ, valid_for_end TIMESTAMPTZ
);
CREATE INDEX idx_skill_individual ON skill (individual_id);

-- ---- OtherName (per party type) ----
CREATE TABLE other_name_individual (
    id TEXT PRIMARY KEY,
    individual_id TEXT NOT NULL REFERENCES individual(id) ON DELETE CASCADE,
    aristocratic_title TEXT, family_name TEXT, family_name_prefix TEXT, formatted_name TEXT,
    full_name TEXT, generation TEXT, given_name TEXT, legal_name TEXT, middle_name TEXT,
    preferred_given_name TEXT, title TEXT, valid_for_start TIMESTAMPTZ, valid_for_end TIMESTAMPTZ
);
CREATE INDEX idx_othername_ind ON other_name_individual (individual_id);

CREATE TABLE other_name_organization (
    id TEXT PRIMARY KEY,
    organization_id TEXT NOT NULL REFERENCES organization(id) ON DELETE CASCADE,
    name TEXT, name_type TEXT, trading_name TEXT, valid_for_start TIMESTAMPTZ, valid_for_end TIMESTAMPTZ
);
CREATE INDEX idx_othername_org ON other_name_organization (organization_id);

-- ---- party_characteristic (SID Characteristic) -- THE extension point (name/value) ----
CREATE TABLE party_characteristic (
    id              TEXT PRIMARY KEY,
    individual_id   TEXT REFERENCES individual(id)   ON DELETE CASCADE,
    organization_id TEXT REFERENCES organization(id) ON DELETE CASCADE,
    name            TEXT NOT NULL,
    value_type      TEXT,
    value           JSONB NOT NULL,                    -- generic value (SID extensibility)
    CHECK (individual_id IS NOT NULL OR organization_id IS NOT NULL)
);
CREATE INDEX idx_partychar_ind  ON party_characteristic (individual_id);
CREATE INDEX idx_partychar_org  ON party_characteristic (organization_id);
CREATE INDEX idx_partychar_name ON party_characteristic (name);

-- ---- RelatedParty (both) ----
CREATE TABLE related_party (
    id              TEXT PRIMARY KEY,
    individual_id   TEXT REFERENCES individual(id)   ON DELETE CASCADE,
    organization_id TEXT REFERENCES organization(id) ON DELETE CASCADE,
    related_party_id TEXT, role TEXT, name TEXT, referred_type TEXT,
    CHECK (individual_id IS NOT NULL OR organization_id IS NOT NULL)
);
CREATE INDEX idx_relparty_ind ON related_party (individual_id);
CREATE INDEX idx_relparty_org ON related_party (organization_id);

-- ---- TaxExemptionCertificate + TaxDefinition (child of the certificate) ----
CREATE TABLE tax_exemption_certificate (
    id              TEXT PRIMARY KEY,
    individual_id   TEXT REFERENCES individual(id)   ON DELETE CASCADE,
    organization_id TEXT REFERENCES organization(id) ON DELETE CASCADE,
    attachment      JSONB, valid_for_start TIMESTAMPTZ, valid_for_end TIMESTAMPTZ,
    CHECK (individual_id IS NOT NULL OR organization_id IS NOT NULL)
);
CREATE INDEX idx_taxcert_ind ON tax_exemption_certificate (individual_id);
CREATE INDEX idx_taxcert_org ON tax_exemption_certificate (organization_id);

CREATE TABLE tax_definition (
    id             TEXT PRIMARY KEY,
    certificate_id TEXT NOT NULL REFERENCES tax_exemption_certificate(id) ON DELETE CASCADE,
    name TEXT, tax_type TEXT
);
CREATE INDEX idx_taxdef_cert ON tax_definition (certificate_id);

-- ---- Organization hierarchy children (array; parent is inline on organization) ----
CREATE TABLE organization_child_relationship (
    id                TEXT PRIMARY KEY,
    organization_id   TEXT NOT NULL REFERENCES organization(id) ON DELETE CASCADE,
    relationship_type TEXT,
    child_organization_id TEXT, child_organization_name TEXT
);
CREATE INDEX idx_orgchild_org ON organization_child_relationship (organization_id);
