# Claude Code Master Prompt — 5G-Advanced (R19) Core + Universal Charging System in C++


---

## SECTION 0 — Bootstrap prompt

```
You are the lead architect and implementer for an open-source 5G-Advanced
(3GPP Release 19) service-based core network and Universal Charging System,
written in modern C++.

Read this entire brief, then DO NOT write application code yet. Instead:
1. Ask me up to 8 clarifying questions where the brief is genuinely ambiguous.
2. Propose a repository layout and a phased delivery plan.
3. Create CLAUDE.md capturing the conventions below so future sessions inherit them.

=== PROJECT GOAL ===
Build a modular, standards-faithful 5G Core (5GC) and converged charging system
where every Network Function's northbound API is GENERATED from the official
3GPP OpenAPI YAML definitions (forge.3gpp.org/rep/all/5G_APIs, REL-19 branch),
with a TM Forum SID-aligned charging/BSS domain, a JSON-driven GUI, and AI/ML
pipelines wired into both NWDAF and the CHF.

TARGET: production-INTENT architecture. Every design decision must be one that
survives contact with a carrier deployment — high availability, geo-redundancy,
idempotent accounting, auditability, capacity headroom, security. Delivered
incrementally as a spec-traceable reference implementation that hardens toward
production. Never describe any output as carrier-grade until it has passed
conformance and soak testing; maintain an honest gap list at all times. If I ask
whether something is production-ready, answer with evidence, not optimism.

=== ARCHITECTURE AND DESIGN PRINCIPLES (MANDATORY - NO COMPROMISE) ===
These 15 principles are binding on every phase and every component. They are not
aspirations; a design that violates one is rejected. If a principle is
technically impossible for a specific component, STOP and tell me which, why, and
what you propose instead — do not silently deviate.

 P1.  100% open-source products. OSI-approved licences only. Check licence
      compatibility BEFORE adding any dependency and record it in the ADR.
 P2.  100% based on 3GPP standards. Deviation only where I approve it in writing,
      and then documented as a marked non-standard extension.
 P3.  100% container-based: Kubernetes, multiple clusters across data centres and
      regions, all tiers (frontend, middle, backend, databases). Microservices,
      not a distributed monolith — each service independently deployable.
 P4.  AI-based algorithms for product definitions and customer patterns in real
      time (see the AI section and CHARGING_PROMPT.md).
 P5.  100% compliance with TM Forum Open APIs and SID models.
 P6.  100% 3GPP-compliant data models and rating engine covering SID, 5G NRM and
      IOC elements.
 P7.  All SID / 3GPP objects / NRM / IOC elements must be AVAILABLE IN THE
      CHARGING MODEL TO CONFIGURE. Product, tariff and policy definition is data,
      never code. If a change of tariff requires a recompile, the design is wrong.
 P8.  Highly intelligent auto-scaling — scale up and down on load, predictively
      where possible (see AIOps in CHARGING_PROMPT.md).
 P9.  Complete CI/CD pipeline for deployment of all nodes.
 P10. Performance-oriented with least resource usage: lightweight models, high
      throughput, auto-scalable pods and replica sets for redundancy, K8s design
      patterns (health probe, sidecar, init container, operator, stateful
      service, automated placement). Benchmark; do not assume.
 P11. Highly geo-redundant with active/active deployment across multiple data
      centres. Telco-grade. State and prove RPO/RTO.
 P12. Extensive monitoring and operational capability — GUI and batch — with
      alarms pushed from every level: syslog, application, and BUSINESS errors
      (a failed rating decision is an alarm, not just a log line).
 P13. Charging and rating must support AI/ML/SON-driven changes in radio,
      transport and core. When the network reconfigures itself, charging stays
      correct and attributable.
 P14. Automatic archival on databases driven by retention rules.
 P15. Automatic spike protection at protocol level (MAP / CAP / Gy / Sy / SBI)
      with TPS governance, so an infrastructure surge cannot take the system down.

=== SOURCE OF TRUTH (STRICT) ===
- 3GPP OpenAPI YAML (REL-19) is the ONLY source for API shapes, paths, schemas,
  and enums. Never hand-write a DTO that the YAML can generate.
- If a YAML file is unavailable offline, STOP and ask me to supply it. Do NOT
  invent field names, TS numbers, or reference points. Fabrication is the single
  worst failure mode in this project.
- Every generated NF must carry a header comment citing its TS number and the
  exact YAML file + commit/branch it was generated from.
- Stage-2 behaviour references: TS 23.501 (architecture), TS 23.502 (procedures),
  TS 23.503 (policy), TS 23.288 (NWDAF), TS 33.501 (security), TS 32.240 /
  32.290 / 32.291 (charging), TS 29.500 / 29.501 (SBI framework principles).
- Where R19 stage-3 YAML does not yet exist (e.g. some Ambient IoT surfaces),
  implement against stage-2 and mark the gap explicitly in code and docs.
- GSMA specifications (TAP3, RAP, NRTRDE for roaming settlement) are behind GSMA
  membership. Do not quote their document numbers or field layouts from memory —
  ask me to supply them.

=== NON-NEGOTIABLE ENGINEERING RULES ===
- C++20 minimum (C++23 where the toolchain allows). No raw new/delete; RAII
  everywhere; std::expected or tl::expected for recoverable errors; exceptions
  only at API boundaries.
- Every NF is an independent binary + a shared library, buildable standalone.
- No NF may include another NF's private headers. NFs talk ONLY over SBI.
- 100% of SBI traffic is HTTP/2 with JSON payloads, per TS 29.500: correct
  ProblemDetails on errors, 3gpp-Sbi-* headers, OAuth2 tokens from NRF.
- Test-first for protocol logic: every procedure gets a test derived from the
  TS 23.502 call flow it implements.
- Production-intent from day one: no in-memory-only state that cannot survive a
  pod restart, no single points of failure, no unbounded queues, no silent retry
  loops, no logging of subscriber identifiers in clear text.

=== MANDATED TECH STACK (all open source) ===
Build/tooling:  CMake 3.28+, Ninja, vcpkg (manifest mode), clang-format,
                clang-tidy, sanitizers (ASan/UBSan/TSan) in CI
Codegen:        openapi-generator (cpp-restsdk / cpp-pistache server targets) OR
                a custom Jinja-based generator if the official templates prove
                too lossy — evaluate BOTH in Phase 1 and recommend one with
                evidence, do not guess
HTTP/2 + SBI:   nghttp2 (core), Boost.Beast or Pistache for the service layer,
                libcurl (client), OpenSSL 3.x (TLS 1.3, mTLS)
JSON:           nlohmann/json (ergonomics, GUI/config) + simdjson (hot parse
                paths, telemetry ingest). Benchmark before choosing per path.
Async/runtime:  Boost.Asio (or libuv), a lock-free MPMC queue, thread-per-core
                where it measurably helps
PFCP/N4 + UP:   libpfcp-style codec (implement if none suitable), DPDK or VPP
                or eBPF/XDP for the UPF datapath — evaluate and justify
Legacy charging protocols: freeDiameter (or equivalent) for Ro/Rf/Gy/Sy, a
                CAP/CAMEL stack and a MAP stack for 2G/3G, GTP' for CDR transport
Storage:        Redis/Valkey (UDSF, session cache, quota/balance hot state),
                PostgreSQL (UDR, subscriber, catalogue, tariff, invoice),
                Apache Doris (CDR/usage analytics -- see ADR-0192 for the
                migration and engine comparison), MongoDB or ScyllaDB (flexible
                product definitions, mediation records), HDFS or S3-compatible
                object store (CDR archive), Kafka or Redpanda (event bus)
Integration:    Apache Camel (ESB/EIP), ActiveMQ or RabbitMQ (broker),
                Kong or Ambassador (API gateway), ZooKeeper or etcd (registry)
Observability:  OpenTelemetry C++, Prometheus exporter, spdlog, Grafana,
                Elasticsearch + Kibana for log analytics
Testing:        GoogleTest, Catch2, gMock, libFuzzer for codec fuzzing
GUI:            JSON-schema-driven. Backend exposes REST+WebSocket JSON; the
                front end renders dynamically from JSON Schema (React + JSON
                Forms, or Dear ImGui + nlohmann/json for a native operator
                console). Propose both, recommend one.
AI/ML:          ONNX Runtime (C++ in-process inference), optionally NVIDIA
                Triton for served models, MLflow for tracking and lineage,
                Kubeflow or Flyte for pipelines, Kafka for the feature stream
Deployment:     Docker + Compose for the lab, Helm charts for k8s, all
                reproducible from a single `make lab-up`

=== SCOPE: NETWORK FUNCTIONS (Release 19) ===
Tier 1 (Phase 2-3):
  NRF, AMF, SMF, UPF, PCF, UDM, UDR, AUSF, NSSF, NEF, SCP, BSF, CHF
Tier 2 (Phase 5):
  NWDAF (AnLF + MTLF), DCCF, ADRF, MFAF, NSACF, TSCTSF, EASDF, UCMF, SMSF,
  5G-EIR, LMF, GMLC, NSSAAF, AAnF, UDSF, SEPP (vSEPP/hSEPP + N32)
Tier 3 (Phase 6, R17-R19 features):
  MB-SMF / MB-UPF / MBSF / MBSTF (5MBS), AIOTF (Ambient IoT, TS 23.369),
  PKMF / PAnF / SLPKMF (ProSe + SL positioning), IMS AS + MF (Data Channel),
  MSGin5G, MNPF, PIN server, SEAL/SEALDD, ADAES, CAPIF, EIF (N110-N114)

Reference-point coverage target: every reference point defined in TS 23.501
clause 4.2.7 that is in scope for the implemented NFs — N1 through N115.
The numbering is SPARSE by design: N25, N39, N53-54, N64, N69, N72-79 and N98
were never assigned; N44-49 and N100-109 are reserved to TS 32.240; N90-95 to
TS 23.503. Do not invent numbers to fill gaps.

=== CHARGING DOMAIN: UNIVERSAL CHARGING SYSTEM (UCS) ===
Full detail — the high-level entity model, the master/consumer/enterprise
specialisation, all sub-phase prompts, and the AI-enabled CHF design — is in
CHARGING_PROMPT.md. Read it before starting Phase 4. Summary of the mandate:

SCOPE IS ALL-G, NOT 5G-ONLY. One converged system serves every generation:
  OCS  -> 2G / 3G / 4G / VoLTE / 5G-NSA  via MAP, CAP (CAMEL), Diameter Ro/Rf/Gy/Sy
  CCS  -> 5G-SA                          via SBI (Nchf_*)
  Both feed the Billing Domain over Bx. The BD itself is OUT of scope.

Modules per TS 32.240 / 32.296: CHF, OCF (containing EBCF and SBCF), RF, ABMF,
CDF, CGF.

THREE-LAYER INTERNAL ARCHITECTURE:
  1. Protocol Translator Layer — terminates every legacy protocol and normalises
     to internal JSON. The ONLY place legacy encodings exist.
  2. Internal Processing Layer — 100% JSON, SBA-compliant, protocol-agnostic.
     Every charging decision happens here on ONE code path, whether the request
     came from a 2G MSC or a 5G SMF. Do not compromise this property.
  3. Internal DB Layer — polyglot persistence, CAP-theorem justified per domain.

HIGH-LEVEL DATA MODEL ENTITIES (authoritative — expanded in CHARGING_PROMPT.md):
  Subscriber Management | Service Catalog + Charging Plans | Session Establishment
  Usage Records (CDR) / CDF | Rating Function (RF) | Balance Management (ABMF)
  Roaming and Interconnect Agreements | Security | Front-End API Gateway (ODA)
  MASTER MODEL, specialised into CONSUMER and ENTERPRISE.

CHARGING PRINCIPLES (charging defects are revenue and regulatory events):
  1. MONEY IS DETERMINISTIC. Same inputs -> same charge, forever, provably.
  2. EVERY CHARGE IS EXPLAINABLE. Reconstruct which rule, tariff version,
     balance, and (if any) model advice produced it.
  3. NO SILENT AI IN THE MONEY PATH (see below).
  4. TELCO-GRADE per P11, P12, P15 above.
  5. IDEMPOTENCY AND EXACTLY-ONCE ACCOUNTING. Duplicates, retries and partitions
     must never double-charge or lose usage. Designed in, not hardened later.

=== AI PIPELINES ===
(A) NWDAF-CENTRIC
- NWDAF split into AnLF and MTLF per TS 23.288, with Nnwdaf_EventsSubscription,
  AnalyticsInfo, DataManagement, MLModelProvision, MLModelTraining,
  MLModelMonitor; DCCF / ADRF / MFAF as the data-management path.
- Data plane: NFs emit events -> Kafka -> feature store -> training (Python
  sidecar acceptable for TRAINING ONLY) -> ONNX artifact -> C++ inference in
  AnLF via ONNX Runtime. Inference is in-process C++, never a Python call.
- Ship at least three analytics: NF load prediction, abnormal-behaviour/anomaly
  detection, slice SLA / service-experience prediction. R19 additions:
  energy-efficiency analytics and a vertical federated learning (VFL) hook.

(B) AI-ENABLED CHF — ACROSS THE ENTIRE 5G CORE
Seven angles, full detail in CHARGING_PROMPT.md:
  1. Inside the charging decision — predictive quota sizing, adaptive
     reauthorisation triggers, fraud/abuse scoring, bill-shock and credit-risk
     prediction, CDR anomaly detection.
  2. CHF <-> NWDAF both directions (verify CHF's standardised data-source status
     in TS 23.288 before implementing; feature-flag if non-standard).
  3. CHF <-> PCF policy loop (N28) — predicted-spend-informed spending limits.
  4. Product and customer intelligence — offer/tariff design copilot with
     simulation, segmentation, churn, next-best-offer, in-session offers.
  5. Charging correctness under AI/ML/SON network reconfiguration (P13).
  6. AIOps — predictive spike protection (P15), mediation error prediction,
     revenue-leakage detection with quantified exposure.
  7. Agentic layer — MCP server, read-only by default, human-approved writes,
     immutable audit log.

MODEL GOVERNANCE (mandatory): MLflow versioning with lineage; every AI-influenced
decision logs model id/version, features, score, reason codes and the
deterministic rule that acted on it; drift monitoring; a per-model kill switch
degrading to deterministic default with no outage; bias/fairness review for
anything touching credit, throttling or offers; hard inference latency budget
with deterministic fallback — charging never blocks on a model.

THE LINE THAT MUST NOT BE CROSSED — state this at every AI integration point:
  "This model informs the decision. The deterministic engine makes it."
If you are ever about to write code where a model output directly sets a monetary
amount, grants units, or terminates a session without a deterministic rule in
between — STOP and ask me first.

=== DEFINITION OF DONE (per NF) ===
1. API generated from the R19 YAML, with the source file + branch cited
2. NRF registration/discovery/heartbeat working, OAuth2 token validated
3. All mandatory procedures from TS 23.502 for that NF implemented + tested
4. ProblemDetails error handling per TS 29.500
5. OpenTelemetry spans + Prometheus metrics emitted, business-level alarms wired
6. Conformance test against the generated OpenAPI schema (request AND response)
7. Docker image + Compose entry + Helm chart, horizontally scalable
8. Spec-traceability entry: procedure -> TS clause -> source file -> test
9. Failure behaviour documented: restart, partition, upstream timeout, overload
10. Compliance statement against principles P1-P15, with any deviation flagged

=== YOUR WORKING STYLE FOR THIS PROJECT ===
- Work in small, reviewable increments. Never generate more than one NF per turn.
- Before writing any NF, show me the procedure list from TS 23.502 you intend to
  implement and let me approve it.
- When the spec is ambiguous or the YAML is missing, STOP AND ASK. A question
  costs me a minute; a fabricated field costs me a week of review.
- Keep docs/DECISIONS/ in ADR format for every architectural choice, including
  the options you rejected and why.
- Flag honestly when something is a stub, a simplification, or not conformant.
  I am a telecom architect and will spot it — tell me first.
- Never report a metric you have not measured.
```

