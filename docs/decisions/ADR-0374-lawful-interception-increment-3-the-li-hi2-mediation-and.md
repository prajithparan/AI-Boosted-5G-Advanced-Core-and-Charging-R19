## ADR-0374: Lawful Interception increment 3 -- the LI_HI2 mediation and the PS-PDU envelope (`li_core::hi2`)

**Date:** 2026-09-18. **Status:** accepted. User-directed ("go ahead with the MDF2"). Continues
ADR-0364's increment plan; ADR-0373 landed the PS-PDU codec, this is the C++ facade over it and
the TS 33.128 clause 5.5 mediation the MDF2 performs. The MDF2 **process** (`nfs/li-mdf`) and the
X2 receiving server are the next unit, not this one -- see Scope below.

**What an MDF2 does to an xIRI, and where each rule comes from.** An IRI-POI sends a BER
`XIRIPayload` over LI_X2 (Payload Format 2). The MDF2 turns it into a BER `IRIPayload` and wraps
that in an ETSI TS 102 232-1 `PS-PDU` for the LEMF. Two tables govern the two steps, so
`libs/li-core/src/hi2.cpp` keeps them as two functions:

- `mediate_xiri()` -- TS 33.128 table 5.5.2-2. `iRIPayloadOID` = `{4 19 19 7 3}` (the version of
  the ASN.1 this MDF2 generates with), the **same event CHOICE alternative** the POI chose,
  `targetIdentifiers` with clause-5.5.5 provenance, and `mediatedFromIndicator` when the received
  xIRI's relative OID differs in release or version from ours.
- `encode_iri_message()` -- tables 5.5.1-1 and 5.5.2-1. `PSHeader` with the li-psDomainId
  `{0 4 0 2 2 5 1 43}`, LIID, `CommunicationIdentifier`, sequence number, `timeStamp` +
  `microSecondTimeStamp`, `timeStampQualifier`, and the two mapped fields the tables name:
  NFID -> `networkFunctionIdentifier` (5.2.14), IPID -> `extendedInterceptionPointID` (5.2.13).
  `timeStampQualifier` is `timeOfInterception(1)`, which is what table 5.5.2-1 specifies -- not
  `timeOfMediation(2)`, the tempting wrong answer for a function called a *mediation* function.

`mediate_x2_pdu()` runs both over one received `li_core::Pdu`, sourcing from the PDU exactly the
fields the tables say come from it and taking the rest (LIID, operator identifiers, HI2 sequence
number, IRI-Type) from a `MediationContext` the MDF2 holds per task.

**The event transfer: 195 alternatives without 195 lines of switch.** Table 5.5.2-2 requires the
IRI to carry "the same choice ... that was received in the xIRIPayload.event", but `XIRIEvent` and
`IRIEvent` are two different ASN.1 CHOICEs and asn1c generates two unrelated C types. Diffing the
module shows the two share **195 alternatives with identical tag, name and type**; only four
differ (`n9HRPDUSessionInfo [100]`, `s8HRBearerInfo [101]`, `iPIRIPacketReport [161]` exist in
`XIRIEvent` alone; `mDFCellSiteReport [16]` in `IRIEvent` alone, since the MDF generates it). So
the alternative's own TLV transfers verbatim: re-encode the decoded `XIRIEvent` with its own
descriptor and `ber_decode` those bytes against `asn_DEF_IRIEvent`. Verified with a throwaway
probe before any of this was written -- a registration event round-tripped with its IMSI, MCC,
AMFPointer and 5G-TMSI intact.

**The trap that probe found, and the guard it forced.** `TS33128Payloads` is
`EXTENSIBILITY IMPLIED`, so every CHOICE is extensible, and asn1c's BER decoder **accepts an
unknown alternative**: feeding it tag `[161]` returns `RC_OK` with `present == IRIEvent_PR_NOTHING`
rather than an error. Checking the return code alone would have sent the LEMF an IRI record with
an empty event. `mediate_xiri()` therefore checks the decoded alternative, not the return code,
and `test_li_hi2.cpp` `RefusesAnXiriOnlyEventAlternative` locks it in with a hand-encoded,
genuinely valid `iPIRIPacketReport` xIRI -- one that `li_core::xiri` itself parses and reports as
an event it does not model, so the test proves the input is well-formed and the refusal is about
the mediation, not the parse.

**ADR-0373's disclosure is closed.** That increment left the PS-PDU types generated, compiled and
linked but unexercised, because no test can reach a `li_generated` header. `hi2.hpp` is the facade
that makes them reachable, and `LiHi2PsPdu.EncodesAndDecodesEveryHeaderField` puts every table
5.5.1-1 field through encode -> decode. A second test asserts the two identifiers that are easy to
cross: `PSHeader.li-psDomainId` (an OBJECT IDENTIFIER, `80 07 04 00 02 02 05 01 2B` on the wire --
`[0]` IMPLICIT replaces the universal tag) and `IRIPayload.iRIPayloadOID` (a RELATIVE-OID,
`81 05 04 13 13 07 03`).

**Target identifiers: two provenances sourced, two disclosed.** Clause 5.5.5 names four. The LI_X2
Matched Target Identifier attribute -> `matchedOn(3)` and Other Target Identifier -> `other(4)` are
read from the PDU (both attributes may repeat, 5.3.18/19). `lEAProvided(1)` comes from the X1
provisioning message and `observed(2)` from identifiers inside the xIRI event -- the first needs
the MDF2's task store and the second needs per-event-type extraction across 195 alternatives;
neither is populated here. `MediationContext.target_identifiers` is the seam through which the
MDF2 turn will supply `lEAProvided`. The X2 attribute carries the *inner XML* of a TS 103 221-1
`TargetIdentifier` element, so `x1::parse_target_identifier_fragment()` was added -- same hardened
libxml2 settings as `parse_request` (`XML_PARSE_NONET`, `NOENT` deliberately unset), but no schema
validation, because a fragment is not a document the X1 schema describes.

**Identifiers with no ASN.1 home are reported, never guessed.** `TS33128Payloads.TargetIdentifier`
has no SUCI alternative (de-concealment happens at the UDM POI, TS 33.127 6.2.2.3), and the X1
codec's `Kind::Other` is by definition unmapped. Both return a named error naming the element
rather than being dropped or coerced into a neighbouring field.

**Scope, stated so the gap is not mistaken for completeness.** This increment produces the HI2
*bytes*. It does **not** include: the X2/X3 receiving server (only the sending client exists,
ADR-0372), the HI2 delivery transport (TS 102 232-1 clause 6.4 profiles it over TCP), the
`nfs/li-mdf` process with its X1-provisioned task store and Valkey state, payload aggregation
(several IRI payloads in one PS-PDU, clause 6.2.3), and MDF3/LI_HI3 (`PSCCPayload` is generated
and compiled but has no facade). Nothing is intercepted end to end yet.

**Rejected.** *A 195-case switch mapping `XIRIEvent` to `IRIEvent`* -- 195 hand-written mappings
is 195 chances to put a field in the wrong place, and the TLV transfer is provably exact for
exactly the alternatives that match. *Treating `RC_OK` as success on the IRIEvent decode* -- the
probe showed it is not. *Modelling the PS-PDU in C++ structs instead of the ASN.1 codec* -- the
whole point of ADR-0373 was to compile the ETSI module. *Putting the HI2 delivery client in this
increment* -- transport is a separate concern from record construction, and the MDF2 turn needs
the receiving side anyway.

