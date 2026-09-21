-- Orchestration / Order-Management domain DB (project_customer_onboarding_orchestration).
-- Its own domain database (project_db_per_domain_rule). TM Forum SID / Open API aligned:
-- Product Order (TMF622) -> Service Order (TMF641) -> Resource Order (TMF652) -> per-node
-- provisioning tasks with state + compensation. Source of truth for "is this customer fully
-- provisioned across all nodes?".
--
-- Scale note (project_tier1_scale_architecture): these are ORDER-volume operational records
-- (bounded by customer onboarding/change events), NOT 300M/day usage time-series -- so they are
-- heavily INDEXED on the real query paths, not date-partitioned. Partitioning is reserved for the
-- usage/CDR (Doris) and rating tables. Ids are globally unique.
--
-- #1 rule: entities/fields reference real TM Forum SID (latest) + TMF622/641/652 Open APIs;
-- variable-shape TMF sub-resources are jsonb (same discipline as bss/product-catalog ADR-0053).

CREATE SCHEMA IF NOT EXISTS orchestration;
SET search_path TO orchestration;

-- TMF622 ProductOrder: the customer's purchase, created by the GUI at the sales point.
CREATE TABLE IF NOT EXISTS product_order (
    id                   TEXT PRIMARY KEY,
    state                TEXT NOT NULL,                 -- acknowledged|inProgress|completed|failed|cancelled (TMF622)
    order_date           TIMESTAMPTZ NOT NULL DEFAULT now(),
    requested_completion TIMESTAMPTZ,
    customer_id          TEXT NOT NULL,                 -- SID Customer/Party
    account_id           TEXT NOT NULL,                 -- SID BillingAccount
    channel              TEXT,                          -- 'gui' | 'api' | ...
    related_party        JSONB NOT NULL DEFAULT '[]',
    note                 JSONB NOT NULL DEFAULT '[]',
    created_at           TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at           TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_product_order_customer ON product_order (customer_id);
CREATE INDEX IF NOT EXISTS idx_product_order_account  ON product_order (account_id);
CREATE INDEX IF NOT EXISTS idx_product_order_state    ON product_order (state);
CREATE INDEX IF NOT EXISTS idx_product_order_date     ON product_order (order_date);

CREATE TABLE IF NOT EXISTS product_order_item (
    id                   TEXT PRIMARY KEY,
    product_order_id     TEXT NOT NULL REFERENCES product_order(id),
    action               TEXT NOT NULL,                 -- add|modify|delete (TMF622)
    product_offering_id  TEXT NOT NULL,                 -- charging product_catalog offering
    segment              TEXT NOT NULL,                 -- CONSUMER|ENTERPRISE(+sub-type)
    charging_mode        TEXT NOT NULL,                 -- PREPAID|POSTPAID
    characteristics      JSONB NOT NULL DEFAULT '{}',   -- S-NSSAI, DNN, AMBR, MSISDN request, ...
    created_at           TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_order_item_order ON product_order_item (product_order_id);

-- TMF641 ServiceOrder: the CustomerFacingService (the line/subscription) realising the product.
CREATE TABLE IF NOT EXISTS service_order (
    id                   TEXT PRIMARY KEY,
    product_order_id     TEXT NOT NULL REFERENCES product_order(id),
    order_item_id        TEXT NOT NULL REFERENCES product_order_item(id),
    state                TEXT NOT NULL,                 -- TMF641 ServiceOrderStateType
    service_spec         TEXT NOT NULL,                 -- CFS spec (e.g. 'mobile-line')
    subscriber_id        TEXT,                          -- charging subscriber once created
    characteristics      JSONB NOT NULL DEFAULT '{}',
    created_at           TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at           TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_service_order_order ON service_order (product_order_id);

-- TMF652 ResourceOrder: allocation of network resources (SUPI/IMSI, MSISDN, K/OPc).
CREATE TABLE IF NOT EXISTS resource_order (
    id                   TEXT PRIMARY KEY,
    service_order_id     TEXT NOT NULL REFERENCES service_order(id),
    state                TEXT NOT NULL,                 -- TMF652 ResourceOrderStateType
    resource_type        TEXT NOT NULL,                 -- SUPI|IMSI|MSISDN|AUTH_CRED
    resource_value       TEXT,                          -- imsi-..., msisdn, ...
    characteristics      JSONB NOT NULL DEFAULT '{}',
    created_at           TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_resource_order_service ON resource_order (service_order_id);

-- Per-node provisioning task: the unit the workflow runner executes + tracks, with idempotency and
-- compensation (saga rollback across nodes) so onboarding is all-or-nothing.
CREATE TABLE IF NOT EXISTS provisioning_task (
    id                   TEXT PRIMARY KEY,
    product_order_id     TEXT NOT NULL REFERENCES product_order(id),
    node                 TEXT NOT NULL,                 -- udr-subscription|udr-auth|udr-policy|chf-balance|nssf|...
    op                   TEXT NOT NULL,                 -- provision|deprovision
    status               TEXT NOT NULL DEFAULT 'pending', -- pending|in_progress|done|failed|compensated
    idempotency_key      TEXT NOT NULL UNIQUE,          -- safe retries; effectively at-most-once
    request_payload      JSONB NOT NULL DEFAULT '{}',
    response             JSONB,
    attempts             INTEGER NOT NULL DEFAULT 0,
    last_error           TEXT,
    created_at           TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at           TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_task_order       ON provisioning_task (product_order_id);
CREATE INDEX IF NOT EXISTS idx_task_node_status ON provisioning_task (node, status);
CREATE INDEX IF NOT EXISTS idx_task_status      ON provisioning_task (status);

CREATE TABLE IF NOT EXISTS order_state_history (
    id                   BIGSERIAL PRIMARY KEY,
    product_order_id     TEXT NOT NULL REFERENCES product_order(id),
    from_state           TEXT,
    to_state             TEXT NOT NULL,
    at                   TIMESTAMPTZ NOT NULL DEFAULT now(),
    detail               TEXT
);
CREATE INDEX IF NOT EXISTS idx_state_hist_order ON order_state_history (product_order_id);
