-- operator_iam: the operator GUI's identity, access, approval, configuration-version and audit
-- domain (ADR-0423, ADR-0424, ADR-0425). Its own database on the postgres-chf instance, per the
-- DB-per-domain rule: nothing here is charging or subscriber data, and nothing outside the GUI
-- backend-for-frontend (gui/bff, "oam-gui-bff") reads or writes it.
--
-- Security model in one paragraph: authentication is delegated to an OIDC IdP (users carry the
-- IdP `sub`, never a password hash); authorization is data -- roles are rows, permissions are
-- (resource, action) rows, and a grant is anchored at an org unit whose SUBTREE is the grant's
-- scope; sensitive actions go through maker-checker requests the DB itself refuses to let the
-- maker decide; and every allowed, denied and approved action lands in an append-only,
-- hash-chained, monthly-partitioned audit table the application role can only INSERT into.
--
-- Requires PostgreSQL 13+ (BEFORE row triggers on partitioned tables; built-in sha256()).
-- Tested against 16.

CREATE SCHEMA IF NOT EXISTS iam;
SET search_path = iam, public;

-- ================================================================================================
-- Organisation: channel -> dealer/partner -> shop, plus back office / NOC / HQ units.
-- ================================================================================================

CREATE TABLE org_unit (
    id          TEXT PRIMARY KEY,
    kind        TEXT NOT NULL,
    parent_id   TEXT REFERENCES org_unit(id),
    name        TEXT NOT NULL,
    status      TEXT NOT NULL DEFAULT 'ACTIVE',
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    CHECK (kind IN ('HQ','CHANNEL','DEALER','SHOP','BACK_OFFICE','NOC')),
    CHECK (status IN ('ACTIVE','CLOSED')),
    CHECK (parent_id IS DISTINCT FROM id),
    -- ids are spliced into provisioning idempotency keys / order ids (ADR-0423): URL-safe only
    CHECK (id ~ '^[A-Za-z0-9_-]{1,64}$')
);
CREATE INDEX idx_org_unit_parent ON org_unit(parent_id);

-- Closure table: every (ancestor, descendant) pair incl. self at depth 0. "Is shop S inside the
-- scope anchored at unit U?" is one indexed lookup, at any depth, for any number of shops.
CREATE TABLE org_unit_closure (
    ancestor_id   TEXT NOT NULL REFERENCES org_unit(id),
    descendant_id TEXT NOT NULL REFERENCES org_unit(id),
    depth         INTEGER NOT NULL,
    PRIMARY KEY (ancestor_id, descendant_id)
);
CREATE INDEX idx_org_closure_desc ON org_unit_closure(descendant_id);

CREATE FUNCTION org_unit_closure_maintain() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    INSERT INTO iam.org_unit_closure(ancestor_id, descendant_id, depth) VALUES (NEW.id, NEW.id, 0);
    IF NEW.parent_id IS NOT NULL THEN
        INSERT INTO iam.org_unit_closure(ancestor_id, descendant_id, depth)
        SELECT c.ancestor_id, NEW.id, c.depth + 1
          FROM iam.org_unit_closure c WHERE c.descendant_id = NEW.parent_id;
    END IF;
    RETURN NEW;
END $$;
CREATE TRIGGER trg_org_unit_closure AFTER INSERT ON org_unit
    FOR EACH ROW EXECUTE FUNCTION org_unit_closure_maintain();
-- Re-parenting (a shop moving dealer) is a mover event handled as close + recreate in this
-- increment; an in-place parent change is refused so the closure can never go stale.
CREATE FUNCTION org_unit_no_reparent() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    IF NEW.parent_id IS DISTINCT FROM OLD.parent_id THEN
        RAISE EXCEPTION 'org_unit re-parenting is not supported; close and recreate the unit';
    END IF;
    RETURN NEW;
END $$;
CREATE TRIGGER trg_org_unit_no_reparent BEFORE UPDATE ON org_unit
    FOR EACH ROW EXECUTE FUNCTION org_unit_no_reparent();

-- Registered operator terminals (the mTLS client certificate a shop PC presents). Recorded on
-- every session and audit row now; binding "this terminal may only be used for its own shop" is
-- designed here and enforced in a later increment (ADR-0423 deferrals).
CREATE TABLE terminal (
    cert_cn      TEXT PRIMARY KEY,
    org_unit_id  TEXT NOT NULL REFERENCES org_unit(id),
    status       TEXT NOT NULL DEFAULT 'ACTIVE',
    registered_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    CHECK (status IN ('ACTIVE','REVOKED'))
);

