## ADR-0331: the MCP tool server, and PII governance built in from commit one

**Date:** 2026-09-10. **Status:** accepted. Item 1 of the user-directed agent roadmap.

`CLAUDE.md` proposed this and it was never built: *"an MCP server exposing read-only NF state and
analytics as tools. Read-only by default; any write/config action requires explicit human
approval."* This is that server, and it is the reusable half of the agent plan in
`docs/AI_AGENTS_AND_MODELS.md` — **multiple agents are one server plus per-agent scoping**, not
several integrations.

Transport is JSON-RPC 2.0 over stdio (MCP's own default): no port, no TLS termination, no auth
layer of its own. The process boundary is the trust boundary.

### Two properties that are structural, not procedural

**There is no write path.** Not "writes are gated" — no tool mutates anything. That is the honest
way to satisfy the human-approval rule in v1: there is nothing to approve. A gated write path
would require trusting the gate; an absent one does not.

**A PII read that cannot be audited does not happen.** The audit row is committed *before* the
answer leaves the process, and a failed audit write refuses the read. The server also **refuses to
start** when the audit database is unreachable — otherwise there is a window in which it can serve
subscriber data with nowhere to log it, which is precisely the state the requirement exists to
prevent.

### The audit records classes, not values

`pii_access_audit` stores who asked (agent **and** the principal behind it — "the agent did it" is
not an accountable answer), which tool and request, whose data, the **field classes** returned,
the classes **withheld**, and the outcome. Never the values: an audit log that copies the PII it
audits doubles the exposure it exists to control. A redaction is recorded as an event, so a
withheld field is visible rather than being an absence nobody can distinguish from "not asked
for". Denials are audited too — an agent reaching past its remit is exactly the signal the table
exists to surface.

### Failure directions were chosen deliberately, and are what the tests assert

- An agent with **no `tools` array can call nothing**, not everything. A misconfigured agent must
  do nothing.
- An **unknown agent id is denied**, never defaulted — otherwise a typo silently becomes a new,
  unscoped identity.
- A **pinned agent is bound to one subscriber**, which is what stops a customer agent from walking
  the subscriber base with the same tool it legitimately uses for its own user.
- `AnUnwritableAuditRefusesRatherThanDegrading` points the store at a dead database and asserts
  refusal. A working audit proves the happy path; only this proves the guarantee.

Scopes are configuration (`config/mcp-server.json`), not code, for the same reason PCF's
`policy_counter_actions` are: narrowing an agent's remit is an operator decision, and making it a
rebuild guarantees it happens late or never.

### The five v1 tools, and why these

`get_balance`, `get_recent_charges`, `explain_charge` (PII); `get_offering`, `list_nf_instances`
(operator domain). Each answers from a record this system already holds, so none can fabricate.

`explain_charge` is the one that matters for honesty: **"why was I charged this?" is answerable
from `rating_decision`** — tariff, version, the rule that fired, and the AI advisory when a model
influenced the grant. That is an explanation from records, not a language model reconstructing a
bill. It is also the motto applied: the model advises, the record decides, and a tool that cannot
ground an answer returns an error rather than inventing one.

`list_nf_instances` is deliberately the ops-agent half that is **not** blocked on NWDAF: live NF
inventory works today; "is this abnormal?" does not exist and is not pretended to.

### Verified against a running server, not only unit-tested

`tools/list` returns only an agent's own tools; `ops-agent` calling `get_balance` is refused; and
that refusal appears in `pii_access_audit` as `denied_scope` with agent, principal, tool and
subject. 6/6 governance tests pass.

