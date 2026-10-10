## ADR-0063: TCAP AARE (dialogue response) -- closing Stage 5b's own disclosed gap

**Date:** 2026-08-15
**Status:** Accepted, tested.

**Context:** the user asked for broader MAP/CAP/TCAP/SCCP coverage ("jss7 to be moved to C++"),
clarified (asked directly, given the real AGPL-3.0 conflict already on record) to mean: more
complete coverage of this project's own C++ stack, using jss7 the same arms-length way as every
prior stage -- not a direct port/translation, which would carry the same AGPL-3.0 conflict forward
regardless of the target language. Confirmed by the user before any code was written.

**What was closed**: Stage 5b's own disclosed gap ("AARE (dialogue response)... own real
`Result`/`ResultSourceDiagnostic` sub-fields need further real evidence this pass didn't fully
gather") -- `libs/tcap-core/include/tcap_core/dialogue_portion.hpp`/`.cpp` gained
`DialogueResponse`/`ResultSourceDiagnostic`/`ResultType`/`DialogServiceUserType`/
`DialogServiceProviderType` and `encode_dialogue_portion_response`/`decode_dialogue_portion_response`,
alongside the existing AARQ (`DialogueRequest`) support.

**Real facts, freshly fetched at the same pinned jss7 commit** (`81a54df19bb24878ab21bb88377ca45533b3a974`,
via `gh api repos/RestComm/jss7/contents/...`, not recalled from the earlier, pre-compaction summary
that named these files but hadn't yet acted on them): `DialogAPDU._TAG_RESPONSE = 0x01` (AARE's
real `[APPLICATION 1]` outer tag, X.227/Table 34-Q.773); `Result._TAG = 0x02`; `ResultType`
real values `Accepted(0)`/`RejectedPermanent(1)`; `ResultSourceDiagnostic._TAG = 0x03`,
`_TAG_U = 0x01` (dialog-service-user)/`_TAG_P = 0x02` (dialog-service-provider);
`DialogServiceUserType` real values `Null(0)`/`NoReasonGiven(1)`/`AcnNotSupported(2)`;
`DialogServiceProviderType` real values `Null(0)`/`NoReasonGiven(1)`/`NoCommonDialogPortion(2)`.

**A real wire-shape fact confirmed from jss7's own encode()/decode() logic, not just its interface
tag constants** (the interface files alone would not have been enough to build a correct codec):
both `Result` and each `ResultSourceDiagnostic` sub-choice wrap a nested real UNIVERSAL INTEGER TLV
*directly* inside their own context-tagged constructed TLV -- functionally EXPLICIT tagging on the
wire, confirmed by reading `ResultImpl.encode()`/`ResultSourceDiagnosticImpl.encode()` line by line
(`aos.writeTag(CONTEXT, false/*constructed*/, _TAG); ...; aos.writeInteger(...)`), not inferred from
the ASN.1 module text (not in hand this pass). Implemented as a small shared
`make_explicit_int`/`decode_explicit_int` helper pair since both `Result` and both
`ResultSourceDiagnostic` sub-choices share this identical real shape.

**Vendored**: `DialogResponseAPDUImpl.java`, `ResultImpl.java`, `ResultSourceDiagnosticImpl.java`
(tcap-impl/asn/) and `DialogAPDU.java`, `DialogResponseAPDU.java`, `Result.java`,
`ResultSourceDiagnostic.java`, `ResultType.java`, `DialogServiceUserType.java`,
`DialogServiceProviderType.java` (tcap-api/asn/) added to `simulators/reference/jss7/` at the same
already-pinned commit -- same arms-length-reference-only treatment as every prior stage (real facts
extracted with citations into this project's own freshly-written Apache-2.0 code; no AGPL-3.0
source copied or linked).

**Real, disclosed scope narrowing**: `decode_dialogue_portion_response` rejects a real AARQ as a
real, disclosed mismatch (not a malformed-message error) -- callers must know which APDU type they
expect from the real dialogue direction (AARQ on a TC-Begin, AARE on the peer's TC-Continue/TC-End),
matching how a real TCAP/ACSE implementation already has to. ABRT-wrapped-in-DialoguePortion (real
U-Abort) remains a real, disclosed, deferred gap -- not attempted without further evidence, same
status as before this update.

**Not yet wired into any NF**: this is a codec-layer addition to `libs/tcap-core` only -- UDM's MAP
client and CHF's CAP server (ADR-0061) still don't send/expect a `dialogue_portion` on their own
real TC-Begin/TC-Continue/TC-End messages (both leave it `std::nullopt`, per their own existing
disclosed gaps: "No AARQ/AARE dialogue-portion negotiation"). Wiring AARE into either NF is real,
separate, future scope, not done here.

5 new unit tests (AARE round trips for both `Accepted`/user-diagnostic and
`RejectedPermanent`/provider-diagnostic outcomes, real cross-rejection in both directions between
`decode_dialogue_portion_request`/`decode_dialogue_portion_response`, and a full round trip inside
a real `TcEnd`) -- all pass. Full rebuild + `ctest` run: 239/239 total tests pass (234 prior + 5
new).