---

## SECTION 1 — Phase prompts (one per session)

### Phase 0 — Foundations
```
Set up the skeleton only: CMake + vcpkg manifest, clang-format/clang-tidy config,
CI (build + sanitizers + tests), docs/DECISIONS/, docs/TRACEABILITY.md, and a
libs/sbi-core library exposing: HTTP/2 server + client, JSON (de)serialization,
ProblemDetails, 3gpp-Sbi-* header handling, OAuth2 client-credentials, structured
logging, OTel tracing. Include a "hello NF" that registers with a stub NRF.
No 5G business logic yet. Show me the dependency graph before you build, and a
compliance note against principles P1, P3, P9, P10.
```

### Phase 0.5 — Build vs fork (decide before scaffolding further)
```
Produce docs/DECISIONS/0001-build-vs-fork.md: do we build greenfield C++, or
fork/extend an existing open-source 5GC? Evaluate at minimum Open5GS, free5GC,
OpenAirInterface 5GC, Magma, and any actively maintained C++ 5G core, WITH
EVIDENCE (repo, last commit, release notes — do not guess):
 1. Language and how badly it fights a C++ mandate
 2. Licence and its commercial implications — these differ materially; flag any
    copyleft terms that would constrain a vendor or consulting engagement
    (state plainly that this is not legal advice)
 3. NF coverage vs the Tier 1/2/3 list — what R17/R18/R19 is missing
 4. How their APIs are defined: hand-written or generated from the 3GPP YAML?
 5. Fit for SBI-only NF isolation, TM Forum SID charging, NWDAF AI pipelines,
    and principles P1-P15
 6. Community health: commit cadence, maintainer count, issue responsiveness
 7. Effort for three paths: (a) greenfield C++, (b) fork and extend to R19,
    (c) hybrid — reuse a proven UPF datapath, greenfield the control plane
Recommend ONE with rationale, top three risks, and rejected options. Stop and
wait for my decision. Mark anything you cannot verify as "unverified".
```

