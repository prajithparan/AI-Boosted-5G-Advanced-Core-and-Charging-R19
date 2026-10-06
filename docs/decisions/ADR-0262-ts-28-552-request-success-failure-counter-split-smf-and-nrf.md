## ADR-0262: TS 28.552 request/success/failure counter split -- SMF and NRF

### Why this specific change

`docs/PERFORMANCE_MAPPING.md` named it itself: "the single highest-leverage instrumentation change:
split request / success / per-cause failure, and the 5GC KPIs become computable." Every TS 28.554
5GC-reachable KPI is a ratio of exactly those three counters, and this project had only flat
"total calls" counters, so none of them could be computed.

### What was built

Spec measurement names are cited in each metric's help text; metric names keep this project's own
`<nf>_*_total` exporter namespace rather than opening a second one.

| Spec measurement | Clause | Metric |
|---|---|---|
| `SM.PduSessionCreationReq` | 5.3.1.3 | `smf_sm_pdu_session_creation_req_total` |
| `SM.PduSessionCreationSucc` | 5.3.1.4 | `smf_sm_pdu_session_creation_succ_total` |
| `SM.PduSessionCreationFail` | 5.3.1.5 | `smf_sm_pdu_session_creation_fail_total` |
| `NFS.RegReq` | 5.10.1.1 | `nrf_nfs_reg_req_total` |
| `NFS.RegSucc` | 5.10.1.2 | `nrf_nfs_reg_succ_total` |
| `NFS.RegFailEncodeErr` | 5.10.1.3 | `nrf_nfs_reg_fail_encode_err_total` |
| `NFS.DiscReq` | 5.10.3.1 | `nrf_nfs_disc_req_total` |
| `NFS.DiscSucc` | 5.10.3.2 | `nrf_nfs_disc_succ_total` |
| `NFS.DiscFailUnauth` | 5.10.3.3 | `nrf_nfs_disc_fail_unauth_total` |
| `NFS.DiscFailInputErr` | 5.10.3.4 | `nrf_nfs_disc_fail_input_err_total` |

**The label sets are asymmetric, and that is the spec's doing.** Req/Succ are filtered per PLMN
*and* S-NSSAI with a subcounter per request type (5.3.1.3(c), 5.3.1.4(c)); Fail is filtered per
PLMN *only*, with a subcounter per rejection cause (5.3.1.5(c)). Applying one uniform label set to
all three would have been easier and wrong.

### Decisions that needed making, recorded rather than defaulted

- **Where "receipt" begins.** 5.3.1.3(c) says "on receipt by the SMF **from AMF**". A caller whose
  OAuth2 token is rejected is not AMF issuing a CreateSMContext Request, so a 401 is counted as
  neither a request nor a failure. Everything past authentication is counted, including a request
  whose body will not parse. Discovery is the exception and follows the spec, not the analogy:
  5.10.3.3 defines an explicit unauthorized-failure measurement, so a rejected discovery token is
  counted as both a request and an unauthorized failure.
- **`requestType` is OPTIONAL** in `SmContextCreateData` (checked against the YAML). An absent one
  is labelled the literal `absent` rather than defaulted to a real `RequestType` value this project
  would then be inventing.
- **Rejection cause** is the `title` of the `ProblemDetails` SMF already returns -- a real value
  that goes out on the wire, not a parallel taxonomy invented for metrics.
- **Two measurements are deliberately NOT exposed.** 5.10.1.4 `NFS.RegFailNrfErr` and 5.10.3.5
  `NFS.DiscFailNrfErr` are both "due to NRF internal error", and neither handler has any
  internal-error path -- every rejection either can produce is a profile-encoding or
  input-parameter failure. A permanently-zero series would read as "measured, and none occurred",
  which is a stronger claim than this build can make. Absent and explained beats present and
  misleading.

### A real defect found and fixed on the way

SMF's pre-existing `smf_create_sm_context_total` increments **after** request validation, so it has
always silently under-counted every request rejected during validation, and never matched
5.3.1.3's own definition. It is left in place and unchanged -- renaming or repurposing it would
break existing dashboards -- and is now accurately described as the "reached the point of
allocating an SM context ref" counter it actually is. The spec-named counter is the one that means
what 28.552 says.

To take the measurements without threading counters through the CreateSMContext handler's ~40 exit
paths, that handler became a named lambda and the route now wraps it, classifying its own response.
Behaviour is unchanged.

### What is now computable, written as expressions rather than claimed

A counter existing is not the same claim as a KPI being computable, so the actual queries:

- **TS 28.554 6.2.5 / 6.2.12 / 6.2.16 -- PDU session establishment success rate:**
  `sum(smf_sm_pdu_session_creation_succ_total) / sum(smf_sm_pdu_session_creation_req_total)`,
  sliceable by `snssai` for the per-slice form.
- **6.2.14 -- PDU Session Per Establishment Request Rate:**
  `sum(rate(smf_sm_pdu_session_creation_req_total[5m]))`.
- **6.2.15 -- Reject Rate:**
  `sum(rate(smf_sm_pdu_session_creation_fail_total[5m])) / sum(rate(smf_sm_pdu_session_creation_req_total[5m]))`,
  with `by (cause)` giving the per-cause breakdown 5.3.1.5 defines.

**6.4.1** (mean number of PDU sessions) is *not* computable from these: it needs the
`SM.SessionNbrMean` gauge (5.3.1.2), which is a different measurement type (SI, not CC) and is not
built here. Named as still-owed rather than quietly folded into the claim above.

### Scope

Only the SMF and NRF rows of `docs/PERFORMANCE_MAPPING.md`'s per-NF table are touched. That
document says every other per-NF audit "needs the same line-by-line treatment NRF got, and that is
real work rather than a formatting exercise" -- those rows stay marked owed rather than being
filled in by inference.