-- ================================================================================================
-- Users (joiner / mover / leaver). Credentials live in the IdP, never here.
-- ================================================================================================

CREATE TABLE operator_user (
    id               TEXT PRIMARY KEY,
    idp_issuer       TEXT NOT NULL,
    idp_subject      TEXT NOT NULL,                 -- OIDC `sub`
    username         TEXT NOT NULL,
    display_name     TEXT,
    email            TEXT,
    home_org_unit_id TEXT NOT NULL REFERENCES org_unit(id),
    status           TEXT NOT NULL DEFAULT 'ACTIVE',
    mfa_required     BOOLEAN NOT NULL DEFAULT true,
    joined_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    left_at          TIMESTAMPTZ,
    last_login_at    TIMESTAMPTZ,
    locked_reason    TEXT,
    created_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (idp_issuer, idp_subject),
    UNIQUE (username),
    CHECK (status IN ('ACTIVE','LOCKED','DORMANT','LEFT')),
    CHECK ((status = 'LEFT') = (left_at IS NOT NULL))
);
CREATE INDEX idx_user_home_unit ON operator_user(home_org_unit_id);

-- Mover history: every change of home unit, kept for the audit reviewer.
CREATE TABLE operator_user_move (
    id           BIGSERIAL PRIMARY KEY,
    user_id      TEXT NOT NULL REFERENCES operator_user(id),
    from_unit_id TEXT REFERENCES org_unit(id),
    to_unit_id   TEXT NOT NULL REFERENCES org_unit(id),
    moved_by     TEXT NOT NULL REFERENCES operator_user(id),
    moved_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    CHECK (moved_by <> user_id)
);

-- Single-row authentication / lifecycle policy. Password rules are the IdP's (delegated); what
-- the GUI itself enforces is session lifetime, dormancy and the MFA evidence it demands.
CREATE TABLE auth_policy (
    id                        BOOLEAN PRIMARY KEY DEFAULT true CHECK (id),
    dormant_after_days        INTEGER NOT NULL DEFAULT 45,
    session_idle_minutes      INTEGER NOT NULL DEFAULT 15,
    session_absolute_minutes  INTEGER NOT NULL DEFAULT 480,
    login_state_ttl_seconds   INTEGER NOT NULL DEFAULT 300,
    approval_ttl_hours        INTEGER NOT NULL DEFAULT 72
);
INSERT INTO auth_policy DEFAULT VALUES;

-- ================================================================================================
-- Authorization: permissions, roles (data-driven), scoped grants, field policy, SoD.
-- ================================================================================================

CREATE TABLE permission (
    id          TEXT PRIMARY KEY,                   -- "<resource>:<action>"
    resource    TEXT NOT NULL,
    action      TEXT NOT NULL,
    description TEXT NOT NULL,
    UNIQUE (resource, action)
);

CREATE TABLE role (
    id                      TEXT PRIMARY KEY,
    name                    TEXT NOT NULL UNIQUE,
    description             TEXT NOT NULL,
    privileged              BOOLEAN NOT NULL DEFAULT false, -- granting it needs four-eyes
    created_at              TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- scope: OWN_SUBTREE = the grant's anchor unit and everything below it (a shop agent anchored at
-- their shop sees that shop; a dealer supervisor anchored at the dealer sees all its shops);
-- GLOBAL = everywhere (security admin, auditor, NOC).
CREATE TABLE role_permission (
    role_id       TEXT NOT NULL REFERENCES role(id),
    permission_id TEXT NOT NULL REFERENCES permission(id),
    scope         TEXT NOT NULL DEFAULT 'OWN_SUBTREE',
    PRIMARY KEY (role_id, permission_id),
    CHECK (scope IN ('OWN_SUBTREE','GLOBAL'))
);

