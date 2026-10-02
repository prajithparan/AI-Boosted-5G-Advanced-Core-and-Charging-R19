# UCS Data Model — Entities E1-E10

**Status: DRAFT — P4.1 deliverable, no code.** Per `CHARGING_PROMPT.md`'s P4.1 gate: "Read
PROMPT.md and CHARGING_PROMPT.md. Produce two documents, no code... Flag every place where the
entity chart and 3GPP disagree, and ask me." This document and `docs/DECISIONS.md`'s new UCS
architecture ADR are those two documents. No BSS/charging implementation code is added or changed
by this pass.

**2026-10-02 reconciliation note (`docs/DECISIONS.md` ADR-0445), left as DRAFT deliberately rather
than silently rewritten:** E6's "Persistence" line below (and the polyglot table's "hot balance"
entry) sketches a two-store design — live balance in Redis/Valkey, durable ledger in PostgreSQL —
that this document itself deferred to "the architecture ADR, not resolved here." That ADR was never
written until ADR-0445, and the implementation that actually shipped
(`bss/balance-management`, ADR-0056/0057/0385) took a different path: **PostgreSQL alone is
authoritative for `Bucket`/balance, reserve, and the ledger** — no Redis/Valkey balance projection
was built. ADR-0445 evaluates this gap directly (as "Option 1" against two alternatives that would
have matched this document's original two-store sketch) and keeps PostgreSQL-only, for reasons
recorded there (no measured need for a hot-balance cache; the harder atomicity problem is already
solved by Postgres row locks). This document's E6 sketch is left as the historical P4.1 planning
artifact it was — not updated to match the ADR line-by-line — so a reader comparing the two sees
the actual divergence instead of a doc quietly rewritten to agree with the code after the fact.

This document expands CHARGING_PROMPT.md's ten authoritative entities (E1-E10) into concrete
schema sketches. For each entity: SID entity mapping, 3GPP mapping, persistence store, and
consistency requirement, as P4.1 requires.

## Sourcing discipline for this document

Same arms-length rule as `docs/CHARGING_MAPPING.md`: every TM Forum field cited below is reused
from that document's own already-confirmed research (fetched from TM Forum's real public swagger
JSON, not recalled from memory) — no new TMF fields are asserted here without a citation back to
it. Every 3GPP field cited below is reused from fields already read out of this repo's own vendored
`specs/5G_APIs-REL-19/` YAML (`TS32291_Nchf_ConvergedCharging.yaml`, already used in
`nfs/chf/`/`nfs/smf/` and in `docs/CHARGING_MAPPING.md`). Where a claim is grounded only in
secondary sources (web search summaries of TS 28.541, not the primary spec text — this repo does
not vendor the 28-series), it is marked **[secondary source, not primary text]** below and is not
used as if it were spec-confirmed the way vendored YAML fields are.

## Finding, flagged per P4.1's instruction: the entity chart's "3GPP NRM/IOC" column does not
## have a real home for most of E1-E10 — flagged, not silently resolved

CHARGING_PROMPT.md's brief says each of the ten entities "must map onto TM Forum SID entities AND
3GPP 5G NRM / IOC elements." Checking this directly against what NRM/IOC actually is:

- **3GPP's NRM/IOC family (TS 28.541 "5G NRM", and the wider TS 28.5xx series) is a *network
  resource / network-function configuration-management* information model** — it defines managed
  object classes for RAN and core network functions (e.g. `NRCellDU`, `GNBDUFunction`,
  `NetworkSlice`, `NetworkSliceSubnet`) for provisioning, fault, and performance management, per
  its own real, secondary-source-confirmed scope **[secondary source, not primary text — this repo
  does not vendor TS 28.541]**. It is not a subscriber/billing/product data model.
- **TS 28.201 / 28.202**, also named in CHARGING_PROMPT.md's Specification Authority list under the
  heading "charging NRM," are real specs, but web search confirms their actual titles are
  narrower than that heading implies: **TS 28.201 is "Network slice performance and analytics
  charging in the 5GS"** and **TS 28.202 is "Network slice management charging in the 5GS"**
  **[secondary source, not primary text]** — both scoped specifically to *charging for network
  slice management/analytics operations*, not a general charging entity model. This matches what
  `docs/CHARGING_MAPPING.md` already found independently, from the real vendored YAML: the
  `nSMChargingInformation` and `nSPAChargingInformation` blocks on `ChargingDataRequest` are for
  exactly this (slice management operations charging, and slice performance/analytics-data
  consumption charging respectively) — see that document's "Reference architecture" section.
- **The real, primary 3GPP authority for E1-E10's subject matter (subscriber, product, session,
  CDR, rating, balance, roaming, security, front-end) is the TS 32.2xx charging series**
  (32.240/32.290/32.291/32.298/32.299) and TS 32.296 (module decomposition), which this repo does
  vendor (32.291's OpenAPI YAML) and already uses — **not** the 28.541 NRM/IOC family.

**Conclusion, stated plainly rather than papered over**: a genuine, confirmable 3GPP NRM/IOC
mapping exists for only a narrow slice of this entity chart — specifically the network-slice
angle of **E3** (session establishment, where a charged session may be *for* slice-management/
slice-analytics consumption per `nSMChargingInformation`/`nSPAChargingInformation`) and **E10**
(slice-as-a-product for the ENTERPRISE branch, where a sold product *is* a `NetworkSlice`/
`NetworkSliceSubnet` instance). For the other eight entities, the table below states "No 3GPP
NRM/IOC home found; real 3GPP authority is TS 32.2xx (charging series), cited instead" rather than
inventing an IOC class name to fill the cell. **This is the disagreement CHARGING_PROMPT.md's own
instruction asked to be flagged and asked about — flagging it here, asking the user to confirm
before P4.2 proceeds under this interpretation.**

## Persistence store legend

Per CHARGING_PROMPT.md Section A's explicit "POLYGLOT PERSISTENCE" table (quoted verbatim, not
reinterpreted):

