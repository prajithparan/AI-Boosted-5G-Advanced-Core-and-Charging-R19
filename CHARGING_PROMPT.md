# CHARGING_PROMPT.md — Universal Charging System (UCS) with AI-Enabled CHF

> **Replaces Phase 4 of PROMPT.md.** Save this beside `PROMPT.md`.
> Paste **Section A** once as context, then run **Section C** prompts one per session.
> Derived from *"An Open Source Universal Charging System (UCS) for All-G Network"*
> (Prajith Paran) and the high-level data-model entity chart, with 3GPP
> specifications as the authority wherever they and the reference material differ.
> Principles P1-P15 in `PROMPT.md` are binding on everything in this file.

---

## SECTION A — Charging brief (paste once)

```
=== WHY THIS SUBSYSTEM EXISTS ===
The open-source telecom ecosystem has reference implementations for RAN (O-RAN),
core (Magma, free5GC, Open5GS, SD-Core) and orchestration (ONAP) — but no
credible open-source charging system. Charging is the highest-cost, most
vendor-locked, least standardised part of the operator stack. This subsystem
fills that gap: a Universal Charging System (UCS) that is 100% open source,
100% 3GPP-conformant, and TM Forum SID / Open API compliant.

=== SCOPE: ALL-G, NOT 5G-ONLY ===
One converged system serves every generation:
  OCS (Online Charging System)    -> 2G / 3G / 4G / VoLTE / 5G-NSA
       via MAP, CAP (CAMEL), Diameter Ro / Rf / Gy / Sy
  CCS (Converged Charging System) -> 5G-SA
       via SBI: Nchf_ConvergedCharging, Nchf_SpendingLimitControl,
                Nchf_OfflineOnlyCharging
  Both feed the Billing Domain over Bx. The BD itself is OUT of scope.

Modules per TS 32.240 / 32.296:
  CHF  Charging Function — the SBI-facing 5G entity
  OCF  Online Charging Function, containing EBCF (event based) and SBCF (session based)
  RF   Rating Function       | ABMF Account Balance Management Function
  CDF  Charging Data Function (CDR generation)
  CGF  Charging Gateway Function (CDR persistence + Bx to BD)

=== THREE-LAYER INTERNAL ARCHITECTURE ===
1. Protocol Translator Layer — terminates every legacy protocol (MAP, CAP/CAMEL,
   Diameter Ro/Rf/Gy/Sy, GTP') and normalises to internal JSON. The ONLY place
   legacy encodings exist.
2. Internal Processing Layer — 100% JSON, SBA-compliant, protocol-agnostic.
   Every charging decision happens here on ONE code path, whether the request
   arrived from a 2G MSC or a 5G SMF. This single-code-path property is the core
   design win — do not compromise it for convenience.
3. Internal DB Layer — polyglot persistence, CAP-theorem justified per domain.

Integration nodes:
  Legacy : MSC, IMS/TAS, SMSC, MMSC, GGSN/SGSN, PGW/SGW, PCRF
  5G-SA  : SMF (N40), PCF (N28), AMF (N41/N42), SMSF, IMS/TAS, NRF, NSSF
  Plus network slice management for slice-aware charging.

=== HIGH-LEVEL ENTITY DATA MODEL (AUTHORITATIVE) ===
These ten entities are the required top level of the charging data model. Each
must map onto TM Forum SID entities AND 3GPP 5G NRM / IOC elements, and each must
appear in docs/CHARGING_MAPPING.md.

E1. SUBSCRIBER MANAGEMENT
    Subscriber-specific information: identification data, service preferences,
    billing details, bill cycles. Prepaid and postpaid in one model.

E2. SERVICE CATALOG + CHARGING PLANS
    Available services, descriptions, pricing, and associated charging policies.
    CHARGING PLANS define how services are charged: rate plans, rating
    mechanisms, pricing models. Maps to SID ProductOffering / ProductPrice /
    ProductSpecification. Per principle P7 this is DATA, never code — a new
    tariff must be a catalogue entry, never a recompile.

E3. SESSION ESTABLISHMENT
    Creates and manages charging sessions; tracks session start and end times;
    N40 / N28 session management; maintains protocol session details; provides
    service units derived from customer profiles. Must be idempotent and
    recoverable across restarts and network partitions.

E4. USAGE RECORDS (CDR) — CDF FUNCTION
    Captures real-time data usage, call duration, SMS counts, and other
    service-specific usage. Supports standardised 3GPP CDR formats (TS 32.298)
    AND custom formats. Duplicate detection and gap detection mandatory.

E5. RATING FUNCTION (RF)
    Online charging: real-time rating for data usage and voice calls.
    Offline charging: batch processing for bulk SMS, TAP-based roaming records,
    and other delayed charging. Deterministic and reproducible (see principles).

E6. BALANCE MANAGEMENT (ABMF)
    Maintains account balances for prepaid and postpaid subscribers; applies
    recharges, debits and incurred charges. Multi-balance (main, bonus,
    promotional) with explicit currency and rounding rules. Balance updates
    require strong consistency — design for it and say so in the ADR.

E7. ROAMING AND INTERCONNECT AGREEMENTS
    Charging agreements with other operators for roaming; interconnect
    agreements for interconnect services; plus reporting and analytics over both.
    Roaming settlement uses GSMA formats (TAP3 for transferred accounts, RAP for
    rejects/returns, NRTRDE for near-real-time fraud exchange). These are GSMA
    documents behind membership: DO NOT quote their document numbers, versions or
    field layouts from memory. Ask me to supply the specifications, and until
    then implement behind a clearly marked interface with a stub codec.

E8. SECURITY
    Secure communication protocols protecting charging and subscriber data;
    authentication and authorization ensuring the integrity of charging requests
    and subscriber access; secure integration interfaces. TLS 1.3 / mTLS on every
    interface, OAuth2 per TS 33.501 on SBI, encryption at rest for subscriber and
    balance data, no subscriber identifiers in clear-text logs, full audit trail
    on every balance or tariff mutation.

E9. FRONT-END LAYER / API GATEWAY — TM FORUM ODA APIs
    Front-end user enablement for system configuration and service creation,
    including a rule-engine language for expressing charging and rating logic
    without code. All external exposure through one JSON API gateway layer.

E10. MASTER MODEL -> CONSUMER | ENTERPRISE
    A single MASTER MODEL specialises into a CONSUMER model and an ENTERPRISE
    model. This is a first-class structural requirement, not a later variant:
      - Consumer: individual subscriber, single account, personal balances.
      - Enterprise: account and party HIERARCHIES (parent company, subsidiary,
        department, employee), shared and pooled quotas, split billing (corporate
        vs personal usage on one subscription), bulk provisioning, contractual
        SLAs, and slice-as-a-product / private-5G charging.
    Design the hierarchy, sharing and split-billing semantics into the core model
    from the first schema. Retrofitting account hierarchy onto a flat consumer
    model is one of the most expensive mistakes in charging systems — if you find
    yourself modelling only the consumer case, STOP and tell me.

=== SPECIFICATION AUTHORITY (verify every one; quote none from memory) ===
  TS 32.240 charging architecture and principles
  TS 32.255 5G data connectivity   | TS 32.256 connection and mobility (AMF)
  TS 32.260 IMS  | TS 32.270 MMS   | TS 32.274 SMS | TS 32.275 MMTel
  TS 32.290 5G charging services   | TS 32.291 5G charging API (Nchf)
  TS 32.295 CDR transfer (GTP')    | TS 32.296 OCS applications and interfaces
  TS 32.297 CDR file format and transfer (Bx)
  TS 32.298 CDR parameter description
  TS 32.299 Diameter charging applications (Ro / Rf)
  TS 29.594 Nchf_SpendingLimitControl | TS 29.219 Sy | TS 29.078 CAP (CAMEL)
  TS 28.201 / 28.202 charging NRM  | TS 28.541 5G NRM
  GSMA: TAP3 / RAP / NRTRDE — supplied by me, never quoted from memory
NOTE: the UCS reference deck says CDRs are transported over "GTP". The correct
protocol is GTP-prime (GTP'), TS 32.295. Verify and use GTP'.
NOTE: the deck states Nnrf_AccessToken is "not used for CHF". Verify against
TS 33.501 whether the CHF needs consumer-side tokens (e.g. for NRF discovery or
inter-PLMN charging over N41/N42) before designing it out. Report your finding.

=== CHARGING PRINCIPLES (charging defects are revenue and regulatory events) ===
1. MONEY IS DETERMINISTIC. The rating engine is a deterministic, auditable,
   reproducible function of (usage, product, tariff, balance, time). Same inputs
   -> same charge, forever, provably.
2. EVERY CHARGE IS EXPLAINABLE. Reconstruct which rule fired, which tariff
   version applied, which balance was debited, which model (if any) advised, and
   who or what authorised it.
3. NO SILENT AI IN THE MONEY PATH. Models predict, advise, flag and pre-compute;
   models NEVER solely change a billable amount or terminate a session.
4. TELCO-GRADE per principles P11, P12, P15 in PROMPT.md.
5. IDEMPOTENCY AND EXACTLY-ONCE ACCOUNTING. Duplicate Charging Data Requests,
   retries and partitions must never double-charge or lose usage. Designed in
   from the first line of code, not hardened later.

=== POLYGLOT PERSISTENCE (CAP-theorem driven, justify each in an ADR) ===
  RDBMS (PostgreSQL)        — subscriber, product catalogue, tariff, invoice
  In-memory (Redis/Valkey)  — live sessions, quota/reservation state, hot balance
  JSON store / NoSQL        — flexible product definitions, mediation records
  TSDB (Prometheus)         — metrics; Apache Doris for CDR/usage analytics
                               (ADR-0192)
  Distributed FS / object   — CDR archive, long-term retention (P14 auto-archival)
Balance updates require strong consistency. Say so, and design for it.
```

