# Charging Mapping: 3GPP CDR/Charging Fields → TM Forum SID → TM Forum Open API

**Status: DRAFT — for review before any BSS/SID mapping code is written**, per CLAUDE.md's explicit
charging-domain deliverable requirement ("Deliverable before any mapping code:
`docs/CHARGING_MAPPING.md`... Ambiguous mappings are marked TODO and asked about, never silently
invented"). No SID/TMF mapping code exists anywhere in this repo yet — this document is that
prerequisite, not a retrofit.

## Sourcing methodology

Same arms-length reference-oracle discipline this project already uses for NGAP/PFCP/GTP-U
(ADR-0016/ADR-0031/ADR-0039): every TM Forum field name and API number below was checked against a
real source (TM Forum's own public GitHub OpenAPI/Swagger specs, or TM Forum's own web pages),
not recalled from memory and asserted. Sources are cited inline. Unlike the 3GPP OpenAPI YAML
(vendored under `specs/5G_APIs-REL-19/`), **no TM Forum Open API spec files are vendored in this
repo** — they were fetched live for this document. If a field can't be confirmed against a real
source, it is marked TODO here rather than guessed.

## Scope of this pass

This maps only the 3GPP fields **actually wired today** (Phase 4 Stage 0/1, ADR-0044): SMF's
`Nchf_ConvergedCharging_Create` call, which currently sends only
`nfConsumerIdentification`/`invocationTimeStamp`/`invocationSequenceNumber`/`subscriberIdentifier`
(see `nfs/smf/src/main.cpp`'s own disclosure — `pDUSessionChargingInformation` and every other
optional block are not populated yet). `ChargingDataRequest`/`ChargingDataResponse`'s ~20 other
optional `*ChargingInformation` blocks (SMS, NEF, registration, N2 connection, IMS, MMTel, edge
infrastructure, ProSe, MMS, MBS session, TSN, NSACF, NSSAA, ranging/SL, LCS) remain **out of scope
for real mapping** — nothing in this codebase generates them yet, so mapping them as if they were
live would be mapping fields that don't exist in any real request this system produces. A
documentation-only reference-architecture pass for them (and for the remaining named SID entities)
was added 2026-08-10 — see the two "Reference architecture" sections below — but none of it is
wired to real code.

## Finding: four SID entities CLAUDE.md/PROMPT.md name don't map to an API in CLAUDE.md's own TMF
## API list — flagged, not silently resolved

CLAUDE.md's stated TMF API list is TMF620/622/632/635/637/666/676/678/727. Checking each in-scope
SID entity (Product, Service, Resource, Customer, Party, Agreement, ProductOffering, ProductPrice,
AppliedCustomerBillingRate, CustomerBill, BalanceTopUp) against TM Forum's own API directory found
**four** whose real, TM-Forum-designated home API is not in that list:

| SID entity | Real TM Forum home API | Not in CLAUDE.md's list because |
|---|---|---|
| `Service` (CFS/RFS) | TMF638 Service Inventory Management / TMF633 Service Catalog Management | Neither is in the stated list |
| `Resource` | TMF639 Resource Inventory Management | Not in the stated list |
| `Agreement` | TMF651 Agreement Management | Not in the stated list |
| `BalanceTopUp` | **TMF654 Prepay Balance Management** (confirmed: `BalanceTopup` resource, `partyAccount`/`paymentMethod`/`recurringPeriod`/`nrOfPeriods` fields — [TM Forum data model](https://datamodel.tmforum.org/en/latest/Customer/BalanceTopup/), [TMF654 spec](https://tmf-open-api-table-documents.s3.eu-west-1.amazonaws.com/Historic/TMF654_PrepayBalance/3.0.0/user_guides/TMF654_Prepay_Balance_Management_API_user_guides_17.0.1.pdf)) | Not in the stated list |

**Resolved 2026-08-10**: asked, user chose to extend the list rather than scope these 4 entities
out. CLAUDE.md's stated TMF API list now includes TMF633+638 (Service: both Catalog and Inventory,
mirroring the existing Product Catalog=620/Inventory=637 split already in the list rather than
picking just one), TMF639 (Resource Inventory), TMF651 (Agreement), and TMF654 (Prepay Balance).

**Also corrected**: CLAUDE.md/an earlier ADR referred to "TMF727" as possibly
"ProductOfferingQualification" — checked directly: **TMF727 is Service Usage Management**
(confirmed via TM Forum's own directory listing and cross-confirmed by TMF635's own migration note,
below); Product Offering Qualification is a different API, **TMF679**, not in CLAUDE.md's list at
all. CLAUDE.md's original "727" citation is correct as a *number*, just needs its name corrected
in any future doc that names it — flagged here, not changed silently in CLAUDE.md itself (this repo
treats CLAUDE.md as project-owner-authored).

## Mapping table

| 3GPP field (TS 32.291, `ChargingDataRequest`/`Response`) | SID entity | TMF Open API / resource / field | Confidence |
|---|---|---|---|
| `subscriberIdentifier` (SUPI) | `Party` (specifically an `Individual`, playing a `Customer` role) | **TMF632** Party Management, `Individual` resource. No single field is "the SUPI" — TM Forum's `Individual.individualIdentification` (array of `IndividualIdentification`: `identificationType` + `identificationId`) is the real extensibility point for an external subscriber identifier ([TMF632 swagger](https://github.com/tmforum-apis/TMF632_PartyManagement/blob/main/TMF632-Party-v4.0.0.swagger.json), confirmed fields: `id`, `href`, `individualIdentification`, `partyCharacteristic`, ...). **Resolved 2026-08-10**: `individualIdentification` (`identificationType="SUPI"`, `identificationId=<supi>`), not `partyCharacteristic`. Reasoning: `individualIdentification` is TM Forum's purpose-built extensibility point for strongly-typed external identity documents/identifiers (the same shape passport/national-ID numbers use); SUPI is a primary, structured network identifier for the party, not a supplementary attribute -- `partyCharacteristic`'s generic name/value bag is the semantically weaker fit. |
| `nfConsumerIdentification.nodeFunctionality` (e.g. `"SMF"`) | Not a SID business entity — this is 3GPP network-function provenance, not a customer/product/billing concept | No TMF mapping. Recorded here explicitly as **out of scope for SID mapping** (a technical audit field for CHF's own PDU-session-charging record, not billing-domain data), not omitted by oversight. |
| `invocationTimeStamp` | Not a standalone SID entity | Realized as the `date`/`creationDate`-shaped field on whichever usage/billing-rate record eventually gets created from this charging event — see `ProductUsage.creationDate` and `AppliedCustomerBillingRate.date` below. Not an entity on its own. |
| `invocationSequenceNumber` | Not a SID entity | CHF-internal correlation only (see ADR-0044's disclosed "echoes the request" simplification) — no TM Forum field represents "the Nth invocation of a 3GPP charging trigger", this is 3GPP-side protocol bookkeeping, not billing-domain data. Not mapped. |
| `pDUSessionChargingInformation` (currently unset — see Scope above) | `Product` (the subscribed PDU-session-capable product instance) realizing `ProductUsage` | **TMF637** Product Inventory (`Product` resource — real TM Forum description confirmed: "a product offering procured by a customer... possible actions creating/updating/retrieving Product", [TMF637 repo](https://github.com/tmforum-apis/TMF637_ProductInventory)) for the subscribed product identity, and **TMF635** Usage Management, `ProductUsage` resource for the actual usage event once populated. Confirmed real `ProductUsage` fields: `id`, `href`, `status` (`ProductUsageStatusType`: initialized/rejected/validated/rated/corrected/superseded/notRated/failed), `usageType`, `usageCharacteristic` (array), `serviceUsage` (array of `ServiceUsageRef` — split out to **TMF727** Service Usage Management as of the v4→v5 uplift), `relatedParty`, `usageProductPrice`, `product` (`ProductRef`), `usageSpecification` ([TMF635 v5.0.0 OAS](https://github.com/tmforum-apis/TMF635_UsageManagement)). **TODO, not populated yet either side**: this field is unset in every real request this codebase currently sends (disclosed in `nfs/smf/src/main.cpp`), so there is no live 3GPP data to map its sub-fields (`chargingId`, `userLocationinfo`, `pduSessionInformation`, ...) against `ProductUsage.usageCharacteristic` yet — deferred to whichever future turn actually populates this field on the SMF side. |
| CHF's allocated `chargingId` (`ChargingId`, `Uint...` alias — TS 32.291) | Correlates a `Product`'s `ProductUsage` records to the `AppliedCustomerBillingRate`/`CustomerBill` that eventually bills for them | No single TMF field *is* `chargingId` — it is this system's own internal correlation key, realized on the BSS side as whatever key value links a stored `ProductUsage.id` to an `AppliedCustomerBillingRate.characteristic` entry (`AppliedCustomerBillingRate`'s confirmed real fields: `appliedTax`, `bill` (`CustomerBillRef`), `date`, `description`, `isBilled`, `name`, `periodCoverage`, `taxExcludedAmount`, `taxIncludedAmount`, `appliedBillingRateType`, `billingAccount`, `product` (`ProductRef`), `characteristic` (array) — [TMF678 v5.0.0 OAS](https://github.com/tmforum-apis/TMF678_CustomerBill/blob/main/TMF678-CustomerBill-v5.0.0.oas.yaml)). **Resolved 2026-08-10**: `characteristic` (name="chargingId", value=<id>), not a custom top-level field. Reasoning: TM Forum Open APIs use the `characteristic` array as the standard, spec-conformant extensibility mechanism precisely for implementation-specific correlation keys like this -- adding a non-standard top-level field would break conformance/validation against the official TMF678 schema, which a commercial-stack swap (CLAUDE.md's own stated ODA-boundary goal) depends on staying valid. **Correction (2026-10-02, docs-audit pass): the "no rating engine exists" premise below is stale.** A real rating engine now exists (ADR-0048, ADR-0057) and `AppliedCustomerBillingRate` is now actually produced -- `libs/bss-sid/{include/bss_sid,src}/rating.hpp/.cpp`, `nfs/chf/src/billing_items.cpp/.hpp`, `nfs/chf/src/rating_decision_store.{hpp,cpp}` -- feeding real `CustomerBill` generation in `bss/balance-management/src/bill_run.{hpp,cpp}` (ADR-0310/ADR-0311/ADR-0314, test coverage in `tests/conformance/test_bill_run.cpp`). This document was not updated when that work landed. Checked directly: `billing_items.cpp` does use the `characteristic`-array mechanism decided here, confirming the decision -- but correlates on `charging_data_ref` (`name="chargingDataRef"`), not the `chargingId` counter this row discusses; a session-level correlator rather than the per-invocation one, a real and reasonable implementation choice, just not literally what this row specified. |
| `ChargingDataResponse.multipleUnitInformation[].grantedUnit`/`.allocatedUnit` (currently never populated — see ADR-0044's disclosed "no rating engine yet") | `AppliedCustomerBillingRate` (once rated) | **TMF678**, `AppliedCustomerBillingRate.taxIncludedAmount`/`.taxExcludedAmount` (both `Money`) for a monetary grant, or `characteristic` for a raw unit-count grant (volume/time/service-specific-units) if the granted quota is never actually money-rated in this system's flow. **TODO, genuinely ambiguous, not resolved here**: TS 32.291's `GrantedUnit` is pre-rating (raw units: `totalVolume`/`uplinkVolume`/`downlinkVolume`/`time`/`serviceSpecificUnits`), while `AppliedCustomerBillingRate` is TM Forum's *post-rating* billing-rate concept — mapping a pre-rating quota grant onto a post-rating billing entity is not a clean 1:1 and needs a real rating-engine design decision (which doesn't exist in this codebase yet) before it can be answered correctly rather than guessed. |

## Deferred, not mapped in this pass (disclosed, not omitted by oversight)

- **Every other `*ChargingInformation` block** on `ChargingDataRequest` (SMS, NEF, registration,
  N2 connection, IMS, MMTel, edge infrastructure, ProSe, MMS, MBS session, TSN, NSACF, NSSAA,
  ranging/SL, LCS) — none are populated by any NF in this codebase yet (only PDU-session charging
  is wired, ADR-0044), so there is no real request shape to map against SID entities without
  inventing one. Each needs its own mapping pass once/if the corresponding NF actually starts
  sending that block.
- **`ChargingDataResponse.sessionFailover`/`.invocationResult`** — protocol-level
  retry/error-handling fields, same "not a SID business entity" reasoning as
  `invocationSequenceNumber` above; not expected to ever need a SID mapping.
- `Customer`/`Party` role modeling beyond the single `individualIdentification` question already
  resolved above, and `CustomerBill` *generation* itself (as opposed to the `AppliedCustomerBillingRate`
  line items that feed it, already documented) — no NF produces or consumes this data yet.
  `ProductOffering`/`ProductOfferingPrice`, `CustomerOrder`, `Agreement`, `CustomerBill`, `Policy`,
  and `Event`'s real TMF homes/fields are now documented below (reference architecture, not yet
  wired to any real 3GPP data) rather than left as a bare "deferred" bullet.

## Reference architecture: remaining SID entities' real TMF homes (not yet backed by real data)

Requested by the user as a documentation-only extension (no BSS/SID code implied or written by
this section) — for entities where no NF in this codebase produces or consumes the corresponding
data yet, so there is nothing real to map a 3GPP field *into*. Recorded here as ready reference
material (real TMF resource + real confirmed fields, every one checked against TM Forum's own
public GitHub swagger JSON, not recalled from memory) for whenever real product-catalog, ordering,
agreement, or billing-document logic actually gets built.

| SID entity | Real TMF API / resource | Confirmed real fields | Represents |
|---|---|---|---|
| `ProductOffering` | **TMF620** Product Catalog Management, `ProductOffering` | `agreement`, `attachment`, `bundledProductOffering`, `category`, `channel`, `description`, `href`, `id`, `isBundle`, `isSellable`, `lastUpdate`, `lifecycleStatus`, `marketSegment`, `name`, `place`, `prodSpecCharValueUse`, `productOfferingPrice`, `productOfferingRelationship`, `productOfferingTerm`, `productSpecification`, `resourceCandidate`, `serviceCandidate`, `serviceLevelAgreement`, `statusReason`, `validFor`, `version` ([TMF620 swagger](https://github.com/tmforum-apis/TMF620_ProductCatalog/blob/main/TMF620-ProductCatalog-v4.1.0.swagger.json)) | A sellable product definition in the catalog — what a customer *can* buy, distinct from `Product` (what a customer *has* bought, TMF637). |
| `ProductPrice` | **TMF620**, real resource name is **`ProductOfferingPrice`**, not "ProductPrice" (CLAUDE.md/PROMPT.md's name doesn't match the real TMF resource name — noted, not corrected in those files without being asked) | `bundledPopRelationship`, `constraint`, `description`, `href`, `id`, `isBundle`, `lastUpdate`, `lifecycleStatus`, `name`, `percentage`, `place`, `popRelationship`, `price`, `priceType`, `pricingLogicAlgorithm`, `prodSpecCharValueUse`, `productOfferingTerm`, `recurringChargePeriodLength`, `recurringChargePeriodType`, `tax`, `unitOfMeasure`, `validFor`, `version` (same TMF620 swagger) | The price/charge structure attached to a `ProductOffering` — recurring, one-time, or usage-based. |
| `CustomerOrder` | **TMF622** Product Ordering Management, real resource name is **`ProductOrder`** (another real, confirmed SID-name/REST-name split, not an error) | `agreement`, `billingAccount`, `cancellationDate`, `cancellationReason`, `category`, `channel`, `completionDate`, `description`, `expectedCompletionDate`, `externalId`, `href`, `id`, `note`, `notificationContact`, `orderDate`, `orderTotalPrice`, `payment`, `priority`, `productOfferingQualification`, `productOrderItem`, `quote`, `relatedParty`, `requestedCompletionDate`, `requestedStartDate`, `state` ([TMF622 swagger](https://github.com/tmforum-apis/TMF622_ProductOrder/blob/master/TMF622-ProductOrder-v4.0.0.swagger.json)) | A customer's order for one or more products. |
| `Agreement` | **TMF651** Agreement Management, `Agreement` | `agreementAuthorization`, `agreementItem`, `agreementPeriod`, `agreementSpecification`, `agreementType`, `associatedAgreement`, `characteristic`, `completionDate`, `description`, `documentNumber`, `engagedParty`, `href`, `id`, `initialDate`, `name`, `statementOfIntent`, `status`, `version` ([TMF651 swagger](https://github.com/tmforum-apis/TMF651_AgreementManagement/blob/master/TMF651-Agreement-v4.0.0.swagger.json)) | A commercial/contractual arrangement between provider and customer, or between operators (e.g. a network-sharing agreement — see `networkSharingChargingInformation` below). |
| `CustomerBill` | **TMF678** Customer Bill Management, `CustomerBill` (distinct from `AppliedCustomerBillingRate`, already documented above) | `amountDue`, `appliedPayment`, `billDate`, `billDocument`, `billNo`, `billingAccount`, `billingPeriod`, `category`, `financialAccount`, `href`, `id`, `lastUpdate`, `nextBillDate`, `paymentDueDate`, `paymentMethod`, `relatedParty`, `remainingAmount`, `runType`, `state`, `taxExcludedAmount`, `taxIncludedAmount`, `taxItem` ([TMF678 swagger](https://github.com/tmforum-apis/TMF678_CustomerBill/blob/main/TMF678-CustomerBill-v4.0.0.swagger.json)) | The actual bill/invoice document that a set of `AppliedCustomerBillingRate` line items feeds into. |
| `Event` | **TMF688** Event Management, `Event` | `analyticCharacteristic`, `correlationId`, `description`, `domain`, `event`, `eventId`, `eventTime`, `eventType`, `priority`, `relatedParty`, `reportingSystem`, `source`, `timeOcurred`, `title` ([TMF688 swagger](https://github.com/tmforum-apis/TMF688-Event/blob/master/TMF688-Event-v4.0.0.swagger.json)) | A generic notification of a one-time occurrence — the realization target for non-metered charging events, see the `*ChargingInformation` table below. **Resolved 2026-08-10**: asked, user chose to extend CLAUDE.md's API list again (same as the earlier four-entity finding). CLAUDE.md now includes TMF688 and lists `Event` as an in-scope SID entity. |
| `Policy` | **TMF723** Policy Management — real API name/number, confirmed via TM Forum's own community forum, **but no public `tmforum-apis` GitHub repo exists for it** (unlike every other API in this document, all of which have public swagger JSON) | **Not confirmed — genuinely unresolved, not guessed.** A `TMF733A Data Policy Management API Profile` extends TMF723 and is also real by name, but neither's field list could be verified against a real source without a TM Forum member-portal login. | Deferred as a hard TODO: don't invent `Policy`'s fields. Revisit if/when this project has a real policy-management need and TM Forum member access, or a public mirror of the spec surfaces. |
| `Product` (shape, home already known) | **TMF637** Product Inventory, `Product` | `agreement`, `billingAccount`, `description`, `href`, `id`, `isBundle`, `isCustomerVisible`, `name`, `orderDate`, `place`, `product`, `productCharacteristic`, `productOffering`, `productOrderItem`, `productPrice`, `productRelationship`, `productSerialNumber`, `productSpecification`, `productTerm`, `realizingResource`, `realizingService`, `relatedParty`, `startDate`, `status`, `terminationDate` ([TMF637 swagger](https://github.com/tmforum-apis/TMF637_ProductInventory)) | What a customer *has* (procured/subscribed to), as opposed to `ProductOffering` (what they *can* buy). |
| `Service` (shape) | **TMF638** Service Inventory, `Service`; specification side is **TMF633** Service Catalog, `ServiceSpecification` | `Service`: `category`, `description`, `endDate`, `feature`, `hasStarted`, `href`, `id`, `isBundle`, `isServiceEnabled`, `isStateful`, `name`, `note`, `place`, `relatedEntity`, `relatedParty`, `serviceCharacteristic`, `serviceDate`, `serviceOrderItem`, `serviceRelationship`, `serviceSpecification`, `serviceType`, `startDate`, `startMode`, `state`, `supportingResource`, `supportingService` ([TMF638 swagger](https://github.com/tmforum-apis/TMF638_ServiceInventory)) | The CFS/RFS instance a `Product` realizes as. |
| `Resource` (shape) | **TMF639** Resource Inventory, `Resource` | `administrativeState`, `attachment`, `category`, `description`, `endOperatingDate`, `href`, `id`, `name`, `note`, `operationalState`, `place`, `relatedParty`, `resourceCharacteristic`, `resourceRelationship`, `resourceSpecification`, `resourceStatus`, `resourceVersion`, `startOperatingDate`, `usageState` ([TMF639 swagger](https://github.com/tmforum-apis/TMF639_ResourceInventory)) | The underlying network/infrastructure resource a `Service` runs on. |

## Reference architecture: the ~20 unused `*ChargingInformation` blocks

Also documentation-only. Every one of these lives inside `ChargingDataRequest`, itself always sent
as an `Nchf_ConvergedCharging_Create`/`_Update` invocation — so structurally, every block is *some*
kind of chargeable event report; the real question per block is whether it's a **metered usage**
(→ `ProductUsage`, TMF635, same shape already documented for `pDUSessionChargingInformation`) or a
**one-time occurrence** (→ `Event`, TMF688, see above).

**Metered usage — `ProductUsage`/TMF635-shaped**, same reasoning as `pDUSessionChargingInformation`:
`sMSChargingInformation`, `nEFChargingInformation` (an API invocation is a usage event),
`n2ConnectionChargingInformation`, `locationReportingChargingInformation`,
`mMTelChargingInformation`, `iMSChargingInformation`, `proSeChargingInformation`,
`mMSChargingInformation`, `mBSSessionChargingInformation` (session-based, same shape as PDU
session), `tSNChargingInformation`, `nSACFChargingInformation`, `rangingSLChargingInformation`,
`lCSInformation`, `directEdgeEnablingServiceChargingInformation`/
`exposedEdgeEnablingServiceChargingInformation` (API exposure usage, same as NEF),
`edgeInfrastructureUsageChargingInformation` (name says "usage"; its content would also plausibly
reference which `Resource`/TMF639 or `Service`/TMF638 was used via `ProductUsage.usageCharacteristic`,
not a separate top-level SID entity), and — checked directly against the vendored
`TS32291_Nchf_ConvergedCharging.yaml` rather than guessed from the field name alone —
**`nSPAChargingInformation`**: the request-level field is just `singleNSSAI` (mandatory), but its
paired `NSPAContainerInformation` schema (same YAML) carries `uplinkLatency`/`downlinkLatency`/
`uplinkThroughput`/`downlinkThroughput`/`maximumPacketLossRateUL`/`DL`, `theNumberOfPDUSessions`,
`theNumberOfRegisteredSubscribers`, `estimatedEnergyConsumption`, and NWDAF-sourced
`serviceExperienceStatisticsData`/`loadLevel` (both `$ref`'d from `TS29520_Nnwdaf_
EventsSubscription.yaml`) — this is charging for **consumption of per-slice performance/analytics
data** (what "NSPA" expands to isn't stated anywhere in the YAML and isn't guessed here; the
content confirms *what it charges for*, not the acronym), which is still a form of usage (of an
analytics/monitoring service) rather than a one-time event.

**Genuinely ambiguous, flagged rather than asserted** (structurally different from "used N bytes
for M seconds" — a real rating-engine design decision, not something to pick blind):
- `registrationChargingInformation` — a UE registering is a one-time state change, not a metered
  quantity; plausibly `Event` (TMF688, `eventType="registration"`) instead of `ProductUsage`.
- `nSSAAChargingInformation` (Network Slice-Specific Authentication/Authorization) — same shape:
  a one-time auth outcome, not a volume/duration.
- `eASDeploymentChargingInformation` (Edge Application Server deployment) — a provisioning/
  lifecycle action (deployed/undeployed), not obviously metered either.
- **`nSMChargingInformation`** — checked directly against the vendored YAML: its mandatory field is
  `managementOperation`, plus `idNetworkSliceInstance`, `managementOperationStatus`, and
  `managementOperationalState`/`managementAdministrativeState` (`$ref`'d from the generic NRM
  management schema `TS28623_ComDefs.yaml`) — this is charging for **network slice management
  operations** (provisioning/config changes on a slice instance), a one-time management action, not
  a metered quantity. Plausibly `Event` (TMF688), same category as the three above.

**Not a SID/billing entity at all**: `interCHFInformation` — CHF-to-CHF protocol correlation data
for multi-CHF/roaming scenarios (which CHF instance, correlation IDs), not a chargeable service
usage — same category as `nfConsumerIdentification`/`invocationSequenceNumber` above.

**Possibly dual-mapped, flagged**: `networkSharingChargingInformation` (inter-operator network
sharing, request-level fields `plmnId`/`singleNSSAI`) — the commercial arrangement behind a network-
sharing relationship is plausibly an `Agreement` (TMF651, see above) in addition to whatever
`ProductUsage` record captures the actual shared-capacity CDR content; not resolved as one or the
other here.

## Next steps (not started, awaiting direction)

1. ~~Resolve the four API-list gaps~~ — **Resolved 2026-08-10**, see above. CLAUDE.md updated.
2. ~~Resolve the two inline TODOs~~ — **Resolved 2026-08-10**, see above.
3. **Done 2026-08-10**: `libs/bss-sid/` -- a new, CHF-independent library mapping
   `subscriberIdentifier` (the one field that's both real and unambiguous today) to a TMF632
   `Individual`, wired into CHF's `Nchf_ConvergedCharging_Create` handler and live-verified
   (ADR-0045). `Nchf_ConvergedCharging_Release` also wired and live-verified (ADR-0046). The
   `chargingId` -> `AppliedCustomerBillingRate` mapping remains documented but not implemented --
   no rating engine exists yet to produce a real `AppliedCustomerBillingRate` to attach it to.
4. **Done 2026-08-10**: reference architecture (real TMF homes + fields) documented for every
   remaining named SID entity and the ~20 unused `*ChargingInformation` blocks, above -- except
   `Policy`/TMF723, whose real field list couldn't be confirmed from any public source. `Event`
   (TMF688) was asked about and added to CLAUDE.md's SID entity/API lists, same as the earlier
   four-entity extension. `CustomerOrder` and `Policy` remain named in PROMPT.md but not added to
   CLAUDE.md's list -- not asked about yet. None of this is wired to real code; it exists so a
   future turn that actually builds product-catalog, ordering, agreement, or full billing-document
   logic doesn't have to re-derive it from scratch.
5. **Correction (2026-10-02, docs-audit pass): the two items below were marked "not started" here
   and are now both done, undetected until this pass because this file was never revisited after
   they landed.** A real rating engine exists (ADR-0048/ADR-0057/ADR-0300/ADR-0303/ADR-0305/
   ADR-0309, `nfs/chf/src/charging_engine.{hpp,cpp}`), making `AppliedCustomerBillingRate` real
   (see the correction above) -- `multipleUnitInformation`/`grantedUnit` mapping itself has not been
   independently re-examined against the now-real rating engine in this pass, flagged rather than
   assumed resolved. `Nchf_ConvergedCharging_Update` is implemented (ADR-0050 Stage 4, driven by
   real UPF Stage 2 Usage Reports). Still genuinely not started, not re-verified otherwise in this
   pass: product-catalog/ordering logic as a TMF622 `ProductOrder` (ADR-0382's customer-onboarding
   saga is a project-owned OAM provisioning API, not a `ProductOrder` implementation -- no file
   matching `ProductOrder` exists in this repo), and populating any of the ~20
   `*ChargingInformation` blocks on the SMF/other-NF side.

## Disclosed divergence from PROMPT.md

CLAUDE.md's header states it condenses `PROMPT.md`, and that a disagreement between the two is a
bug to flag, not silently resolve. **Citation corrected 2026-10-02 (docs-audit pass): the narrower
list actually lives in `CHARGING_PROMPT.md` line 337-338 (P4.7), not `PROMPT.md` line 156** --
`PROMPT.md` itself delegates all charging detail to `CHARGING_PROMPT.md` and contains no TMF-numbered
list of its own; the original citation was wrong about which file, though the underlying finding
holds. `CHARGING_PROMPT.md` still states the original, narrower TMF API list
(620/622/632/635/637/666/676/678) that this document's research found incomplete for the named SID
entities. CLAUDE.md has been updated (2026-08-10, this document's finding, user-approved) to the
fuller, real list; `CHARGING_PROMPT.md`/`PROMPT.md` have **not** been touched — flagged here rather
than edited without being asked, since both read as the user's own original brief.
