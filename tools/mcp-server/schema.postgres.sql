-- ADR-0331: the PII access audit trail for the MCP tool server.
--
-- User-directed and mandatory: every tool call that reads subscriber-identifying data records who
-- asked, what was read, why, when, and what was withheld. Modelled on rating_decision.ai_advisory
-- (ADR-0074), which already proved that recording the reasoning behind a decision is what makes it
-- reviewable afterwards.
--
-- The hard rule this table exists to enforce: if the audit write fails, the READ fails. An
-- unlogged PII access must be impossible rather than merely discouraged, which is why the server
-- writes here BEFORE returning any subscriber data to a caller.

CREATE TABLE IF NOT EXISTS pii_access_audit (
    id                TEXT PRIMARY KEY,
    -- WHO. agent_id is the persona (customer-agent, ops-agent); principal is the human or system
    -- on whose behalf it acted. Both, because "the agent did it" is not an accountable answer.
    agent_id          TEXT NOT NULL,
    principal         TEXT NOT NULL,
    -- WHY. The tool invoked and the caller's own request id, so an access can be traced back to
    -- the question that caused it.
    tool_name         TEXT NOT NULL,
    request_id        TEXT,
    -- WHAT. The subscriber touched, and the CLASSES of field returned rather than the values --
    -- an audit log that copies the PII it is auditing doubles the exposure it exists to control.
    subject_id        TEXT NOT NULL,
    field_classes     JSONB NOT NULL DEFAULT '[]',
    -- WHAT WAS WITHHELD. A redaction is a governance event, not an absence of one: an empty
    -- array means "nothing withheld", never "not recorded".
    withheld_classes  JSONB NOT NULL DEFAULT '[]',
    -- Outcome, so a denied access is as visible as a granted one. Scope denials are the signal
    -- that an agent is reaching beyond its remit.
    outcome           TEXT NOT NULL CHECK (outcome IN ('granted', 'denied_scope', 'denied_policy')),
    accessed_at       TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS pii_access_audit_subject_idx ON pii_access_audit (subject_id, accessed_at DESC);
CREATE INDEX IF NOT EXISTS pii_access_audit_agent_idx   ON pii_access_audit (agent_id, accessed_at DESC);
