## ADR-0042: Phase 3 Stage 3 -- real N4 Session Establishment, wired into CreateSMContext

**Date:** 2026-08-08
**Status:** Accepted

**Context:** ADR-0041 (Stage 2) gave SMF a real, verified Sx Association with UPF but nothing used
it yet. This stage wires a real PFCP Session Establishment (TS 29.244 §7.5.2/§7.5.3) into the
existing `CreateSMContext` flow, so a real PDU Session Establishment now creates a real N4 session
end to end -- the control-plane arc Phase 3 set out to close.

**New spec research: grouped IEs.** Unlike every PFCP message implemented so far (Heartbeat,
Association Setup -- both flat IE lists), Session Establishment needs Create PDR/PDI/Create
FAR/Forwarding Parameters, all *grouped* IEs (an IE whose value is itself a sequence of child IEs,
TS 29.244 §7.2.3.3). Read the real spec text for this (clauses 7.5.2.2/7.5.2.3/8.2.2-8.2.3/8.2.11/
8.2.24/8.2.37/8.2.74, from the already-vendored `specs/PFCP/29244-e30.pdf`) before writing any
code, confirming: a grouped IE's on-wire value is simply its child IEs' bytes concatenated --
meaning `pfcp_core::encode_ie`/`decode_ies` (built for flat IEs in ADR-0039) needed **no
modification** to handle grouped IEs; a grouped IE's "value" passed to `encode_ie` is just another
buffer built by calling `encode_ie` repeatedly, and `decode_ies` recurses into it by construction.
Confirmed by a new unit test (`PfcpSessionIes.GroupedIeRoundTripsViaExistingIeCodec`) before using
it for anything real.

**New `libs/pfcp-core/session_ies.{hpp,cpp}`**: F-SEID, PDR ID, Precedence (confirmed from the real
spec: *lower* value means *higher* precedence, the non-intuitive direction, not assumed), FAR ID,
Apply Action (FORW only -- the only action this build's minimal PDR/FAR needs), Source/Destination
Interface (confirmed as the same value table via two independently-read spec figures, not assumed
identical), and F-TEID in both its "CH request" (CP asks UP to allocate) and "allocated" (UP's real
answer) shapes. 10 new unit tests.

**Minimal but genuinely spec-correct session shape: one uplink PDR/FAR pair, no downlink.** SMF's
`perform_n4_session_establishment` builds: a PDR (Source Interface=Access, F-TEID CH-requested so
UPF allocates its own local GTP-U endpoint) paired with a FAR (Apply Action=FORW, Destination
Interface=Core). No downlink PDR/FAR is created -- that would need the gNB's real N3 GTP-U
endpoint (TEID+IP), which only arrives via NGAP's PDU Session Resource Setup procedure, still not
implemented (a disclosed gap predating this turn, first noted in ADR-0038's N2 SM info comment).
Disclosed explicitly in `nfs/upf/src/main.cpp`'s own header comment, not left for a reader to
discover: UPF allocates a real F-TEID and echoes it back, but no packet will ever actually flow
through it yet (Stage 4's datapath doesn't exist either).

**UPF's Association Setup response now honestly declares FTUP support** (`UP Function Features`
bit 5, "F-TEID allocation/release in the UP function is supported") -- Stage 1 sent an all-zero
"no optional features" bitmask because that was true at the time; it would have been dishonest to
leave unchanged now that UPF genuinely does allocate F-TEIDs on request.

**Cross-thread storage, deferred from Stage 2 until genuinely needed.** ADR-0041 explicitly held
off on persisting the discovered UPF endpoint because nothing read it yet. This stage is that
reader: new `UpfEndpointStore` (mutex-protected, matching `NgapUeRegistry`'s discipline in AMF) is
written once by `run_pfcp_lifecycle` after a successful Association and read by `CreateSMContext`'s
route handler (the `ioc` thread) -- the same kind of genuine cross-thread handoff AMF's
`NgapUeRegistry` needed for N1N2MessageTransfer (ADR-0038), now needed here for the same underlying
reason: a blocking-transport background thread's state has to reach an HTTP request handler on a
different thread.

**Refactored, not duplicated: `send_pfcp_request_and_await_response`.** Stage 2's Association Setup
and this stage's Session Establishment both need identical send/wait-with-timeout/retry mechanics
(T1=2s via `SO_RCVTIMEO`, N1=3 retries, TS 29.244 §6.4) -- extracted into one shared function
(`nfs/smf/src/main.cpp`) rather than copy-pasting Stage 2's loop a second time, since this is now a
genuine second real call site, not a hypothetical one.

**Best-effort, matching ADR-0038's own precedent**: N4 Session Establishment failure is logged, not
fatal to `CreateSMContext`'s 201 response -- the same non-blocking-on-a-downstream-real-integration
discipline already established for the N1N2MessageTransfer call in the same handler.

**Verification:** full real end-to-end interop, first attempt, zero regressions -- real
`nrf`/`udr`/`udm`/`ausf`/`pcf`/`upf`/`smf`/`amf` plus real `nr-gnb`/`nr-ue`. `smf`'s log: `N4
Session Establishment succeeded for pduSessionId 1, UPF F-SEID=0x1, allocated uplink F-TEID=0x1`,
correctly ordered *before* `PDU Session Establishment Accept delivered to AMF` (matching TS 23.502's
real step ordering: N4 Session Establishment happens before the Accept is sent). `upf`'s own,
independently-generated log confirms the same exchange: `allocated F-TEID 0x1 for PDR ID 1` /
`Sx Session established from 127.0.0.1`. The real UE's own log is unaffected and still shows the
full procedure succeeding (`PDU Session establishment is successful PSI[1]`) -- this stage adds a
real N4 session underneath an already-working procedure, it doesn't change UE-visible behavior
(expected: UPF's datapath doesn't exist yet, so there was nothing for the UE to notice). 10 new
unit tests; full `ctest` suite re-run clean, 117/117 passing.

**Consequence:** Phase 3's control-plane arc (PFCP Association + real Session Establishment,
triggered by a real PDU Session Establishment procedure end to end) is now genuinely complete.
Stage 4 (the eBPF/XDP datapath itself, ADR-0039's own evaluated-and-approved choice) is the only
remaining increment before Phase 3's stated scope ("User plane: N4/PFCP, UPF datapath") is fully
met.

