## ADR-0282: business alarming (P12), autoscaling (P8) and the compliance matrix -- P4.12 closes

**Date:** 2026-09-05
**Status:** Accepted

### P12: the detection existed, the alarm did not

`CdrWriter::detect_gaps` has been in this codebase since ADR-0058 and **was never called by
anything**. `docs/DATA_MODEL.md`'s E4 says a CDR sequence gap "is itself a P12 business-level alarm
condition, not just a log line" -- and nothing invoked it.

It now runs at Release, which is the right moment: the session is over, so its sequence numbers
should be contiguous, and a hole means a charging record for a completed session never reached the
CDR store. That is usage that was served and cannot be billed. It raises
`chf_cdr_sequence_gap_total` and logs at error level.

`sbi_requests_shed_total` is also now exported, closing the gap ADR-0280 named for itself.

`deploy/prometheus/business_alerts.yml` carries four rules. **Every metric a rule references was
verified to be exported by this code** -- and that check caught a real mistake before it shipped: a
rule was first written against `chf_reserve_rejected_total`, which does not exist; the real counter
is `chf_balance_reserve_rejected_total`. An alert on an unexported metric looks like coverage and
is silence, which is exactly the failure the file's own header warns about.

### P8: autoscaling is blocked by architecture, not by manifests

The finding, from reading where every NF's state lives:

| Scalable | Not scalable |
|---|---|
| UDR (PostgreSQL only), CHF (Redis + PostgreSQL) | NRF, AMF, SMF, UDM, PCF, AUSF, NSACF -- all hold live state in in-process `std::unordered_map` |

A second replica of any NF in the right-hand column answers from a different view of the network: a
UE registered on replica A does not exist on replica B. The AMF chart already pins `replicas: 1`
and says so.

So **one** HPA was added, for UDR, and the others deliberately were not. Giving them an HPA would
not be scaling; it would be a data-consistency bug with a manifest in front of it. CPU-based,
because metrics-server is universal -- scaling on real signalling load needs the Prometheus Adapter,
which this project does not deploy, and that is named rather than written as a rule that would
never fire. Off by default.

Closing P8 properly means moving live state out of process, NF by NF -- the work UDR and CHF have
already had. That is the honest blocker.

### The compliance matrix

`docs/COMPLIANCE_P1_P15.md`: every principle, what is really there, and a production-blocker list
ranked by how hard each would bite. Its top entry is not geo-redundancy (deferred by user decision)
but P8: a single AMF pod is simultaneously the capacity ceiling and the failure domain.

### What P4.12 did NOT close, plainly

- **P11** geo-redundancy: deferred by user decision. Nothing exists.
- **P14** retention/archival: not started.
- **P15**: SBI only. Diameter and SS7 have no ceiling. Validated as a mechanism, not under load.
- **P10**: still zero comparison against free5GC.
- **P13**: blocked on NWDAF.

---