CREATE TABLE role_assignment (
    id                  TEXT PRIMARY KEY,
    user_id             TEXT NOT NULL REFERENCES operator_user(id),
    role_id             TEXT NOT NULL REFERENCES role(id),
    org_unit_id         TEXT NOT NULL REFERENCES org_unit(id),   -- scope anchor
    valid_from          TIMESTAMPTZ NOT NULL DEFAULT now(),
    valid_to            TIMESTAMPTZ,
    granted_by          TEXT REFERENCES operator_user(id),       -- NULL only for bootstrap seed
    approval_request_id TEXT,
    revoked_at          TIMESTAMPTZ,
    revoked_by          TEXT REFERENCES operator_user(id),
    CHECK (granted_by IS DISTINCT FROM user_id),                 -- SoD: no self-grant
    CHECK (valid_to IS NULL OR valid_to > valid_from)
);
CREATE INDEX idx_role_assignment_user ON role_assignment(user_id) WHERE revoked_at IS NULL;
CREATE INDEX idx_role_assignment_unit ON role_assignment(org_unit_id) WHERE revoked_at IS NULL;

-- Segregation of duties: a user may not hold both permissions of a pair at the same time.
CREATE TABLE sod_rule (
    id           TEXT PRIMARY KEY,
    permission_a TEXT NOT NULL REFERENCES permission(id),
    permission_b TEXT NOT NULL REFERENCES permission(id),
    description  TEXT NOT NULL,
    CHECK (permission_a <> permission_b)
);

-- Field-level policy. SECRET_WRITE_ONLY: accepted on input, never returned, never stored, never
-- audited (SIM K/OPc). PII: masked in every response unless the caller holds unmask_permission
-- and states a reason (the unmask is itself audited). CREDENTIAL: config values such as a DB URL
-- with a password -- shown masked, replaced only as a whole.
CREATE TABLE field_policy (
    resource          TEXT NOT NULL,
    field_path        TEXT NOT NULL,             -- dotted JSON path, e.g. sim.k
    classification    TEXT NOT NULL,
    unmask_permission TEXT REFERENCES permission(id),
    PRIMARY KEY (resource, field_path),
    CHECK (classification IN ('SECRET_WRITE_ONLY','PII','CREDENTIAL'))
);

-- Which actions need four-eyes. threshold_amount: only above this (balance adjustments);
-- NULL = always.
CREATE TABLE approval_policy (
    permission_id      TEXT PRIMARY KEY REFERENCES permission(id),
    threshold_amount   NUMERIC(20,4),
    threshold_currency TEXT,
    approver_permission TEXT NOT NULL REFERENCES permission(id)
);

-- ================================================================================================
-- Maker-checker.
-- ================================================================================================

CREATE TABLE approval_request (
    id              TEXT PRIMARY KEY,
    permission_id   TEXT NOT NULL REFERENCES permission(id),     -- the action requested
    org_unit_id     TEXT NOT NULL REFERENCES org_unit(id),       -- scope of the request
    target_ref      TEXT,                                        -- e.g. nf name, offering id, grantee
    payload         JSONB NOT NULL,                              -- redacted per field_policy
    payload_sha256  TEXT NOT NULL,
    requested_by    TEXT NOT NULL REFERENCES operator_user(id),
    requested_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    reason          TEXT NOT NULL,
    status          TEXT NOT NULL DEFAULT 'PENDING',
    decided_by      TEXT REFERENCES operator_user(id),
    decided_at      TIMESTAMPTZ,
    decision_reason TEXT,
    executed_at     TIMESTAMPTZ,
    execution_result JSONB,
    expires_at      TIMESTAMPTZ NOT NULL,
    CHECK (status IN ('PENDING','APPROVED','REJECTED','EXPIRED','EXECUTED','FAILED')),
    CHECK (decided_by IS DISTINCT FROM requested_by),             -- four-eyes, in the DB itself
    CHECK ((status = 'PENDING') = (decided_by IS NULL))
);
CREATE INDEX idx_approval_pending ON approval_request(org_unit_id, requested_at) WHERE status = 'PENDING';
CREATE INDEX idx_approval_requester ON approval_request(requested_by, requested_at);

-- A role grant's grantee may not approve it either.
CREATE FUNCTION approval_no_grantee_decision() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    IF NEW.permission_id = 'role_grant:create' AND NEW.decided_by IS NOT NULL
       AND NEW.decided_by = NEW.payload->>'userId' THEN
        RAISE EXCEPTION 'segregation of duties: the grantee cannot decide their own role grant';
    END IF;
    RETURN NEW;
END $$;
CREATE TRIGGER trg_approval_no_grantee BEFORE INSERT OR UPDATE ON approval_request
    FOR EACH ROW EXECUTE FUNCTION approval_no_grantee_decision();

-- ================================================================================================
-- Sessions and the OIDC login state.
-- ================================================================================================

