# Agents and AI product models: what is actually feasible here

Written in response to a direct question: can this platform support multiple agents (customer,
technical, …) from CHF and NWDAF, and which AI product models could be enabled without needing the
Phase 7 GUI?

The short answer is **yes for a customer agent and a partial operations agent today, no for
anything grounded in NWDAF**, and the useful models split cleanly by whether the training data
already exists in this system. Everything below is checked against the code and schemas, not
assumed.

## What data this platform actually holds

This is the constraint that decides everything else.

| Source | Where | Contents |
|---|---|---|
| CDRs | Doris (`nfs/chf/schema.doris.sql`) | `subscriber_identifier`, `rating_group`, `granted_total_volume`, `used_total_volume`, `granted_service_specific_units`, `reserved_cost` + currency, `serving_plmn`, `is_roaming`, `operation`, `service_type`, timestamps |
| Rolling usage | Redis (`chf::QuotaFeatureStore`) | per `SUPI`+`ratingGroup` usage window, already feeding the live quota-sizing model |
| Rating decisions | Postgres (`rating_decision`) | `tariff_id`/`version`, `rated_amount`, `currency`, `rule_fired_id`, `ai_advisory`, `acbr_is_billed`, `acbr_tax_excluded` |
| Balances | Postgres (TMF654) | bucket balance, `isShared`, `relatedParty` (family/group membership) |
| Bills | TMF678 | `CustomerBill` + `AppliedCustomerBillingRate` line items per period |
| Catalog | TMF620 | offerings, prices, `chargingScope`, `unitPooling`, rating groups |
| NF/network state | NRF registry, SMF SM contexts, Prometheus TS 28.552 counters | live NF inventory, session state, per-NF measurement families |

**Not held anywhere:** subscription termination/churn labels, payment and collections history,
customer-care contact history, per-flow or per-application traffic detail, and any NWDAF analytics
output. Models needing those are listed below as blocked, with what would have to be collected
first -- not quietly omitted.

## Agents

The architecture `CLAUDE.md` already specifies is the right one and is not yet built: an **MCP
server exposing read-only NF and charging state as tools**, read-only by default, with any
write/config action requiring explicit human approval. Agents are then thin: each is a persona plus
a restricted tool subset over that one server. That matters because it means "multiple agents" is
**one** piece of infrastructure plus per-agent tool scoping, not N integrations.

| Agent | Feasible today? | Grounded in | Notes |
|---|---|---|---|
| **Customer agent** ("how much data is left?", "why was I charged this?", "am I roaming?") | **Yes, fully** | balance buckets, CDRs, rating decisions, catalog | Every answer already exists as a queryable record. "Why was I charged this" is genuinely answerable because `rating_decision` stores the tariff, the rule that fired, and the AI advisory -- an explanation, not a guess |
| **Technical / operations agent** ("which NFs are unhealthy?", "why did this session fail?") | **Partly** | NRF registry, SM contexts, PFCP state, TS 28.552 counters | The inventory/state half works now. The *analytics* half ("is this abnormal?") needs NWDAF |
| **Revenue / commercial agent** ("which offerings underperform?") | **Yes** | rating decisions + bills + catalog | Read-only aggregation over data that exists |
| **Network-analytics agent** | **No** | — | Requires NWDAF (AnLF/MTLF), which is Phase 5 and has no directory yet |
| **Care / retention agent** | **No** | — | Needs churn labels and contact history, neither collected |

**Guardrail, non-negotiable:** subscriber-facing agents read PII (SUPI, MSISDN, spend). Tools must
be scoped per agent, every tool call audited like `aiAdvisory` already is, and no agent gets a
write path without human approval in the loop. A customer agent that can *adjust a balance* is a
fraud vector, not a feature.

## AI product models, ranked by what it would take

All of these deploy the way quota sizing already does -- a config entry plus an ONNX artifact, no
GUI involved. The Phase 7 GUI would make them *editable by non-engineers*; it is not needed to run
them.

### Buildable now, data already present

1. **Quota-exhaustion forecasting** -- "this subscriber runs out in ~6 hours." Same feature store
   and the same inference path as the live quota-sizing model; a different target variable. The
   cheapest real model to add.
2. **ARPU / RPU-driven grant and offer selection** -- revenue per subscriber per period is
   computable *today* from `rating_decision.rated_amount` plus TMF678 line items. Enables
   revenue-aware grant sizing and offer eligibility.
