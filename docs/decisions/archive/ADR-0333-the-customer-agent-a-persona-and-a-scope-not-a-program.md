## ADR-0333: the customer agent -- a persona and a scope, not a program

**Date:** 2026-09-10. **Status:** Closed (work pushed; last citing commit 9855558 on origin/main, 2026-10-06). accepted. Item 2 of the agent roadmap.

### There is no LLM in this repository, deliberately

MCP splits the work three ways: the **server** supplies tools, the **host** supplies the model, and
the **agent** is the definition that joins them. So the customer agent is a system prompt, a tool
scope and a launcher — not a program that calls a model.

Embedding an LLM client would add a proprietary API dependency, which `CLAUDE.md` forbids, and
would tie a telecom core to a vendor's uptime for a customer-facing function. The persona lives in
`agents/customer-agent/AGENT.md`; the *enforcement* lives in the server, where an agent cannot
argue with it. That split is the point: instructions in a prompt are advisory, and the things that
must not be negotiable are checked in C++.

### The subject is pinned by the launcher, never by the agent

`launch.sh <subscriber-id>` sets `MCP_PIN_CUSTOMER_AGENT` before the process starts, and the pin is
applied at startup so it is fixed for the life of the process. Nothing sent over JSON-RPC can widen
it.

This is the single most important property of the design. An agent that could choose its own
subject could **enumerate the entire subscriber base using the same tool it legitimately uses for
one customer**, and every one of those reads would look ordinary in the audit log. The launcher
already knows whose session it is; the model never needs to.

The launcher **refuses to start without an argument** rather than defaulting to unpinned, and
`pin_subject` returns false for an unknown agent so a typo in a launcher cannot mint a new identity
with a real pin and no scope.

### What it deliberately cannot do

No `list_nf_instances`, no `get_offering` — a customer agent has no business enumerating network
functions, and catalog browsing belongs to the revenue agent. **No write tool of any kind**: it
cannot adjust a balance, credit an account or change a plan. A customer-facing agent that can move
money is a fraud vector, not a feature.

`get_recent_charges` withholds `serving_plmn` and records the withholding — it says which country
the subscriber was in, and a "what did I spend" question never needs it.

### The operating rules are the motto as instructions

Every factual claim comes from a tool result. A tool error is reported plainly, which is exactly
why `explain_charge` returns an error rather than an empty object when no decision was recorded —
"no rating decision was recorded" is the correct answer, and constructing a plausible reason for a
charge is the failure the whole design exists to prevent. If `ai_advisory` is present the agent
must say a model influenced the grant and which bound applied: a customer is entitled to know an
algorithm sized their allowance.

Three things it must answer "not available" to, rather than approximate: network conditions (no
NWDAF), contact history (not collected), and future spend (no forecasting model yet — roadmap
item 3).

### A real bug this surfaced

The startup pin loop iterated `config.value("agents", json::object()).items()`. `value()` returns a
**temporary**, and nlohmann documents that iterating `.items()` over a temporary is undefined — it
threw `invalid_iterator.214` on the first launch rather than failing to compile. Bound to a named
reference. Caught because the agent was actually launched, not because a unit test covered it.

7/7 MCP tests. Verified end to end: the launcher pin overrode the config value, a different
subscriber was refused, and an out-of-scope tool was refused.

