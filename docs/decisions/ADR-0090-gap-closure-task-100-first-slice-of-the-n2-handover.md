## ADR-0090: gap-closure task #100 (first slice of the N2 handover remainder) -- real NGAP PathSwitchRequest

### Context

`docs/CAPABILITY_GAP_ANALYSIS.md`'s AMF section named "zero N2 handover support" as the single
highest-impact real gap found in this project's whole capability sweep. ADR-0076/ADR-0078 already
closed the `ServiceRequest` and RAN-initiated `UEContextRelease` halves of task #100; the
handover-family procedures (`HandoverRequired`/`Request`/`RequestAcknowledge`/`Command`/`Notify`/
`Cancel`, `PathSwitchRequest`/`Acknowledge`/`Failure`) remained entirely open, disclosed in
ADR-0078 as "a separate, larger body of work." Before starting this turn, real investigation (not
guessed) found: (1) the NGAP ASN.1 module already vendors every one of these message types --
zero spec gap; (2) UERANSIM, this project's only live gNB/UE simulator, has no CLI-triggerable
handover/path-switch scenario at all (confirmed by reading `cmd_handler.cpp`: only `STATUS`/
`INFO`/`AMF_LIST`/`AMF_INFO`/`UE_LIST`/`UE_COUNT`/`UE_RELEASE_REQ` exist), so this project's
usual live-verification path (a real UERANSIM-triggered scenario, as ADR-0076/ADR-0078 both used)
does not exist for any handover-family message; (3) no committed NGAP-level test harness existed
at all before this ADR (only whitebox HTTP-level `AmfIntegration.*` tests). This was surfaced to
the user (AskUserQuestion) before starting, given the real investment-level implications; the user
chose: build `PathSwitchRequest` first (the smallest single-round-trip real member of the
handover family), verified with a new hand-crafted NGAP test client.

### Scope: `PathSwitchRequest` (TS 38.413 §8.4.4), the AMF-facing tail of Xn-based handover