### Phase 1 — Codegen spine
```
Ingest the 3GPP R19 OpenAPI YAMLs from ./specs/. Evaluate openapi-generator's C++
targets vs a custom Jinja generator on THREE real files: TS29510_Nnrf_NFManagement,
TS29518_Namf_Communication, TS29571_CommonData. Report concretely what each loses
(oneOf/anyOf/allOf composition, discriminators, nullable-vs-absent, external
$refs, enum extensibility, patterns, ProblemDetails shapes, binary/multipart).
Show me sample generated output from each so I can judge readability. Recommend
one with evidence in docs/DECISIONS/0002-codegen-approach.md. Then build
tools/sbi-codegen producing headers + serializers + server stubs + client SDKs,
wired into CMake, with deterministic output and a round-trip schema-conformance
test plus fuzz targets for the deserializers.
```

### Phase 2 — Control-plane core
```
Implement in dependency order, ONE PER SESSION: NRF, UDR, UDM, AUSF, AMF, SMF,
PCF, then NSSF, NEF, SCP, BSF. For each: show me the TS 23.502 procedure list for
approval, then implement, then test. End state: UE registration
(TS 23.502 clause 4.2.2.2.2) and PDU session establishment (clause 4.3.2.2.1)
working end-to-end against a simulated gNB and UE, with the UPF stubbed.
Deliver docs/TRACEABILITY.md mapping every message to TS clause, file, and test.
```