---

## SECTION B — AI-enabled CHF (across the entire 5G core)

```
The CHF is not "a charging box with an ML sidecar". It is one of the few NFs that
sees usage, money, product and behaviour together, which makes it the richest AI
vantage point in the core. Implement AI across ALL seven angles.

--- ANGLE 1: INSIDE THE CHARGING DECISION (advisory, never authoritative) ---
  a) Predictive quota sizing — learn per-subscriber, per-rating-group consumption
     velocity; size granted units to minimise Nchf round-trips while bounding
     credit risk. Measure and report the reduction in Update requests.
  b) Adaptive reauthorisation triggers — predict which triggers matter for a
     given session profile instead of firing a static set.
  c) Real-time fraud and abuse detection — SIM-box, roaming fraud, subscription
     fraud, unusual velocity. Output is a SCORE + reason codes; enforcement is a
     deterministic rule consuming that score.
  d) Bill-shock and credit-risk prediction — forecast end-of-cycle spend; drive
     proactive notification and spending-limit policy.
  e) CDR-stream anomaly detection for revenue assurance.

--- ANGLE 2: CHF <-> NWDAF (BOTH DIRECTIONS) ---
  As CONSUMER: subscribe via Nnwdaf_EventsSubscription / AnalyticsInfo to
  analytics that change charging behaviour — candidates to verify in TS 23.288:
  Slice Load Level, NF Load, Observed Service Experience, UE Mobility,
  UE Communication, Abnormal Behaviour, User Data Congestion, QoS Sustainability,
  Network Performance, Dispersion (R18). Use for slice-aware and
  experience-aware charging, congestion-sensitive quota, fraud corroboration.
  As DATA SOURCE: expose charging-derived features (usage velocity, rating-group
  mix, spend patterns) via DCCF / ADRF where permitted.
  VERIFY FIRST in TS 23.288 whether the CHF is a standardised NWDAF data source
  and which analytics IDs list it. If not standardised, implement as an
  explicitly-marked non-standard extension behind a feature flag and document it.
  Do not silently invent standard behaviour.

--- ANGLE 3: CHF <-> PCF POLICY LOOP (N28) ---
  Policy counters and spending limits informed by PREDICTED spend, not only
  realised spend. AI proposes thresholds; PCF policy remains the deterministic
  enforcement point; threshold changes are human-approved.

--- ANGLE 4: PRODUCT, OFFER AND CUSTOMER INTELLIGENCE (entities E2, E10) ---
  a) Offer/tariff design copilot over the SID product catalogue — simulate a
     proposed tariff against historical usage BEFORE it goes live.
  b) Segmentation, churn propensity, next-best-offer, customer lifetime value —
     for both the CONSUMER and ENTERPRISE models (enterprise needs account-level
     and hierarchy-level analytics, not just per-subscriber).
  c) Personalised real-time offers at quota exhaustion — the highest-converting
     moment in telecom — served in-session.
  All BSS-side model outputs are recommendations to a human or a rules engine.

--- ANGLE 5: CHARGING UNDER AI/ML/SON NETWORK CHANGE (principle P13) ---
  When radio/transport/core reconfigures itself — slice reconfiguration, R19
  energy-saving states, NTN vs terrestrial path, edge vs central breakout —
  charging must remain correct and attributable. Charging context must capture
  WHICH network condition produced the usage, and the rating engine must be able
  to price differently on that basis.

--- ANGLE 6: AIOps FOR THE PLATFORM ITSELF ---
  a) TPS spike prediction and pre-emptive autoscaling — make principle P15
     predictive, not merely reactive.
  b) Mediation error prediction and CDR pipeline anomaly detection.
  c) Revenue-leakage detection reconciling network usage vs rated usage vs billed
     usage; alert on divergence with a quantified exposure figure.
  d) Capacity forecasting for quota and balance hot partitions.

--- ANGLE 7: AGENTIC / MCP LAYER ---
  MCP server exposing charging state as tools: natural-language CDR and usage
  queries, tariff explanation ("why was this subscriber charged X?"), dispute
  investigation, offer simulation. READ-ONLY by default. Any write — tariff
  change, balance adjustment, session termination — requires explicit human
  approval, shows the exact change before execution, and is written to an
  immutable audit log.

=== MODEL GOVERNANCE (mandatory for a money system) ===
  - Every model versioned in MLflow with full training-data lineage.
  - Every AI-influenced decision logs: model id + version, input feature vector,
    output score, reason codes, and the deterministic rule that acted on it.
  - Drift monitoring wired to Nnwdaf_MLModelMonitor where the model serves NWDAF.
  - Kill switch per model: disabling any model degrades to deterministic default
    behaviour with NO charging outage and NO revenue impact.
  - Bias and fairness review for anything touching credit, throttling or offers —
    these decisions face consumer-protection scrutiny in most markets.
  - Online inference is in-process C++ (ONNX Runtime) with a hard latency budget;
    if inference exceeds budget the deterministic path proceeds WITHOUT it.
    Charging must never block on a model.

=== THE LINE THAT MUST NOT BE CROSSED ===
State this in code comments and docs at every AI integration point:
  "This model informs the decision. The deterministic rating engine makes it."
If you are ever about to write code where a model output directly sets a monetary
amount, grants units, or terminates a session without a deterministic rule in
between — STOP and ask me first.
```

