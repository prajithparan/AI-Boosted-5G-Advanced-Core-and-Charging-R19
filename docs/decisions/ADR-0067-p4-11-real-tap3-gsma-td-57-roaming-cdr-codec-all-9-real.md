## ADR-0067: P4.11 -- real TAP3 (GSMA TD.57) roaming CDR codec, all 9 real `CallEventDetail` variants

### Context

P4.11 (real roaming settlement) was blocked pending a real spec, since CLAUDE.md forbids
fabricating field names/tag numbers. The user supplied the real GSMA TAP3 spec
(`/home/mastermind/TAP-SPEC.pdf` -- GSM Association Official Document TD.57, "TAP 3.12 Format
Specification", Version 36.4, 15 May 2019, 317 pages, confirmed genuine by direct reading).

**Real, load-bearing confidentiality constraint**: unlike the freely-published 3GPP TS PDFs already
committed to this repo, TAP-SPEC.pdf is marked **"Confidential - Full, Rapporteur, Associate and
Affiliate Members"** -- GSMA-membership-restricted. Raised proactively with the user before any
commit; the user chose (AskUserQuestion) to keep spec-derived files out of the public repo. No
spec-derived file (the PDF itself, a transcribed `.asn` grammar, or large verbatim excerpts) is
committed here -- only real cited facts (field names, real `[APPLICATION N]` tag numbers, real
clause numbers) in code comments, matching this project's existing TCAP/MAP/CAP citation
discipline.

**Decision 1 (confidentiality-driven): hand-roll the codec, do NOT use asn1c.** NGAP's own real
precedent (ADR-0030/0031) used `asn1c` against a freely-redistributable UERANSIM-vendored `.asn`
file. TAP3's grammar lives inside a GSMA member-confidential document -- transcribing it into a
vendored `.asn` file would mean either committing confidential content (ruled out) or keeping it
local-only (which CI could never build against). Hand-rolled instead, the same way TCAP/MAP/CAP
already are.

**Decision 2 (reuse): `libs/tcap-core/include/tcap_core/ber.hpp`'s primitives are generic X.690
BER** (`Tlv`, `encode_tlv`/`decode_tlv`/`decode_tlvs`, `encode_integer`/`decode_integer`,
`TagClass`) -- nothing TCAP-specific in that file. New `libs/tap3-core` depends on `tcap_core`
directly for these rather than duplicating them or extracting a new shared `libs/ber-core`.

**Real transfer syntax** (TAP-SPEC.pdf section 6.2): plain BER (ITU-T X.690), not PER -- simpler
than NGAP's Aligned PER. **Real X.680 rule reused** (already established once for CAP in
`cap_types.hpp`): a CHOICE cannot be implicitly tagged even under the module's own `IMPLICIT TAGS`
default, so any CHOICE given its own `[APPLICATION N]` (e.g. `ChargeableSubscriber ::=
[APPLICATION 427] CHOICE {...}`) is real EXPLICIT tagging on the wire --
`wrap_explicit`/`unwrap_explicit` generalized from CAP's context-only version to accept the tag
class, since TAP3's tagged CHOICEs use `TagClass::kApplication`. The module's other CHOICE shape
(`DataInterchange`, `CallEventDetail`) is UNTAGGED -- decoded by tag-dispatch directly on whichever
alternative's own real tag appears.

### Real, disclosed scope: all 9 real `CallEventDetail` alternatives, not just one

