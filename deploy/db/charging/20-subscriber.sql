-- ============================================================================
-- SUBSCRIBER MANAGEMENT -- SID Customer/Account + Service(CFS) + Resource ABEs.
-- Sourced from docs/DATA_MODEL.md E1 (Subscriber Management) + E10 (MASTER Account model,
-- consumer/enterprise), grounded there against TMF632/620/CHARGING_MAPPING. Account hierarchy is
-- the E10 self-referential MASTER (arbitrary depth); consumer -> party.individual, enterprise ->
-- party.organization (whose own org hierarchy lives in 10-party). SUPI/IMSI/MSISDN/SIM are the
-- authoritative Resource ABE store (own table -> history + find-by-MSISDN/SUPI, per advisor).
-- ============================================================================
SET search_path TO subscriber_mgmt;

-- MASTER Account (E10): one table for CONSUMER and ENTERPRISE; enterprise builds a self-FK tree.
CREATE TABLE account (
    id                 TEXT PRIMARY KEY,
    account_kind       TEXT NOT NULL,                 -- CONSUMER | ENTERPRISE
    parent_account_id  TEXT REFERENCES account(id),   -- ENTERPRISE hierarchy (arbitrary depth); CONSUMER null
    individual_id      TEXT REFERENCES party.individual(id),   -- CONSUMER: the person
    organization_id    TEXT REFERENCES party.organization(id), -- ENTERPRISE: the org (dept-level)
    billing_mode       TEXT,                          -- INDIVIDUAL | SPLIT
    cost_center        TEXT,                           -- ENTERPRISE split-billing tag
    contract_sla_id    TEXT,                           -- ENTERPRISE only (SLA ref)
    provisioning_mode  TEXT,                           -- INDIVIDUAL | BULK
    status             TEXT NOT NULL DEFAULT 'active',
    created_at         TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at         TIMESTAMPTZ NOT NULL DEFAULT now(),
    CHECK (account_kind IN ('CONSUMER','ENTERPRISE')),
    CHECK (billing_mode IS NULL OR billing_mode IN ('INDIVIDUAL','SPLIT')),
    CHECK (provisioning_mode IS NULL OR provisioning_mode IN ('INDIVIDUAL','BULK'))
);
CREATE INDEX idx_account_parent     ON account (parent_account_id);
CREATE INDEX idx_account_individual ON account (individual_id);
CREATE INDEX idx_account_org        ON account (organization_id);
CREATE INDEX idx_account_kind       ON account (account_kind);

-- Subscriber = the CustomerFacingService / line (E1). Real identity fields as columns; prepaid/
-- postpaid + bill-cycle are operational columns here (E1 schema sketch), mirrored to SID as
-- Individual.partyCharacteristic at the API layer.
CREATE TABLE subscriber (
    id                  TEXT PRIMARY KEY,
    account_id          TEXT NOT NULL REFERENCES account(id),
    individual_id       TEXT REFERENCES party.individual(id),   -- the user of the line
    service_spec        TEXT,                          -- CFS spec, e.g. 'mobile-line'
    charging_mode       TEXT NOT NULL,                 -- PREPAID | POSTPAID
    bill_cycle_day      INTEGER CHECK (bill_cycle_day BETWEEN 1 AND 28),
    status              TEXT NOT NULL DEFAULT 'pendingActive', -- pendingActive|active|suspended|terminated
    service_preferences JSONB NOT NULL DEFAULT '{}',   -- genuinely open prefs (notification, spend-limit opt-ins)
    created_at          TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at          TIMESTAMPTZ NOT NULL DEFAULT now(),
    CHECK (charging_mode IN ('PREPAID','POSTPAID'))
);
CREATE INDEX idx_subscriber_account    ON subscriber (account_id);
CREATE INDEX idx_subscriber_individual ON subscriber (individual_id);
CREATE INDEX idx_subscriber_status     ON subscriber (status);

-- Resource ABE (TMF639): SUPI/IMSI/MSISDN/SIM -- authoritative operational store; MSISDN/SIM change
-- over time (valid_from/to history); one ACTIVE value per (type,value); the find-by-SUPI/MSISDN path.
CREATE TABLE resource (
    id             TEXT PRIMARY KEY,
    subscriber_id  TEXT NOT NULL REFERENCES subscriber(id),
    resource_type  TEXT NOT NULL,                     -- SUPI|IMSI|MSISDN|SIM_ICCID
    resource_value TEXT NOT NULL,
    status         TEXT NOT NULL DEFAULT 'active',    -- active|released
    valid_from     TIMESTAMPTZ NOT NULL DEFAULT now(),
    valid_to       TIMESTAMPTZ,
    characteristic JSONB NOT NULL DEFAULT '{}',
    CHECK (resource_type IN ('SUPI','IMSI','MSISDN','SIM_ICCID'))
);
CREATE INDEX idx_resource_subscriber ON resource (subscriber_id);
CREATE INDEX idx_resource_lookup     ON resource (resource_type, resource_value);
CREATE UNIQUE INDEX uq_resource_active ON resource (resource_type, resource_value) WHERE status = 'active';
