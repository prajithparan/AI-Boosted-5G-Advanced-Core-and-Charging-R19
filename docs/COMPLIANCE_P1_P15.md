# P1–P15 compliance matrix and honest gap list

P4.12's own final deliverable: "a full compliance matrix against P1–P15. Produce an honest gap
list: what would still block a production deployment."

**Date:** 2026-09-05. **Scope note:** P11 (geo-redundant active/active) is deferred by explicit
user decision and is recorded here as deferred, not as done or failed.

The rule applied throughout: a row says **Done** only when something in this repository actually
does it and a test or a live run proves it. Everything else says what is really there. This
project's standing rule — no "carrier-grade" claim without conformance and soak evidence — applies
to this document more than to any other.

---

## Matrix

| # | Principle | Status | Evidence / what is actually missing |
|---|---|---|---|
| **P1** | OSI-approved open source only | **Done** | Every dependency is OSI-approved. Two license decisions were made *against* convenience and recorded: Osmocom (GPL-2+) and jss7 (AGPL-3.0) were rejected for SS7 rather than linked (ADR-0059); Valkey (BSD-3-Clause) is mandated over Redis, which relicensed to SSPL/RSALv2 (ADR-0044) |
| **P2** | 3GPP-standards-based | **Substantially done** | Every DTO is generated from the R19 OpenAPI YAML (86 files in the codegen pilot list; 2,928 types). Stage-3 gaps are named where they exist rather than invented around. Open: NRM/IOC coverage in `DATA_MODEL.md`, and **58 of the 60 `TS29122_*`/`TS29522_*` AF-facing YAML files present on disk are unwired** -- NEF's whole northbound surface (ADR-0294) |
| **P3** | 100% container / K8s, multi-cluster | **Partial** | Docker + Compose for all 22 NF/BSS components. **Helm for 7 of 18 NFs.** Multi-cluster: nothing |
| **P4** | AI-based real-time product/customer algorithms | **Partial** | ONNX in-process inference for quota sizing (P4.8 capability 1). Capabilities 2–6 deferred |
| **P5** | 100% TM Forum Open API / SID compliance | **Substantially done** | `DATA_MODEL.md` maps every SID entity to a real TMF API; 4 BSS components built |
| **P6** | 3GPP data models + rating engine, SID+NRM+IOC | **Partial** | Rating engine and SID mapping real; NRM/IOC coverage remains the open question ADR-0044 raised |
| **P7** | Product/tariff/policy is data, never code | **Done** | TMF620 `prodSpecCharValueUse` extension points; `Balance.rounding_rule` — data fields, not code paths |
| **P8** | Predictive auto-scaling | **Blocked — architectural, see below** | HPA for UDR only. **7 of 9 core NFs cannot be scaled at all** |
| **P9** | Full CI/CD | **Done** | GitHub Actions: build, lint (clang-format + clang-tidy), ASan/UBSan, TSan. 498 tests |
| **P10** | Performance / resource efficiency, benchmarked | **Partial** | First real baseline measured (ADR-0263): 9,717 req/s at concurrency 32; 500 req/s open-loop at 1.7 ms median. **No comparison against free5GC or anything else has ever been run** |
| **P11** | Geo-redundant active/active, proven RPO/RTO | **Deferred** | By user decision, 2026-09-05. Nothing exists |
| **P12** | Business-level alarming | **Done (first real slice)** | CDR sequence-gap alarm wired at Release + `chf_cdr_sequence_gap_total`; `sbi_requests_shed_total`; 4 Prometheus rules in `deploy/prometheus/business_alerts.yml`, every one verified against a metric this code really exports |
| **P13** | Charging correctness under AI/ML/SON change | **Not started** | Belongs to P4.9, which is blocked on NWDAF |
| **P14** | Retention-driven auto-archival | **Partial** | Archive-then-delete sweep in CHF (ADR-0283), hourly, **off by default**. Archives to newline-delimited JSON; a real deployment points it at object storage, which is not deployed here. Validated against real Doris in CI's `build` job (the only leg with a Doris service); skips locally and in both sanitizer legs |
| **P15** | Protocol-level spike protection / TPS governance | **Substantially done** | A ceiling on **all three** protocol front doors: SBI's 22 servers with spec-defined `503` shedding (ADR-0280), Diameter answering the real `DIAMETER_TOO_BUSY` (ADR-0285/0291), and SS7/M3UA dropping rather than answering, because a TCAP abort costs as much as service (ADR-0288). All off unless configured. All three now have an end-to-end test (ADR-0295): SBI's 503, Diameter's real `3004` answer over a real CER/CEA'd connection, and SS7's drop -- the last proved against a control run showing the same InitialDP reaches CHF's CAP handler when no ceiling is set. **Remaining gap: no load campaign** |

---

## The single most consequential finding: P8 is blocked by in-process state

Autoscaling is not a missing manifest. It is prevented by where NF state lives.

| NF | State backing | Horizontally scalable? |
|---|---|---|
| **UDR** | PostgreSQL only | **Yes** — HPA added |
| **CHF** | Redis + PostgreSQL | **Yes in principle** — no Helm chart exists yet |
| NRF | in-process `unordered_map` | **No** |
| AMF | Redis *and* in-process maps | **No** |
| SMF | in-process | **No** |
| UDM | in-process | **No** |
| PCF | in-process | **No** |
| AUSF | Redis *and* in-process | **No** |
| NSACF | in-process | **No** |

