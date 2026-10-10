## ADR-0300: every Partial and Not-supported commercial product is now mandatory, plus slice-based rating

**Date:** 2026-09-05. **Status:** accepted, user-directed, mandatory. **Scheduling:** after the
in-flight items (ADR-0299 MAP server, NEF's AF-facing surface).

User direction, verbatim in intent: *"Partial and Not supported is a MUST to implement as it is
required for a standard Telco. Also Slice based products also needs commercial rating model to
support."*

This closes the gap between what `README.md`'s commercial-products table honestly reports and what
a real operator can actually sell. Every non-Supported row becomes committed work rather than a
disclosure.

### The mandate, itemised

| # | Product | Today | What "done" requires |
|---|---|---|---|
| C1 | **Shared / family / group bundle** | **Not supported** | Balance buckets are keyed by SUPI (`bucket.id = supi`). Needs a group-bucket concept: a bucket a set of SUPIs draw from, membership held in the BSS (TMF632 Party / TMF637 Product Inventory), and reservation/finalize routed to the group bucket when the subscriber belongs to one |
| C2 | **Time-based bundle / per-minute voice** | Partial | `GrantedUnit.time` is never populated. Needs a duration `unitOfMeasure` in the rating engine, which then makes CAP's `maxCallPeriodDuration` real (ADR-0298) and CAP's finalization proportional (ADR-0297) — one change closes three disclosed gaps |
| C3 | **Tiered / fair-use throttling** | Partial — decided, not enforced | PCF pushes the `authSessAmbr` decision and SMF records it; it is never applied on the user plane. Needs the SMF→UPF PFCP path to install the rate limit |
| C4 | **Voice + data combined bundle** | Partial | Expressible only as separate rating groups. Needs a shared allowance one offering can span across rating groups |
| C5 | **Roaming bundle** | Partial | Rating does not distinguish roaming from home traffic. The rating half is buildable now (serving-PLMN vs home-PLMN is already known at charging time); see the blocked note below for settlement |
| C6 | **Postpaid billing / invoicing** | Partial | CDRs land in Doris; there is no bill run. Needs TMF678 `CustomerBill` generation from rated usage, and TMF666 account balance roll-up |
| C7 | **Slice-based products** | **Does not exist as a concept** | New. S-NSSAI is carried through the charging path but is not a rating dimension: an operator cannot price a slice differently, sell a slice-scoped allowance, or rate per-slice. Needs S-NSSAI as a first-class rating input alongside `ratingGroup`, and slice-scoped offerings in the catalog |

### Both dependencies I flagged are RESOLVED -- and the first flag was factually wrong

**Correction, same day.** I wrote that roaming settlement was "not startable without the
specification" and that "no copy is in this repository". That was wrong, and checking before
writing it would have shown so. `libs/tap3-core` cites **GSMA TD.57, "TAP 3.12 Format
Specification" V36.4, 15 May 2019** in its own header and contains **112 encode/decode functions**
covering the full envelope (`DataInterchange` -> `TransferBatch`/`Notification` ->
`BatchControlInfo`/`AccountingInfo`/`NetworkInfo`/`AuditControlInfo`) and **all nine
`CallEventDetail` variants** -- MobileOriginatedCall, MobileTerminatedCall, GprsCall,
ContentTransaction, MessagingEvent, MobileSession, LocationService, SupplServiceEvent,
ServiceCentreUsage -- plus ChargeInformation/ChargeDetail/TaxInformation/DiscountInformation and
AggregatedUsageRecord. Both directions. The ASN.1 detail the user pointed me back to is already
extracted and in the codebase.

So the real gap was never spec material. It is **processing**, and that is a different and much
more tractable piece of work:

| | What exists | What is missing |
|---|---|---|
| **TAP OUT** | `encode_data_interchange`, `make_tap3_roaming_cdr_file` (bss/roaming-interconnect) | Nothing collects this project's own roaming CDRs into a batch, applies sequence numbering/audit totals, or emits a file on a schedule |
| **TAP IN** | `decode_data_interchange`, `decode_tap3_roaming_cdr_file` | Nothing ingests a partner's file, validates it, rates the inbound records, or posts them to the BSS |
| **RAP** (returned accounts) | nothing | Genuinely absent -- a separate GSMA document (TD.32), and no `ReturnBatch`/`RapBatch` type exists in `tap3-core` |
| **NRTRDE** (near-real-time fraud exchange) | nothing | Genuinely absent -- separate GSMA document (TD.35) |

**User direction: TAP IN and TAP OUT processing are both required.** They are in scope now and
need no further documents. RAP and NRTRDE remain honestly unstarted and would need TD.32/TD.35 --
that narrower statement is the one that is actually true, unlike the sentence it replaces.

### C7 answered: attribute-based charging, not a fixed slice model

**User direction, verbatim in intent:** *"10GB is usable on Slice ID 1 OR 5GB on Slice ID 10 OR
UPF ID 5. Such model MUST be supported. Ideally any attribute coming to CHF on N40/N28 shall be
used for Charging and model product."*

That is broader than the three options I offered, and it supersedes them. The requirement is not
"S-NSSAI as a rating dimension" -- it is **a general attribute-based rating model**: any attribute
arriving at CHF over N40 (`Nchf_ConvergedCharging`, from SMF) or N28
(`Nchf_SpendingLimitControl`, from PCF) must be usable both as a rating input and as a product
definition dimension.

Concretely that means an allowance is scoped by a *predicate over request attributes*, not by a
rating group alone:

- 10 GB usable where `sNssai == {sst:1, sd:...}`
- 5 GB usable where `sNssai == {sst:10, ...}`
- an allowance usable where `uPFID == <id>`
- and, by construction, the same mechanism for `dnn`, `ratType`, `servingNetworkId` (which is what
  makes C5's roaming *rating* fall out of the same change), `chargingCharacteristics`, and anything
  else the real TS 32.291 request already carries.

Design consequence, stated now because it determines the shape: the rating engine currently keys
on `ratingGroup` alone (`build_rating_grant(catalog_client, rating_group, ...)`). This needs the
whole `MultipleUnitUsage` plus its parent `ChargingDataRequest` context available to matching, and
TMF620 offerings need an attribute-predicate characteristic so the scope is **catalog data, not
code** -- principle P7, which would otherwise be broken by hardcoding slice ids.

This makes C7 the largest item in this ADR and arguably the one the others depend on: C5's roaming
rating, C1's group scoping and C2's time grants all become configurations of the same mechanism
rather than three separate features.

### Order (revised after the clarifications above)

**C7 first, not last.** It was ordered last when it looked like one more product type awaiting a
business decision. With attribute-based rating as the requirement, it is the *mechanism* the others
are expressed in: C5's roaming rating is a predicate on `servingNetworkId`, C1's group scoping and
C2's time grants both plug into the same matching. Building C2 or C5 first would mean building them
twice.

So: **C7** (attribute-predicate rating + catalog shape), then **C2** (duration `unitOfMeasure` --
still one change closing three disclosed gaps across two protocols), **C5-rating**, **C1** (group
buckets), **TAP IN / TAP OUT processing**, **C3**, **C4**, **C6**.

Nothing here is claimed as started. `README.md`'s table stays exactly as honest as it is until each
row actually changes.

