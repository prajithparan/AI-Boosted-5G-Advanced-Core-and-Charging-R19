-- LI ADMF warrant store (ADR-0462; engine per ADR-0462 decision 2: a dedicated PostgreSQL, because
-- warrants and legal documents are durable legal records, not rebuildable cache, and TS 33.127 5.6
-- places LI functions in a separate security domain from the network functions' datastores).
--
-- Every HI1 object (TS 103 120 clause 7) is stored as its own self-contained XML element, so GET
-- returns exactly what the LEA created (nothing is re-modelled field by field); the typed columns
-- are projections for lookup and LIST filtering only, rewritten on every change.
CREATE TABLE IF NOT EXISTS hi1_object (
    object_id         TEXT PRIMARY KEY,                 -- ObjectIdentifier (a UUID)
    object_type       TEXT NOT NULL,                    -- Authorisation | LITask | Document | Notification | Other
    owner_identifier  TEXT,
    country_code      TEXT,
    generation        BIGINT NOT NULL DEFAULT 0,        -- HI1Object.Generation: bumped on every change
    external_id       TEXT,
    authorisation_id  TEXT,                             -- the Authorisation a task/document/notification belongs to
    lea               TEXT,                             -- "<CountryCode>/<UniqueIdentifier>" of the LEA that owns this object chain
    last_txn          TEXT,                             -- HI1 TransactionIdentifier of the request that last changed it
    status            TEXT,                             -- dictionary Value of the object's status, for LIST
    xml               TEXT NOT NULL,                    -- the HI1Object element, self-contained
    created_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_changed      TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS hi1_object_type_changed ON hi1_object (object_type, last_changed);
CREATE INDEX IF NOT EXISTS hi1_object_authorisation ON hi1_object (authorisation_id);

-- Append-only audit trail of every HI1 exchange (TS 33.127 requires LI actions to be auditable).
-- Rows are only ever inserted by the ADMF; nothing in this codebase updates or deletes them.
CREATE TABLE IF NOT EXISTS hi1_audit (
    seq             BIGSERIAL PRIMARY KEY,
    at              TIMESTAMPTZ NOT NULL DEFAULT now(),
    peer            TEXT NOT NULL,                      -- mTLS client certificate CN
    path            TEXT NOT NULL,
    transaction_id  TEXT,
    sender          TEXT,                               -- "<CountryCode>/<UniqueIdentifier>"
    actions         INTEGER NOT NULL DEFAULT 0,
    outcome         TEXT NOT NULL,                      -- ok | rejected | error
    detail          TEXT
);

-- What the LIPF has provisioned onto the network elements for each LI task, so the reconcile loop is
-- idempotent and crash-safe: a task is (re)provisioned when its generation differs from the one that
-- was provisioned, and a delivery change knows which destinations to retire.
CREATE TABLE IF NOT EXISTS lipf_task (
    object_id               TEXT PRIMARY KEY,           -- the LITask's ObjectIdentifier (= the X1 XID)
    provisioned_generation  BIGINT NOT NULL,
    destinations            TEXT NOT NULL DEFAULT '[]', -- JSON array of the "ip:port" addresses provisioned
    state                   TEXT NOT NULL,              -- provisioned | deprovisioned
    updated_at              TIMESTAMPTZ NOT NULL DEFAULT now()
);
