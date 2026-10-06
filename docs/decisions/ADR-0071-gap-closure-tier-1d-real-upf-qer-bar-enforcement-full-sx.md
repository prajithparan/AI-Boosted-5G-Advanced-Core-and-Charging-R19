## ADR-0071: gap-closure Tier 1d -- real UPF QER/BAR enforcement + full Sx session management

### Context

Last item of the free5GC/open5gs Tier 1 gap-closure sequence. Real, verified gap: `nfs/upf` (Phase
3, ADR-0039/0042/0043/0050) only ever implemented Heartbeat, Association Setup, Session
Establishment (uplink PDR/FAR + optional URR), and Session Modification (Update URR only) -- no
QER (QoS Enforcement Rule: gating/rate-limiting), no BAR (Buffering Action Rule), and Session
Deletion fell into the dispatch loop's catch-all "no handler yet, ignoring" branch, never
responding at all. Both real references implement QER gate+MBR enforcement and Session Deletion as
a matter of course.

### What changed

**`libs/pfcp-core`**: new real IE type numbers (`CreateQer`=7, `UpdateQer`=14, `RemoveQer`=18,
`GateStatus`=25, `Mbr`=26, `CreateBar`=85, `UpdateBar`=86, `RemoveBar`=87, `BarId`=88, `QerId`=109)
confirmed directly against TS 29.244 V14.3.0 Table 8.1.2-1, plus their codecs in
`session_ies.{hpp,cpp}` (`encode/decode_qer_id`, `encode/decode_bar_id`, `GateStatus`/`Mbr` structs
+ codecs). Two real, confirmed asymmetries documented in `ie.hpp`, not assumed: (1) "Update BAR"
has two different real type numbers depending on direction (86 CP->UP in Session Modification
Request, 12 UP->CP in Session Report Response -- this project only ever needs 86); (2) the "Usage
Report" grouped IE has two different real type numbers depending on which message carries it (80,
Table 7.5.8.3-1, Session Report Request -- already in use since ADR-0050; 79, Table 7.5.7.2-1,
Session Deletion Response -- new this ADR, `UsageReportSessionDeletion`), re-verified directly
against the spec PDF page 95 rather than trusted from a research fork's secondary transcription (a
real discrepancy the fork itself flagged as unresolved; resolved here by reading the primary
source). Also added: `Cause::SessionContextNotFound` (real value 65, Table 8.2.1-1, "the F-SEID
included in a Sx Session Modification/Deletion Request message is unknown") and
`encode_usage_report_trigger_termr` (TERMR, octet 6 bit 4, the real trigger value TS 29.244's own
text names for "a usage report being reported ... due to the termination of the Sx session").

**`nfs/upf/bpf/gtpu_decap.bpf.c`**: new `qer_map` (`BPF_MAP_TYPE_HASH`, keyed by TEID, holding
`ul_gate_closed`/`dl_gate_closed`/`mbr_ul_kbps`/`bucket_bytes`/`last_refill_ns`). Packet-path check
runs BEFORE URR usage counting (a real, disclosed, non-spec-mandated microarchitecture choice:
dropped/gated traffic shouldn't count toward Volume Threshold/Quota): closed UL gate drops
immediately; otherwise a real integer-only token-bucket (1-second burst allowance, same convention
ADR-0050's own MBR-adjacent design already used) drops if the bucket can't cover the packet's
T-PDU length.

**`nfs/upf/src/datapath.{hpp,cpp}`**: `register_qer`/`remove_qer`/`remove_teid` (full session
teardown: `teid_map`+`urr_map`+`qer_map`, returns the real cumulative `total_octets` for a Session
Deletion Usage Report). A real, separate `update_qer` (distinct from `register_qer`, same relation
`update_urr_thresholds` already has to `register_urr`): Gate Status/MBR are real Conditional fields
in Update QER (only present if changing), so a naive re-run of `register_qer` would silently reopen
a closed gate or drop an MBR cap on any update that changed some other QER field -- `update_qer`
does a real read-modify-write instead, `std::nullopt` meaning "leave unchanged", and deliberately
preserves the running token bucket (no free bonus bandwidth on every unrelated update).

**`nfs/upf/src/main.cpp`**: Create QER/Create BAR parsing added to Session Establishment (QER:
Gate Status is real Mandatory, malformed/missing is logged and this session gets no enforcement;
MBR is real Conditional, absent means unlimited). Update QER/Remove QER/Update BAR/Remove BAR added
to Session Modification (restructured from a single early-return per-IE-type dispatch into one that
applies every present IE and only rejects if any genuinely failed -- Remove QER is idempotent by
design, absent-already-removed is not a failure). A brand-new `build_session_deletion_response_ies`
+ dispatch branch: resolves SEID->TEID, tears down all datapath state via `remove_teid`, emits a
real Session Deletion Usage Report (type 79, TERMR trigger) if a URR existed, and correctly rejects
a re-sent/unknown SEID with `SessionContextNotFound`. Real, disclosed BAR scope: PFCP-level
parse/log/acknowledge only -- this project has no downlink datapath (no downlink PDR/FAR, since
that needs NGAP PDU Session Resource Setup, still not implemented), so a BAR's real purpose
(buffering downlink data while paging) cannot actually be enforced; Create/Update/Remove BAR are
accepted and logged, never stored or applied.

**Real bug found and fixed while wiring this in, not present before this ADR's own new code
exposed it**: `SeidToTeidStore` was only ever populated when a session ALSO provisioned a URR
(`result->allocated_teid_with_urr`), because that was previously the only real consumer (Update
URR). Once Update/Remove QER, Update/Remove BAR, and Session Deletion all need SEID->TEID
resolution too, a QER-only session (no charging URR -- an entirely ordinary real case) could
establish successfully but then be permanently unreachable by SEID for any later
Modification/Deletion at all. Fixed by adding a separate `allocated_teid` field (set whenever an
F-TEID was allocated, regardless of URR) and registering `SeidToTeidStore` unconditionally on that,
while keeping `allocated_teid_with_urr`/`TeidSessionStore` registration scoped to real URR-bearing
sessions only (its own real purpose -- addressing an unsolicited Session Report Request --
genuinely doesn't apply otherwise). Found via live testing (see below), not by inspection.

### Testing and verification

`tests/conformance/test_pfcp_core.cpp`: 8 new IE round-trip tests (QerId, BarId, 3x GateStatus
combinations, 2x Mbr including a near-max value) plus 2 more added this turn (`Usage Report Trigger`
TERMR wire bytes, `Cause::SessionContextNotFound` round-trip) -- 44/44 `Pfcp*` tests pass.

**Real, live eBPF verification** (not just control-plane IE round-trips): the sandboxed dev
environment initially lacked `CAP_NET_ADMIN`/`CAP_BPF`/`CAP_SYS_ADMIN`/`CAP_DAC_OVERRIDE`, so a
first pass only exercised the PFCP control-plane path (`Datapath::create()` correctly degraded,
every QER/BAR message still round-tripped with the right Cause). The user then granted the built
`upf` binary the full capability set via `setcap` (two corrections needed along the way: the
initial grant omitted `cap_dac_override`, needed for the netns bind-mount step ADR-0043 already
disclosed; then omitted `cap_sys_admin` on a retry). With the real eBPF/XDP datapath active:
- A session established with QER Gate Status=OPEN/OPEN and MBR=8kbps (1000-byte/1s token bucket)
  correctly passed a single 44-byte T-PDU, then a 40-packet back-to-back burst (single-process,
  ~0.1ms total, no time for refill) delivered **exactly 22 of 40** packets (1000 / 44 = 22.7,
  floor 22) -- an exact match to the real token-bucket arithmetic, not just "some passed, some
  didn't".
- A real Update QER closing the UL gate correctly dropped a subsequent packet (0 delivered);
  reopening correctly restored delivery (1 delivered).
- A real Session Deletion correctly tore down ALL datapath state -- a post-deletion packet on the
  same TEID was not decapsulated at all (the TEID is no longer known to the XDP program); a
  replayed deletion on the same SEID correctly returned `Cause::SessionContextNotFound` (65).

Verification tooling: two throwaway (not committed, scratchpad-only) C++ clients linking directly
against the real `pfcp_core` library (`pfcp_client.cpp`, a fixed end-to-end sequence; `pfcp_step.cpp`,
individually-invocable subcommands for interleaving with packet injection) plus the pre-existing
`gtpu_test.py`/a new `gtpu_burst.py` (single-process tight-loop sender, needed because per-process
Python startup overhead made the original single-shot script's packets arrive too slowly to ever
exhaust the token bucket).

Full rebuild + `ctest`: **290/290 pass**, zero regressions (this run also required starting six
Postgres containers that had been stopped since a prior session, and setting `UDR_DATABASE_URL`/
`TEST_*_POSTGRES_URL` env vars matching CI's own values -- both real environment-setup gaps in this
session, not code defects, disclosed here rather than silently worked around).

### What this ADR does NOT include

Real downlink QoS/buffering enforcement (DL Gate Status/MBR are stored but never applied by the
XDP program, which only ever processes uplink traffic; BAR is parse-only) -- both genuinely require
a downlink datapath this project doesn't have yet (same NGAP PDU Session Resource Setup dependency
already disclosed throughout Phase 3). Multiple QERs per PDR (real spec allows a PDR to reference
several QER IDs; this project's per-TEID datapath map holds exactly one QER, same "one per session"
narrowing already established for URR). Per-IE `Failed Rule ID` reporting on a partial Session
Modification failure (this build's Cause is all-or-nothing across every IE in one request). N28/
Sy PCF-side consumption of `Nchf_SpendingLimitControl` and any GUI-facing data model for it --
unrelated to this ADR, a separate, real gap found and flagged the same day (see the standing
project-status memory), explicitly the next priority per the user.

