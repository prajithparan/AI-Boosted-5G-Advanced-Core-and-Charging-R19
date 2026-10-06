## ADR-0373: Lawful Interception increment 3 foundation -- the ETSI TS 102 232-1 PS-PDU delivery envelope, hand-derived to a 5G closure

**Date:** 2026-09-18. **Status:** Closed (work pushed; last citing commit 18c0e56 on origin/main, 2026-10-06). accepted. Continues ADR-0364's increment plan and ADR-0372
(increment 2); this is the codec floor increment 3 (the MDF2) stands on, landed on its own so the
spec-fidelity argument is reviewable before any mediation code exists.

**Found.** An MDF2 delivers a TS 33.128 IRI record to the LEMF over LI_HI2 wrapped in the ETSI
TS 102 232-1 `PS-PDU` (TS 33.128 clause 5.5); LI_HI3 does the same for CC. Increment 1 gave us
the *inner* payload (`TS33128Payloads`, BER) and increment 2 the *transport* into the MDF
(X2/X3 over mTLS). The outer HI2/HI3 envelope was the missing layer. The normative module
`LI-PS-PDU.asn` (version43) cannot be fed to asn1c as-is: it IMPORTS ~25 legacy delivery-variant
modules (Email, IPAccess, L2, IP-multimedia, PSTN/ISDN, UMTS/EPS/CONF/ProSe/GCSE HI2/HI3,
TS 101 909, ILHI, HI1 notification), which drags in the whole legacy LI module family and, with
it, a second wave of type-name collisions on top of the 47 NGAP ones ADR-0364 already contains.
Two of those imported modules' type names (`IRIPayload`, `CCPayload`) are also defined by
`TS33128Payloads`, and asn1c writes one flat output directory.

**Decision.** Vendor `LI-PS-PDU.asn` verbatim for provenance and compile a **hand-extracted**
subset, `specs/etsi/102232-1/LI-PS-PDU-3GPP-subset.asn`, added to `LI_ASN1_MODULES` in
`libs/li-generated/CMakeLists.txt`. It keeps `PS-PDU`/`PSHeader`/`Payload`/`CommunicationIdentifier`/
`NetworkIdentifier`/`MicroSecondTimeStamp`/`TimeStampQualifier`/`PayloadDirection`/`IRIType` and the
IRI/CC payload types, and IMPORTS nothing. Five derivation edits, each recorded in the file's own
header, which is the audit record: (1) `IRIContents`/`CCContents` keep only the
`threeGPP33128DefinedIRI [19]` / `threeGPP33128DefinedCC [23]` alternatives with the
`CONTAINING TS33128Payloads.IRIPayload/CCPayload` constraint dropped to a plain OCTET STRING -- the
MDF inserts the already-BER-encoded TS 33.128 payload as opaque bytes, so the subset has no
dependency on `TS33128Payloads` and cannot clash with it; (2) `Payload` keeps only
`iRIPayloadSequence [0]` / `cCPayloadSequence [1]`; (3) the ETSI `IRIPayload`/`CCPayload` SEQUENCEs
are renamed `PSIRIPayload`/`PSCCPayload` (the collision above); (4) `LawfulInterceptionIdentifier`,
`::= Common-Parameters.LIID` upstream, is inlined as the verbatim TS 103 280 definition
(`OCTET STRING (SIZE (1..25))`, module version281(281)); (5) two extension-addition fields naming
dropped modules are removed. `li-psDomainId` is the verbatim version43 value assignment.

**Wire fidelity, stated in both directions.** Every edit is wire-neutral for what a 5G MDF2/MDF3
*encodes*: ASN.1 type names never appear on the wire, a CHOICE alternative keeps its context tag
whether or not its siblings are present and whether it sits before or after the extension marker,
and `OCTET STRING (CONTAINING X)` encodes identically to a plain OCTET STRING holding X's BER
(asn1c 0.9.29 does not implement contents constraints in any case). It is **not** a claim that the
subset can *decode* everything the full module can: a PS-PDU carrying `tRIPayload [2]`, or an
`IRIContents` alternative other than `[19]`, is an unrecognized tag here. Nothing in this project
produces or consumes those; a general-purpose TS 102 232-1 decoder would need the full module.

**Verified, not assumed.** Both new ETSI files were confirmed against the upstream repository by
blob hash (`git hash-object` == the tree entry at `e531df05fd5c21c596ffed1028775b87bd61c2a0`,
forge.etsi.org/rep/li/schemas-definitions), not from recall; `specs/etsi/SOURCES.md` records them,
and marks the subset hand-extracted rather than tool-generated -- unlike the TS 102 232-3 subset
there is no script, so re-running a tool does not reproduce it. Every retained type, tag,
OPTIONAL marker and extension marker was diffed against the verbatim module, and the
`li-psDomainId` OID against its value assignment. None of the 14 type names the subset introduces
is defined by `TS33128Payloads` or the TS 102 232-3 subset. `build/generated/li_gen` was deleted
and regenerated from scratch: asn1c emits 1116 `.c` files including `PS-PDU`/`PSHeader`/
`PSIRIPayload`/`PSCCPayload`/`Payload`/`IRIContents`/`CCContents`, all compile clean, and
`libli_core.so` still exports 51 symbols, all `li_core::` (ADR-0364's symbol-hiding invariant).
All 15 LI tests pass, `LiNgapCoexistence.BothCodecsWorkInOneProcess` included.

**Disclosed.** The PS-PDU types are generated, compiled and linked but **unexercised** -- there is
no round-trip test for them in this increment, because by ADR-0364's design no test can reach a
`li_generated` header: the codec is hidden inside `libli_core.so` and is only reachable through a
`li_core` C++ facade (as `xiri.hpp` wraps `XIRIPayload`). The HI2/HI3 facade is MDF2 work, so the
first PS-PDU encode/decode test arrives with it in increment 3 proper. Until then, `asn_DEF_PS_PDU`
in the archive is evidence the module compiles, not evidence the envelope is correct on the wire.
The fidelity evidence above is a **hand diff** against the verbatim module, not an executing
check: nothing mechanical confirms this envelope until that facade and its round-trip test exist.
*(Closed by ADR-0374: `li_core::hi2` is that facade and `test_li_hi2.cpp` that round-trip test.)*

**Rejected.** *Compiling the full `LI-PS-PDU.asn`* -- pulls in the legacy LI module family and a
second collision wave, to gain delivery variants (email, PSTN, ProSe) this project does not
implement. *A `trim_*.py` script as for TS 102 232-3* -- that script extracts a verbatim closure of
one type; this subset needs semantic edits (dropped CHOICE alternatives, renames, an inlined
import) that a closure tool cannot decide, and a script that encoded them would be a less legible
audit record than the annotated ASN.1 itself. *Filing the subset under "project-owned, not ETSI"
in `SOURCES.md`* -- it is a derivative of BSD-3-Clause ETSI material and stays under that licence;
that section is for files this project authored outright.