Per TS 23.502 §4.9.1.2.2, an Xn-based inter-gNB handover happens directly between the source and
target gNBs (entirely outside AMF's own visibility -- this project has no gNB-to-gNB Xn
simulation either way); the target gNB's own `PathSwitchRequest` to AMF is the only point AMF is
involved at all. This is the smallest real member of the handover family (one request, one
success/failure response -- unlike `HandoverRequired`/`Request`/`RequestAcknowledge`/`Command`/
`Notify`'s real 5-message, 2-gNB chain), matching ADR-0078's own disclosure that named
`PathSwitchRequest` alongside the others as in-scope for this remainder.

### Real, previously-missing architectural prerequisite found and built

Every NGAP procedure this project handled before this one (`NGSetupRequest`, `InitialUEMessage`,
`UplinkNASTransport`, `ServiceRequest`, `UEContextReleaseRequest`) arrives on the SAME SCTP
association a UE's own context already lives on. `PathSwitchRequest` is structurally different: it
arrives on a BRAND NEW association, from a DIFFERENT (target) gNB, carrying only the UE's
`SourceAMF-UE-NGAP-ID` -- an AMF-local integer the UE itself never presents. Real, disclosed
finding made while investigating this: this project's real NGAP accept loop
(`nfs/amf/src/ngap_task.cpp`'s `run_ngap_lifecycle`) is NOT one-thread-per-association as
`ngap_task.hpp`'s own pre-existing header comment claims -- it is a single sequential loop
(`while (true) { assoc = listener.accept(); handle_association(...); }`) on run_ngap_lifecycle's
own one thread, meaning the AMF can only ever have ONE live NGAP association at a time. `ngap_task.hpp`
now documents this correction directly; not fixed (out of this ADR's scope), and `PathSwitchRequest`'s
own design already accounts for it (reads persisted Redis state, not live per-association memory,
so it works correctly regardless of whether the source association is still open).

Closing this needed a new, real, previously-missing piece: `nfs/amf/src/amf_ue_id_index_store.hpp/
.cpp` (new `AmfUeIdIndexStore`, Redis-backed, same pattern as `UeSecurityContextStore`) -- a real
`amf_ue_ngap_id -> tmsi` index, populated in `handle_uplink_nas_transport_smc_complete` right
alongside the existing `UeSecurityContextStore::put` call, so `PathSwitchRequest` can find a UE's
persisted security context (`UeSecurityContextStore`, keyed by tmsi) from its own
`SourceAMF-UE-NGAP-ID` alone.

### Real vertical key derivation added (TS 33.501 Annex A.9/A.10)

`PathSwitchRequestAcknowledge`'s mandatory `SecurityContext` IE (`{nextHopChainingCount, nextHopNH}`)
needs a real KgNB/NH derivation this project never had (no `InitialContextSetup` procedure exists
here, so no prior AS security context/NH chain has ever been established for any UE). Added
`aka_crypto::derive_kgnb`/`derive_nh` (`libs/aka-crypto/include/aka_crypto/kdf.hpp`/`src/kdf.cpp`),
real citations confirmed directly against the same local `specs/TS_33_501.pdf` v19.6.0 copy
ADR-0081's SoR-MAC derivation already cited: Annex A.9 (FC=0x6E, `KDF(KAMF, uplink NAS COUNT,
access-type-distinguisher)`) and Annex A.10 (FC=0x6F, `KDF(KAMF, SYNC-input)`). Real, disclosed
scope: since this project has no prior NH chain for any UE, every call derives chain position 0
(`NCC=0`, `SYNC-input` = the freshly-derived KgNB itself), never a later position -- documented
directly in `kdf.hpp`'s own header comment, not hidden.

### Real, disclosed scope narrowing

`PDUSessionResourceToBeSwitchedDLList` is structurally parsed (real PDU session IDs, real
mandatory-IE presence checks) but NOT acted on -- no SMF/UPF call updates real GTP-U forwarding
for the new gNB; that is task #101's own explicit, separate, not-yet-built scope (SMF
`UpdateSMContext` real N2SmInfo dispatch). The `PDUSessionResourceSwitchedList` sent back echoes
each PDU session ID with an all-OPTIONAL-fields-empty `PathSwitchRequestAcknowledgeTransfer` -- a
real, spec-valid encoding (`uL-NGU-UP-TNLInformation` is genuinely OPTIONAL per the ASN.1 module),
not a fabricated tunnel endpoint. `UserLocationInformation`/`UESecurityCapabilities` are checked
for mandatory presence only, not decoded -- same "mandatory but log-only" precedent `Cause`
already set in `handle_ue_context_release_request` (ADR-0078). `AllowedNSSAI` is this lab's own
fixed single S-NSSAI (`sst=1, sd=1`), the same value NG Setup/registration already use everywhere
else in this file, not derived per-UE.

If `SourceAMF-UE-NGAP-ID` doesn't match any persisted context, this AMF replies with a real
`ErrorIndication` (TS 38.413's generic, all-optional-fields error procedure, `id-ErrorIndication`,
newly patched onto `ConcreteProtocolIE-Container` alongside `PathSwitchRequest`/`Acknowledge`/
`Failure` -- twelve -> thirteen total patched message types, see the shared comment in
`specs/NGAP/ngap-17.9.asn`) carrying `Cause=radioNetwork/unknown-local-UE-NGAP-ID` (real, precise
cited value 14) -- not `PathSwitchRequestFailure`, since that procedure's own
`PDUSessionResourceReleasedListPSFail` IE is mandatory with a real `SIZE(1..)` ASN.1 constraint
this project has no real PDU session IDs to populate for a wholly-unrecognized UE.

### Real, load-bearing side effect on success

Re-points `NgapUeRegistry`'s entry for the UE's SUPI to the NEW association/RAN-UE-NGAP-ID --
without this, a later `Namf_Communication N1N2MessageTransfer` (ADR-0038) would still try to
deliver to the stale source-gNB association. The stale source association itself is not
force-closed (no way to reach into a different association's own thread) -- real, disclosed
simplification, not a new regression.

### Implementation

`specs/NGAP/ngap-17.9.asn`: `PathSwitchRequest`/`PathSwitchRequestAcknowledge`/
`PathSwitchRequestFailure`/`ErrorIndication` repointed at `ConcreteProtocolIE-Container`, same
ADR-0031 workaround already applied to 9 other message types. `nfs/amf/src/ngap_codec.hpp`/`.cpp`
gained `encode_value`/`decode_value` (generic single-type Aligned PER encode/decode, reused for
the nested `PathSwitchRequestTransfer`/`PathSwitchRequestAcknowledgeTransfer` OCTET-STRING-wrapped
transparent containers). `libs/ngap-core`'s `SctpSocket` gained a real client-role `connect()`
(this library only ever had the server/gNB-facing `accept()` role before -- a real, small,
generically-useful addition, not test-only). `nfs/amf/src/ngap_task.cpp`'s new
`handle_path_switch_request` (~230 lines) implements the full decode/lookup/derive/respond flow
described above; dispatched from `handle_association`'s existing procedureCode switch
(`id-PathSwitchRequest`=25).

### Live verification (real, cross-process, this project's strongest tier)

Since UERANSIM cannot trigger this scenario, built a small, real, hand-crafted NGAP test client
(`path_switch_client.cpp`, compiles AMF's own `ngap_codec.cpp` directly in -- the exact same
generic PER codec AMF itself uses, not a second independently-hand-rolled encoder) acting as a
second, target gNB. Real sequence: (1) full real lab stack (nrf/udr/udm/ausf/chf/pcf/smf/upf/amf)
started with zero env overrides; (2) real UERANSIM gNB+UE completed a genuine Initial Registration
+ PDU Session Establishment for `imsi-999700000000001` -- AMF's own log gave the real, live
`AMF-UE-NGAP-ID=1`; direct Redis inspection confirmed `amf:ueidindex:1 -> 3` and
`amf:uesecctx:00000003` holding a real KAMF/uplink_count/downlink_count; (3) UERANSIM was killed
(this project's real NGAP accept loop is single-association-at-a-time, a real finding disclosed
above) so the AMF's accept loop was free for a second association; (4) the test client connected
as a new, second gNB and sent a real `PathSwitchRequest` (`SourceAMF-UE-NGAP-ID=1`, a fresh target
`RAN-UE-NGAP-ID=777`, one real PDU session). **Real, observed success**: client received a real
`PathSwitchRequestAcknowledge` (76 bytes) with `AMF-UE-NGAP-ID`/`RAN-UE-NGAP-ID`/
`PDUSessionResourceSwitchedList`/`AllowedNSSAI` all present and a real 32-byte `nextHopNH`
(`e3b59ae6...fb6bea`, `NCC=0`) -- independently corroborated by AMF's own log line-for-line
(`"sent PathSwitchRequestAcknowledge (76 bytes) ... NCC=0"`, `"re-pointed NGAP registry entry for
SUPI imsi-999700000000001"`). **Real negative path**: a second run with a fabricated, unrecognized
`SourceAMF-UE-NGAP-ID=999` received a real `ErrorIndication` (20 bytes), AMF's own log
independently confirming `"referenced an unrecognized SourceAMF-UE-NGAP-ID=999 ... sending
ErrorIndication"`. Not independently re-verified: an actual post-handover
`N1N2MessageTransfer` delivery to the NEW association (the re-pointing's own downstream effect) --
disclosed as a real, narrower verification scope than ideal, not claimed as tested.

`amf` built clean. Full `conformance_tests`: **325/325 pass** (unchanged -- this ADR added no new
committed conformance test, the live-verification client above is a real, disclosed manual
verification tool, `path_switch_client.cpp`, kept in scratch, not committed, same "manual live
interop is an acceptable, disclosed verification tier for this class of NGAP work" precedent
ADR-0076/ADR-0078 both already established), zero regressions.

### What this ADR does NOT include

`HandoverRequired`/`HandoverRequest`/`HandoverRequestAcknowledge`/`HandoverCommand`/
`HandoverNotify`/`HandoverCancel` (the real N2-based handover chain, still fully open -- a
genuinely larger body of work: 5 messages, 2 live gNB associations, SMF/UPF path coordination).
Task #101 (SMF `UpdateSMContext` real N2SmInfo dispatch) -- `PathSwitchRequest`'s own PDU-session
list is parsed but not acted on, explicitly deferred to that task. Any live trigger for a real
GTP-U path update at UPF. AMF-initiated re-close of the stale source association. A committed,
automated NGAP-level test (the live verification above is real but manual, matching this
project's own established precedent for this class of work). **Task #100 remains open** --
`PathSwitchRequest` is one real, closed slice of its handover remainder, not the whole thing.