---

## SECTION C — Sequenced prompts (one per session)

### P4.1 — Entity model + architecture ADR (no code)
```
Read PROMPT.md and CHARGING_PROMPT.md. Produce two documents, no code:
 1. docs/DATA_MODEL.md — the ten high-level entities E1-E10 expanded into a
    concrete schema sketch, showing for each: its SID entity mapping, its 3GPP
    NRM/IOC mapping, its persistence store, and its consistency requirement.
    Model the MASTER -> CONSUMER | ENTERPRISE specialisation explicitly, with
    account hierarchy, pooled quota and split billing in the enterprise branch.
 2. docs/DECISIONS/0010-ucs-architecture.md — module decomposition
    (CHF/OCF/EBCF/SBCF/RF/ABMF/CDF/CGF), the three-layer internal split, the
    polyglot persistence choices with CAP-theorem justification per domain, the
    idempotency and exactly-once accounting design, and a compliance statement
    against principles P1-P15.
Flag every place where the entity chart and 3GPP disagree, and ask me.
```

### P4.2 — CHF core (5G-SA)
```
Implement the CHF per TS 32.290 / 32.291 / 29.594: Nchf_ConvergedCharging
(Create/Update/Release/Notify), Nchf_SpendingLimitControl (Subscribe/Unsubscribe/
Notify), Nchf_OfflineOnlyCharging (Create/Update/Release), wired to N40 (SMF),
N28 (PCF), N41/N42 (AMF). Map each service operation to its Charging Data
Request/Response [Initial | Update | Termination | Event] per TS 32.290.
Implement entity E3 (Session Establishment) as the session core. Procedure list
for approval first. Include the Nnrf_AccessToken finding from Section A.
```

