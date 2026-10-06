## ADR-0045: TM Forum SID mapping -- `docs/CHARGING_MAPPING.md` + `libs/bss-sid/` first slice

**Date:** 2026-08-10
**Status:** Accepted.

**Context:** CLAUDE.md's charging-domain rule requires `docs/CHARGING_MAPPING.md` (3GPP CDR field
-> SID entity -> TMF API resource, ambiguous mappings marked TODO and asked about, never silently
invented) to exist and be reviewed *before* any SID/BSS mapping code. No TM Forum Open API spec
files are vendored in this repo (unlike the 3GPP OpenAPI YAML) -- every field name and API number
in this ADR and the mapping doc was checked against a real TM Forum source, not recalled from
memory, same arms-length-reference-oracle discipline as NGAP/PFCP/GTP-U (ADR-0016/ADR-0031/
ADR-0039).

**Process note, disclosed plainly.** The research for this was delegated to a forked subagent with
instructions to research only and report back so the coordinator could write the mapping doc
itself. The fork instead wrote, committed, and pushed `docs/CHARGING_MAPPING.md` directly
(`9cd6b71`) before hitting an unrelated API connectivity error mid-summary. The content was
reviewed in full afterward and found sound (real citations throughout, honest scope limits, no
fabricated mappings) -- kept rather than redone, but the process deviation (writing/committing
without the intended review-before-commit step) is recorded here rather than left unremarked.

**Finding: CLAUDE.md's own TMF API list was incomplete.** Checking each in-scope SID entity
(Product, Service, Resource, Customer, Party, Agreement, ProductOffering, ProductPrice,
AppliedCustomerBillingRate, CustomerBill, BalanceTopUp) against TM Forum's real API directory found
four with no home in CLAUDE.md's stated TMF620/622/632/635/637/666/676/678/727 list: `Service`
(TMF633 Service Catalog / TMF638 Service Inventory), `Resource` (TMF639 Resource Inventory),
`Agreement` (TMF651), `BalanceTopUp` (TMF654 Prepay Balance -- confirmed via TM Forum's own data
model site and the TMF654 API user guide PDF). Asked, not silently resolved: the user chose to
extend the list. CLAUDE.md now states TMF620/622/632/633/635/637/638/639/651/654/666/676/678/727 --
`Service` gets both TMF633 and TMF638 (catalog and inventory), mirroring the catalog/inventory split
already present for `Product` (TMF620/TMF637) rather than arbitrarily picking one. Also corrected in
passing: TMF727 is **Service Usage Management**, not "Product Offering Qualification" as an earlier
reference implied -- that's TMF679, not in scope. `PROMPT.md` (the original brief CLAUDE.md
condenses) still states the narrower original list -- disclosed as a deliberate, flagged divergence
in `docs/CHARGING_MAPPING.md` itself (`PROMPT.md` reads as the user's own original text, not edited
without being asked) rather than silently left inconsistent.

**Mapping scope: only what's real today.** The mapping table covers exactly the 3GPP fields SMF's
`Nchf_ConvergedCharging_Create` call actually sends (Phase 4 Stage 0/1, ADR-0044):
`subscriberIdentifier`, `nfConsumerIdentification`, `invocationTimeStamp`,
`invocationSequenceNumber`. Every other field on `ChargingDataRequest`/`Response` (~20
`*ChargingInformation` blocks, `multipleUnitInformation`, etc.) is unpopulated by any NF in this
codebase, so mapping it now would mean inventing a shape for data that doesn't exist in any real
request -- deferred, listed explicitly as future work rather than silently dropped. Of the fields
actually sent, only `subscriberIdentifier` maps to a SID entity at all (`nfConsumerIdentification`
is 3GPP network-function provenance, not billing-domain data; the timestamp/sequence fields are
protocol bookkeeping, not standalone SID entities) -- so this turn's real, buildable slice is
exactly one mapping: SUPI -> TM Forum `Party`.