CREATE TABLE login_state (
    state_hash    TEXT PRIMARY KEY,                 -- sha256 of the `state` value
    nonce         TEXT NOT NULL,
    code_verifier TEXT NOT NULL,                    -- PKCE; single use, deleted on callback
    browser_binding_hash TEXT NOT NULL,             -- sha256 of the login cookie
    terminal_cn   TEXT,
    created_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at    TIMESTAMPTZ NOT NULL
);

CREATE TABLE operator_session (
    id              TEXT PRIMARY KEY,               -- sha256 of the opaque cookie value
    user_id         TEXT NOT NULL REFERENCES operator_user(id),
    active_org_unit_id TEXT NOT NULL REFERENCES org_unit(id),
    auth_method     TEXT NOT NULL,                  -- e.g. oidc
    idp_session_id  TEXT,                           -- OIDC `sid`
    acr             TEXT,
    amr             TEXT[],
    terminal_cn     TEXT,                           -- mTLS client certificate of the terminal
    client_ip       TEXT,
    user_agent      TEXT,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_seen_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at      TIMESTAMPTZ NOT NULL,
    ended_at        TIMESTAMPTZ,
    end_reason      TEXT,
    CHECK (end_reason IS NULL OR end_reason IN ('LOGOUT','IDLE','EXPIRED','REVOKED','USER_DISABLED'))
);
CREATE INDEX idx_session_user ON operator_session(user_id, created_at);

-- ================================================================================================
-- Scope registry: which org unit owns which customer (SUPI) and which provisioning order. The
-- provisioning API carries no shop, so the BFF claims ownership here before it forwards.
-- ================================================================================================

