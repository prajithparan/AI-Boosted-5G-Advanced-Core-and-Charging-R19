-- ============================================================================
-- PRODUCT ABE -- faithful to TMF620 Product Catalog v4 (as modeled in bss_sid/product.hpp).
-- 3 main entities (ProductSpecification, ProductOffering, ProductOfferingPrice) + normalized child
-- tables for every array sub-entity. Value objects (Money/Quantity/Duration/TimePeriod) inlined as
-- columns; single Refs inlined; array Refs/relationships as child tables. Extensibility via the SID
-- characteristic tables (product_spec_characteristic + prod_spec_char_value_use + char values).
-- ============================================================================
SET search_path TO product_catalog;

-- ---- ProductSpecification ----
CREATE TABLE product_specification (
    id               TEXT PRIMARY KEY,
    href             TEXT,
    brand            TEXT,
    description      TEXT,
    is_bundle        BOOLEAN,
    lifecycle_status TEXT,
    last_update      TIMESTAMPTZ,
    name             TEXT,
    product_number   TEXT,
    version          TEXT,
    target_product_schema JSONB,                       -- TargetProductSchema (polymorphism marker, opaque)
    attachment       JSONB,                             -- single AttachmentRefOrValue
    valid_for_start  TIMESTAMPTZ, valid_for_end TIMESTAMPTZ,
    created_at       TIMESTAMPTZ NOT NULL DEFAULT now(), updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_prodspec_name   ON product_specification (name);
CREATE INDEX idx_prodspec_status ON product_specification (lifecycle_status);

-- ProductSpecificationCharacteristic (the characteristic DEFINITION) -- extensibility.
CREATE TABLE product_spec_characteristic (
    id               TEXT PRIMARY KEY,
    specification_id TEXT NOT NULL REFERENCES product_specification(id) ON DELETE CASCADE,
    name             TEXT, description TEXT, value_type TEXT, regex TEXT,
    configurable     BOOLEAN, extensible BOOLEAN, is_unique BOOLEAN,
    min_cardinality  INTEGER, max_cardinality INTEGER,
    valid_for_start  TIMESTAMPTZ, valid_for_end TIMESTAMPTZ
);
CREATE INDEX idx_pspecchar_spec ON product_spec_characteristic (specification_id);
CREATE INDEX idx_pspecchar_name ON product_spec_characteristic (name);

-- CharacteristicValueSpecification -- one concrete value/range for a characteristic or a char-value-use.
CREATE TABLE char_value_specification (
    id                 BIGSERIAL PRIMARY KEY,
    spec_characteristic_id TEXT REFERENCES product_spec_characteristic(id) ON DELETE CASCADE,
    char_value_use_id  TEXT,                            -- FK added after prod_spec_char_value_use below
    is_default         BOOLEAN, range_interval TEXT, regex TEXT,
    unit_of_measure_amount NUMERIC, unit_of_measure_units TEXT,
    value_from TEXT, value_to TEXT, value_type TEXT, value JSONB,
    valid_for_start TIMESTAMPTZ, valid_for_end TIMESTAMPTZ
);
CREATE INDEX idx_charval_char ON char_value_specification (spec_characteristic_id);
CREATE INDEX idx_charval_use  ON char_value_specification (char_value_use_id);

-- ---- ProductOffering ----
CREATE TABLE product_offering (
    id               TEXT PRIMARY KEY,
    href             TEXT,
    name             TEXT NOT NULL,
    description      TEXT,
    lifecycle_status TEXT,
    last_update      TIMESTAMPTZ,
    status_reason    TEXT,
    is_bundle        BOOLEAN,
    is_sellable      BOOLEAN,
    version          TEXT,
    product_specification_id TEXT REFERENCES product_specification(id),
    -- single Refs inlined:
    service_level_agreement_id TEXT, agreement_id TEXT,
    resource_candidate_id TEXT, service_candidate_id TEXT,
    attachment JSONB, place JSONB,
    valid_for_start TIMESTAMPTZ, valid_for_end TIMESTAMPTZ,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(), updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_offering_name     ON product_offering (name);
CREATE INDEX idx_offering_status   ON product_offering (lifecycle_status);
CREATE INDEX idx_offering_sellable ON product_offering (is_sellable);
CREATE INDEX idx_offering_spec     ON product_offering (product_specification_id);

-- ---- ProductOfferingPrice ----
CREATE TABLE product_offering_price (
    id               TEXT PRIMARY KEY,
    href             TEXT,
    name             TEXT,
    description      TEXT,
    lifecycle_status TEXT,
    last_update      TIMESTAMPTZ,
    price_type       TEXT,                              -- recurring|oneTime|usage
    percentage       DOUBLE PRECISION,
    version          TEXT,
    price_unit       TEXT,                              -- Money.unit (ISO4217)
    price_value      NUMERIC(18,4),                     -- Money.value
    recurring_charge_period_length INTEGER,
    recurring_charge_period_type   TEXT,
    unit_of_measure_amount NUMERIC, unit_of_measure_units TEXT,  -- Quantity
    constraint_ref   JSONB, place JSONB, pricing_logic_algorithm JSONB, product_offering_term JSONB, tax JSONB,
    valid_for_start TIMESTAMPTZ, valid_for_end TIMESTAMPTZ,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_price_type   ON product_offering_price (price_type);
CREATE INDEX idx_price_status ON product_offering_price (lifecycle_status);

-- prodSpecCharValueUse -- the offering/price extensibility "use" of a spec characteristic.
CREATE TABLE prod_spec_char_value_use (
    id               TEXT PRIMARY KEY,
    offering_id      TEXT REFERENCES product_offering(id) ON DELETE CASCADE,
    price_id         TEXT REFERENCES product_offering_price(id) ON DELETE CASCADE,
    name             TEXT, description TEXT, value_type TEXT,
    min_cardinality  INTEGER, max_cardinality INTEGER,
    product_specification_id TEXT REFERENCES product_specification(id),
    valid_for_start TIMESTAMPTZ, valid_for_end TIMESTAMPTZ,
    CHECK (offering_id IS NOT NULL OR price_id IS NOT NULL)
);
CREATE INDEX idx_pscvu_offering ON prod_spec_char_value_use (offering_id);
CREATE INDEX idx_pscvu_price    ON prod_spec_char_value_use (price_id);
CREATE INDEX idx_pscvu_name     ON prod_spec_char_value_use (name);
ALTER TABLE char_value_specification
    ADD CONSTRAINT fk_charval_use FOREIGN KEY (char_value_use_id)
    REFERENCES prod_spec_char_value_use(id) ON DELETE CASCADE;

-- offering price link (ProductOfferingPriceRef array on offering)
CREATE TABLE offering_price_ref (
    offering_id TEXT NOT NULL REFERENCES product_offering(id) ON DELETE CASCADE,
    price_id    TEXT NOT NULL REFERENCES product_offering_price(id),
    PRIMARY KEY (offering_id, price_id)
);

-- offering array Refs (CategoryRef, ChannelRef, MarketSegmentRef): distinct SID types, id/href/name.
CREATE TABLE offering_category      (id TEXT, offering_id TEXT NOT NULL REFERENCES product_offering(id) ON DELETE CASCADE, ref_id TEXT NOT NULL, href TEXT, name TEXT, version TEXT, PRIMARY KEY (offering_id, ref_id));
CREATE TABLE offering_channel       (offering_id TEXT NOT NULL REFERENCES product_offering(id) ON DELETE CASCADE, ref_id TEXT NOT NULL, href TEXT, name TEXT, PRIMARY KEY (offering_id, ref_id));
CREATE TABLE offering_market_segment(offering_id TEXT NOT NULL REFERENCES product_offering(id) ON DELETE CASCADE, ref_id TEXT NOT NULL, href TEXT, name TEXT, PRIMARY KEY (offering_id, ref_id));

-- offering relationships / bundling / terms
CREATE TABLE offering_relationship (
    id TEXT PRIMARY KEY, offering_id TEXT NOT NULL REFERENCES product_offering(id) ON DELETE CASCADE,
    name TEXT, relationship_type TEXT, role TEXT, valid_for_start TIMESTAMPTZ, valid_for_end TIMESTAMPTZ
);
CREATE INDEX idx_offrel_offering ON offering_relationship (offering_id);
CREATE TABLE bundled_offering (
    id TEXT PRIMARY KEY, parent_offering_id TEXT NOT NULL REFERENCES product_offering(id) ON DELETE CASCADE,
    bundled_id TEXT NOT NULL, lifecycle_status TEXT, name TEXT
);
CREATE INDEX idx_bundoff_parent ON bundled_offering (parent_offering_id);
CREATE TABLE offering_term (
    id TEXT PRIMARY KEY, offering_id TEXT NOT NULL REFERENCES product_offering(id) ON DELETE CASCADE,
    name TEXT, description TEXT, duration_amount INTEGER, duration_units TEXT,
    valid_for_start TIMESTAMPTZ, valid_for_end TIMESTAMPTZ
);
CREATE INDEX idx_offterm_offering ON offering_term (offering_id);

-- spec relationships / bundling / related party / candidate refs
CREATE TABLE spec_relationship (
    id TEXT PRIMARY KEY, specification_id TEXT NOT NULL REFERENCES product_specification(id) ON DELETE CASCADE,
    name TEXT, relationship_type TEXT, valid_for_start TIMESTAMPTZ, valid_for_end TIMESTAMPTZ
);
CREATE INDEX idx_specrel_spec ON spec_relationship (specification_id);
CREATE TABLE bundled_specification (
    id TEXT PRIMARY KEY, parent_specification_id TEXT NOT NULL REFERENCES product_specification(id) ON DELETE CASCADE,
    bundled_id TEXT NOT NULL, lifecycle_status TEXT, name TEXT
);
CREATE INDEX idx_bundspec_parent ON bundled_specification (parent_specification_id);
CREATE TABLE spec_related_party (
    id TEXT PRIMARY KEY, specification_id TEXT NOT NULL REFERENCES product_specification(id) ON DELETE CASCADE,
    party_id TEXT NOT NULL, href TEXT, name TEXT, role TEXT
);
CREATE INDEX idx_specrp_spec ON spec_related_party (specification_id);
CREATE TABLE spec_candidate_ref (
    id BIGSERIAL PRIMARY KEY, specification_id TEXT NOT NULL REFERENCES product_specification(id) ON DELETE CASCADE,
    candidate_kind TEXT NOT NULL,                       -- RESOURCE | SERVICE
    ref_id TEXT NOT NULL, href TEXT, name TEXT, version TEXT
);
CREATE INDEX idx_speccand_spec ON spec_candidate_ref (specification_id);

-- price relationships / tax
CREATE TABLE price_relationship (
    id TEXT PRIMARY KEY, price_id TEXT NOT NULL REFERENCES product_offering_price(id) ON DELETE CASCADE,
    kind TEXT NOT NULL,                                 -- POP | BUNDLED_POP
    name TEXT, relationship_type TEXT, role TEXT
);
CREATE INDEX idx_pricerel_price ON price_relationship (price_id);
CREATE TABLE price_tax_item (
    id TEXT PRIMARY KEY, price_id TEXT NOT NULL REFERENCES product_offering_price(id) ON DELETE CASCADE,
    tax_category TEXT, tax_rate DOUBLE PRECISION, tax_amount_unit TEXT, tax_amount_value NUMERIC(18,4)
);
CREATE INDEX idx_pricetax_price ON price_tax_item (price_id);

-- ---- Product inventory: the purchased product per subscriber (TMF637), FKs now resolvable ----
CREATE TABLE subscriber_mgmt.product_subscription (
    id                  TEXT PRIMARY KEY,
    subscriber_id       TEXT NOT NULL REFERENCES subscriber_mgmt.subscriber(id),
    account_id          TEXT NOT NULL REFERENCES subscriber_mgmt.account(id),
    product_offering_id TEXT NOT NULL REFERENCES product_catalog.product_offering(id),
    status              TEXT NOT NULL DEFAULT 'active',
    start_date          TIMESTAMPTZ NOT NULL DEFAULT now(),
    end_date            TIMESTAMPTZ,
    characteristics     JSONB NOT NULL DEFAULT '{}',    -- provisioned S-NSSAI/DNN/AMBR/rating-group values
    created_at          TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_prodsub_subscriber ON subscriber_mgmt.product_subscription (subscriber_id);
CREATE INDEX idx_prodsub_account    ON subscriber_mgmt.product_subscription (account_id);
CREATE INDEX idx_prodsub_offering   ON subscriber_mgmt.product_subscription (product_offering_id);
