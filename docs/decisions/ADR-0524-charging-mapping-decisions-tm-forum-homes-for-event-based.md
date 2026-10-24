## ADR-0524: Charging mapping decisions: TM Forum homes for event-based charging, grants, sharing, ordering (CHG-02)

**Date:** 2026-10-24. **Status:** Accepted (user-approved 2026-10-24).

**Context.** REVIEW-5ae0ef1 CHG-02: `docs/CHARGING_MAPPING.md` still called itself a draft with no
mapping code, while mapping code exists, and it carried seven open questions plus about 20
`*ChargingInformation` blocks without a decided mapping. CLAUDE.md requires those to be asked, not
decided silently. Recommendations were put to the user, who approved all of them ("CHG-02 approved").

**Decision.** Recorded as the "Mapping decisions (CHG-02)" table in `docs/CHARGING_MAPPING.md`:
1. Event-based blocks (registration, NSSAA, EAS deployment, NSM) -> TMF635 `ProductUsage`,
   quantity 1; TMF688 `Event` only as an optional notification.
2. `networkSharingChargingInformation` -> `ProductUsage` per record, one referenced TMF651
   `Agreement`.
3. TMF723 `Policy` out of scope until its specification is supplied.
4. `CustomerOrder` in scope as TMF622 `ProductOrder` (added to CLAUDE.md's SID entity list).
5. `grantedUnit` is a reservation (TMF654 `ReserveBalance`), not a billing entity; only used units
   become `ProductUsage` -> `AppliedCustomerBillingRate`.
6. TMF622 `ProductOrder` is built after CHG-01.
7. `CHARGING_PROMPT.md` P4.7 aligned to CLAUDE.md's TMF API list.
All remaining blocks follow rule 1: metered -> `ProductUsage` with quantities, one-off ->
`ProductUsage` quantity 1. The document header now states what is and is not backed by code.

**Rejected alternatives.**
- *TMF688 `Event` as the record for one-off charging.* TMF688 is a notification hub; billing works
  from usage, so an Event-only record would never be rated or billed.
- *Mapping each network-sharing record to both an Agreement and a usage.* Creates one contract per
  CDR; the contract is a single long-lived entity.
- *Billing granted units.* Charges the subscriber for unused quota.
- *Implementing TMF723 from the community-forum description.* Unconfirmed field names (guardrail 1).

**Disclosed.** Documentation and scope only; no mapping code changes in this ADR. TMF622
`ProductOrder` and the per-block `ProductUsage` population are not built. TM Forum field names are
to be checked against the published swagger when code is written.