CREATE TABLE customer_ownership (
    supi        TEXT PRIMARY KEY,
    org_unit_id TEXT NOT NULL REFERENCES org_unit(id),
    claimed_by  TEXT NOT NULL REFERENCES operator_user(id),
    claimed_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_customer_unit ON customer_ownership(org_unit_id, claimed_at);

CREATE TABLE order_ownership (
    order_id    TEXT PRIMARY KEY,
    supi        TEXT NOT NULL,
    org_unit_id TEXT NOT NULL REFERENCES org_unit(id),
    created_by  TEXT NOT NULL REFERENCES operator_user(id),
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_order_unit ON order_ownership(org_unit_id, created_at);
CREATE INDEX idx_order_creator ON order_ownership(created_by, created_at);

-- ================================================================================================
-- NF configuration versions (ADR-0425). Every accepted change is a new version row; rollback is
-- a new version whose content equals an old one. Credential fields are stored as supplied (the
-- config file needs them) but never returned unmasked and never audited in clear.
-- ================================================================================================

CREATE TABLE nf_config_version (
    nf_name        TEXT NOT NULL,
    version        INTEGER NOT NULL,
    content        JSONB NOT NULL,
    content_sha256 TEXT NOT NULL,
    created_by     TEXT REFERENCES operator_user(id),   -- NULL: imported from the file on disk
    approval_request_id TEXT REFERENCES approval_request(id),
    created_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    applied_at     TIMESTAMPTZ,                          -- written to the NF's config file
    restart_required BOOLEAN NOT NULL DEFAULT true,
    comment        TEXT,
    PRIMARY KEY (nf_name, version)
);

-- ================================================================================================
-- Audit trail: append-only, hash-chained, partitioned by month.
-- ================================================================================================

CREATE TABLE audit_event (
    id              BIGINT GENERATED ALWAYS AS IDENTITY,
    occurred_at     TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    chain_key       TEXT NOT NULL,                   -- one chain per BFF instance
    chain_seq       BIGINT NOT NULL DEFAULT 0,       -- position in the chain, set under the lock
    user_id         TEXT,                            -- NULL: unauthenticated attempt
    username        TEXT,
    session_id      TEXT,
    org_unit_id     TEXT,
    terminal_cn     TEXT,
    client_ip       TEXT,
    action          TEXT NOT NULL,                   -- permission id or auth event
    resource_ref    TEXT,                            -- e.g. customerOrder/ord-...
    customer_ref    TEXT,                            -- SUPI the action concerned, if any
    outcome         TEXT NOT NULL,
    http_status     INTEGER,
    reason          TEXT,
    before_state    JSONB,                           -- redacted per field_policy
    after_state     JSONB,                           -- redacted per field_policy
    approval_request_id TEXT,
    prev_hash       TEXT NOT NULL,
    row_hash        TEXT NOT NULL,
    PRIMARY KEY (occurred_at, id),
    CHECK (outcome IN ('ALLOWED','DENIED','ERROR','PENDING_APPROVAL','APPROVED','REJECTED'))
) PARTITION BY RANGE (occurred_at);
CREATE INDEX idx_audit_user ON audit_event(user_id, occurred_at);
CREATE INDEX idx_audit_unit ON audit_event(org_unit_id, occurred_at);
CREATE INDEX idx_audit_customer ON audit_event(customer_ref, occurred_at);
CREATE INDEX idx_audit_chain ON audit_event(chain_key, chain_seq);

-- Creates the monthly partition holding `ts` if missing. Called for this and next month below,
-- and by the BFF at start-up; a DEFAULT partition catches anything a missed rotation would lose.
CREATE FUNCTION ensure_audit_partition(ts TIMESTAMPTZ) RETURNS void LANGUAGE plpgsql AS $$
DECLARE
    lo DATE := date_trunc('month', ts AT TIME ZONE 'UTC')::date;
    hi DATE := (lo + INTERVAL '1 month')::date;
    part TEXT := format('audit_event_%s', to_char(lo, 'YYYYMM'));
BEGIN
    IF to_regclass('iam.' || part) IS NULL THEN
        EXECUTE format('CREATE TABLE iam.%I PARTITION OF iam.audit_event FOR VALUES FROM (%L) TO (%L)',
                       part, lo, hi);
    END IF;
END $$;
CREATE TABLE audit_event_default PARTITION OF audit_event DEFAULT;
SELECT ensure_audit_partition(now());
SELECT ensure_audit_partition(now() + INTERVAL '1 month');

-- The canonical text a row's hash covers. Deterministic: UTC timestamp text, jsonb's own
-- normalised ::text, explicit separators, NULL spelled out.
CREATE FUNCTION audit_canonical(e audit_event) RETURNS TEXT LANGUAGE sql IMMUTABLE AS $$
    SELECT concat_ws('|',
        e.prev_hash, e.chain_key, e.chain_seq::text,
        to_char(e.occurred_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.US"Z"'),
        coalesce(e.user_id, '~'), coalesce(e.username, '~'), coalesce(e.session_id, '~'),
        coalesce(e.org_unit_id, '~'), coalesce(e.terminal_cn, '~'), coalesce(e.client_ip, '~'),
        e.action, coalesce(e.resource_ref, '~'), coalesce(e.customer_ref, '~'), e.outcome,
        coalesce(e.http_status::text, '~'), coalesce(e.reason, '~'),
        coalesce(e.before_state::text, '~'), coalesce(e.after_state::text, '~'),
        coalesce(e.approval_request_id, '~'))
$$;

CREATE TABLE audit_chain_head (
    chain_key  TEXT PRIMARY KEY,
    last_hash  TEXT NOT NULL,
    last_seq   BIGINT NOT NULL,
    updated_at TIMESTAMPTZ NOT NULL
);

-- Links every new row to its chain's previous row. The per-chain advisory lock serialises
-- inserts on ONE chain only (one chain per BFF instance), so instances never contend. Order in
-- the chain is chain_seq, assigned under the lock -- not the identity id, which concurrent
-- inserts draw before they queue for the lock. occurred_at is NOT touched here: it is the
-- partition key, and a BEFORE trigger may not move a row to another partition.
CREATE FUNCTION audit_chain_link() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE
    head TEXT;
BEGIN
    PERFORM pg_advisory_xact_lock(hashtext('iam.audit_event:' || NEW.chain_key));
    SELECT last_hash INTO head FROM iam.audit_chain_head WHERE chain_key = NEW.chain_key;
    NEW.chain_seq := coalesce((SELECT last_seq FROM iam.audit_chain_head
                                WHERE chain_key = NEW.chain_key), 0) + 1;
    NEW.prev_hash := coalesce(head, repeat('0', 64));
    NEW.row_hash := encode(sha256(convert_to(iam.audit_canonical(NEW), 'UTF8')), 'hex');
    INSERT INTO iam.audit_chain_head(chain_key, last_hash, last_seq, updated_at)
         VALUES (NEW.chain_key, NEW.row_hash, NEW.chain_seq, NEW.occurred_at)
    ON CONFLICT (chain_key) DO UPDATE SET last_hash = EXCLUDED.last_hash,
                                          last_seq = EXCLUDED.last_seq,
                                          updated_at = EXCLUDED.updated_at;
    RETURN NEW;
END $$;
CREATE TRIGGER trg_audit_chain BEFORE INSERT ON audit_event
    FOR EACH ROW EXECUTE FUNCTION audit_chain_link();

-- Append-only, belt and braces: no grant to UPDATE/DELETE/TRUNCATE (below) AND triggers that
-- refuse them for everyone short of a superuser disabling triggers -- which verification then
-- exposes. Tamper-EVIDENT, not tamper-proof: the ADR says so.
CREATE FUNCTION audit_refuse_change() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    RAISE EXCEPTION 'audit_event is append-only (% refused)', TG_OP;
END $$;
CREATE TRIGGER trg_audit_no_update BEFORE UPDATE OR DELETE ON audit_event
    FOR EACH ROW EXECUTE FUNCTION audit_refuse_change();
CREATE TRIGGER trg_audit_no_truncate BEFORE TRUNCATE ON audit_event
    FOR EACH STATEMENT EXECUTE FUNCTION audit_refuse_change();

-- Walks one chain in insertion order and returns the first row whose stored hash or link does
-- not match a recomputation (NULL = the whole chain verifies).
CREATE FUNCTION audit_verify_chain(p_chain_key TEXT)
RETURNS TABLE(bad_id BIGINT, problem TEXT) LANGUAGE plpgsql AS $$
DECLARE
    r iam.audit_event;
    expected_prev TEXT := repeat('0', 64);
BEGIN
    FOR r IN SELECT * FROM iam.audit_event WHERE chain_key = p_chain_key ORDER BY chain_seq LOOP
        IF r.prev_hash <> expected_prev THEN
            bad_id := r.id; problem := 'broken link'; RETURN NEXT; RETURN;
        END IF;
        IF r.row_hash <> encode(sha256(convert_to(iam.audit_canonical(r), 'UTF8')), 'hex') THEN
            bad_id := r.id; problem := 'row content changed'; RETURN NEXT; RETURN;
        END IF;
        expected_prev := r.row_hash;
    END LOOP;
    IF expected_prev <> coalesce((SELECT last_hash FROM iam.audit_chain_head
                                   WHERE chain_key = p_chain_key), repeat('0', 64)) THEN
        bad_id := NULL; problem := 'chain truncated (head does not match last row)'; RETURN NEXT;
    END IF;
END $$;

-- Compliance export: a stable, read-only view for the reviewer (and the export job, deferred).
CREATE VIEW audit_export AS
    SELECT id, occurred_at, chain_key, chain_seq, user_id, username, org_unit_id, terminal_cn, client_ip,
           action, resource_ref, customer_ref, outcome, http_status, reason,
           before_state, after_state, approval_request_id, prev_hash, row_hash
      FROM audit_event;

-- ================================================================================================
-- Application role: the BFF connects as this login, never as a superuser.
-- ================================================================================================

DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'oam_gui_bff') THEN
        CREATE ROLE oam_gui_bff LOGIN;   -- password / cert auth set by the deployment, not here
    END IF;
END $$;
GRANT USAGE ON SCHEMA iam TO oam_gui_bff;
GRANT SELECT ON ALL TABLES IN SCHEMA iam TO oam_gui_bff;
GRANT INSERT ON audit_event, audit_chain_head, login_state, operator_session, customer_ownership,
                order_ownership, approval_request, nf_config_version TO oam_gui_bff;
GRANT UPDATE ON audit_chain_head, operator_session, approval_request, nf_config_version,
                operator_user TO oam_gui_bff;
GRANT DELETE ON login_state, customer_ownership TO oam_gui_bff;
GRANT EXECUTE ON FUNCTION ensure_audit_partition(TIMESTAMPTZ), audit_verify_chain(TEXT)
    TO oam_gui_bff;
-- Partition creation at start-up needs ownership-level rights; the BFF's role gets them only on
-- the audit table family via a SECURITY DEFINER wrapper rather than DDL rights.
ALTER FUNCTION ensure_audit_partition(TIMESTAMPTZ) SECURITY DEFINER SET search_path = iam, pg_temp;
REVOKE UPDATE, DELETE, TRUNCATE ON audit_event FROM PUBLIC, oam_gui_bff;
