## ADR-0053: UCS (Universal Charging System) architecture -- module decomposition, three-layer design, polyglot persistence, idempotency, P1-P15 compliance

**Date:** 2026-08-10
**Status:** Accepted (architecture only -- no code in this ADR; P4.1 deliverable per
`CHARGING_PROMPT.md`).

**Context:** The user pointed this project at an updated `PROMPT.md` (now stating 15 binding
principles P1-P15) and a new `CHARGING_PROMPT.md` ("Replaces Phase 4 of PROMPT.md"), with three
explicit new mandates: (1) proper open-source DB selection for data-model persistence, (2)
CHF must be built AI-native with a full NWDAF-integration ecosystem "in later stage", (3) follow
`CHARGING_PROMPT.md`'s own sequenced sub-phases P4.1-P4.12, starting with P4.1's hard gate:
"Produce two documents, no code" before any further charging-domain implementation. This ADR is
P4.1's second deliverable (`docs/DATA_MODEL.md` is the first). Both are documentation only; no
implementation code changed as part of this ADR.

**Document-location deviation, flagged per CLAUDE.md's own disagreement rule**: `CHARGING_PROMPT.md`
literally asks for this decision at `docs/DECISIONS/0010-ucs-architecture.md` (a new per-decision
file, numbered from 0010, in a directory that doesn't exist in this repo). This project's actual,
established convention -- used for 52 prior decisions, including the two immediately above this
one in this same session -- is a single `docs/DECISIONS.md` with sequential `ADR-NNNN` sections.
Restarting a second, differently-numbered ADR series in a new location for one subsystem would
fragment traceability for no real benefit. Resolved here by keeping the established convention
(this entry, ADR-0053, in the existing file) -- flagged for the user to confirm or override, not
silently decided as a foregone conclusion.

### Module decomposition (TS 32.240 / 32.296)

Per `CHARGING_PROMPT.md` Section A, verbatim module list, each to become its own
independently-buildable binary+library per this project's existing one-NF-per-turn/standalone-
binary convention (`CLAUDE.md`'s "Every NF is an independent binary + shared library" rule extended
to these charging modules, which are NF-shaped SBI-facing components in their own right):

- **CHF** -- Charging Function, the SBI-facing 5G entity. Already exists (`nfs/chf/`,
  `Nchf_ConvergedCharging` Create/Release wired per ADR-0044/0045/0046/0050/0051) -- P4.2 extends
  it (Update, `Nchf_SpendingLimitControl`, `Nchf_OfflineOnlyCharging`), not a new module.
- **OCF** (Online Charging Function) -- containing **EBCF** (event-based) and **SBCF**
  (session-based) sub-components. New in P4.2/P4.3. Session-based charging is what E3 (Session
  Establishment) already sketches in `docs/DATA_MODEL.md`; event-based charging handles the
  one-time-occurrence blocks `docs/CHARGING_MAPPING.md` already catalogued (registration, NSSAA,
  EAS deployment, etc. -- the `Event`/TMF688 cases, not `ProductUsage`/TMF635 cases).
- **RF** (Rating Function) and **ABMF** (Account Balance Management Function) -- new in P4.3,
  correspond directly to E5/E6 in `docs/DATA_MODEL.md`.
- **CDF** (Charging Data Function, CDR generation) and **CGF** (Charging Gateway Function, CDR
  persistence + Bx) -- new in P4.4, correspond to E4.
- **Protocol Translator** (Diameter Ro/Rf/Gy, Sy, CAP/CAMEL, MAP) -- new in P4.5, a distinct
  module, not folded into CHF -- see three-layer split below.

Each module is a distinct binary/library per this project's existing convention, communicating
over internal APIs (not each other's private headers, same "NFs talk only over SBI" rule extended
internally) -- exact internal transport (in-process library calls within one CHF process vs.
separate SBI-style services per module) is an implementation decision deferred to P4.2, not fixed
here; this ADR fixes the module *boundaries*, not their wire format.

### Three-layer internal architecture

Per `CHARGING_PROMPT.md` Section A, adopted as-is:

1. **Protocol Translator Layer** -- terminates every legacy protocol (MAP, CAP/CAMEL, Diameter
   Ro/Rf/Gy/Sy, GTP') and normalizes to internal JSON. The *only* place legacy encodings exist.
   Built in P4.5. Note on `CHARGING_PROMPT.md`'s own embedded correction, carried forward
   unmodified: CDRs transport over **GTP-prime (GTP')**, TS 32.295 -- not plain "GTP" as an
   informal reference deck the brief derives from apparently states; this ADR uses GTP' throughout,
   to be verified again against TS 32.295 directly when P4.4/P4.5 write real code.
2. **Internal Processing Layer** -- 100% JSON, SBA-compliant, protocol-agnostic. Every charging
   decision happens on **one code path**, regardless of whether the request arrived from a 2G MSC
   or a 5G SMF. This is the architecture's core invariant: E3-E6 (session, usage, rating, balance)
   are implemented exactly once; `CHARGING_PROMPT.md`'s own P4.5 test requirement (identical usage
   via Gy and via Nchf must produce an identical rated result) is the conformance check for this
   invariant, deferred to P4.5, not built here.
3. **Internal DB Layer** -- polyglot, CAP-theorem-justified per domain. See below.

### Polyglot persistence, with CAP-theorem justification per domain

`docs/DATA_MODEL.md` already assigns a concrete store to each of E1-E10; this section states the
*why* (CAP-theorem trade-off) per store, not the *what* (already tabulated there):

- **PostgreSQL** (E1 Subscriber, E2 Catalog header fields, E5 Rating ledger, E6 Balance ledger,
  E7 Agreements, E8 Audit, E10 Account): **CP** choice. These are all financially/legally
  consequential records (identity, tariff publication, rated charges, balance transactions,
  contracts, audit trail, account hierarchy) where an unavailable-but-consistent read is
  preferable to a fast-but-possibly-stale one -- a rating decision made against a stale tariff
  version, or a balance debit against a stale balance, is a direct revenue/compliance defect
  (`CHARGING_PROMPT.md`'s own "charging defects are revenue and regulatory events" framing).
  Single-region strong consistency (synchronous replication within a region) is the P4.1-level
  decision; cross-region behavior under P11's active/active geo-redundancy mandate is explicitly
  **not resolved here** -- flagged as a real, open question for P4.12 (telco-grade hardening),
  since PostgreSQL's native synchronous replication does not trivially extend to active/active
  multi-region writes without an explicit topology decision (e.g. per-region primary with
  conflict-free partitioning by account/tenant, vs. a distributed-SQL layer) that this ADR does
  not pick blind.
- **Redis/Valkey** (E3 live session state, E6 hot balance): **AP-leaning, deliberately, for the
  narrow subset of state that must be low-latency and horizontally scalable under charging-request
  load** -- but `docs/DATA_MODEL.md` already flags that E6's hot balance sits in real tension with
  the entity's own explicit strong-consistency requirement, which this ADR resolves as: hot balance
  in Redis is a **cache/working-set copy backed by PostgreSQL's ledger as the durable source of
  truth**, not the authoritative record -- every debit is committed to the PostgreSQL
  `BalanceTransaction` ledger (E6) as part of the same idempotent operation (see below) that
  updates the Redis working value, so a Redis failure loses availability of the hot path, never
  correctness of the durable ledger. E3's session state has no such tension (a lost in-flight
  session on Redis failure is recoverable by re-deriving from the last `ChargingDataRequest`
  the CP-side SMF/AMF will retry, per SBI's own retry semantics) -- genuinely AP-appropriate there.
- **ClickHouse** (E4 CDR/usage analytics): **AP-leaning**, appropriate because CDR analytics reads
  tolerate eventual consistency (a dashboard being seconds behind is not a revenue-integrity
  issue) while the *write* path's duplicate/gap detection (E4's own explicit requirement) is
  handled at ingestion, not by ClickHouse's own consistency model.
- **Distributed FS / object store** (E4 CDR archive, E7 roaming CDR files): **AP**, appropriate for
  immutable, retention-governed archival where availability of historical reads matters more than
  any single write's immediate global visibility.
- **JSON/NoSQL for "flexible product definitions"**: `docs/DATA_MODEL.md`'s E2 section resolves
  this as PostgreSQL `jsonb` columns rather than a separate NoSQL engine -- flagged there as a
  deliberate simplification (one database technology satisfies both the relational and
  flexible-schema needs of the product catalog), not an oversight; revisit if a real requirement
  for document-store-specific features (e.g. full-text search across product definitions at a
  scale `jsonb` GIN indexes can't serve) emerges in P4.7.

### Idempotency and exactly-once accounting design

`CHARGING_PROMPT.md`'s principle 5 ("duplicate Charging Data Requests, retries and partitions must
never double-charge or lose usage... designed in from the first line of code") is addressed
structurally, not as a later hardening pass:

- **Idempotency key**: the `(charging_data_ref, invocation_sequence_number)` pair --
  `docs/DATA_MODEL.md`'s E3 sketch, and *already real, shipped code* in this repo
  (`ChargingDataInvocationSeqStore`, ADR-0051) -- is the exactly-once key threaded through E3
  (session), E4 (`UsageRecord.dedup_key`), E5 (rating decisions keyed to the session+sequence that
  produced them), and E6 (`BalanceTransaction.idempotency_key`). One key, reused end-to-end, rather
  than each layer minting its own.
- **Balance mutation** (the highest-consequence case): every debit/credit is a single logical
  operation -- (1) check `BalanceTransaction.idempotency_key` doesn't already exist, (2) apply the
  delta to `Balance` under its optimistic-concurrency `version` token (`docs/DATA_MODEL.md`'s E6
  sketch), (3) append the `BalanceTransaction` row -- committed as one PostgreSQL transaction, with
  the Redis hot-balance cache updated only after that transaction commits (cache-updated-after-
  durable-write, not before, so a crash between the two never leaves Redis ahead of the ledger).
  This is a design decision fixed here; P4.3 implements it.
- **Partition recovery**: E3's session state living in Redis (see above) is explicitly reconciled
  against the CP-side NF's own retry behavior (SBI's existing retry/idempotent-request semantics,
  already relied on elsewhere in this codebase, e.g. `ChargingDataInvocationSeqStore`'s own
  "no NF has cause to retry a successful `Create`... but *does* retry a failed one" reasoning from
  ADR-0051) rather than inventing a new distributed-consensus mechanism for session recovery.

### AI-native CHF / NWDAF ecosystem -- acknowledged and phased, not built here

Per the user's explicit mandatory instruction ("CHF MUST be AI native system with complete
ecosystem... integrate the pipeline with NWDAF components in later stage") and
`CHARGING_PROMPT.md` Section B's already-complete seven-angle specification (advisory-only signals
inside the charging decision; bidirectional CHF<->NWDAF; CHF<->PCF N28 predictive policy loop;
product/customer intelligence; charging correctness under AI/ML/SON network change; AIOps for the
platform; agentic/MCP layer) plus its mandatory model-governance rules (MLflow versioning, full
decision logging, drift monitoring via `Nnwdaf_MLModelMonitor`, per-model kill switch, bias/
fairness review, hard latency budget with deterministic fallback, and the explicit line -- "This
model informs the decision. The deterministic engine makes it.") -- **nothing here needs
re-specifying**; Section B already is the specification. This ADR's role is to record that:

1. `docs/DATA_MODEL.md`'s E5 (`RatingDecision.ai_advisory`) and E8 (`AuditRecord.ai_advisory_ref`)
   schema fields already reserve the governance-required logging fields (model id/version, score,
   reason codes) so P4.8/P4.9 don't have to retrofit them onto an already-shipped schema.
2. The architectural boundary is: every AI signal is **advisory input to a deterministic rule**
   (`RatingDecision.rule_fired_id` is what actually acts; `ai_advisory` is logged alongside it,
   never instead of it) -- enforced structurally by this schema shape, not just by policy.
3. Implementation is explicitly deferred: P4.8 (online-path AI: predictive quota sizing, adaptive
   reauth, fraud scoring, bill-shock prediction) and P4.9 (NWDAF consumption/exposure, N28
   predictive policy loop, offer/tariff intelligence) are the "later stage" the user's instruction
   refers to -- not started as part of this ADR, consistent with the user's own phrasing.
4. **Verification finding still owed, not yet done**: `CHARGING_PROMPT.md` Section B itself
   requires verifying in TS 23.288 whether CHF is a standardized NWDAF data source before writing
   any exposure code ("VERIFY FIRST... Do not silently invent standard behaviour") -- this is
   explicitly assigned to P4.9's own prompt text, not this ADR; recorded here so it isn't lost.

### Compliance statement against P1-P15

| # | Principle | Status at this ADR |
|---|---|---|
| P1 | OSI-approved open source only | PostgreSQL (PostgreSQL License), Redis/Valkey (Valkey: BSD-3-Clause -- note: Redis itself relicensed to SSPL/RSALv2 from v7.4, **Valkey is the OSI-approved fork this project must use, not Redis**, flagged explicitly here so P4.3 doesn't default to the wrong package), ClickHouse (Apache-2.0) -- all satisfy P1. No license file/vcpkg entries added yet -- this ADR is documentation only. |
| P2 | 3GPP-standards-based | This ADR's own top-level finding (NRM/IOC gap) is a direct P2 compliance question -- flagged to the user in `docs/DATA_MODEL.md` rather than silently deviating. |
| P3 | 100% container/K8s, multi-cluster | Not addressed by this ADR (architecture-level document; container/Helm artifacts are a later, per-module deliverable, same pattern as every existing NF). |
| P4 | AI-based product/customer real-time algorithms | Acknowledged and phased into P4.8/P4.9, per the AI-native section above -- not built yet. |
| P5 | 100% TM Forum Open API/SID compliance | `docs/DATA_MODEL.md` maps every entity to a real, confirmed TMF resource where one exists (E9 is the one entity where CHARGING_PROMPT.md's own chart conflates a SID entity with the ODA API layer -- flagged there, not forced). |
| P6 | 100% 3GPP-compliant data models + rating engine, SID+NRM+IOC | Partially blocked on the NRM/IOC finding above -- flagged as an open question, not silently declared compliant. |
| P7 | Product/tariff/policy is data, never code | Directly designed in: E2's `ProductOfferingPrice`/`prodSpecCharValueUse` (already the existing, approved TMF620 extension direction) and E6's `Balance.rounding_rule` are both data fields, not code paths. |
| P8 | Predictive auto-scaling | Not addressed by this architecture-level ADR; belongs to P4.12 (telco-grade hardening) and this project's existing K8s/Helm conventions once built. |
| P9 | Full CI/CD | This project's existing CI pipeline (`.github/workflows/ci.yml`, hardened this same session) already covers build/sanitize/lint for whatever charging-module code P4.2+ adds -- no new CI work implied by this ADR itself. |
| P10 | Performance/resource efficiency, benchmarked | Not addressed here -- ADR-0049's own disclosure ("zero benchmarking of any kind has been performed") still stands; this ADR does not change that. |
| P11 | Geo-redundant active/active, proven RPO/RTO | Explicitly flagged above (PostgreSQL cross-region topology) as unresolved, deferred to P4.12, not assumed solved by picking PostgreSQL. |
| P12 | Business-level alarming | `docs/DATA_MODEL.md`'s E4 (CDR sequence-gap alarm) and E7 (settlement dispute exposure) sections already name specific business-alarm conditions -- not wired to any alerting implementation yet. |
| P13 | Charging correctness under AI/ML/SON network change | E3's schema notes network-condition attribution as a session-establishment concern; not implemented -- belongs to P4.9's Angle 5. |
| P14 | Retention-driven auto-archival | E4/E7's object-store archival assignment is the storage decision; the retention-rule automation itself is not built -- belongs to P4.4/P4.12. |
| P15 | Protocol-level spike protection/TPS governance | Explicitly named as P4.5's own deliverable ("Implement per-protocol TPS spike protection here"); not addressed by this ADR. |

**Honest summary**: this ADR satisfies P4.1's own scope (architecture + compliance statement, no
code) -- it does not itself make the system P1-P15-compliant; most rows above are "acknowledged,
deferred to a named future P4.x session" rather than "done." That is the correct state for a
no-code architecture document, consistent with this project's standing "never describe output as
carrier-grade until it has passed conformance and soak testing" rule.

**Consequence:** P4.1's gate is satisfied; the already-approved TMF620/PostgreSQL product-catalog
extension work (paused behind this gate) may resume, now explicitly grounded in E2's persistence
assignment above (PostgreSQL, `jsonb` for `prodSpecCharValueUse`) rather than an independent,
earlier-session guess. P4.2 (CHF core extension) is the next `CHARGING_PROMPT.md`-sequenced session
after that.

**Update, same date:** all five of `docs/DATA_MODEL.md`'s open questions have since been resolved
or explicitly deferred with a stated reason (user: "go ahead with recommended options") --
notably, two follow-up TMF fetches (TMF654, TMF632) were done for real rather than left as TODOs,
finding TMF654's real `Bucket`/`AccumulatedBalance` balance-query resources (E6) and correcting
E10's account-hierarchy field from an unconfirmed guess (`partyRelationship`) to the real
`Organization.organizationParentRelationship`/`organizationChildRelationship` mechanism. See
`docs/DATA_MODEL.md`'s "Open questions" section for the full resolution detail. P4.1 is closed.

---