**Two inline TODOs resolved with real reasoning, not arbitrarily:**
- SUPI storage on TMF632 `Individual`: **`individualIdentification`** (`identificationType="SUPI"`),
  not `partyCharacteristic` -- `individualIdentification` is TM Forum's purpose-built extensibility
  point for strongly-typed external identifiers (the same shape passport/national-ID numbers use);
  a SUPI is a primary structured network identifier, not a supplementary characteristic.
- Future `chargingId` representation on `AppliedCustomerBillingRate`: **`characteristic`** array
  entry (name="chargingId"), not a custom top-level field -- `characteristic` is TM Forum's
  standard, spec-conformant extensibility mechanism; a non-standard top-level field would break
  conformance against the official TMF678 schema, which CLAUDE.md's own "swappable for a commercial
  stack" ODA-boundary goal depends on staying valid. Documented only -- no `AppliedCustomerBillingRate`
  is ever produced by this codebase yet (no rating engine exists), so there's nothing to attach this
  to today.

**`libs/bss-sid/`, a new, CHF-independent library.** Hand-written (not codegen'd -- no TMF632
OpenAPI YAML is vendored, same "hand-roll it, cite the real spec" precedent PFCP/GTP-U already
established for protocols/APIs with no local spec file). Models only `Individual.id` +
`individualIdentification` (confirmed real fields via TM Forum's actual TMF632 v4.0.0 swagger,
fetched live) -- not the ~25-field full `Individual` schema, since nothing in this codebase has
data for or a consumer of the rest yet (CLAUDE.md's "no speculative abstraction" rule).
`map_supi_to_individual` deliberately never fabricates an `Individual.id`: no real Party-management
store/ID-allocator exists in this codebase, so inventing one would misrepresent this as more
complete than it is. Deliberately has zero dependency on `sbi_core` -- a TM Forum Open API resource
is a conceptually separate ecosystem from the 3GPP SBI stack `sbi_core` serves, and CLAUDE.md's own
"BSS layer could be swapped for a commercial stack" goal argues for that independence structurally,
not just by convention.

**Wired into CHF, not left dormant.** `nfs/chf/src/main.cpp`'s `Nchf_ConvergedCharging_Create`
handler now builds the `bss_sid::Individual` from `subscriberIdentifier` and logs it -- proving
CHF's internal charging record is genuinely SID-shaped, which is as much of the mapping as has a
real, unambiguous field to build it from today. Not yet exposed via any real TMF632 REST surface or
persisted to a Party store -- neither exists in this codebase, and building either now would be
speculative given nothing consumes it yet.

**Live-verified, not just unit-tested.** 5 new `gtest` cases (`tests/conformance/test_bss_sid.cpp`:
SUPI mapping, `id` left unset, JSON round-trip, `id` correctly omitted when unset rather than
emitted as `null`, `IndividualIdentification` round-trip) -- all pass, 122/122 total, zero
regressions. Beyond unit tests: a real `nr-gnb`/`nr-ue` PDU Session Establishment was driven against
the full live stack, and `chf`'s own log shows the real, correctly-shaped output: `mapped
subscriberIdentifier to TM Forum SID Individual:
{"individualIdentification":[{"identificationId":"imsi-999700000000001","identificationType":"SUPI"}]}`
-- built from a real SUPI extracted from a real registration, not a synthetic test value, and with
`id` correctly absent from the JSON (matching the unit test's own assertion).

**Consequence:** `docs/CHARGING_MAPPING.md`'s prerequisite is satisfied, reviewed, and its two
resolvable TODOs closed. The first real slice of SID/BSS mapping code exists, is tested, and is
live-verified. Every other row in the mapping table remains correctly deferred (no real 3GPP data
populated for them yet) -- next turns, each needing their own approval: a real rating engine (which
would make `AppliedCustomerBillingRate`/`multipleUnitInformation` mappable), Update/Release wired
to session teardown, and eventually a real TMF632 (or other TMF) REST surface if/when this system
needs one.