| Category | Technology | Used for |
|---|---|---|
| RDBMS | **PostgreSQL** | subscriber, product catalogue, tariff, invoice |
| In-memory | **Redis / Valkey** | live sessions, quota/reservation state, hot balance |
| JSON store / NoSQL | (to be evaluated — see ADR) | flexible product definitions, mediation records |
| TSDB | **Prometheus** (metrics), **Apache Doris** (CDR/usage analytics, ADR-0192) | metrics; CDR/usage analytics |
| Distributed FS / object store | (to be evaluated — see ADR) | CDR archive, long-term retention |

This directly answers the user's earlier, separate question about product-catalog persistence
(currently in-memory-only in `bss/product-catalog`): **PostgreSQL**, per this table's own
"product catalogue" line — not a new decision, just this table applied to that already-approved
extension work, which resumes after this P4.1 gate closes.

---

## E1. Subscriber Management

**Purpose** (CHARGING_PROMPT.md): identification data, service preferences, billing details, bill
cycles; prepaid and postpaid in one model.

**Schema sketch** (proposed for this project — not itself a spec-mandated shape; the *identifiers
and identity fields* inside it are grounded in real sources as cited):

```
Subscriber
  id                    (internal PK)
  supi                  string      -- TS 23.501 identifier; already the one field wired
                                        end-to-end in this codebase today (bss-sid, ADR-0045)
  msisdn                string, optional
  account_type          enum { CONSUMER, ENTERPRISE }   -- see E10
  parent_account_id     FK -> Account, nullable          -- populated only for ENTERPRISE
  charging_mode         enum { PREPAID, POSTPAID }
  bill_cycle_day        int, 1-28
  service_preferences   jsonb   -- notification prefs, spending-limit opt-ins, etc.
  created_at / updated_at
```

**SID mapping**: `Party` played as `Individual` (TMF632 Party Management). Real confirmed fields
(from `docs/CHARGING_MAPPING.md`, already live-wired): `id`, `href`, `individualIdentification`
(array of `{identificationType, identificationId}` — SUPI stored as
`identificationType="SUPI"`), `partyCharacteristic`. Prepaid/postpaid and bill-cycle-day are
`Individual.partyCharacteristic` entries (generic name/value bag — the correct fit here, unlike
SUPI, since these are supplementary attributes, not a structured external identity document).

**3GPP mapping**: No 3GPP NRM/IOC home (see flagged finding above). Real 3GPP authority:
TS 32.291's `SubscriberIdentifier` field on `ChargingDataRequest` (already vendored, already
wired) supplies the SUPI value this entity is keyed on; TS 23.501 defines SUPI itself. Not an
NRM/IOC mapping.

**Persistence**: **PostgreSQL** — subscriber data, per the polyglot table.

**Consistency**: Strong (single-row ACID) for identity/account-type/parent-account fields — these
gate authorization and billing-hierarchy decisions elsewhere. Bill-cycle-day and preferences can
tolerate eventual consistency in read replicas.

---

## E2. Service Catalog + Charging Plans

**Purpose**: available services, descriptions, pricing, associated charging policies; charging
plans (rate plans, rating mechanisms, pricing models) as **data, never code** (P7).

