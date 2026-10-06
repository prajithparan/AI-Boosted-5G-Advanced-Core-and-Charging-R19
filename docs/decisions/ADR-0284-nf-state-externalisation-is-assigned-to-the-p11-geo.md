## ADR-0284: NF state externalisation is assigned to the P11 geo-redundancy phase

**Date:** 2026-09-05
**Status:** Accepted (user decision)

ADR-0282 found that P8 autoscaling is blocked by architecture: only UDR and CHF hold no in-process
state, while NRF, AMF, SMF, UDM, PCF, AUSF and NSACF keep live state in in-process
`std::unordered_map` stores, so a second replica answers from a different view of the network.

**Decision (user, 2026-09-05):** that work is scheduled with **P11 (geo-redundant active/active)**
rather than as a standalone P8 task, and is recorded here so it cannot be lost between phases.

### Why the grouping is correct, not merely convenient

P11 requires active/active across two data centres. Active/active is only meaningful if a UE
registered in DC-A is visible in DC-B -- which is the **same requirement** as a second replica in
one cluster seeing what the first replica did. Externalising NF state is not a prerequisite *of*
geo-redundancy so much as the first half *of* it. Doing it under P8 first and again under P11 would
be doing it twice, and the second pass would find the first one's assumptions wrong (a Redis that
is fine for one cluster is not automatically fine across regions -- ADR-0044's own unresolved
PostgreSQL cross-region question is the same shape).

### What that phase inherits

NRF's profile registry, AMF's `UeContextStore` (its security contexts and AMF-UE-ID index are
already Redis-backed), SMF's SM context store, UDM's subscription/registration maps, PCF's policy
association maps, AUSF's in-process auth state, and NSACF's slice counters and subscriptions
(ADR-0276 already discloses those as in-memory and process-local).

### Until then

P8 stays **Blocked** in `docs/COMPLIANCE_P1_P15.md` rather than being quietly recoloured, and the
production-blocker list keeps "a single AMF pod is simultaneously the capacity ceiling and the
failure domain" as its top entry. One HPA (UDR) remains the only one, for the reason ADR-0282 gave:
an HPA on a stateful NF is a data-consistency bug with a manifest in front of it.

---