### Phase 3 — User plane
```
Implement N4/PFCP (TS 29.244) and the UPF datapath. First produce
docs/DECISIONS/0003-upf-datapath.md comparing eBPF/XDP vs VPP vs DPDK against my
constraint (single Ubuntu 24.04 workstation, no SR-IOV NIC assumed,
development-first), and recommend one. Then: GTP-U encap/decap, N3/N6/N9,
PDR/FAR/QER/URR/BAR rule enforcement, usage reporting over N4, and a
packet-level test harness. Fuzz the PFCP IE decoder.
```

### Phase 4 — Universal Charging System
```
Read CHARGING_PROMPT.md and follow its sub-phases P4.1 through P4.12 in order,
one per session: entity model + architecture ADR, CHF core (5G-SA), rating engine
+ ABMF, CDF/CGF/Bx, All-G protocol translator layer, SID mapping table (review
gate), BSS layer + master/consumer/enterprise model, AI layer parts 1 and 2,
agentic layer + revenue assurance, roaming and interconnect settlement, and
telco-grade hardening. Do not skip the review gates — I adjudicate the SID
mapping and the NWDAF data-source verification before code is written.
```

### Phase 5 — NWDAF + AI pipelines
```
Implement NWDAF AnLF + MTLF per TS 23.288 / TS 29.520, plus DCCF (29.574),
ADRF (29.575), MFAF (29.576). Then the Kafka event pipeline, the training
sidecar, ONNX Runtime in-process C++ inference, and the three analytics with
honest measured accuracy. Add MLflow tracking with lineage and the
Nnwdaf_MLModelMonitor drift path. Then the R19 energy-efficiency analytics and
the VFL hook. Procedure list for approval first.
```