**Schema sketch**: this is the TMF620 extension already approved (separately from this P4.1 gate)
and currently paused behind it — see `libs/bss-sid/include/bss_sid/product.hpp`'s existing
`ProductOffering`/`ProductOfferingPrice` structs, to be extended with the already-researched real
TMF620 fields (`prodSpecCharValueUse`, `category`, `productSpecification`, etc. — full list in
`docs/CHARGING_MAPPING.md`'s reference-architecture section). Not re-specified field-by-field here
to avoid duplicating that already-approved work; this entity's row exists so the persistence/
consistency columns are recorded alongside the other nine.

**SID mapping**: `ProductOffering` + `ProductOfferingPrice` (**TMF620**), `ProductSpecification`
(**TMF620**), `Service`/`ServiceSpecification` (**TMF638**/**TMF633**) for the charging-plan's
underlying service definition. All real, confirmed fields already listed in
`docs/CHARGING_MAPPING.md`.

**3GPP mapping**: No 3GPP NRM/IOC home found for "charging plan" as a concept (it is a TMF/BSS
construct, not a 3GPP-defined object). Where a charging plan's *subject* is a network slice
product (ENTERPRISE branch, E10), the underlying resource it prices is a `NetworkSlice`/
`NetworkSliceSubnet` **[secondary source, not primary text]** — flagged as the one confirmable
tie-in, same caveat as the top-level finding above.

**Persistence**: **PostgreSQL** — "product catalogue, tariff" per the polyglot table (RDBMS row)
for the structured, relationally-queried catalog entities (`ProductOffering`,
`ProductOfferingPrice`, `ProductSpecification` header rows); **JSON store/NoSQL** for the
catalog's "flexible product definitions" per the polyglot table's own explicit second line for
this exact case — specifically `prodSpecCharValueUse`'s variable-shape characteristic-value
constraints (regex, cardinality, per-characteristic value sets), which do not have a fixed
relational shape and are the TMF620 mechanism this project already identified as the key to
GUI-driven dynamic configuration. **Concretely: ProductOffering/ProductOfferingPrice/
ProductSpecification header fields → PostgreSQL columns; each one's `prodSpecCharValueUse` array →
a `jsonb` column on the same PostgreSQL row**, not a separate NoSQL store — PostgreSQL's native
`jsonb` type satisfies the "flexible product definitions" requirement without introducing a second
database technology purely for this one entity. (A dedicated NoSQL store is reserved for E4's
mediation records, where the polyglot table's own wording applies more literally — see E4.)

**Consistency**: Strong for catalog *publication* (a `lifecycleStatus` transition to `Active` must
be atomic and immediately visible to the rating engine — a subscriber must never be rated against
a half-published tariff). Read-heavy at rating time — expect a cached/replicated read path in
front of PostgreSQL once P4.2/P4.3 build the rating engine's hot path, not designed here.

---

## E3. Session Establishment

**Purpose**: creates/manages charging sessions; tracks start/end times; N40 (SMF)/N28 (PCF)
session management; protocol session details; service units derived from customer profiles.
Idempotent and recoverable across restarts and network partitions (explicit requirement).

**Schema sketch**, grounded directly in the real, already-vendored, already-used
`TS32291_Nchf_ConvergedCharging.yaml` (`ChargingDataRequest`/`ChargingDataResponse` — the exact
fields this codebase's CHF/SMF integration already sends today, per ADR-0044/ADR-0050/ADR-0051):

```
ChargingSession
  charging_data_ref        string (PK)  -- CHF-issued, TS 32.291 chargingDataRef
  supi                     FK -> Subscriber
  nf_consumer_id           string       -- nfConsumerIdentification.nodeFunctionality (SMF/AMF/...)
  invocation_time          timestamp    -- invocationTimeStamp
  next_invocation_seq      bigint       -- CHF-internal; see ADR-0051's ChargingDataInvocationSeqStore,
                                            already implements exactly this, real code today
  session_state            enum { OPEN, UPDATING, CLOSED }
  multiple_unit_usage      jsonb        -- MultipleUnitUsage[] as sent/received per TS 32.291
                                            (ratingGroup, grantedUnit, usedUnitContainer, ...)
  created_at / last_updated_at
```

**SID mapping**: no direct SID entity *is* a charging session — TM Forum's nearest concept is
`ProductUsage` (**TMF635**) once the session's usage is realized as a usage record (see E4); the
session itself is 3GPP-native protocol state, same "not a SID business entity" category
`docs/CHARGING_MAPPING.md` already assigned to `nfConsumerIdentification`/
`invocationSequenceNumber`.

**3GPP mapping**: **Real 3GPP authority, directly**: TS 32.291 `ChargingDataRequest`/
`ChargingDataResponse` (already vendored YAML, already implemented in `nfs/chf/`). Not an
NRM/IOC mapping (see top-level finding) — except where the session's *subject matter* is slice
management/analytics consumption (`nSMChargingInformation`/`nSPAChargingInformation`, already
catalogued in `docs/CHARGING_MAPPING.md`), which is the one place this entity does touch a real,
secondary-source-confirmed IOC concept: `NetworkSlice`/`NetworkSliceSubnet` **[secondary source]**.

**Persistence**: **Redis/Valkey** — "live sessions, quota/reservation state" is the polyglot
table's own explicit first in-memory line, and matches this codebase's existing direction (ADR-0050
already put quota-consumption tracking's live counters in a BPF-map-backed, non-PostgreSQL path
for exactly this kind of hot, session-lifetime state). Session records that must survive past
session end (for CDR/audit purposes) are written through to PostgreSQL or Apache Doris at close —
which store is E4's concern, not E3's.

**Consistency**: The entity's own stated requirement is explicit: "idempotent and recoverable
across restarts and network partitions." Design implication for the ADR: session state needs an
idempotency key (the real `chargingDataRef`/`invocationSequenceNumber` pair this codebase already
generates) and a write-ahead or replicated Redis configuration (not a bare single-instance cache) —
detailed in the architecture ADR's exactly-once-accounting section, not repeated here.

---

## E4. Usage Records (CDR) — CDF Function

**Purpose**: real-time data usage, call duration, SMS counts, other service-specific usage;
standardised 3GPP CDR formats (TS 32.298) AND custom formats; duplicate detection and gap
detection mandatory.

**Schema sketch**:

```
UsageRecord
  id                    (PK, ULID/UUID — not a 3GPP field, internal only)
  charging_data_ref      FK -> ChargingSession (E3)
  supi                   FK -> Subscriber
  rating_group            int, optional     -- TS 32.291 ratingGroup
  service_id              int, optional     -- TS 32.291 serviceId
  usage_type               enum { VOLUME, TIME, EVENT, SERVICE_SPECIFIC }
  volume_octets           bigint, optional
  duration_seconds        bigint, optional
  event_count             int, optional
  cdr_format               enum { TS32298_ASN1, TS32298_JSON, CUSTOM }
  raw_cdr                  bytea/jsonb       -- the actual CDR payload, format per cdr_format
  sequence_number          bigint            -- for gap detection
  dedup_key                string, unique    -- (charging_data_ref, invocation_sequence_number) —
                                                 for duplicate detection, per E3's idempotency key
  recorded_at
```

**SID mapping**: `ProductUsage` (**TMF635**). Real confirmed fields (`docs/CHARGING_MAPPING.md`):
`id`, `href`, `status`, `usageType`, `usageCharacteristic`, `serviceUsage` (→ **TMF727** Service
Usage Management as of the v4→v5 uplift), `relatedParty`, `usageProductPrice`, `product`,
`usageSpecification`.

**3GPP mapping**: **Real 3GPP authority, directly**: TS 32.298 (CDR parameter description — named
in CHARGING_PROMPT.md's own Specification Authority list) for the standardised CDR field set, and
the same TS 32.291 `MultipleUnitUsage` structures E3 uses for the online-charging-time view of the
same data. **Caveat, disclosed**: TS 32.298 itself is not vendored in this repo (only its OpenAPI
sibling 32.291 is) — its exact field-level parameter list is not confirmed from primary text here
and must be sourced (vendored YAML if one exists, or asked-for spec text) before P4.4 (CDF/CGF)
writes real encode/decode code, not assumed from this sketch. No NRM/IOC mapping (see top-level
finding) — a CDR is a charging-domain record, not a network-configuration object.

**Persistence**: **Apache Doris** (ADR-0192 records the migration and the full engine
comparison) — "CDR/usage analytics" per the polyglot table's TSDB row, directly. Raw CDR
archival (long-term, retention-driven per P14) additionally needs **distributed FS / object
store** per the polyglot table's last line — Doris for query/analytics access, object storage for
the immutable archival copy; both, not either/or, matching the table's own two separate rows.

**Consistency**: Duplicate detection and gap detection are explicit, named requirements — implies
the `dedup_key` uniqueness constraint above must be enforced at write time (Doris's own Unique Key
model with Merge-on-Write performs real, immediate dedup at insert time — a genuine improvement
over the previous engine's merge-time-only dedup, see ADR-0192 for the comparison) and `sequence_number` continuity must be monitorable (a gap is itself
a P12 business-level alarm condition, not just a log line).

---

## E5. Rating Function (RF)

**Purpose**: online charging (real-time rating for data/voice); offline charging (batch for bulk
SMS, TAP-based roaming records, delayed charging). Deterministic and reproducible.

**Schema sketch**:

```
RatingDecision
  id                      (PK)
  usage_record_id           FK -> UsageRecord (E4), nullable (online path may rate before full
                                                                  UsageRecord exists — see below)
  tariff_id                  FK -> ProductOfferingPrice (E2), with tariff_version pinned
  rating_group / service_id  (as E3/E4)
  input_snapshot             jsonb  -- usage, product, tariff-version, balance-at-decision-time,
                                        timestamp: the full (usage, product, tariff, balance, time)
                                        tuple CHARGING_PROMPT.md's principle 1 requires be
                                        reproducible from
  rated_amount                numeric(18,6), currency char(3)   -- OR raw unit count if unit-rated
  rule_fired_id                string  -- which tariff rule/rate-plan-line fired, for principle 2
                                            ("every charge is explainable")
  ai_advisory                 jsonb, nullable  -- model id/version/score/reason-codes IF an AI
                                                    signal informed this decision (Angle 1); the
                                                    deterministic rule_fired_id above is what
                                                    actually acted — governance requirement, not
                                                    built until P4.8
  decided_at
```

**SID mapping**: `AppliedCustomerBillingRate` (**TMF678**) once rated. Real confirmed fields
(`docs/CHARGING_MAPPING.md`): `appliedTax`, `bill`, `date`, `description`, `isBilled`, `name`,
`periodCoverage`, `taxExcludedAmount`, `taxIncludedAmount`, `appliedBillingRateType`,
`billingAccount`, `product`, `characteristic`. **Already-flagged open TODO, restated not
re-resolved here**: mapping TS 32.291's pre-rating `GrantedUnit`/`AllocatedUnit` onto this
post-rating TMF concept is not a clean 1:1 (`docs/CHARGING_MAPPING.md`'s own TODO) — belongs to
P4.3's real rating-engine design, not this entity sketch.

**3GPP mapping**: No 3GPP NRM/IOC home (rating is a charging-domain function, not a network
resource). Real 3GPP authority: TS 32.290 (5G charging services, defines rating-adjacent concepts
like rating group) and TS 32.291's `MultipleUnitInformation`/`GrantedUnit` structures, both
already vendored/cited above.

**Persistence**: Not explicitly named in the polyglot table under its own line; by the same
reasoning as E2 (structured, relationally-queryable, audit-critical financial records) →
**PostgreSQL**, alongside tariff/invoice per that table's own RDBMS row (`RatingDecision` is
closer in kind to "invoice" than to "live session state"). The `input_snapshot`/`ai_advisory`
variable-shape fields use PostgreSQL `jsonb` columns, same reasoning as E2's
`prodSpecCharValueUse` — not a second database.

**Consistency**: Strong, and specifically **reproducible**, per principle 1 ("same inputs -> same
charge, forever, provably") — this is a stronger requirement than ordinary ACID durability; it
requires the `input_snapshot`/`tariff_version` pinning above so a rating decision can be
re-derived and checked later even if the live tariff has since changed. Detailed further in the
architecture ADR's "deterministic rating" section.

---

## E6. Balance Management (ABMF)

**Purpose**: account balances (prepaid/postpaid); recharges, debits, incurred charges;
multi-balance (main, bonus, promotional); explicit currency/rounding rules. **Explicit
requirement: balance updates require strong consistency.**

**Schema sketch**:

```
Balance   -- realized as a TMF654 Bucket, see SID mapping below; fields renamed to match the real
             confirmed TMF654 resource rather than an invented shape
  id                       (PK)                    -- Bucket.id
  account_id                 FK -> Account (E10)   -- NOT directly Subscriber: balances are held
                                                        at the account level so ENTERPRISE pooled/
                                                        shared quota (E10) is the SAME mechanism as
                                                        a CONSUMER's single balance, not a special
                                                        case -- Bucket.partyAccount, real TMF654 field
  usage_type                  enum { monetary, voice, data, sms, other }   -- Bucket.usageType, real
                                  TMF654 field -- **correction, 2026-08-11**: this document originally
                                  guessed this enum was `{MAIN, BONUS, PROMOTIONAL}`; re-checking the
                                  real TMF654 swagger directly (`UsageType` definition) found the actual
                                  enum is `{monetary, voice, data, sms, other}` -- it says WHAT KIND of
                                  quantity a bucket tracks, not which "pool" (main/bonus/promotional) it
                                  belongs to. TMF654 has no fixed enum for that distinction at all --
                                  multi-balance (main/bonus/promotional) is modeled as **separate Bucket
                                  resources**, distinguished by `name`/`description` (e.g. "Main Balance"
                                  vs "Promotional Bonus"), same as real telco balance-management practice.
                                  Flagged and fixed here rather than silently carried forward into P4.3.
  currency                   char(3)
  remaining_value             numeric(18,6)   -- Bucket.remainingValue, real TMF654 field
  reserved_value               numeric(18,6)   -- Bucket.reservedValue, real TMF654 field (online-
                                                    charging in-flight reservations, E3's granted units)
  rounding_rule               string   -- e.g. "round-half-up, 2dp" — data, not code, per P7 -- not
                                            a TMF654 field itself, project-internal rating-engine input
  is_shared                  bool     -- Bucket.isShared, real TMF654 field (!) -- confirms E10's
                                          ENTERPRISE pooled-quota concept is TMF654's own modeled
                                          case, not something this project had to invent
  status                     -- Bucket.status, real TMF654 field (BucketStatusType)
  version                    bigint   -- optimistic-concurrency token; every debit/credit CAS's
                                          on this, per the strong-consistency requirement below --
                                          project-internal, not a TMF654 field
  updated_at

AccumulatedBalance   -- realized as a TMF654 AccumulatedBalance, real resource, aggregates one
                         partyAccount's Buckets (Bucket.id array) into a totalBalance -- the actual
                         "what's my balance" query surface, resolves this document's earlier open
                         question (see below)
  id                        (PK)                    -- AccumulatedBalance.id
  account_id                  FK -> Account (E10)   -- AccumulatedBalance.partyAccount
  bucket_ids                   array<FK -> Balance>   -- AccumulatedBalance.bucket
  total_balance                 numeric(18,6)          -- AccumulatedBalance.totalBalance

BalanceTransaction   -- realized via TMF654's action resources (TopupBalance/AdjustBalance/
                        TransferBalance/ReserveBalance, each a real, separate TMF654 resource) plus
                        BalanceActionHistory (real TMF654 audit-trail resource) rather than one
                        generic table -- kept as one project-internal table here for the ledger
                        sketch; P4.3 should model the real per-action-type TMF654 resources directly
  id                        (PK)
  balance_id                  FK -> Balance
  rating_decision_id           FK -> RatingDecision (E5), nullable (top-ups aren't rating-driven)
  delta_amount
  reason                      enum { DEBIT, CREDIT_TOPUP, CREDIT_REFUND, EXPIRY, RESERVE, TRANSFER }
                                  -- last two added after confirming TMF654's own ReserveBalance/
                                  TransferBalance actions are real, separate resources
  idempotency_key             string, unique   -- exactly-once accounting, principle 5
  recorded_at
```

**SID mapping — resolved, real TMF654 resources confirmed directly against the actual swagger
(`tmforum-apis/TMF654_PrepayBalanceManagement`, `TMF654-PrepayBalance-v4.0.0.swagger.json`), not
guessed**: TMF654 is far richer than the single `TopupBalance` resource this document originally
checked. Real paths/resources: `/bucket` (**`Bucket`** — the actual balance-snapshot resource this
document's earlier draft flagged as an open question; confirmed real fields: `id`, `href`,
`confirmationDate`, `description`, `isShared`, `name`, `remainingValueName`, `requestedDate`,
`logicalResource`, `partyAccount`, `product`, `relatedParty`, `remainingValue`, `reservedValue`,
`status`, `usageType`, `validFor`), `/accumulatedBalance` (**`AccumulatedBalance`** — aggregates a
party account's buckets: `id`, `href`, `description`, `name`, `bucket`, `logicalResource`,
`partyAccount`, `product`, `relatedParty`, `totalBalance` — this is the real "what's my current
balance" query surface), `/topupBalance` (**`TopupBalance`** — the resource this document
originally cited, real fields as before: `partyAccount`, `paymentMethod`, `recurringPeriod`,
`nrOfPeriods`), `/adjustBalance` (**`AdjustBalance`**), `/transferBalance` (**`TransferBalance`**),
`/reserveBalance` (**`ReserveBalance`** — the online-charging quota-reservation action, directly
relevant to E3's granted-unit reservations), `/balanceActionHistory`
(**`BalanceActionHistory`** — the real TMF654 audit-ledger resource, an alternative real-TMF-home
for part of what this document's E8 `AuditRecord` sketch covers for balance mutations
specifically). **Open question resolved**: yes, TMF654 has a real balance-query resource (`Bucket`
directly, `AccumulatedBalance` for the aggregate/total view) — not guessed, confirmed from the real
swagger definitions list.

**3GPP mapping**: No 3GPP NRM/IOC home. Real 3GPP authority: TS 29.594 (`Nchf_SpendingLimitControl`,
already named in CHARGING_PROMPT.md's Specification Authority list) for the SBI-facing spending-
limit/balance-notification surface.

**Persistence**: **Redis/Valkey** — "hot balance" is explicit in the polyglot table's in-memory
line — for the live, contended `Balance.remaining_value`/`version` fields debited on every rating
decision. **Not what was built — see this document's 2026-10-02 reconciliation note at the top and
`docs/DECISIONS.md` ADR-0445: the shipped implementation is PostgreSQL-only for balance, by
deliberate decision, not yet a drift to fix.**
**PostgreSQL** for `BalanceTransaction`'s durable ledger (every debit/credit is a financial record
needing durable, queryable, auditable storage — matches the RDBMS row's "invoice"-adjacent
reasoning). This is a genuine two-store design (hot mutable state in Redis, durable append-only
ledger in PostgreSQL) — the write pattern (debit Redis balance, append PostgreSQL transaction,
both must succeed or neither does) is exactly the kind of cross-store atomicity CHARGING_PROMPT.md
flags as needing explicit design, detailed in the architecture ADR, not resolved here.

**Consistency**: **Strong, explicitly required by CHARGING_PROMPT.md itself.** The `version`
optimistic-concurrency column above and `BalanceTransaction.idempotency_key`'s uniqueness
constraint are this document's proposed mechanism; the ADR must additionally address Redis's own
durability posture (AOF/replication configuration) given a hot balance living primarily in an
in-memory store is in tension with "strong consistency" unless the persistence/replication
configuration is deliberately chosen for it — flagged for explicit treatment in the ADR rather
than assumed safe by default.

---

## E7. Roaming and Interconnect Agreements

**Purpose**: charging agreements with other operators for roaming; interconnect agreements;
reporting/analytics over both. Roaming settlement via GSMA TAP3/RAP/NRTRDE — **GSMA documents
behind membership; do not quote from memory; implement behind a stub codec until supplied.**

**Schema sketch** (intentionally shallow — the codec itself is explicitly out of reach until GSMA
specs are supplied, so this entity's schema is only the agreement/settlement wrapper, not any
TAP3/RAP/NRTRDE field layout):

```
InterconnectAgreement
  id                        (PK)
  partner_operator_plmn_id     string   -- TS 23.501 PLMN ID, real/confirmed field type
  agreement_type               enum { ROAMING, INTERCONNECT }
  rate_terms                   jsonb    -- partner-specific rating; shape TBD, not guessed
  valid_from / valid_to
  status

RoamingCdrFile
  id                          (PK)
  agreement_id                  FK -> InterconnectAgreement
  format                        enum { TAP3, RAP, NRTRDE, STUB }  -- STUB until GSMA specs supplied
  raw_payload                   bytea    -- opaque until a real codec exists
  received_at / processed_at
```

**SID mapping**: `Agreement` (**TMF651**). Real confirmed fields (`docs/CHARGING_MAPPING.md`):
`agreementAuthorization`, `agreementItem`, `agreementPeriod`, `agreementSpecification`,
`agreementType`, `associatedAgreement`, `characteristic`, `completionDate`, `description`,
`documentNumber`, `engagedParty`, `href`, `id`, `initialDate`, `name`, `statementOfIntent`,
`status`, `version`.

**3GPP mapping**: No NRM/IOC home. Real 3GPP authority for the *charging* side: TS 32.296 (OCS
applications and interfaces, named in CHARGING_PROMPT.md's own list) covers roaming/interconnect
OCS integration at a stage-2 level; TAP3/RAP/NRTRDE themselves are GSMA, not 3GPP, documents and
are explicitly out of reach per the brief's own instruction.

**Persistence**: `InterconnectAgreement` → **PostgreSQL** (matches TMF651's relationally-shaped
fields, same reasoning as E2/E7). `RoamingCdrFile.raw_payload` → **distributed FS / object store**
per the polyglot table's archival line — a settlement file is exactly the kind of large, immutable
artifact that row exists for.

**Consistency**: Strong for `InterconnectAgreement` (a settlement dispute over which rate terms
applied at a given moment is a real financial/legal exposure). `RoamingCdrFile` ingestion is
append-only and can tolerate eventual consistency on the analytics/reporting read path.

---

## E8. Security

**Purpose**: secure comms protecting charging/subscriber data; auth/authz for charging-request and
subscriber-access integrity; secure integration interfaces. TLS 1.3/mTLS everywhere, OAuth2 per
TS 33.501 on SBI, encryption at rest, no subscriber identifiers in clear-text logs, full audit
trail on every balance/tariff mutation.

**Schema sketch**: this entity is a cross-cutting control, not a data-bearing entity with its own
CRUD resource in the way E1-E7 are — its "schema" is the audit-trail table every other entity's
ADR-mandated mutations write to:

```
AuditRecord
  id                        (PK)
  entity_type                  enum { SUBSCRIBER, BALANCE, TARIFF, AGREEMENT, ... }  -- matches E1-E7
  entity_id                     string
  action                        string    -- e.g. "balance.debit", "tariff.publish"
  actor                          string    -- NF instance id, or human operator id for E9-driven changes
  before_snapshot / after_snapshot   jsonb
  ai_advisory_ref                FK -> nullable, if an Angle-1/4 model informed this action —
                                     governance requirement, not built until P4.8
  recorded_at
```

**SID mapping**: No dedicated SID entity for "security" as such — TM Forum's SID does not model
authN/authZ/audit as a business entity (it is infrastructure, same category as 3GPP's own
`nfConsumerIdentification`). Not mapped, same disclosed reasoning `docs/CHARGING_MAPPING.md`
already applied to comparable non-business fields.

**3GPP mapping**: No NRM/IOC home. Real 3GPP authority, directly: TS 33.501 (already this
project's standing security reference across every NF, not new to charging) for OAuth2/TLS
requirements on SBI; this codebase's existing `scripts/gen-lab-pki.sh`/OAuth2-from-NRF machinery
(already built, already used by every NF including the existing CHF work) is the concrete
implementation this entity's requirements already ride on — E8 does not need new transport-security
infrastructure, it needs the `AuditRecord` table above wired into every mutation E1-E7 make, which
is new.

**Persistence**: **PostgreSQL** for `AuditRecord` (durable, queryable, must survive at least as
long as the longest-retained financial record it audits — ties to P14 archival policy). Not named
in its own line in the polyglot table; placed here by the same "durable financial-adjacent record"
reasoning as E5/E6's ledger tables.

**Consistency**: Strong and **synchronous with the action it audits** — CHARGING_PROMPT.md's
"full audit trail on every balance or tariff mutation" reads as a hard requirement, not
best-effort logging; the ADR should treat `AuditRecord` writes as part of the same transaction/
idempotent-write unit as the mutation itself (same pattern as E6's Balance+BalanceTransaction
pairing), not a fire-and-forget side channel.

---

## E9. Front-End Layer / API Gateway — TM Forum ODA APIs

**Purpose**: front-end enablement for system configuration/service creation, including a
rule-engine language for expressing charging/rating logic without code (P7); all external exposure
through one JSON API gateway layer.

**Schema sketch**: this entity is architectural (a gateway + a rule-authoring surface), not a
distinct persisted business entity — its data-bearing part is the rule/tariff definitions it lets
an operator author, which already live in E2 (`ProductOfferingPrice`, data-driven per P7) and E6
(`Balance.rounding_rule`). No new schema proposed here beyond what E2/E6 already define; recorded
as its own row per CHARGING_PROMPT.md's ten-entity structure, not because it needs independent
storage.

**SID mapping**: The ODA "Party & Account Management" / "Product" / "Engaged Party" API family
(TMF620/632/622/etc., already cited above per-entity) collectively **is** this gateway's exposed
surface — TM Forum's SID does not name "API Gateway" itself as a business entity (it is TM Forum's
Open API *layer*, not a SID *entity* — the two are different parts of the TM Forum stack, and
CHARGING_PROMPT.md's own entity chart conflates them here, worth noting rather than forcing a
false SID-entity mapping).

**3GPP mapping**: No NRM/IOC home; not a 3GPP concept at all (API gateway is a BSS/architecture
concern).

**Persistence**: None of its own — stateless routing/auth layer; the rule/tariff data it exposes
is E2/E6's, already assigned above.

**Consistency**: N/A at this entity's own level (no state of its own); inherits E2/E6's
consistency requirements for whatever it's editing at the time.

---

## E10. Master Model → Consumer | Enterprise

**Purpose**: a single MASTER MODEL specializes into CONSUMER (individual subscriber, single
account, personal balances) and ENTERPRISE (account/party hierarchies, shared/pooled quotas,
split billing, bulk provisioning, contractual SLAs, slice-as-a-product/private-5G charging).
Explicit warning in the brief: modeling only the consumer case is a stop-and-tell-me condition.

This is the specialization every other entity above already threads through (`Subscriber
.account_type`/`.parent_account_id` in E1, `Balance.account_id`/`.is_shared` in E6) rather than a
separate table set bolted on afterward — per the brief's own explicit instruction against
retrofitting. Schema sketch, the MASTER `Account` entity both branches share:

```
Account   (MASTER — CONSUMER and ENTERPRISE are both instances of this, not separate tables)
  id                         (PK)
  account_kind                 enum { CONSUMER, ENTERPRISE }
  parent_account_id             FK -> Account, nullable, self-referential
                                    -- CONSUMER: always null (flat, single account)
                                    -- ENTERPRISE: forms the hierarchy (parent company -> subsidiary
                                       -> department -> employee), arbitrary depth via self-FK,
                                       NOT a fixed 4-level enum -- real enterprises have irregular
                                       depth and this must not hardcode a level count
  billing_mode                  enum { INDIVIDUAL, SPLIT }
                                    -- SPLIT: corporate vs personal usage on one subscription (E10's
                                       explicit split-billing requirement) -- realized via
                                       BalanceTransaction.reason / a per-transaction cost-center tag,
                                       not a separate transaction table
  cost_center                    string, nullable   -- ENTERPRISE split-billing tag
  contract_sla_id                 FK -> ServiceLevelAgreement, nullable  -- ENTERPRISE only;
                                                                             TMF651-adjacent, see below
  provisioning_mode               enum { INDIVIDUAL, BULK }  -- ENTERPRISE bulk provisioning flag
  created_at

Subscriber.parent_account_id  -- FK into Account (E1, already sketched above) -- an employee's
                                  Subscriber row hangs off the enterprise Account this way
Balance.account_id            -- FK into Account (E6, already sketched above), Balance.is_shared
                                  true for a pooled ENTERPRISE quota shared across several
                                  Subscriber rows under the same Account -- confirmed, this is
                                  literally TMF654 Bucket.isShared, not a project invention (see E6)
```

**Account hierarchy, resolved against the real TMF632 swagger** (not the earlier unconfirmed
`partyRelationship` guess): TM Forum's actual hierarchy mechanism lives on **`Organization`**
(`tmforum-apis/TMF632_PartyManagement`, `TMF632-Party-v4.0.0.swagger.json`), not `Individual` —
`Organization.organizationParentRelationship` / `Organization.organizationChildRelationship`, each
a real, confirmed `{relationshipType, organization}` structure referencing another `Organization`.
This means E10's `Account.parent_account_id` self-FK is realized, for the ENTERPRISE branch, as a
chain of real TMF632 `Organization` resources linked by these two fields — arbitrary depth,
matching this document's "not a fixed level count" requirement exactly, since TM Forum's own
mechanism is itself unbounded-depth. An individual employee's `Subscriber` (E1, `Individual`) links
to their enterprise's `Organization` via `Individual.relatedParty` (real field, generic
party-to-party reference) rather than via `organizationParentRelationship` (which is
`Organization`-to-`Organization` only) — so the full ENTERPRISE tree is: `Organization` chain
(company → subsidiary → department) via `organization{Parent,Child}Relationship`, with
`Individual` (employee) records attached to their department-level `Organization` via
`relatedParty`. CONSUMER accounts remain plain `Individual` records with no `Organization`
involved at all.

**Slice-as-a-product / private-5G charging** (ENTERPRISE-only, explicitly named in the brief): a
sold `ProductOffering` (E2) whose `resourceCandidate`/`serviceCandidate` (real TMF620 fields,
already confirmed in `docs/CHARGING_MAPPING.md`) references the specific `NetworkSlice`/
`NetworkSliceSubnet` **[secondary source, not primary text]** instance being charged for — this is
the concrete mechanism tying E2's product catalog to E10's enterprise branch and to the one
confirmable NRM/IOC touchpoint noted in this document's top-level finding. Contractual SLAs are
`ProductOffering.serviceLevelAgreement` (real TMF620 field, `SLARef`).

**SID mapping**: `Organization` (**TMF632**) for the ENTERPRISE hierarchy nodes, linked via the
real `organizationParentRelationship`/`organizationChildRelationship` fields confirmed above;
`Individual` (**TMF632**, E1) for employees, linked to their `Organization` via `relatedParty`;
`Agreement` (**TMF651**, E7) for `contract_sla_id`'s contractual side.

**3GPP mapping**: This is the one entity with a real, if secondary-sourced, NRM/IOC touchpoint —
`NetworkSlice`/`NetworkSliceSubnet` **[secondary source]** for the slice-as-a-product case, as
described above.

**Persistence**: **PostgreSQL** — `Account` is structurally identical in kind to `Subscriber` (E1),
same RDBMS reasoning.

**Consistency**: Strong, and specifically **hierarchy-aware**: a pooled-quota debit under an
ENTERPRISE `Account` must be consistent with respect to every `Subscriber` sharing that pool
(two employees spending the last of a shared quota concurrently must not both succeed) — this is
a strictly harder version of E6's own strong-consistency requirement (contention scales with
hierarchy fan-out, not just per-subscriber), flagged for explicit concurrency-control treatment
(likely the same optimistic-version-token mechanism as E6, but under real concurrent load from
many subscribers against one shared `Balance` row) in the architecture ADR.

---

## Summary table

| Entity | SID home(s) | 3GPP NRM/IOC | Persistence | Consistency |
|---|---|---|---|---|
| E1 Subscriber | TMF632 `Individual` | None found (TS 32.291 `SubscriberIdentifier` instead) | PostgreSQL | Strong (identity/account fields) |
| E2 Catalog+Plans | TMF620 (`ProductOffering`/`Price`/`Specification`), TMF638/633 | None found; slice tie-in via E10 only | PostgreSQL (+jsonb) | Strong on publish |
| E3 Session | (none — protocol state) | TS 32.291 directly; slice-charging tie via `nSM`/`nSPAChargingInformation` **[secondary]** | Redis/Valkey | Idempotent, partition-recoverable |
| E4 Usage/CDR | TMF635 `ProductUsage`, TMF727 | TS 32.298 (not vendored — caveat) | Apache Doris (ADR-0192) + object store | Dedup + gap detection |
| E5 Rating | TMF678 `AppliedCustomerBillingRate` | None found (TS 32.290/291 instead) | PostgreSQL (+jsonb) | Strong + reproducible |
| E6 Balance | TMF654 `Bucket`/`AccumulatedBalance`/`TopupBalance`/`AdjustBalance`/`TransferBalance`/`ReserveBalance` | None found (TS 29.594 instead) | Redis (hot) + PostgreSQL (ledger) | Strong, explicit requirement |
| E7 Roaming/Interconnect | TMF651 `Agreement` | None found; GSMA codecs deferred | PostgreSQL + object store | Strong (agreement), eventual (CDR ingest) |
| E8 Security | None (infra, not SID) | TS 33.501 (existing project-wide) | PostgreSQL (audit) | Strong, synchronous with action |
| E9 Front-end/Gateway | ODA layer (not a SID entity itself) | None (BSS concern) | None (stateless) | N/A |
| E10 Master→Consumer\|Enterprise | TMF632 `Organization` (hierarchy) + `Individual` (E1) + TMF651 (SLA) | `NetworkSlice`/`NetworkSliceSubnet` **[secondary]** for slice-as-product | PostgreSQL | Strong, hierarchy-aware |

## Open questions for the user — resolution status (user directed "go ahead with recommended options")

1. **The NRM/IOC finding**: **accepted as documented, no further pursuit** — TS 28.541/28.201/
   28.202 remain unvendored; "No 3GPP NRM/IOC home found, TS 32.2xx cited instead" stands as this
   document's answer for E1/E2/E5/E6/E7/E8/E9 going into P4.2. Revisit only if the user later
   supplies the 28-series text directly (low expected value given its confirmed network-config,
   not billing, scope — not worth chasing further speculatively).
2. **TS 32.298**: **accepted as reference-only for now** — P4.4 (CDF/CGF) proceeds against
   TS 32.291's already-vendored `MultipleUnitUsage`/CDR-adjacent shapes; if a real gap surfaces
   once CDR encode/decode is actually designed in P4.4, ask again at that point with the specific
   missing field, rather than blocking P4.1-P4.3 on it now.
3. **TMF654's balance-query surface — resolved, real source confirmed.** Fetched the actual
   TMF654 swagger (`tmforum-apis/TMF654_PrepayBalanceManagement`,
   `TMF654-PrepayBalance-v4.0.0.swagger.json`) directly. TMF654 is far richer than the single
   `TopupBalance` resource originally checked: real, separate resources exist for `Bucket`
   (the actual balance snapshot — `remainingValue`, `reservedValue`, `isShared`, `status`,
   `usageType`), `AccumulatedBalance` (aggregates a party account's buckets into a `totalBalance`),
   `TopupBalance`, `AdjustBalance`, `TransferBalance`, `ReserveBalance` (each a distinct real
   action resource), and `BalanceActionHistory` (a real audit-ledger resource). E6's schema sketch
   above has been rewritten against these real names/fields rather than the originally-flagged
   TODO. Notably, `Bucket.isShared` is a **real, TM-Forum-modeled field** — confirming E10's
   ENTERPRISE pooled-quota requirement is something TMF654 already anticipated, not a bespoke
   extension this project has to justify on its own.
4. **TMF632 party-hierarchy fields — resolved, real source confirmed.** Fetched the actual TMF632
   swagger (`tmforum-apis/TMF632_PartyManagement`, `TMF632-Party-v4.0.0.swagger.json`) directly.
   The original candidate guess (`partyRelationship`) was **wrong** — the real mechanism is
   `Organization.organizationParentRelationship` / `Organization.organizationChildRelationship`
   (each a real `{relationshipType, organization}` structure), which is `Organization`-to-
   `Organization` only, not a generic `Party` relationship. E10's schema section above has been
   rewritten: the ENTERPRISE tree is a chain of `Organization` resources via these two fields,
   with `Individual` employees attached to their department-level `Organization` via
   `Individual.relatedParty` (a real, generic field). This is a materially different (and now
   correctly sourced) structure from the original unconfirmed guess — worth noting since it
   affects how P4.7 actually implements the hierarchy.
5. **Document location convention**: resolved in the prior turn — **ADR-0053** added to the
   existing `docs/DECISIONS.md`, keeping this project's one established convention rather than
   starting CHARGING_PROMPT.md's literally-requested `docs/DECISIONS/0010-ucs-architecture.md`
   path. Not revisited here; say so if a real split is wanted going forward.

**All five open questions are now resolved or explicitly deferred with a stated reason. P4.1 is
closed.** The already-approved TMF620/PostgreSQL product-catalog extension work resumes next,
grounded in E2's persistence assignment above (PostgreSQL, `jsonb` for `prodSpecCharValueUse`).