### P4.3 — Rating engine (E5) + ABMF (E6)
```
Implement the Rating Function and Account Balance Management Function.
Deterministic, reproducible, fully audited: tariff versioning, rating groups,
service identifiers, unit reservation and refund, multi-balance (main, bonus,
promotional), explicit currency and rounding rules. Every rating decision emits
an audit record sufficient to reconstruct the charge. Property-test: same inputs
-> same charge, across restarts and across versions. Strong consistency on
balance mutation — prove it under concurrent debit tests.
```

### P4.4 — CDF / CGF / Bx (E4)
```
Implement CDR generation per TS 32.298, file format and transfer per TS 32.297,
GTP' transport per TS 32.295, persistence to Apache Doris (ADR-0192)
plus distributed-FS or object-store archive with retention-driven
auto-archival (principle P14). Bx to
the Billing Domain (BD out of scope — provide a test consumer). Include duplicate
detection, gap detection, and support for custom CDR formats alongside 3GPP.
```

### P4.5 — Protocol translator layer, All-G
```
Implement the legacy termination layer: Diameter Ro/Rf/Gy (TS 32.299), Sy
(TS 29.219), CAP/CAMEL (TS 29.078) and MAP, each normalising to the SAME internal
JSON representation used by the 5G path. Prove the single-code-path property with
a test that charges an identical usage event arriving via Gy and via Nchf and
asserts an identical rated result. Fuzz every decoder. Implement per-protocol TPS
spike protection here (principle P15).
```