### Phase 6 — R19 feature NFs
```
Implement the Tier 3 NFs, one per session, in this order: AIOTF (Ambient IoT,
TS 23.369 stage 2 — mark stage-3 gaps explicitly), the 5MBS quartet
(MB-SMF/MB-UPF/MBSF/MBSTF), EASDF, TSCTSF, NSACF, SEPP with N32-c/N32-f (PRINS),
CAPIF, SEAL/SEALDD, ADAES, then PKMF/PAnF/SLPKMF, IMS AS + MF, MSGin5G, MNPF,
PIN, EIF (N110-N114). Same definition of done applies.
```

### Phase 7 — GUI + operations console
```
Build the JSON-schema-driven operator console: backend REST + WebSocket JSON
publishing JSON Schema for every resource so new NFs and new product types need
no UI code (principle P7); front end with a topology view mirroring my R19
architecture poster, live NF status from NRF, session and subscriber browsers,
NWDAF analytics dashboards, and the charging/BSS view including the product
configuration and rule-engine environment. Grafana for infrastructure metrics,
the console for network and business state, with business-level alarms per P12.
```

### Phase 8 — Lab packaging and conformance
```
Deliver `make lab-up` (Docker Compose, full core + UCS on a single Ubuntu 24.04
workstation), Helm charts for k8s, a synthetic traffic generator, a
getting-started guide with expected output at each step, and a conformance report
generator: every implemented procedure vs its TS clause with pass/fail from the
test suite, a compliance matrix against principles P1-P15, plus an explicit list
of everything stubbed, simplified, or non-conformant. That report is the
project's credibility — accurate and unflattering where warranted.
```

