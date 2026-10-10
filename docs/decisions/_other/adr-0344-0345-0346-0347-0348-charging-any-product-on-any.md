## ADR-0344/0345/0346/0347/0348: charging any product, on any protocol, at 700M-CDR scale

**Date:** 2026-09-11. **Status:** accepted. User-directed: the charging system must be comparable
to a production vendor's, with no capability gaps.

### ADR-0344 — CHF implemented 1 of TS 32.291's 25 charging-information types

The specification defines **twenty-five** charging-information blocks. CHF parsed
`pDUSessionChargingInformation` and **silently discarded the other twenty-four**. An SMS, an MMTel
call, an MBS session and a PDU session produced byte-identical CDRs apart from rating group — the
charging system could accept an SMS charging request and lose every fact that made it an SMS.

All 25 are now detected, named and preserved. Coverage was **verified by diffing the generated DTO's
fields against the implemented list**, which is how `n2ConnectionChargingInformation` was caught:
25 on the DTO, 25 implemented, 0 missing. Asserting completeness without that diff would have been
a claim, not a check.

Storage is a **discriminator plus the preserved block** (`charging_information_type`,
`service_charging_information`), not a column per service. A column-per-service design means
hundreds of columns and a migration per 3GPP release; this one absorbs a new charging type with no
schema change at all.

### ADR-0345 — price on any field, without a code change

The rating engine scopes on a flat attribute map that was populated by hand, field by field. An
operator could scope on `dnnId` because someone had written that line, and could not scope on an
SMS message type or an MBS session id **at all**. Now every scalar in every present block is
flattened to `<Type>.<dotted.path>`, arrays indexed rather than collapsed (a tariff priced on the
first recipient of a multi-recipient MMS is a real product). Depth-bounded, so a pathological
document cannot turn one charging request into unbounded work.

### ADR-0346 — 4G and CAMEL traffic was unproductisable

README stated the gap: *"N40 only: Diameter Gy and CAP carry no TS 32.291 request and so match only
unscoped offerings."* Both call the same `charge_one_usage`, which already accepted an `attributes`
argument — they simply never passed one. Legacy voice, data, SMS and content could not be priced.

**What this deliberately does NOT do is invent AVP names.** TS 32.299 and RFC 4006 are not vendored
here, and the Diameter dictionary has no constant for Called-Station-Id or Service-Context-Id.
Writing `attributes["calledStationId"] = avp(30)` would be a code-to-name mapping recalled from
memory — invisible until an operator's tariff prices the wrong thing. Attributes are keyed by what
the **wire** carries: `Gy.avp.30`, `Gy.avp.10415.21`, with the vendor id included so code 21 from
3GPP cannot collide with code 21 from another vendor. Total coverage, zero invention, and vendor
extensions work the day they arrive. **If TS 32.299 is supplied, friendly names go alongside the
codes — never instead of them.**

Values are offered as both string and u32 where each applies, because an AVP's type is only known
from a dictionary this project does not have; a wrong type guess makes a tariff match nothing,
silently.

### ADR-0347 — the indexes the hot paths needed

**Balance bucket lookup was a sequential scan.** CHF resolves a bucket on *every* charging request;
with only a primary key that was 6,495 rows discarded and 2.278 ms per reservation, pegging one
Postgres core and capping the pipeline at ~60 CDRs/sec. A GIN index took it to 0.058 ms (**~39x**)
and end-to-end throughput from **60 to 131 CDRs/sec**. It is O(n) in bucket count: a slow query at
thousands, an outage at millions.

**`rating_decision` had only a primary key** — every `explain_charge` lookup was a sequential scan
plus a sort. That defect was introduced by ADR-0332 and is fixed with an expression index, plus
composite indexes for the bill run and for tariff/time analytics.

### ADR-0348 — physical design for 700M+ CDRs

The CDR table was **unpartitioned**, 10 buckets, distributed on `charging_data_ref`. At 700M rows
that is ~70M rows per tablet, every billing-period query scans everything, and retention must DELETE
rows rather than drop partitions.

Now: **date-partitioned** with dynamic 400-day partitions, **32 buckets**, hashed on
`subscriber_identifier` and with it second in the key — so a bill run, the customer agent's charge
history and NWDAF's per-subscriber sequences are all single-tablet prefix scans after pruning
instead of scatter-gather. **Verified on a real Doris: `partitions=1/4` with a date predicate
against `2/4` without.**

Two consequences stated rather than discovered: a retransmission **crossing midnight** lands in a
different partition and will not dedup (a narrow edge case, and the price of being able to prune at
all), and **`replication_num=1` is lab-only** — billing records with one replica have no
redundancy, and production must raise it.