### P4.6 — SID mapping table (REVIEW GATE — no code)
```
Produce docs/CHARGING_MAPPING.md only: for each entity E1-E10, the mapping
3GPP CDR field (TS 32.298) -> TM Forum SID entity -> TMF Open API resource, plus
the merged SID + 5G NRM/IOC model. Show the CONSUMER and ENTERPRISE variants
where they differ. Mark every ambiguous mapping TODO with the specific question.
Cite each TMF specification number; mark "unverified" where you cannot confirm.
I will adjudicate before any mapping code is written.
```

### P4.7 — BSS layer + master/consumer/enterprise model (E1, E2, E9, E10)
```
Implement the SID-aligned BSS layer per the APPROVED mapping: TMF620, 622, 632,
633, 635, 637, 638, 639, 651, 654, 666, 676, 678, 688, 727, aligned to ODA
component boundaries. Implement the
MASTER -> CONSUMER | ENTERPRISE specialisation with account hierarchy, pooled and
shared quota, and split billing. Product, tariff and charging-plan definition
must be fully data-driven and configurable via the front-end layer and rule
engine (principle P7) — demonstrate by creating a new tariff with NO code change
and NO restart. End-to-end test: session -> CDR -> rated usage -> applied billing
rate -> customer bill, for both a consumer and an enterprise hierarchy.
```

### P4.8 — AI layer, part 1 (online path)
```
Implement Angles 1 and 6: predictive quota sizing, adaptive reauthorisation
triggers, fraud scoring, bill-shock prediction, predictive TPS spike protection,
mediation error prediction. ONNX Runtime in-process C++ inference with a hard
latency budget and deterministic fallback. Every AI-influenced decision logged
per the governance rules. Report measured impact: Nchf round-trip reduction,
fraud detection rate with false-positive counts. Do not report a metric you have
not measured.
```

### P4.9 — AI layer, part 2 (NWDAF + policy + BSS)
```
Implement Angles 2, 3, 4 and 5: NWDAF consumption and (verified) exposure, the
N28 predictive policy-counter loop, offer/tariff simulation and next-best-offer
over the SID catalogue for both consumer and enterprise, and charging-context
capture for AI/ML/SON-driven network changes. Deliver the TS 23.288 CHF
data-source verification finding FIRST, before writing exposure code.
```

### P4.10 — Agentic layer + revenue assurance
```
Implement Angle 7 (MCP server, read-only by default, human-approved writes,
immutable audit log) and revenue-leakage reconciliation (network usage vs rated
vs billed, with quantified exposure alerting and business-level alarms per P12).
```

### P4.11 — Roaming and interconnect settlement (E7)
```
Implement the roaming and interconnect agreement model: agreement terms,
partner-specific rating, interconnect settlement, and reporting/analytics.
Roaming file exchange (TAP3 / RAP / NRTRDE) behind a clearly marked codec
interface — ask me for the GSMA specifications before implementing any format,
and ship a stub codec plus tests until they are supplied. Never guess a GSMA
field layout.
```

### P4.12 — Telco-grade hardening
```
Geo-redundant active/active across two simulated data centres (P11), K8s
autoscaling (P8, P10), per-protocol TPS spike protection validated under load
(P15), retention-driven archival validated (P14), chaos tests (kill a node
mid-session, partition the balance store) asserting no double-charge and no lost
usage, documented RPO/RTO, and a full compliance matrix against P1-P15.
Produce an honest gap list: what would still block a production deployment.
```

---

## SECTION D — Note on the "production carrier core" target

`PROMPT.md` targets production-INTENT architecture: every decision must survive a
carrier deployment, delivered incrementally, with an honest gap list maintained
throughout and no "carrier-grade" claim without conformance and soak evidence.

For calibration: the 2023 UCS plan estimated the charging system alone at 80
weeks of implementation plus 50 weeks of integration and testing with roughly 50
specialists. AI compresses the coding — not the interop campaigns, the
conformance testing, or the field hardening. Principles P11 and P15 in particular
cannot be validated on a single workstation; design for them from day one, prove
them when multi-node infrastructure exists, and say so plainly until then.