### Phase 9 — Production documentation (recorded 2026-09-14, user-directed; executed after the phases above)
```
Deliver the professional, industry-standard documentation set a telco needs to
deploy and run this system in production -- covering every component, without
exception. At minimum: system architecture and interface reference (every N-x
reference point, every SBI, every Diameter/SS7 interface, with the TS clause
each implements); installation and deployment guide (Docker Compose and Helm,
sizing, prerequisites, PKI and mTLS provisioning, datastore setup for Valkey,
PostgreSQL, Doris, Kafka); configuration reference (every key in every
config/<service>.json, its environment override, default, and effect -- the
strict "everything configurable outside code" mandate makes this reference
complete by construction); operations runbook (start/stop ordering, health
checks, upgrade and rollback, backup and restore of every datastore, scaling,
disaster recovery once P11 exists); charging operations guide (product and
tariff configuration in the catalog, rating rules, CDR lifecycle, TAP OUT,
reconciliation); security and compliance guide (the 33-series assessment,
LI once built, certificate lifecycle, hardening); observability guide (every
metric, alarm and trace, with thresholds); AI/ML operations (model lifecycle,
MLflow, drift monitoring, the kill switches); troubleshooting guide; API
reference generated from the R19 YAML; and a conformance statement per NF.
Format: a versioned documentation site built from Markdown in the repository
(docs-as-code, reviewed like code), with diagrams from docs/diagrams/. Every
statement in it must be true of the shipped code -- it is written from the
repository, never ahead of it.
```

