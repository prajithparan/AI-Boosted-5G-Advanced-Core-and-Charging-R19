## ADR-0064: AARQ/AARE dialogue-portion wired into UDM's MAP client and CHF's CAP server

**Date:** 2026-08-15
**Status:** Accepted. Closes ADR-0063's own disclosed gap ("Not yet wired into any NF").

**Context:** ADR-0063 added real AARE (dialogue response) codec support to `libs/tcap-core` but
left both real NF integrations (UDM's MAP client, CHF's CAP server) still setting
`dialogue_portion = std::nullopt` on their own real TC-Begin/TC-Continue/TC-End messages -- a real,
explicitly disclosed, deferred gap. This ADR closes it.

### Real Application Context OID gap resolved for MAP

`libs/map-core/include/map_core/map_dictionary.hpp` previously disclosed that
`subscriberDataMngtContext-v3`'s full numeric OID could not be resolved from TS 29.002 alone
(clause 17.1.6 states `MobileDomainDefinitions`, which defines the `gsm-NetworkId`/`ac-Id` root
arcs `map-ac` is built from, "is defined in the technical specification Mobile Services Domain" --
not reproduced in TS 29.002 itself). Resolved here with two independent, cross-checked real
sources, now that the real TS 29.002 V19.0.0 PDF is available locally
(`specs/ts_129002v190000p.pdf`):
- TS 29.002's own text (grep-confirmed) fixes `map-ac OBJECT IDENTIFIER ::= {gsm-NetworkId ac-Id}`
  under `gsm-Network(1)` = `{itu-t(0) identified-organization(4) etsi(0) mobileDomain(0)
  gsm-Network(1)}` = the numeric prefix `0.4.0.0.1` -- independently corroborated by
  `libs/cap-core`'s own already-real `kGsmssfScfGenericAcOid` living under the same 5-arc prefix as
  a sibling arc.
- The remaining unresolved piece, `ac-Id`'s own numeric value, is confirmed as `0` directly from
  RestComm jss7's own real, working `MAPApplicationContext.java`
  (`simulators/reference/jss7/`-pinned-commit fresh fetch, arms-length reference only, same
  AGPL-3.0 license-check treatment as every other jss7 fact cited in this project): its
  `oidTemplate`/`res` arrays are `{0, 4, 0, 0, 1, 0, 0, 0}` with only the context-code and version
  indices ever written by real running code.
- Combined, cross-checked real value: `subscriberDataMngtContext-v3 = {0, 4, 0, 0, 1, 0, 16, 3}`
  (`subscriberDataMngt(16)` from TS 29.002 clause 17.3.2.17's own symbolic definition, `version3(3)`
  from the same clause). Added as `map_core::kSubscriberDataMngtContextV3Oid`.

### Real NF wiring

- **UDM (`nfs/udm/src/map_client.cpp`)**: `send_insert_subscriber_data` now builds a real
  `DialogueRequest` (`application_context_name = kSubscriberDataMngtContextV3Oid`) and attaches it
  to the real TC-Begin's own `dialogue_portion`. On the peer's real TC-End/TC-Continue response, a
  present `dialogue_portion` is decoded as a real AARE; `ResultType::kRejectedPermanent` is treated
  as a real, disclosed failure (returns `false`) without going on to interpret the component
  portion. A real, disclosed leniency: an ABSENT `dialogue_portion` in the response is NOT itself a
  failure (some real peers, including CHF's own pre-this-ADR CAP server, never negotiated a
  dialogue portion at all) -- only an explicit real reject is treated as a real failure.
- **CHF (`nfs/chf/src/cap_server.cpp`)**: on receiving a real TC-Begin, a present `dialogue_portion`
  is decoded and logged as a real AARQ (not required -- same real leniency as UDM's side, symmetric
  by design). When CHF sends its real `RequestReportBCSMEvent`+`ApplyCharging` TC-Continue response,
  it now attaches a real AARE (`ResultType::kAccepted`, echoing back the real
  `cap_core::kGsmssfScfGenericAcOid` the peer proposed -- real X.227 semantics: an accept echoes the
  SAME application context, not a different one -- with a real
  `DialogServiceUserType::kNoReasonGiven` diagnostic), but only when the peer itself opened with a
  real structured AARQ (ACSE only expects a dialogue response when a dialogue was actually
  proposed).

### Real, disclosed gap NOT closed by this ADR

Rejection is still one-way: CHF always accepts (`ResultType::kAccepted`) regardless of what AC OID
the peer's AARQ actually proposed -- no real rejection path (e.g. `AcnNotSupported` for an
unrecognized application context) exists yet on the CHF/CAP-server side. Real, disclosed, deferred,
not attempted without a concrete real scenario driving it.

### Live verification (not just self-consistency)

Both directions verified against the REAL running binaries over real kernel SCTP, not just
isolated round-trip unit tests, matching this project's own established discipline (see
`docs/DECISIONS.md`'s prior ADR-0061/0062 live-verification precedent):
- **CHF**: the real `chf` binary was started (real Redis via a throwaway Docker container backing
  `ChargingDataStore`; ClickHouse/PostgreSQL absent and gracefully degraded per their own existing
  disclosed behavior). A throwaway test client, linked against the REAL `libcap_core`/`libtcap_core`/
  `libss7_core` static libraries (not mocks), played the real gsmSSF role: real M3UA ASPSM/ASPTM
  handshake, real TC-Begin carrying a real AARQ + `InitialDP`. CHF's own real log confirmed:
  `"real CAP peer opened the dialogue with a structured AARQ (applicationContextName has 8 arcs)"`,
  then `"real CAP InitialDP received"`. The test client independently decoded CHF's real TC-Continue
  response and confirmed a real AARE (`result=0`/Accepted, OID matching
  `kGsmssfScfGenericAcOid` exactly, diagnostic `NoReasonGiven`).
- **UDM**: a throwaway "VLR" test peer (same real-library-linked approach) bound real SCTP, did the
  real M3UA handshake as responder, and independently decoded UDM's real outbound TC-Begin: a real
  AARQ carrying exactly `kSubscriberDataMngtContextV3Oid`, and a real `insertSubscriberData` Invoke
  (opcode 7) with a decodable `InsertSubscriberDataArg`. The peer replied with a real TC-End (AARE
  Accepted + `ReturnResultLast`); UDM's real `send_insert_subscriber_data` returned `true`.

Full rebuild + `ctest`: 239/239 tests pass (3 PostgreSQL-backed tests skipped, no live PostgreSQL in
this pass -- pre-existing, unrelated to this ADR). No regressions.

