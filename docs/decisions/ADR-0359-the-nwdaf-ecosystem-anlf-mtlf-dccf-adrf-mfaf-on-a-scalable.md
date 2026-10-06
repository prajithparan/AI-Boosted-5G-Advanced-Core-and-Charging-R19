## ADR-0359: the NWDAF ecosystem -- AnLF, MTLF, DCCF, ADRF, MFAF -- on a scalable architecture

**Date:** 2026-09-14. **Status:** accepted. User mandate: *"make sure these NWDAF core functions
are properly built with proper scalable architecture."* This entry is the architecture; each
function then lands under its own ADR with tests.

### What each function is, from TS 23.288 V19.7.0, and the YAML it must expose

| Function | Defined in | Role (spec's words, abridged) | Services (TS 29.5xx YAML, all present) |
|---|---|---|---|
| **AnLF** | 5.1 | "performs inference, derives analytics information ... and exposes analytics services" | Nnwdaf_AnalyticsInfo, Nnwdaf_EventsSubscription, (RoamingAnalytics) |
| **MTLF** | 5.1, 7.5, 7.6 | "trains ML models and exposes new training services (e.g. providing trained ML Model)" | Nnwdaf_MLModelProvision, MLModelTraining, MLModelMonitor, (VFLTraining/Inference) |
| **DCCF** | 5A.2 | coordinates data collection: "if the requested data is already being collected from the Data Source, no subscriptions ... need to be created" -- one collection, many consumers | Ndccf_DataManagement, Ndccf_ContextManagement |
| **ADRF** | 4.2.1, 5B | "store, retrieve and delete data and analytics"; StorageRequest / StorageSubscriptionRequest / RetrievalRequest / RetrievalSubscribe; also ML model storage | Nadrf_DataManagement, Nadrf_MLModelManagement |
| **MFAF** | 5A.3.2 | adapts the 5GS to "a Messaging Framework" that "formats and processes data ... and sends notifications to all Data Consumers": 3DA (DCCF configures it), 3CA (it delivers) | Nmfaf_3daDataManagement, Nmfaf_3caDataManagement, Nmfaf_ContextManagement |

5.1 NOTE 1: "NWDAF can contain an MTLF or an AnLF or both." 5.1: "Different NWDAF instances may
be present in the 5GC, with possible specializations per type of analytics ... described in the
NWDAF profile stored in the NRF." That sentence is the scaling model the spec hands us.

### The architecture

**1. Five deployable units, one shared library.** AnLF and MTLF are *logical functions of one NF*
per the spec, so they are one binary, `nfs/nwdaf`, with a `role` in config -- `anlf`, `mtlf`, or
`both` -- and register the matching capability in their NRF profile (`NwdafInfo`). That is what
lets an operator run six AnLF replicas and one MTLF, or scale them apart, while the code that
turns a feature row into an analytic lives in one place. DCCF, ADRF and MFAF are separate NFs
(`nfs/dccf`, `nfs/adrf`, `nfs/mfaf`), each an independent binary per CLAUDE.md, each registered
with the NRF, each discoverable by its type. Shared analytics/feature logic that more than one of
them needs goes to `libs/analytics-features` (already exists), never into another NF's `src/`.

**2. No state in a process. Ever.** This is the whole difference between "runs" and "scales", and
it is the debt ADR-0350's P8 finding named. Every instance of every function must be
interchangeable behind the NRF, so:

| State | Store | Why this store |
|---|---|---|
| Subscriptions, contexts, transfers (NWDAF, DCCF, MFAF) | **Valkey** | Already the session-state store for AMF and CHF; keyed, TTL-able, shared across replicas |
| Collected data and analytics (ADRF) | **Apache Doris** | Already the analytics repository: CDRs, the feature store. ADRF's storage/retrieval *is* this table family |
| ML model metadata, versions, lineage (ADRF / MTLF) | **PostgreSQL + MLflow** | MLflow lineage already exists for the quota model; Postgres for the ADRF's model index |
| The Messaging Framework (MFAF) | **Apache Kafka** | The spec's "Messaging Framework" is exactly a durable pub/sub bus; ADR-0355 built it. MFAF is the 3GPP-shaped adaptor over it, not a second bus |

Phase A's in-process subscription map is therefore the first thing to go: **ADR-0360 externalises
it to Valkey before anything is built on top.** An NWDAF that holds subscriptions in memory cannot
be replicated, and a design that starts by replicating a non-replicable thing is not a design.

**3. Scaling through the NRF, as the spec describes.** N instances of a function register with
their capability; consumers discover and select. For the AnLF, `NwdafInfo.eventIds` says which
analytics an instance computes, so an operator can specialise instances per analytic and the NRF
routes accordingly. For the DCCF, coordination state in Valkey means any DCCF replica sees every
existing collection and can answer "already being collected" correctly -- the dedup 5A.2 requires
only works if the replicas share the view.

**4. The data plane, end to end, with the functions in their spec positions:**

```
Data Source NF ──(subscription created by DCCF)──▶ MFAF (3DA) ──▶ Kafka topic ──▶ MFAF (3CA)
                                                                                     │
                     ┌──── ADRF (Nadrf_DataManagement: store) ◀────────────────────┤
                     │                                                               ▼
                     ▼                                                        Data Consumers
                Doris tables ──▶ feature store ──▶ MTLF (train, MLflow) ──▶ Nnwdaf_MLModelProvision
                     │                                                               │
                     └──────────────────────▶ AnLF (infer, ONNX in-process) ◀────────┘
                                                      │
                                             Nnwdaf_AnalyticsInfo / EventsSubscription
```

Today's CHF -> Kafka -> Doris path (ADR-0355) is that top row with CHF as the Data Source and
the MFAF's role played by Doris' Routine Load. Building the MFAF makes that path generic to
every source NF; building the DCCF makes it coordinated; building the ADRF makes it addressable
by the spec's own API instead of by SQL.

### Build order, each its own turn and ADR, each moving its diagram box from dashed to solid

0. **Externalise NWDAF state to Valkey** (ADR-0360) -- prerequisite for everything below.
1. **MFAF** on Kafka: 3da (DCCF configures routing), 3ca (delivery to consumers), ContextManagement.
2. **DCCF**: DataManagement with real dedup across consumers, ContextManagement; delivers via MFAF.
3. **ADRF**: DataManagement (storage, subscription-storage, retrieval, retrieval-subscribe) on
   Doris; MLModelManagement on Postgres + MLflow.
4. **NWDAF Nnwdaf_DataManagement** (Phase C): the AnLF collecting via DCCF/MFAF rather than by
   reading Doris directly -- which retires the feature_store.cpp shortcut of Phase A.
5. **MTLF**: MLModelProvision, MLModelTraining, MLModelMonitor (5C accuracy monitoring) over the
   existing training sidecar and MLflow, models stored through the ADRF.
6. **Roaming and VFL** services (Phase D).

### Disclosed up front

- The spec leaves DCCF's internal dedup logic unspecified ("not specified", 5A.3.2 NOTE). Ours
  will be documented in its ADR; it is a design, not a conformance claim.
- Each of these five needs an LI IRI-POI eventually (TS 33.127 7.18 for NWDAF); none gets one
  until LI is built (blocker #0).
- Horizontal scaling of the NFs that *feed* this (AMF, SMF, CHF) is still blocked by their own
  in-process state (P8). This ADR fixes the analytics plane; it does not fix the sources.

### Rejected

- **One monolithic "nwdaf" that is also the DCCF, ADRF and MFAF.** The spec permits an NWDAF to
  play DCCF/ADRF roles when those NFs are absent (5.1, 5A.1). Doing so would recreate CHF's
  scaling problem in the analytics plane on day one.
- **Building MTLF before the data-collection functions.** A model trained on a Doris shortcut
  cannot be retrained from spec-shaped collection later without rework; the collection path first.

