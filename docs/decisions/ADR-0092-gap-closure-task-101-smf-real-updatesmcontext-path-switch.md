## ADR-0092: gap-closure task #101 -- SMF real UpdateSMContext PATH_SWITCH_REQ (real downlink GTP-U)

### Context

`docs/CAPABILITY_GAP_ANALYSIS.md`'s SMF section named `UpdateSMContext` a near-total stub,
directly coupled to AMF's own N2-handover gap ADR-0090 partially closed (`PathSwitchRequest`).
Real investigation before writing code found the true scope substantially larger than "add one
N2SmInfoType case," surfaced to the user (AskUserQuestion) before starting:

1. `UpdateSMContext` didn't even parse multipart bodies -- `n2SmInfo` (a `RefToBinaryData`, same
   binary-multipart-part convention as `n1SmMsg`) could never actually reach the handler.
2. Deeper: this project's PDU session establishment has only ever created ONE PDR/FAR pair,
   uplink-only (Access->Core) -- no downlink FAR with a real `OuterHeaderCreation` pointing back
   at a gNB has ever existed anywhere in this codebase. `PATH_SWITCH_REQ` is fundamentally about
   *redirecting* an existing downlink tunnel; there was nothing real to redirect.
3. Deeper still: this project has never allocated or tracked a real UE IP address anywhere -- the
   real match criterion a spec-correct downlink PDR would use. Found mid-implementation, not in
   the original scoping pass; disclosed here rather than silently worked around.

The user chose (AskUserQuestion): build real `OuterHeaderCreation`/downlink-FAR support across
pfcp-core/SMF/UPF first, with SMF's `PATH_SWITCH_REQ`/`_ACK` on top, AMF-side relay wiring
explicitly deferred.

### Real design resolution for the missing-gNB-F-TEID/missing-UE-IP problem

