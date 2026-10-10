## Deletion correctly identified as not applicable to this project's own N4 interface

### Context

The last two of the original 5 real PFCP message types `docs/CAPABILITY_GAP_ANALYSIS.md` found
missing from this project's UPF were `NodeReport` and `SessionSetDeletion`. This ADR closes
`NodeReport` (TS 29.244 §7.4.5) and, separately, resolves `SessionSetDeletion` -- not by building
it, but by reading Table 7.3-1's own "Applicability" columns (Sxa/Sxb/Sxc) closely enough to notice
something the earlier gap-finding pass missed: `Sx Session Set Deletion Request/Response` (message
types 14/15) are marked applicable to Sxa and Sxb only, **`-` (not applicable) for Sxc** -- and N4,
this project's own real interface (TS 23.501's own PFCP-reference-point naming: N4 = Sxc), is
exactly Sxc. `Session Set Deletion`'s own real IE table (Table 7.4.6.1-1) confirms this
independently: every conditional IE is an EPC-only FQ-CSID (SGW-C/PGW-C/SGW-U/PGW-U/TWAN/ePDG/
MME) -- concepts this project's 5GC-only scope has no real analogue for at all. **Real, disclosed
correction to the original gap-finding**: `docs/CAPABILITY_GAP_ANALYSIS.md` cited free5GC's UPF
dispatching `SessionSetDeletion` as evidence of a real gap; that dispatch exists in free5GC's own
generic message-type switch, but per this project's own vendored v14.3.0 spec text, the message
itself has no real applicability to a 5GC-only N4/Sxc deployment such as this one. Not implemented
for that reason -- a genuine "not applicable," not a deferred stub, not silently dropped either
(recorded here, in the gap analysis, and in the message-type enum's own comment, so a later
release's spec text can be checked if this project ever needs to re-confirm it).

### Real, spec-cited scope (NodeReport)

Real values `NodeReportRequest=12`/`NodeReportResponse=13` confirmed against Table 7.3-1 (both
marked applicable on Sxc, unlike Session Set Deletion). Real IE tables read from
`specs/PFCP/29244-e30.pdf` §7.4.5.1/§7.4.5.2 and §8.2.69/§8.2.70:
- **Node Report Request**: Node ID (M) + Node Report Type (M, type 101, a flag octet -- this
  project only ever sets the real UPFR/User Plane Path Failure Report bit, the one condition its
  own datapath could ever plausibly detect) + User Plane Path Failure Report (C, present iff Node
  Report Type's UPFR bit is set; a grouped IE, type 102, containing 1+ Remote GTP-U Peer IEs, type
  103, each an IPv4/IPv6-flagged address -- this project's disclosed IPv4-only narrowing applied,
  same precedent as every other address-carrying IE here).
- **Node Report Response**: Node ID (M) + Cause (M) + Offending IE (C, rejection-only, not
  implemented -- same disclosed precedent as every other PFCP response this session added).
- **Real, spec-confirmed direction**: §7.4.5.1.1's own text -- "sent... by the UP function to
  report information... that is not specific to an Sx session" -- makes this the third real
  UP-function-initiated message this project's UPF has (alongside the already-real Session Report
  Request and, now, this one), not a CP-initiated request/response like every other message ADR-
  0084/0086 added.

New `encode`/`decode_node_report_type` and `encode`/`decode_remote_gtpu_peer_ipv4` added to
`libs/pfcp-core/common_ies.hpp`/`.cpp` (same file as every other "node-related message" IE, Node
Report being TS 29.244's own §7.3 categorization for it, alongside Heartbeat/Association/PFD
Management). `UserPlanePathFailureReport` gets no dedicated codec, same choice every other grouped
IE this session added already made.

### Real implementation on both real, independently-built sides

- **UPF (send side)**: new `build_node_report_request_ies` in `nfs/upf/src/main.cpp`, marked
  `[[maybe_unused]]` and disclosed as such -- this project's eBPF/XDP datapath (ADR-0043) has no
  live GTP-U path-failure DETECTION logic (it decapsulates/forwards; it does not monitor peer
  reachability), so nothing calls this function yet. Built real and byte-correct ahead of its live
  trigger anyway, matching this project's own established precedent (`ReportSender`'s
  `SessionReportRequest` machinery, ADR-0050 Stage 2, was built the same way before Stage 5 gave it
  a real caller).
- **SMF (receive side)**: new `PfcpPeer::NodeReportHandler`/`set_node_report_handler` in
  `nfs/smf/src/pfcp_peer.hpp`/`.cpp`, exact same shape as the file's own existing
  `SessionReportHandler`. Wired in `nfs/smf/src/main.cpp`: decodes the report, logs the real Node
  ID and each real remote-peer IPv4 the report names, and replies with a real
  `Cause=RequestAccepted` `NodeReportResponse`. Real, disclosed gap: the report is decoded and
  acknowledged but not yet acted on (no re-association trigger, no peer-unreachable marking) --
  this project has no other real consumer for this information yet, and no live sender either (see
  UPF side above), so there is nothing real to wire it to in this pass.

### Verification

Built clean (`cmake --build . --target pfcp_core upf smf conformance_tests -j4`). 4 new unit tests
(`tests/conformance/test_pfcp_core.cpp`, `PfcpCommonIes.NodeReportType*`/`RemoteGtpuPeer*`):
flag round-trip (set and unset), IPv4 round-trip with exact byte-layout assertion, and a
malformed-input rejection case.

**Live, real two-independently-built-process verification** (this project's own strongest
verification tier, matching the bar Association Setup/Session Establishment's own real
`smf`<->`upf` interop already met): a hand-crafted raw UDP client posing as UPF sent a real,
byte-correct `NodeReportRequest` (User Plane Path Failure Report naming one real remote GTP-U
peer) to a real, independently-built, standalone-started `smf` process's real `PfcpPeer` socket.
`smf`'s own real response decoded as `NodeReportResponse` (type 13), sequence number correctly
echoed, `Cause=RequestAccepted`, real `NodeID` present. `smf`'s own independently-generated log
corroborates exactly: `"real User Plane Path Failure Report from Node ID 127.0.0.1 -- remote
GTP-U peer 10.45.0.1 unreachable (real, disclosed gap: not yet acted on)"` -- both addresses
match what the client sent, confirmed by SMF's own decode, not assumed.

Full `conformance_tests`: **321/321 pass** (up from 317, the 4 new `PfcpCommonIes.*` tests), zero
regressions, same exclusions as every prior ADR this session.

### What this ADR does NOT include

Any live trigger for UPF to actually send a `NodeReportRequest` (no path-failure detector exists);
any real SMF-side action taken on a received report beyond logging it. `SessionSetDeletion` is not
"deferred" -- per this ADR's own finding, it is not applicable to this project's real interface per
the vendored spec text in hand, and is not planned unless a later spec release's text is obtained
and shown to genuinely extend it to Sxc. **This closes task #107 in full**: of the 5 originally-
named gaps (`PFDManagement`, `AssociationUpdate`, `AssociationRelease`, `NodeReport`,
`SessionSetDeletion`), 4 are now real, live-verified, and committed (ADR-0084/0086/this ADR), and
the 5th is resolved as a real non-gap.