The first slice of this work (committed separately, same session) implemented only
`MobileOriginatedCall`. The user then explicitly asked ("make sure that all call types like GPRS
etc mentioned in ASN SPEC" are covered) for full coverage. This ADR closes that: all 9 real
`CallEventDetail ::= CHOICE` alternatives (TAP-SPEC.pdf section 6.1, p.257) are now implemented,
each in its own file:

| Real type | Real tag | File |
|---|---|---|
| `MobileOriginatedCall` | `[APPLICATION 9]` | `tap3_mo_call.{hpp,cpp}` |
| `MobileTerminatedCall` | `[APPLICATION 10]` | `tap3_mt_call.{hpp,cpp}` |
| `SupplServiceEvent` | `[APPLICATION 11]` | `tap3_suppl_service.{hpp,cpp}` |
| `ServiceCentreUsage` | `[APPLICATION 12]` | `tap3_scu.{hpp,cpp}` |
| `GprsCall` | `[APPLICATION 14]` | `tap3_gprs_call.{hpp,cpp}` |
| `ContentTransaction` | `[APPLICATION 17]` | `tap3_content_transaction.{hpp,cpp}` |
| `LocationService` | `[APPLICATION 297]` | `tap3_location_service.{hpp,cpp}` |
| `MessagingEvent` | `[APPLICATION 433]` | `tap3_messaging_event.{hpp,cpp}` |
| `MobileSession` | `[APPLICATION 434]` | `tap3_mobile_session.{hpp,cpp}` |
| `AggregatedUsageRecord` | `[APPLICATION 453]` | `tap3_aggregated_usage.{hpp,cpp}` |

A new shared file, `tap3_charging.{hpp,cpp}`, holds the real charging-detail chain
(`ChargeDetail`/`ChargeDetailList`, `TaxInformation`/`TaxInformationList`, `DiscountInformation`,
`CallTypeGroup`, `ChargeInformation`/`ChargeInformationList`) reused by nearly every variant above
(`SupplServiceUsed.chargeInformation`, `ServiceCentreUsage.chargeInformation`,
`GprsServiceUsed.chargeInformationList`, `ContentServiceUsed.chargeInformationList`,
`LocationServiceUsage.chargeInformationList`, `SessionChargeInformation.{chargeDetailList,
taxInformationList}`).

`CallEventDetailList` (`tap3_envelope.hpp`) was rewired from a single `MobileOriginatedCall`-only
bucket to one `vector<Tlv>` bucket per real variant, dispatching by real top tag number on decode;
anything not matching one of the 10 real tags lands in `unrecognizedTagNumbers` (disclosed, not
dropped) rather than being silently discarded.

**Real 8-byte-INTEGER exception** (TAP-SPEC.pdf Table 44, p.252-253): ~20 real fields (Total
Charge, Data Volume Incoming/Outgoing, Charging Id, Aggregated Usage Charge, AUR Tax
Value/Taxable Amount, Total Data Volume, etc.) need up to 8 bytes, unlike every other INTEGER in
the module (real implicit 4-byte max). Rather than widen `tcap_core::encode_integer`/
`decode_integer` (32-bit, relied on by every other TCAP/MAP/CAP consumer), added
`encode_int64_field`/`decode_int64_field` to `tap3_common.{hpp,cpp}` -- same minimal-length two's
complement algorithm (X.690 section 8.3.2), widened to 8 bytes, kept local to this codec.

**Real, disclosed uncertainty, not resolved**: `AggregatedUsageRecord.operatorSpecInformation`'s
real list-type name could not be confirmed with certainty from this session's own spec reading (it
may be the same `OperatorSpecInfoList` tag 162 used everywhere else, or a distinct
`OperatorSpecInformationList`); modeled reusing the confirmed tag (162) rather than guessing a new
one. Flagged in `tap3_aggregated_usage.hpp`'s own header comment, not silently resolved either way.

**Real, disclosed scope narrowing kept from the first slice**: `TransferBatch.messageDescriptionInfo`
and `AuditControlInfo.totalAdvisedChargeValueList` remain deferred (real, cited, not needed to
prove the codec round-trips real batches). `BasicServiceUsed.chargeInformationList` and
`CamelServiceUsed.{taxInformation,discountInformation}` (MoCall-specific fields) remain deferred
too -- this session's own spec extraction confirmed these fields exist and confirmed
`tap3_charging.hpp`'s types can now represent them, but did NOT confirm their declared field ORDER
within `BasicServiceUsed`/`CamelServiceUsed` (ASN.1 requires OPTIONAL SEQUENCE components to
appear in declared order when present) -- left deferred rather than guessing a position, disclosed
in `tap3_mo_call.hpp`'s own header comment.

### Real wiring into `roaming_interconnect::RoamingCdrFileStore`

`RoamingCdrFile.format` gains a real `"TAP3"` value alongside `"STUB"` (RAP/NRTRDE remain
unsupplied, still `"STUB"`). Two new free functions in `bss/roaming-interconnect/src/store.{hpp,
cpp}`: `make_tap3_roaming_cdr_file` (builds a `RoamingCdrFile` from a real BER-encoded
`tap3_core::DataInterchange`) and `decode_tap3_roaming_cdr_file` (the reverse, real-tag-checked
against `format == "TAP3"`). Real, disclosed gap: what actually POPULATES a real
`DataInterchange` from this project's own live CDR data (a CHF-triggered rating event) is separate,
later wiring -- these two functions only prove the file carries a real, tag-correct TAP3 byte
stream end-to-end through `RoamingCdrFile`'s own storage shape, not yet fed by a live production
data path.

### What this ADR does NOT include

RAP/NRTRDE (still fully unsupplied and deferred); live wiring from CHF's real CDR data into a real
outbound TAP3 file; Annex A (Supplementary Services)/Annex C (3GPP release mapping) detail beyond
what the 9 implemented variants' own fields need; real file-transfer (TD.28, out of scope, no
transport requirement implied by BA.12).

### Testing and verification

Real, hand-constructed round-trip encode/decode unit tests (`tests/conformance/test_tap3_core.cpp`,
27 tests) for every new variant, the shared charging chain, and a `CallEventDetailList` test that
dispatches one instance of all 9 real variants through a single encode/decode round trip and
confirms each lands in its own correct bucket with an empty `unrecognizedTagNumbers`. Two more
tests (`test_roaming_interconnect_tap3.cpp`) cover the `RoamingCdrFileStore` wiring, pure-function
(no PostgreSQL needed). Verification bar matches the first slice's own: (a) internal round-trip
correctness, (b) structural cross-check against the real ASN.1 module's own field order/tag
numbers as read from the spec, (c) `[APPLICATION N]` tag numbers spot-checked against Table 45's
real tag-range table -- no genuine external TAP3 sample file exists to cross-check against without
violating the same confidentiality boundary.

`clang-format-18 --dry-run --Werror` clean on all new/changed files. Full rebuild + `ctest`: 275
tests run (9 PostgreSQL-integration tests skipped, no local Postgres for this pass), 100% pass,
zero regressions.