Rather than fabricate a downlink PDR/FAR at Session Establishment time (when no real gNB F-TEID
or UE IP has ever existed in this project), the real, spec-legal path taken: TS 29.244 Table
7.5.4.1-1 confirms `CreatePDR`/`CreateFAR` are legal INSIDE a Session Modification Request, not
just Establishment. `PATH_SWITCH_REQ` is the FIRST moment this project's SMF ever has real
downlink tunnel info (the target gNB's F-TEID, forwarded verbatim by AMF) -- so the downlink
PDR/FAR is created for the first time exactly there, via `CreatePDR`/`CreateFAR` inside the
Modification request the PATH_SWITCH_REQ handler already needs to send. Real, disclosed scope
narrowing: the new downlink PDR's own match criteria is `SourceInterface=Core` only, no `UE IP
Address` IE -- since this project has no real UE IP allocation anywhere, same simplification
class the existing uplink PDR's own scope already carries (no real subscriber-identifying match
criteria beyond `SourceInterface`).

### Real NGAP codec reuse across NFs -- SMF's own local copy, not AMF's private header

SMF needs to decode/encode the real `PathSwitchRequestTransfer`/`PathSwitchRequestAcknowledgeTransfer`
NGAP transparent containers AMF would relay verbatim as `n2SmInfo`. Rather than reach into AMF's
private `ngap_codec.{hpp,cpp}` (CLAUDE.md's "no NF includes another NF's private headers" rule),
SMF links the same shared `ngap_core`/`ngap_generated` libraries directly (already a real,
legitimate dependency class -- AMF's own use of them, ADR-0090) and reimplements the same small
`aper_encode_to_new_buffer`/`aper_decode_complete` wrapper locally in its own `main.cpp` -- two
real, independent uses of the shared codec, not a private-header violation.

### Real pfcp-core additions (TS 29.244 §7.5.4.3, §8.2.56)

`IeType::UpdateFar` (10), `UpdateForwardingParameters` (11), `OuterHeaderCreation` (84) -- real
type numbers confirmed against the vendored spec PDF. `encode_outer_header_creation_gtpu_ipv4`/
`decode_outer_header_creation_gtpu_ipv4` (real byte layout: 2-octet description bitmask, 4-octet
TEID, 4-octet IPv4 -- this project's only real encapsulation choice, GTP-U/UDP/IPv4).

### Real, previously-undiscovered bug found and fixed at UPF: false-positive success

Before this ADR, UPF's `SessionModificationRequest` handler only recognized `UpdateUrr`/
`UpdateQer`/`RemoveQer`/`UpdateBar`/`RemoveBar` -- a request carrying ONLY `CreatePDR`/`CreateFAR`
(exactly what SMF's new `PATH_SWITCH_REQ` handler sends) matched none of those IEs, left `failed`
at its default `false`, and UPF would have unconditionally replied `Cause=RequestAccepted`
without applying anything at all. Found by reading UPF's own handler before assuming success,
not discovered via a failing test. Fixed: UPF now really decodes `CreatePDR`/`CreateFAR` inside a
Modification request, logs the real decoded peer TEID/IPv4, and acknowledges at the real PFCP
control-plane level -- explicitly NOT wired into the real eBPF/XDP datapath (which has no real
downlink GTP-U encapsulation path at all, a real, disclosed, separate gap; `datapath->
register_teid`'s own real scope is uplink decapsulation only, Phase 3) -- same "PFCP-level only"
disclosure class this exact file already established for Update BAR/Remove BAR (ADR-0071).

### Real, persisted N4 addressing state (a real, previously-discarded value)

`perform_n4_session_establishment` already computed UPF's own allocated N3 uplink F-TEID from the
real `CreatedPdr` response IE but discarded it after only logging it. Now persisted (alongside
`upSeid`/`upfIp`, unconditionally -- not gated on a granted charging quota like `cp_seid_sessions`
already is, since `PATH_SWITCH_REQ` can arrive for any established session) into `sm_contexts`, so
`PATH_SWITCH_REQ`'s real `PathSwitchRequestAcknowledgeTransfer` response carries UPF's own real,
previously-allocated N3 receive endpoint -- closing the exact "all-OPTIONAL-fields-empty
placeholder" gap ADR-0090 disclosed as open.

### Live verification (real, cross-process, three-way corroborated)

Real full lab stack (nrf/udr/udm/ausf/pcf/chf/upf/smf/amf), real UERANSIM gNB+UE completed a
genuine Initial Registration + PDU Session Establishment -- SMF's own log: real N4 Session
Establishment, UPF F-SEID=`0x1`, allocated uplink F-TEID=`0x1`. A hand-crafted scratch tool
(reusing the real `ngap_generated` PER codec directly, not a second hand-rolled encoder) built a
real `PathSwitchRequestTransfer` (gNB TEID=`0x2aaa`, IPv4=`10.45.0.99`); a second script
replicated `sbi_core::multipart::encode`'s own exact wire format and POSTed a real
`multipart/related` `UpdateSMContext` (`n2SmInfoType=PATH_SWITCH_REQ`) straight to SMF's real,
running process. **Real, observed success, independently corroborated three ways**: (1) SMF's own
log: `"PATH_SWITCH_REQ real N4 Session Modification succeeded (UP F-SEID=0x1, new gNB
TEID=0x2aaa)"`; (2) UPF's own log: `"real Create PDR 2 / Create FAR 2 ... peer TEID=0x2aaa, peer
IPv4=10.45.0.99"` -- an exact match to what the test client sent; (3) the real HTTP response: a
`200` with a real multipart `SmContextUpdatedData{n2SmInfoType=PATH_SWITCH_REQ_ACK}` plus a real
binary `n2SmInfo` part, independently decoded (a second scratch tool) back into a real
`PathSwitchRequestAcknowledgeTransfer` whose `uL-NGU-UP-TNLInformation` carried TEID=`00000001`/
IPv4=`127.0.0.1` -- an exact match against UPF's own real, independently-logged allocated N3
F-TEID from Session Establishment. Real negative path: `PATH_SWITCH_REQ` against an unknown
`smContextRef` -> real `404`.

`pfcp_core`/`smf`/`upf` all built clean. Full `conformance_tests`: 325/325 pass (unchanged --
same disclosed "manual live verification, not a committed automated test" precedent ADR-0090/
ADR-0091 already established, applied here to a real cross-NF PFCP/NGAP flow), zero regressions.

### What this ADR does NOT include

AMF's own `PathSwitchRequest` handler (ADR-0090) does NOT call this endpoint yet -- that relay
wiring, plus persisting the SM context ref somewhere AMF can retrieve it across associations
(a real, separate architectural piece), remains deliberately deferred, disclosed scope, not built
this pass. Real UE IP allocation (a real, deeper, separate gap found while scoping this ADR) --
the new downlink PDR's own match criteria stays `SourceInterface`-only. Real eBPF/XDP downlink
GTP-U encapsulation -- the new FAR's `OuterHeaderCreation` is real at the PFCP control-plane level
only, not wired into the actual datapath. The other 20 real `N2SmInfoType` values (`PDU_RES_*`,
`HANDOVER_*`, ...) remain unreal, same disclosed scope this file's own header already states.
`Npanf`-class NFs, EBI allocation, and every other `SmContextUpdatedData` field beyond
`n2SmInfo`/`n2SmInfoType`. **Task #101 is closed for its real, scoped first slice**
(`PATH_SWITCH_REQ`/`_ACK`); the other 20 N2SmInfoType values remain a real, open, disclosed gap.