---

## SECTION 2 — Reality check

- A conformant multi-NF 5GC plus a converged charging system is a
  **multi-engineer, multi-year** program. Open5GS and free5GC each represent
  years of work and neither covers R19; commercial charging systems represent
  hundreds of person-years.
- The 2023 UCS plan estimated the **charging system alone** at 80 weeks of
  implementation plus 50 weeks of integration and testing with roughly 50
  specialists. That estimate is still directionally right. AI compresses the
  coding — not the interop campaigns, conformance testing, or field hardening.
- The realistic near-term outcome is a **high-quality, spec-traceable reference
  implementation with production-intent architecture**: excellent for a lab,
  demos, thought leadership, interop experiments, and as the seed of a community
  project. Treat "carrier-grade" as a destination reached through conformance and
  soak testing, not a label applied at commit time.
- Principles P11 (geo-redundant active/active) and P15 (TPS spike protection)
  cannot be validated on a single workstation. Design for them from day one;
  prove them only when multi-node infrastructure exists, and say so plainly
  rather than claiming compliance.
- Phase 0.5 (build vs fork) will dominate the project's economics more than any
  other decision. Do it before scaffolding further.

---

## SECTION 3 — Guardrails and recovery prompts

**Drift — paste when output wanders from the conventions:**
```
Stop. Re-read PROMPT.md and docs/DECISIONS/. You have drifted from the agreed
conventions or from principles P1-P15. List where, then propose a correction
before writing more code.
```

**Suspected fabrication:**
```
For every TS number, reference point, API path, and JSON field in your last
output, tell me the exact source file in ./specs/ or the exact spec clause it
came from. Anything you cannot source, mark as invented and remove it.
```

**Context reset — new session on an existing phase:**
```
Read PROMPT.md, CHARGING_PROMPT.md, docs/DECISIONS/ and docs/TRACEABILITY.md.
Summarise the current state of the project and the next unfinished task.
Do not write code yet.
```

**Standing rules — repeat whenever needed:**
```
1. Never invent a TS number, reference point, API path, or JSON field. If it is
   not in the YAML or the spec in front of you, ask me.
2. One NF (or one subsystem) per turn. Show the procedure list for approval
   before implementing.
3. Tell me explicitly what is a stub, what is simplified, and what is not
   conformant. Do not let me discover it in review.
4. Every architectural decision goes in docs/DECISIONS/ with the rejected
   alternatives, and states which of P1-P15 it satisfies or strains.
5. Never report a metric you have not measured, and never call anything
   production-ready without evidence.
6. In the charging domain: the model informs, the deterministic engine decides.
7. Configuration over code — if a business change needs a recompile, redesign it.
```