A second replica of any "No" row answers from a different view of the network: a UE registered on
replica A does not exist on replica B. The AMF Helm chart already pins `replicas: 1` and says so.
**Adding an HPA to those NFs would not be scaling — it would be a data-consistency bug with a
manifest in front of it**, which is why exactly one was added.

Closing P8 properly means moving live state out of process for each NF (the work UDR and CHF have
already had done to them), NF by NF. That is a real, sizeable programme and it is the honest
blocker, not YAML.

### Assigned: state externalisation belongs to the P11 geo-redundancy phase

**User decision, 2026-09-05.** This work is scheduled with **P11**, not as a standalone P8 task,
and that grouping is architecturally right rather than administratively convenient:

P11 requires **active/active across two data centres**. Active/active is only meaningful if a UE
registered in DC-A is visible in DC-B -- which is the *same* requirement as a second replica in one
cluster seeing what the first replica did. Externalising NF state is therefore not a prerequisite
*of* geo-redundancy so much as the first half *of* it. Doing it under P8 first and then again under
P11 would be doing it twice.

Concretely, that phase owns:

| NF | What has to move out of process |
|---|---|
| NRF | NF profile registry (`unordered_map`) |
| AMF | `UeContextStore` (its security contexts and AMF-UE-ID index are already Redis) |
| SMF | SM context store |
| UDM | subscription/registration maps |
| PCF | policy association maps |
| AUSF | the in-process half of its auth state |
| NSACF | slice admission counters and subscriptions (ADR-0276 discloses these as in-memory) |

Until then, P8 stays honestly **Blocked** in the matrix above, and the production-blocker list keeps
"a single AMF pod is both the capacity ceiling and the failure domain" as its top entry.

---

## What would still block a production deployment

Ordered by how hard each would bite:

1. **No horizontal scalability for 7 of 9 core NFs** (P8, above). A single AMF pod is the capacity
   ceiling *and* the failure domain.
2. **No geo-redundancy, no measured RPO/RTO** (P11) — deferred by decision, but still absent. It
   now also carries the state-externalisation work from blocker 1, by user decision (see above):
   the two are the same problem at different distances.
3. **No performance evidence against any reference implementation** (P10). The commercialization
   mandate (ADR-0049) requires exceeding free5GC; **no such comparison has ever been run**, and
   nothing in this repository claims otherwise.
4. **The synchronous HTTP/2 client** (ADR-0006/0009). It is why handover preparation blocks a gNB
   association for up to 10 s and why per-session SMF calls are serial.
5. **Spike protection exists on all three protocols but has never been load-tested** (P15). Each ceiling is validated as a mechanism under a small deliberate overload, not a campaign. All three are now covered by an end-to-end test (ADR-0295, closing ADR-0290's gap), but a test that proves a mechanism fires is not a load campaign and this project does not claim it is. All three are off unless configured, which is an opt-in policy choice, not a caveat.
6. **UPF's datapath is control-plane only in practice** — eBPF/XDP does not start without ambient
   capabilities, and downlink GTP-U encapsulation is not implemented in it at all.
7. **Retention exists but archives to a local directory, not object storage** (P14). The sweep is
   real and off by default; `docs/DATA_MODEL.md`'s E4 assigns archival to an object store, and no
   object store is deployed in this project.
8. **Helm covers 7 of 18 NFs**; no multi-cluster story (P3).
9. **N28/Sy is incomplete against the user's own directive**: PCF↔CHF works, but SMF has no
   `policyCounterId` code and no GUI exists for the data model that directive requires.
10. **NWDAF does not exist** (Phase 5), which also blocks P4.9 and P13.
11. **NEF exposes nothing to an AF** (ADR-0294). All 14 `Nnef_*` SBI files are built, but the
    AF-facing TS 29.122 / TS 29.522 APIs -- the "exposure" the NF is named for -- are unbuilt (58
    of 60 spec files on disk, unwired), and NEF's only outbound HTTP is NRF registration, so
    traffic-influence data it accepts is written to an in-process map instead of UDR and reaches
    no SMF or PCF. free5GC has both halves.
12. **NSSF's slice selection ignores the availability data it stores** (ADR-0294), never returns
    `nsiInformationList` (so an AMF is never told which NRF serves the selected slice instance),
    and ignores the TAI, so a slice restricted in one tracking area is allowed everywhere.

## What is genuinely solid

Stated because an honest gap list that omits the strengths is its own kind of distortion:

- Registration → PDU session → N2 handover works end to end across real NGAP/NAS/SCTP and real
  TLS 1.3 + mTLS SBI, with a real UE driving it and cryptography verified independently on both
  sides.
- 498 tests, green under ASan and TSan in CI.
- Every deployment endpoint is config-driven; no hardcoded addresses remain in `nfs/` or `bss/`.
- The charging path survives a hard kill of CHF without losing usage or double-releasing, and
  refuses to record a reservation it could not make when the balance store is partitioned.
