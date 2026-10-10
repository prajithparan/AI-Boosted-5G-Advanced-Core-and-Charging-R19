## ADR-0061: NF ownership for live SS7/MAP/CAP transport -- UDM owns MAP, CHF owns CAP

**Date:** 2026-08-15
**Status:** Accepted; UDM-side (MAP) implementation in progress this same update.

**Context:** ADR-0059's own Stage 5 update deliberately left one question unresolved: `ss7_core::
SctpSocket` and the M3UA/SCCP/TCAP codec stack (Stages 5a/5b) are real, tested transport/codec
*primitives*, but no NF in CLAUDE.md's Tier 1-3 list is named as an SS7 gateway, so no NF's `main()`
binds a live listener. Stage 6 (CAP) and Stage 7 (MAP) then built real operation-argument codecs
(`InitialDP`/`ApplyCharging`/`ApplyChargingReport`/etc. for CAP; `insertSubscriberData` for MAP) on
top of that stack -- both still pure codecs, same disclosed gap. This ADR resolves which existing
NF owns each side, asked of and confirmed by the user rather than decided unilaterally (the same
"stop and ask on genuine architecture forks, even under standing autonomy" pattern this session has
followed throughout P4.5).

**Decision:**
- **UDM owns the MAP/HLR side.** `insertSubscriberData` (and any further MAP operations built
  later) terminates in UDM's real SS7 listener, dispatching into UDM's own existing subscriber
  store (`nfs/udm/src/stores.hpp`). Rationale: UDM already models real subscriber data and is this
  project's closest analogue to an HSS/HLR convergence point for 2G/3G/4G interworking -- a real
  HLR's `insertSubscriberData` and a real UDM's `Nudm_SDM`/`Nudm_UECM` surfaces are, architecturally,
  the same subscriber-data-management responsibility exposed over two different protocols.
- **CHF owns the CAP/gsmSCF side.** `InitialDP`/`ApplyCharging`/`ApplyChargingReport`/etc. terminate
  in CHF's real SS7 listener, dispatching into CHF's existing `chf::` charging engine
  (`nfs/chf/src/charging_engine.hpp`) alongside the already-real Diameter Gy path. Rationale: CAP is
  literally the CAMEL protocol a real OCS uses for prepaid interception on 2G/3G -- CHF is already
  this project's OCS entity (`Nchf_ConvergedCharging`, real Gy CCR/CCA), so CAP is a second real
  protocol face on the *same* rating/charging decision, exactly the "protocol translator, one
  internal code path" principle this whole P4.5 effort (ADR-0059's own opening framing) was built
  around -- not a new charging engine, a new transport in front of the existing one.

**Rejected alternative:** a new, dedicated SS7-gateway NF (considered for both MAP and CAP). Real
downside: CLAUDE.md's Tier 1-3 NF list does not currently name one, so introducing it would be new,
undiscussed scope rather than resolving an existing gap -- and it would duplicate real subscriber-
data and charging-decision logic that already lives correctly in UDM and CHF respectively, working
against ADR-0059's own single-code-path principle. Not chosen.

**Scope of this update:** UDM's MAP-side wiring for `insertSubscriberData` is implemented in this
same update -- see below for real facts, a real correction found mid-implementation, disclosed
gaps, and live-verification evidence. CHF's CAP-side wiring is real, disclosed, deferred scope, not
started here -- a separate, later increment, matching this project's "one subsystem per turn"
discipline.

### Implementation: UDM's real MAP client (this same update)

**A real correction found before writing any code, not after:** this ADR's own Decision section
above (written first) said UDM's MAP side would be a "live listener." Re-reading TS 29.002 clause
17.2.2.15's own real package definition before implementing showed this was backwards for
`insertSubscriberData` specifically: `subscriberDataMngtStandAlonePackage-v3` states plainly
"-- Supplier is VLR or SGSN if Consumer is HLR or CSS, CONSUMER INVOKES { insertSubscriberData }" --
the HLR is the real CONSUMER (the one that INVOKES the operation), the VLR/SGSN is the real
SUPPLIER (the one that responds). Since UDM plays the HLR role (ADR-0061's own Decision above),
this makes UDM a real MAP **client**, not a listener, for this operation -- corrected before any
code was written, not discovered as a bug afterward. (A real future MAP operation where UDM WOULD
be a listener, e.g. a real `updateLocation` received FROM a VLR, is real, disclosed, deferred scope
-- see the gap list below.)

**New capability, real transport chain**: `nfs/udm/src/map_client.hpp/.cpp`
(`udm::send_insert_subscriber_data`) opens a real kernel SCTP association (client `connect()`, a
real, small, disclosed addition to `ss7_core::SctpSocket` -- every prior use of that class, and its
`ngap_core` precedent, was server-side `bind_and_listen`/`accept` only), performs the real M3UA
ASPSM/ASPTM activation handshake as the initiating side (RFC 4666 §3.5/§3.7 -- this side sends
ASP-Up/ASP-Active, the real Application-Server-Process-toward-Signalling-Gateway convention), wraps
a real TCAP TC-Begin (carrying one real `insertSubscriberData` Invoke, opcode `local:7`) inside a
real SCCP UDT (addressed calling=`SubsystemNumber::kHlr`(6), called=`SubsystemNumber::kVlr`(7), both
real ITU-T Q.713 SSN values already in this codebase's own `sccp_dictionary.hpp`) inside a real M3UA
DATA message, and decodes the peer's real TC-End/TC-Continue response
(`ReturnResultLast`/`ReturnResult` -> `map_core::decode_insert_subscriber_data_res`, or a real
`ReturnError`/`Reject` surfaced as a disclosed `false` return, not an exception).

**Live-verified against a real kernel SCTP socket, not just self-consistency** (this project's own
established discipline for anything touching a real OS/network API): a standalone, ephemeral
"VLR peer" test program (not committed -- same disclosed treatment as Stage 5's own SCTP
live-verification artifact) accepted a real association from `udm::send_insert_subscriber_data` and
independently decoded everything sent, printing real field values at every layer: `SCCP UDT
decoded: called SSN=7 calling SSN=6`, `Invoke opcode = 7 (expect 7)`, `decoded IMSI present=1
msisdn present=1 vlrCamelSubscriptionInfo present=1`, `O-CSI tdp_data_list.size()=1`, `O-CSI[0]
serviceKey=100 triggerDetectionPoint=2 defaultCallHandling=0` -- every value matches exactly what
the client side sent, confirmed independently on the peer side, not by round-tripping through the
same in-process code. The peer then sent back a real `InsertSubscriberDataRes` inside a real TC-End,
and `send_insert_subscriber_data` correctly decoded it and returned `true`.

**Not yet done, stated plainly**: no real trigger event exists in this codebase for calling
`send_insert_subscriber_data` automatically (the real trigger, a real MAP `updateLocation` received
FROM a VLR, has no receive-side implementation -- UDM's own `main()` does not call this function
anywhere yet, it is a tested, live-verified, but currently-unwired capability). Single-dialogue,
synchronous, blocking call only -- no persistent/multiplexed association pool the way CHF's real
`DiameterServer` keeps long-lived peer connections. No AARQ/AARE dialogue-portion negotiation (the
TC-Begin here carries no `dialogue_portion` -- `tcap_core::TcBegin`'s own field is `std::optional`
and left absent). CHF's CAP-side wiring (the other half of this ADR's decision) is not started.

### Implementation: CHF's real CAP server (this same update)

**Real direction confirmed, no correction needed this time**: `capssf-scfGenericAC`
(TS 29.078 clause 6.1.2) shows the gsmSSF opens the real dialogue (sends `InitialDP`), so the
gsmSCF (CHF, here) is the real responder -- the opposite of UDM's own MAP client role above, and
matching what this ADR's Decision section already said, so `CapServer` is a real listener, mirroring
`DiameterServer`'s own accept-thread-per-association shape and per-connection dedicated
`catalog_client`/`balance_client` pair.

**New capability**: `nfs/chf/src/cap_server.hpp/.cpp` (`chf::CapServer`) binds the real M3UA/SCTP
port (`ss7_core::dictionary::kSctpPort`=2905, RFC 4666 §1.4.8), performs the real ASPSM/ASPTM
handshake as the responder (opposite side of UDM's own client-role handshake), and on a real
`InitialDP` (opcode `local:0`): decodes `InitialDpArg`, converts its real TBCD-packed `imsi` into
SUPI `"imsi-" + digits` (a genuinely new, real TBCD codec, see below), and dispatches into
`chf::charge_one_usage` -- the EXACT SAME shared code path `Nchf_ConvergedCharging`'s HTTP
handlers, Diameter Gy, Rf, and Sy already use, now extended to a fourth real protocol
(CHARGING_PROMPT.md's own single-code-path requirement). `MultipleUnitUsage_Nchf_ConvergedCharging.
ratingGroup` is set to CAP's own real `serviceKey` -- both are real integer identifiers selecting a
rating/service context, a real, disclosed conceptual mapping, not an arbitrary placeholder. The
resulting `GrantedUnit.time` (real TS 32.291 seconds field) converts directly into
`ApplyChargingArg.max_call_period_duration` (real CAP 100ms-unit field, `time * 10`). Responds with
a single real TC-Continue carrying two real Invokes: `RequestReportBCSMEvent` (arming
`oAnswer`/`oDisconnect`, `monitorMode=notifyAndContinue`) and `ApplyCharging`. A real `InitialDP`
missing a decodable `imsi` gets a real `ReturnError` (`missingParameter`, TS 29.078 clause 5.4),
not a silent drop.

**Genuinely new to this codebase: `libs/tbcd-core`** (TS 23.003 clause 2.2 TBCD-STRING codec).
Needed because CAP/MAP's `imsi` fields are TBCD-packed bytes, but this codebase's own SUPI handling
(UDM/AUSF) never needed one -- 5G SBI JSON already carries subscriber identity as a plain digit
string. **A real, disclosed correction made in this same update**: `cap_operations.hpp` and
`map_operations.hpp`'s own prior comments claimed IMSI TBCD handling followed "the same convention
already established elsewhere in this codebase (UDM/AKA)" -- checked before writing `tbcd-core`
and found FALSE (no TBCD codec existed anywhere before this file); both comments corrected to cite
`libs/tbcd-core` instead of a nonexistent precedent, rather than left standing.

**Live-verified against the actual running `chf` binary, not a standalone harness** (a step further
than Stage 5/UDM's own precedent, which live-verified transport primitives in isolation): CHF's
real Redis, PostgreSQL, and ClickHouse backing stores were brought up (existing `chf-test-postgres`/
`chf-test-redis` containers from a prior session, restarted; a new `chf-test-clickhouse` container,
both project schema files applied), and the actual `chf` executable was run directly (not a mock).
ClickHouse rejected the fresh container's default-user auth -- confirmed CHF's own existing graceful
degradation ("chf: ClickHouse unavailable, CDF/CDR generation disabled for this process") handled it
without crashing, an already-proven code path, not new risk. A standalone, ephemeral "gsmSSF" test
client (not committed, same disclosed treatment as every other live-verification artifact this
session) connected over real kernel SCTP, completed the real ASP handshake, and sent a real
`InitialDP` (`serviceKey=100`, IMSI `999700000000001`). The real `chf` process's own log confirms
every layer: `real CAP (gsmSSF) peer connected` -> `real CAP InitialDP received (SUPI=imsi-
999700000000001, serviceKey=100)` -- the SUPI exactly matches the TBCD-encoded IMSI sent, confirming
`tbcd-core` end-to-end -- `could not reach bss/product-catalog for rating, granting nothing` (product
-catalog was not stood up for this specific run; this is CHF's own already-proven, real graceful-
degradation path from ADR-0048, not new risk, so not re-verified with a live grant here) ->
`real CAP RequestReportBCSMEvent+ApplyCharging sent (maxCallPeriodDuration=0 = 0s)`. The gsmSSF test
client independently decoded the real response and confirmed both components present:
`RequestReportBCSMEvent: bcsm_events.size()=2` (`event=7`/`oAnswer`, `event=9`/`oDisconnect`, both
`monitorMode=1`/`notifyAndContinue`) and `ApplyCharging: maxCallPeriodDuration=0 releaseIfExceeded=1`
-- `0` here is the real, expected result of no product-catalog being reachable, not a bug.

**Not yet done, stated plainly**: the rating chain's own "real grant produces a real non-zero
`maxCallPeriodDuration`" path was not live-verified in THIS update (would need product-catalog and
balance-management also stood up and seeded -- deferred, not fabricated as done). No
`EventReportBCSM`/`ApplyChargingReport` handling (the "close the charging" half of the real call
flow) -- any further message on an already-open association is logged and ignored, per
`cap_server.hpp`'s own disclosed scope. CDR write was skipped in this verification run (ClickHouse
auth, environmental, not this code's own defect). ADR-0061's own NF-ownership decision is now fully
implemented on both sides (UDM/MAP, CHF/CAP); wiring either side to a real automatic trigger (a real
MAP `updateLocation` receive path for UDM; nothing further needed for CHF, which is already a real
listener) remains open, disclosed, deferred scope.

5 new unit tests (`tbcd-core` encode round-trips at even/odd digit-string length, filler-nibble
handling, decode round trips at both lengths) -- all pass, plus the live-verification evidence
above (not a `ctest`-automated run, matching this project's own established treatment of anything
requiring a live multi-process SS7 association). Full rebuild + `ctest` run: 234/234 total tests
pass (229 prior + 5 new).

### Update, same day: CHF's CAP dialogue now closes the loop (EventReportBCSM + ApplyChargingReport)

The prior update's own disclosed gap -- "the close the charging half... is NOT implemented" -- is
closed. `CapServer::handle_connection` now keeps real per-association dialogue state
(`current_ref`/`current_supi`/`peer_transaction_id`, persisted across loop iterations on the same
thread, not a new concurrency mechanism) and dispatches real `TC-Continue` messages the peer sends
later in the same real dialogue, not just the opening `TC-Begin`:

- **`EventReportBCSM`** (opcode `local:24`) is decoded and logged. Real, cited fact confirmed
  before writing this: the real operation definition is Class 4 ("ALWAYS RESPONDS FALSE" per
  TS 29.078 clause 6.1.1) -- there is no real response to send, so logging the real event IS the
  complete real obligation, not a stub.
- **`ApplyChargingReport`** (opcode `local:36`) is decoded (real `CallResult`
  `timeDurationChargingResult`: `partyToCharge`, `elapsedHundredMsUnits`), and finalizes the real
  reservation via `chf::finalize_subscriber_balance` -- the exact same real "finalize the full
  reserved total" code path Diameter Gy's own CCR-Termination handler already uses (ADR-0057), not
  a new, CAP-specific finalization scheme. A second real, cited fact confirmed before writing:
  `applyChargingReport`'s own real operation definition is Class 2 ("RESULT FALSE", only `ERRORS`
  defined) -- there is no real successful RESULT payload, so the dialogue closes with a real,
  empty `TC-End` (no `ReturnResultLast` component), not an invented acknowledgment shape.

**Live-verified against the actual running `chf` binary** (same real backing-store setup as the
prior update, reused): an extended standalone gsmSSF test client (not committed) sent the full
real sequence -- `InitialDP` -> (real `TC-Continue` response) -> `EventReportBCSM(oAnswer)` ->
`ApplyChargingReport(elapsed=45.0s)` -- on ONE association. CHF's own log confirms every step:
`real CAP EventReportBCSM received (eventTypeBCSM=7)` then `real CAP ApplyChargingReport received
(SUPI=imsi-999700000000001, elapsedSeconds=45)` -- `45` exactly matches the `450` (100ms units)
sent, confirming the real unit conversion -- then `CAP peer association closed`. The test client
independently confirmed the response: message tag `0x4` (`TC-End`) with `0` components, exactly
matching the real Class 2 "RESULT FALSE" semantics above, not assumed.

**Real, disclosed gaps still open, stated plainly**: no periodic re-authorization when
`maxCallPeriodDuration` expires mid-call (a real multi-`ApplyChargingReport` call is not modeled,
only one InitialDP -> one ApplyChargingReport -> close). No `ReleaseCall`/`Connect` handling. The
finalize step still uses the FULL reserved total regardless of the real elapsed time
`ApplyChargingReport` reports (logged, not applied to a proportional refund) -- the same disclosed
simplification already carried from the Diameter path, not a new one introduced here. No automated
`ctest` coverage for this multi-message dialogue (matching this project's own established
treatment of anything requiring a live multi-process SS7 association -- live-verified, not
unit-tested). Full rebuild + `ctest` run: 234/234 total tests pass (unchanged -- no new automated
tests this update, live-verification only).