3. **Bill-shock / spend-anomaly detection** -- flag a subscriber whose spend rate departs sharply
   from their own history. Purely CDR + rating-decision data. High operator value, low data cost.
4. **Charging-pattern fraud signals** -- implausible roaming transitions, SIM-box-like volume and
   `serving_plmn` patterns. `is_roaming` and `serving_plmn` are already on every CDR.
5. **Shared-bundle allocation insight** -- which member of a family bucket consumes what.
   `relatedParty` on the TMF654 bucket already carries membership.

### Blocked on data this system does not collect

6. **Churn propensity** -- needs subscription termination labels. Nothing records why or when a
   subscriber leaves.
7. **Next-best-offer** -- needs purchase history; the catalog exists but take-up is not recorded.
8. **Credit risk / deposit sizing** -- needs payment and collections history, which the billing
   domain does not yet reach (delivery, dunning and payment are all unbuilt).

### Blocked on NWDAF

9. NF load prediction, 10. abnormal-behaviour detection, 11. slice SLA / service-experience
prediction, 12. energy-efficiency analytics, 13. federated learning. All are NWDAF-resident, and
NEF's AF-facing surfaces for several of them already answer 501 for exactly this reason.

## Recommendation

Build the **MCP tool server first**, read-only, then the **customer agent** on top of it. That
order is deliberate: the server is the reusable half, the customer agent is the persona with the
most complete data behind it, and doing it read-only first means the audit and scoping discipline
exists before anything gets a write path. Then **quota-exhaustion forecasting** and **spend-anomaly
detection**, which reuse the quota-sizing pipeline end to end and need no new data collection.

## Tracked commitments (user-directed, 2026-09-10)

These are **not** optional future ideas. They are committed work, blocked only on the capability
each one names, and this section exists so that when the blocker clears the agent is built rather
than forgotten.

| Agent | Blocked on | Build it when |
|---|---|---|
| **Network-analytics agent** | NWDAF (AnLF + MTLF) | NWDAF exists and serves real analytics. NEF's `AnalyticsExposure` / `ReportingNetworkStatus` routes stop answering 501 at the same moment |
| **Technical / ops agent** (full) | NWDAF for the "is this abnormal?" half | the state half can land with the MCP server; the analytics half completes when NWDAF does |
| **Care / retention agent** | churn labels + contact history, neither collected today | subscription lifecycle events are recorded and a contact-history source exists. **Collecting that data is itself the prerequisite task**, not a precondition someone else supplies |

## PII governance (user-directed, mandatory)

`aiAdvisory` already proves the pattern works: every AI-influenced rating decision records the
model, version, input vector, output, and which deterministic bound applied. **PII access needs its
own equivalent, and it is a hard requirement, not a nice-to-have.**

Every tool call that reads subscriber-identifying data -- SUPI, GPSI/MSISDN, IMEI, location,
balance, spend, bill contents -- must record:

- **who** asked (agent identity, and the human or system principal behind it),
- **what** was read (the subscriber identifier and the field classes returned),
- **why** (the tool invoked and the request that triggered it),
- **when**, and
- **what was withheld** -- a redaction is a governance event, not an absence of one.

Two rules follow and are not negotiable:

1. **No agent gets a PII read path without this audit record being written first.** If the audit
   write fails, the read fails. An unlogged PII access must be impossible, not merely discouraged.
2. **Least privilege per agent.** A technical/ops agent has no business reading a subscriber's
   bill; a customer agent has no business enumerating other subscribers. Tool scoping is per agent
   and enforced server-side, never by asking the agent politely.

This layer is built **with** the MCP server, not after it. Retrofitting audit onto a system that
already reads PII means a window where reads went unlogged, and there is no way to reconstruct
what happened in that window.

## The motto is a design constraint

**"Built by AI. Built for AI. Bound by the spec."**

The third clause governs the first two. Concretely, when an agent or model decision is being made
here: the model advises and a deterministic, spec-grounded path decides (as quota sizing already
does with its [0.5x, 2.0x] clamp); nothing an agent surfaces may be a field or value the
specification does not define; and "the AI suggested it" is never a reason to relax a guardrail.
An agent that cannot ground an answer in a record should say so rather than generate one.
