## ADR-0050: Quota consumption tracking/re-authorization, Stage 0 -- SMF's real bidirectional PFCP peer

**Date:** 2026-08-10
**Status:** Accepted.

**Context:** ADR-0048 built a real rating engine that grants a quota once, at session
establishment, but never tracks consumption or re-authorizes -- a real, disclosed gap. Closing it
properly requires TS 29.244's real Usage Reporting Rule (URR) mechanism: UPF counts usage and sends
an **unsolicited** Sx Session Report Request when a threshold is crossed. This is a real, 7-stage
effort (comparable in size to the original PFCP/UPF work), staged and approved before
implementation, same discipline as every other multi-stage effort in this project. Researched
directly from the real, vendored `specs/PFCP/29244-e30.pdf` before any code: TS 29.244 §7.5.2.4
(Create URR IE, real type=6), §7.5.8.3 (Usage Report IE, real type=80), and Annex C.2.1.1's real
worked example ("Online charging with intermediate and final quotas"), which this project's Stage
1-6 plan follows directly rather than reconstructing the flow from first principles.

**The real architectural gap this stage closes.** SMF's PFCP code (ADR-0041/ADR-0042) used a fresh
`io_context`+socket *per call*, blocking on `receive_from()` for that one request's response only.
This works for CP-initiated request/response (Association Setup, Session Establishment) but cannot
receive an unsolicited message at all -- UPF has no stable address to send a Session Report Request
to, and even if it did, nothing would be listening for it outside an active outbound call.

**Real IEs added to `pfcp_core`** (`ie.hpp`/`header.hpp`/`session_ies.hpp`/`.cpp`), every byte
layout confirmed against the real spec PDF, not assumed: `SessionReportRequest`/`Response` message
types (56/57, confirmed via Table 7.3-1), `CreateUrr`/`UsageReport` grouped IE types (6/80), and the
child IEs Stage 1-3 will need: `UrrId` (81), `UrSeqn` (104), `MeasurementMethod` (62, VOLUM bit),
`ReportingTriggers` (37, VOLTH/VOLQU bits), `VolumeThreshold`/`VolumeQuota`/`VolumeMeasurement` (31/
73/66 -- confirmed independently to share one byte layout, one shared codec function), `ReportType`
(39, USAR bit), `UsageReportTrigger` (63, VOLTH/VOLQU bits, a different bit assignment than
Reporting Triggers despite the similar name -- confirmed independently, not assumed identical).
Only the fields Annex C.2.1.1's real call flow needs are modeled (volume-based Total Volume
threshold/quota) -- not time-based/event-based measurement or the many other optional Create
URR/Usage Report fields this project has no real use for yet. 12 new unit tests
(`tests/conformance/test_pfcp_core.cpp`), byte-exact against the real spec figures.

**New `nfs/smf/src/pfcp_peer.hpp`/`.cpp`: `PfcpPeer`.** One persistent socket, bound to a new,
disclosed-as-lab-only port (`pfcp_core::kSmfCpFunctionPfcpPort` = 8806 -- real PFCP has no spec-
assigned CP-function port the way UPF's 8805 is IANA-assigned; real deployments convey this
out-of-band, same as every other hardcoded-per-NF-port convention already in this lab), replacing
every per-call ephemeral socket. One dedicated receive thread dispatches every incoming datagram by
message type: a `*Response` matching an outstanding request's sequence number is handed to that
caller via a `condition_variable` (keyed by sequence number, now genuinely unique per logical
request via `allocate_sequence_number()` -- the old code could safely hardcode `sequence_number=1`
everywhere since each call had its own private socket; a shared socket needs real uniqueness); an
unsolicited Session Report Request is handed to a caller-installed handler. The handler is a
post-construction setter (`set_session_report_handler`), not a constructor parameter -- it needs to
capture the `PfcpPeer` itself by reference (to send the ack), which would otherwise be a reference
to a not-yet-constructed object; the setter sidesteps that cleanly. This turn's handler is
architecture-proof only: acknowledges with a schema-valid `Cause=RequestAccepted`, doesn't yet parse
real Usage Report content or call `Nchf_ConvergedCharging_Update` (Stage 3), and echoes the
request's own SEID in the response header rather than looking up the session's real UP-side SEID
(no per-session PFCP state store exists yet) -- disclosed simplifications for later stages to close,
not oversights.

**Live-verified, both the regression and the new capability.** Full stack + a real `nr-gnb`/`nr-ue`
PDU Session Establishment confirmed zero regression: Association Setup and N4 Session Establishment
both still succeed, first attempt, on the new shared-socket architecture (`smf`'s log unchanged in
substance from every prior stage's own verification). Then, independently, a hand-crafted-but-real
Sx Session Report Request (TS 29.244-correct header, Report Type IE with the USAR bit) was sent
directly to SMF's new port 8806 -- `smf`'s own log confirms the real handler ran (`received real Sx
Session Report Request from 127.0.0.1 (seq=12345)`), and the sending script received a real,
correctly-typed Sx Session Report Response (message type 57) back, proving the receive-dispatch and
fire-and-forget-response code paths both work for real, not just in theory.

**Consequence:** The real architectural blocker to quota-consumption tracking is closed. Next:
Stage 1 (SMF sends a real Create URR, derived from CHF's ADR-0048 grant, as part of N4 Session
Establishment), then Stages 2-6 per the approved plan.

### Stage 1 (2026-08-10): SMF provisions a real Create URR from CHF's real grant

**Real schema checked first, not assumed.** `Create PDR`'s own real field table (TS 29.244
§7.5.2.2) confirms it has an optional `URR ID` field ("present if a measurement action shall be
applied to packets matching this PDR") -- the real mechanism a PDR gets associated with a URR for
measurement, same `UrrId` IE type Stage 0 already added.

**Reordered CreateSMContext's handler**: `Nchf_ConvergedCharging_Create` now runs *before* N4
Session Establishment (previously after) -- TS 29.244 Annex C.2.1.1's real call flow requests
credit first, then provisions the UP function with the resulting quota (its steps 1 then 2); the
old order couldn't have included a real grant-derived URR in the same Session Establishment
Request. `perform_n40_charging_data_create` now returns a `ChargingDataCreateResult` (charging
data ref + the real parsed `GrantedUnit.totalVolume`, when CHF's response includes one) instead of
just the ref.

**`perform_n4_session_establishment` provisions a real URR** when a grant is present: `URR ID`
referenced from the uplink PDR, `Measurement Method` (VOLUM), `Reporting Triggers` (VOLTH+VOLQU),
`Volume Threshold` = 90% of the grant, `Volume Quota` = the full grant -- the exact 90/100 ratio
Annex C.2.1.1's own worked example uses, not an arbitrary choice.

**Live-verified with the real seeded catalog data from ADR-0048's own test plan.** A real
`nr-gnb`/`nr-ue` PDU Session Establishment against a real 10GB/$25 seeded plan produced: `smf`'s
log -- `Nchf_ConvergedCharging_Create succeeded ... granted total volume=10000000000 octets` then
`provisioning URR 1 for pduSessionId 1: threshold=9000000000 octets, quota=10000000000 octets`
(exactly 90%/100% of the real grant) then `N4 Session Establishment succeeded`. UPF -- an
independently-built process with no knowledge of Stage 1's changes beyond parsing whatever IEs
arrive -- accepted the Session Establishment Request containing the new Create URR IE without
rejecting it (`upf`'s own log: `Sx Session established from 127.0.0.1`), real proof the encoding is
wire-correct, not just internally self-consistent. (UPF's eBPF datapath itself did not start this
particular run -- an unrelated, already-disclosed capability-grant issue from a rebuild earlier in
this session wiping `setcap`, not a Stage 1 regression; the PFCP control-plane flow under test here
does not depend on the datapath being up.) 137/137 tests pass, zero regressions.

**Consequence:** UPF now receives everything it needs to measure and report usage -- it just
doesn't act on the URR yet (Stage 2's job: real per-TEID byte counting and the real unsolicited
Session Report Request when the threshold is crossed).

### Stage 2 (2026-08-10): UPF real per-TEID byte counting + real unsolicited Session Report Request

**BPF-side: real in-kernel counting, not a userspace estimate.** `gtpu_decap.bpf.c` gained a
`urr_map` (TEID -> `struct urr_state`: threshold/quota/running total/two one-shot report latches)
and a `usage_report_ringbuf`. On every matched G-PDU, `__sync_fetch_and_add` atomically adds the
real T-PDU length to the running total (a real atomic, not merely correct-on-one-CPU today --
forward-compatible with multi-queue/multi-CPU XDP where it would matter); crossing Volume
Quota/Volume Threshold pushes a `usage_report_event` (checked quota before threshold, since a
single large burst could cross both in one packet) and the latch stops it from repeating on every
subsequent packet -- TS 29.244 Annex C.2.1.1's own real behaviour is for UP to keep forwarding
after a Volume Threshold report, so the counter keeps climbing past the point that already fired.
**Real, disclosed gap, not silently different:** Annex C.2.1.1 has UP function stop forwarding once
Volume Quota is reached (until a new quota arrives); this stage does NOT implement that stop --
doing so before Stage 5 exists to ever provision a fresh quota would strand every session
permanently the first time this is tested. Revisit once Stage 5 lands.

**`datapath.hpp`/`.cpp`: a second BPF map wired into the *same* ring-buffer poll loop.**
`Datapath::create()` now takes a `UsageReportHandler` (invoked on the datapath's own polling
thread), registers `usage_report_ringbuf` via `ring_buffer__add()` onto the existing `ring_buffer`
manager (one polling thread services both maps -- no second thread), and a new
`Datapath::register_urr(teid, threshold, quota)` writes the real per-TEID state UPF's control
plane parses out of a Create URR. One real implementation snag: the ring-buffer callback needs the
complete `Datapath::Impl` type (to reach the handler stashed on it), but `Impl` is a private nested
type -- a free function can't name it from outside. Fixed by making the callback a `static` member
function of `Impl` itself rather than adding a friend declaration.

**`nfs/upf/src/main.cpp`: parses the real Create URR, remembers what a report needs, sends it.**
`build_session_establishment_response_ies` now also decodes `CreateUrr`'s child `UrrId`/
`VolumeThreshold`/`VolumeQuota` (URR ID read off the wire, not assumed to always be SMF's `1`) and
calls `register_urr` alongside the existing `register_teid`. A new `TeidSessionStore` (mutex-
guarded -- written by `run_pfcp_lifecycle`'s main thread on Session Establishment, read and its
per-URR UR-SEQN counter advanced by the datapath's polling thread when a report fires) remembers,
per TEID, exactly what a Session Report Request needs to be addressed and correlated: SMF's real
sender endpoint, the session's CP F-SEID, and the URR ID. The SMF endpoint needs no separate
discovery -- Stage 0's `PfcpPeer` sends every request (including the Session Establishment Request
that reaches this code) from the same persistent socket it also listens on, so `sender` on receipt
already **is** SMF's real, addressable peer. A new `ReportSender` (its own dedicated UDP socket,
mutex-protected `send`) lets the datapath's thread fire the report without touching
`run_pfcp_lifecycle`'s own receive socket concurrently. Two new `pfcp_core` encoders were needed
(UPF is now the encoder, not just the decoder, of these two IEs -- the reverse direction from every
prior stage): `encode_report_type_usage_report()` and `encode_usage_report_trigger_volth()`/
`_volqu()`, added with round-trip unit tests. The header's Sequence Number field uses UPF's own new
node-level counter (`next_pfcp_sequence_number`), deliberately NOT the same value as UR-SEQN --
TS 29.244 gives Sequence Number and UR-SEQN different scopes (per-message node-level correlator vs.
per-URR-lifetime counter) and conflating them was considered and rejected while writing this.

**Live-verified end to end, all real.** Full stack + real `nr-gnb`/`nr-ue`, with a deliberately tiny
seeded grant (`bss/product-catalog` ProductOfferingPrice `unitOfMeasure={amount: 0.000001,
units: "GB"}` -> 1,000 real octets, so a handful of real packets could practically cross it) so this
run's crossing is reachable without sending gigabytes: `smf`'s log --
`granted total volume=1000 octets` -> `provisioning URR 1 ... threshold=900 octets, quota=1000
octets`; `upf`'s log -- `registered URR 1 for TEID 0x1: threshold=900 quota=1000 octets`. A 25-packet
real GTP-U burst (44 real T-PDU octets each, 1,100 cumulative) sent from the isolated peer namespace
(same mechanism ADR-0043's own live verification established) produced, in order: `upf-datapath:
delivered decapsulated T-PDU` x25 (decapsulation itself unaffected), then at real total=924 octets
`upf: sent Sx Session Report Request to 127.0.0.1 for TEID 0x1: total=924 octets, trigger=VOLTH`,
then at real total=1012 octets `... trigger=VOLQU` -- **exactly one of each**, confirming the
one-shot latches work under continued post-crossing traffic, not just once by luck. `smf`'s log
independently confirms real receipt of both, via Stage 0's already-proven handler: `received real
Sx Session Report Request from 127.0.0.1 (seq=1)` then `(seq=2)`. 140/140 tests pass (3 new for this
stage's new UPF-side encoders: `EncodeReportTypeUsageReportRoundTrips`,
`EncodeUsageReportTriggerVolthRoundTrips`, `EncodeUsageReportTriggerVolquRoundTrips`) -- zero
regressions, up from Stage 1's 137/137.

**Consequence:** UPF now genuinely measures real usage and reports it, unsolicited, to a real SMF
that receives it -- the exact real capability ADR-0048's rating engine was missing. SMF's handler
still only architecture-proof-acks (Stage 0's disclosed scope); it does not yet parse the real
Usage Report content or call `Nchf_ConvergedCharging_Update` -- that is Stage 3's job next.

### Stage 3 (2026-08-10): SMF parses the real Usage Report and calls Nchf_ConvergedCharging_Update

**Real spec path confirmed before writing any code.** The vendored
`specs/5G_APIs-REL-19/TS32291_Nchf_ConvergedCharging.yaml` has a real
`POST /chargingdata/{ChargingDataRef}/update` path (verbatim, sharing Create's own
`ChargingDataRequest`/`ChargingDataResponse` schemas), and `MultipleUnitUsage.usedUnitContainer`
(→`UsedUnitContainer.localSequenceNumber`/`totalVolume`) is the real, schema-correct place to
report consumed usage back -- confirmed by reading the YAML directly, not assumed from the Create
side's shape.

**SMF's handler now genuinely decodes the report it receives.** Stage 0's handler only
acknowledged unconditionally; it now decodes `ReportType` (confirms the USAR bit), finds the
`UsageReport` grouped IE, and decodes `UrrId`/`UrSeqn`/`UsageReportTrigger`/`VolumeMeasurement`
from inside it -- all real `pfcp_core` decoders Stage 0/2 already added for exactly this. The Sx
Session Report Response is still sent unconditionally afterward, regardless of whether the
resulting CHF call succeeds -- PFCP acknowledgment and the SBI/N40 call are different protocol
layers, and a CHF outage must not leave UPF's own report unacknowledged.

**New `CpSeidSessionStore`: resolving a report's SEID back to a session.** A Session Report
Request's header SEID is the same `cp_seid` `perform_n4_session_establishment` generated and sent
as the CP F-SEID at Session Establishment (UPF's Stage 2 code echoes it back verbatim, per TS
29.244's addressing rule this file already relies on elsewhere). `perform_n4_session_establishment`
now returns that `cp_seid` on success (was plain `bool`) so `CreateSMContext`'s handler can register
it, alongside the real SUPI and `ChargingDataRef`, in a new mutex-guarded `CpSeidSessionStore` --
written on the ioc thread, read on `PfcpPeer`'s own receive thread (same cross-thread-store
reasoning as `nfs/upf/src/main.cpp`'s `TeidSessionStore`, Stage 2). The store also tracks a real,
per-session `invocationSequenceNumber` counter for TS 32.291's "strictly increasing per invocation"
requirement (Create used `1`; Update calls get `2`, `3`, ...). **Disclosed, pre-existing gap NOT
fixed by this stage:** `perform_n40_charging_data_release` still hardcodes
`invocationSequenceNumber=2` rather than sharing this counter -- if both an Update and a Release
land on the same `ChargingDataRef`, the real strictly-increasing requirement could be violated.
Release predates this stage; fixing it was out of Stage 3's approved scope, flagged here rather
than silently left unnoticed.

**A dedicated CHF client for `PfcpPeer`'s own thread.** The Session Report handler runs on
`PfcpPeer`'s receive thread, not the HTTP/2 server's `ioc` thread that the route handlers'
`chf_client` is only safe to touch from (this file's own established one-client-per-thread
discipline, same reasoning Stage 0's own ADR text used for AMF's NGAP-thread AUSF client). A
second, dedicated `chf_report_client`/`chf_report_oauth` pair was added rather than sharing the
existing one.

**New `perform_n40_charging_data_update`.** Same shape as `_create`/`_release`: builds a real
`ChargingDataRequest` with one `MultipleUnitUsage` (the session's fixed `kDefaultRatingGroup`,
same simplification Create already carries -- no real per-service rating-group mapping exists in
this codebase) carrying one `UsedUnitContainer` (`localSequenceNumber` = the real UR-SEQN this
report carried, `totalVolume` = the real consumed octets), POSTs it to CHF, and logs the real
outcome. A non-200 (expected: CHF has no Update handler yet, Stage 4's job) is logged as an
explicitly-expected, disclosed state, not misreported as a bug in this stage's own request.

**Live-verified end to end.** Same small-seeded-grant setup as Stage 2 (1,000 real octets), same
25-packet real GTP-U burst. `smf`'s log: `received real Sx Session Report Request ... (seq=1)`
immediately followed by `CHF Nchf_ConvergedCharging_Update returned status 404 for
ChargingDataRef=chg-1 (expected until ADR-0050 Stage 4 implements CHF's Update endpoint)`, then the
same pair for `(seq=2)` -- both real Usage Reports (VOLTH and VOLQU) were genuinely decoded, both
resolved to the real session via `CpSeidSessionStore`, both produced a real HTTP/2 POST that
genuinely reached CHF (confirmed: CHF's own generic server returned a real, unregistered-route 404,
not a connection failure) with no crash, deadlock, or interference with the rest of SMF's request
handling. 140/140 tests pass, zero regressions.

**Consequence:** SMF now genuinely closes the loop from "UPF measured real usage" to "CHF was told
about it" -- the request is real, spec-correct, and reaches CHF; CHF just doesn't yet have anything
to say back. Stage 4 (CHF implements the real Update endpoint: applies the reported usage, issues a
follow-on grant) is next.

### Stage 4 (2026-08-10): CHF implements real Nchf_ConvergedCharging_Update

**Real route added, same discipline as Create/Release.** `POST /chargingdata/{ChargingDataRef}/
update` (confirmed verbatim in the vendored `TS32291_Nchf_ConvergedCharging.yaml`, shared
`ChargingDataRequest`/`ChargingDataResponse` schemas). Validates the ref is still active first --
same 404 convention Release already established, but via a new non-destructive
`ChargingDataStore::is_active()` (Release's own `release()` removes the ref as a side effect of
checking it, which Update must not do).

**Real content, not just a schema-valid echo.** Logs the actual reported usage
(`multipleUnitUsage[].usedUnitContainer[].totalVolume`/`localSequenceNumber`) SMF's Stage 3 call
now genuinely carries -- CHF's own evidence the full loop closed, not a placeholder. Re-
authorizes by calling the same `build_rating_grant` catalog-lookup rating engine Create already
uses, returning a fresh `GrantedUnit` in `multipleUnitInformation`, HTTP 200 (not 201 -- the
resource already exists, this updates it, per the YAML's own response code).

**Disclosed, real simplifications, stated plainly rather than presented as a full OCS:** no
balance/wallet deduction against what was already consumed -- this build has no such store
(`docs/CHARGING_MAPPING.md`'s own noted TMF654 Prepay Balance gap); the fresh grant is always the
catalog's full price-plan amount again, not a remaining-balance-aware amount. No differentiation
between a Volume-Threshold report and a Volume-Quota-exhaustion report -- both re-authorize
identically, and this isn't fixable on CHF's side alone: SMF's own Stage 3 code doesn't forward
that distinction as a real `Trigger` in the request body either (a real, disclosed gap on the SMF
side, out of this stage's scope to fix).

**Live-verified end to end, the full loop closed for real.** Same small-seeded-grant (1,000
octets) setup, same real `nr-gnb`/`nr-ue` session, same 25-packet GTP-U burst as Stages 2-3.
`chf`'s log: `Update for ChargingDataRef=chg-1 reports ratingGroup=1 used 924 octets
(localSequenceNumber=1)` immediately followed by a real fresh `rating engine granted 1000 octets`
-- then the same pair for `used 1012 octets (localSequenceNumber=2)`. `smf`'s log confirms real
success this time (not the Stage 3 404): `Nchf_ConvergedCharging_Update succeeded for
ChargingDataRef=chg-1, reported 924 octets used` then `... reported 1012 octets used`. 140/140
tests pass, zero regressions.

**Consequence:** the entire quota-consumption-tracking loop this 7-stage effort set out to build is
now real end to end: UPF measures real usage → reports it unsolicited to SMF → SMF decodes it and
calls CHF → CHF applies it and re-authorizes. What remains: Stage 5 (SMF pushes the new quota back
to UPF via a real Session Modification, so the datapath's own `urr_map` state reflects the
re-authorized quota rather than staying latched at the original one) and Stage 6 (a dedicated
end-to-end live-verification pass across all six stages together, plus a documentation summary).

### Stage 5 (2026-08-10): SMF pushes the re-authorized quota to UPF via real Session Modification

**Real spec path read directly, not assumed, before writing any code.** TS 29.244 §7.5.4 (Sx
Session Modification Request) and §7.5.4.4 (Update URR IE, Table 7.5.4.4-1) read from the vendored
`specs/PFCP/29244-e30.pdf`: real Update URR IE type = **13** (decimal, confirmed), URR ID mandatory,
every other field (including Volume Threshold/Volume Quota) conditional -- "present if X needs to
be modified" -- so this stage's Update URR carries only URR ID + the two fields actually changing,
correctly omitting Measurement Method/Reporting Triggers (unchanged since Create). Also read
§5.2.2.2.1 NOTE 3/4: real online-charging deployments arm Volume Threshold/Volume Quota relative to
a UP-side counter that keeps accumulating across re-authorizations, with the threshold sized to give
the OCS round-trip time to complete before the quota itself is reached -- confirmed this project's
existing cumulative-Volume-Measurement design (Stage 1 onward) is the real, spec-recognized
approach, not an invented one, and directly informed how this stage computes the new absolute
threshold/quota values (see below).

**The real deadlock this stage's design had to avoid.** SMF's Session Report handler runs
synchronously on `PfcpPeer`'s own receive thread (`pfcp_peer.cpp`'s `receive_loop`, unchanged since
Stage 0). A naive Stage 5 implementation -- call `Nchf_ConvergedCharging_Update`, then call
`send_request_and_await_response` for the Session Modification directly from inside that same
handler -- would deadlock: `send_request_and_await_response` blocks on a response that can only be
delivered BY `receive_loop`, which is the very thread currently blocked inside the handler that
called it. Caught before it was ever live-tested (blocking-call chain traced through
`pfcp_peer.cpp` first), not discovered as a hang. **Fix:** the handler still acks the Sx Session
Report Request and does its (fast, non-blocking) decode inline, then hands the two real network
calls (CHF Update, Session Modification) off to a detached `std::thread`. Captured references
(`pfcp_peer`, the dedicated `chf_report_client`/`_oauth`, `smf_instance_id`) are all `main()`'s own
locals, safe to capture into a detached thread specifically because this process never terminates
(same disclosed simplification every other NF in this project already carries) -- not despite that,
because of it. This also retroactively improves on Stage 3's own inline CHF call, which blocked the
receive thread for the HTTP round-trip without being a hard deadlock, but was never ideal either.

**New Volume Threshold/Volume Quota computed relative to the report's own real cumulative usage,
not a fresh baseline.** `perform_n40_charging_data_update` now also parses and returns CHF's
re-authorized `GrantedUnit.totalVolume` (same best-effort parse discipline as Create's own grant
parsing). The new absolute values: `new_quota = reported_used_octets + new_grant`,
`new_threshold = reported_used_octets + 0.9 * new_grant` -- the exact real technique §5.2.2.2.1
NOTE 3/4 describes, and the same 90%/100% ratio Stage 1's own Create URR already uses. This also
means UPF's own `total_octets` counter is deliberately NOT reset on a Modification (only
`register_urr`, Create's own path, zeroes it) -- a new
`Datapath::update_urr_thresholds(teid, new_threshold, new_quota)` does a real read-modify-write of
the BPF map entry (`bpf_map_lookup_elem` then `bpf_map_update_elem` with `BPF_EXIST`), preserving
`total_octets` and resetting only the two one-shot report latches so the new (necessarily higher)
values can be crossed and reported again.

**UPF-side wiring, real and new:** a `SeidToTeidStore` resolves a Session Modification Request's
header SEID (UPF's own F-SEID for the session, allocated at Establishment) back to the TEID it
belongs to; a new pure `TeidSessionStore::get()` (non-mutating, unlike the existing
`get_and_advance_seqn`) resolves the session's real `cp_seid` so the Modification Response's header
SEID is addressed *correctly* this time -- UP-to-CP direction uses the CP's own SEID, per this
file's own already-established addressing rule -- rather than repeating Stage 0's disclosed
echo-the-request's-own-SEID simplification for Session Report Response. `build_session_modification_
response_ies` decodes the real Update URR, applies it via `update_urr_thresholds`, and returns
Cause=RequestAccepted/RequestRejected accordingly.

**Live-verified end to end, including a real, honest timing finding.** Same small-seeded-grant
(1,000 octets, threshold=900) setup, real `nr-gnb`/`nr-ue`, 25-packet GTP-U burst as Stages 2-4.
Both re-authorizations succeeded for real: `smf`'s log -- `Nchf_ConvergedCharging_Update succeeded
... reported 924 octets used, re-authorized 1000 octets` immediately followed by `N4 Session
Modification succeeded for URR 1 (UP F-SEID=0x1): threshold=1824 octets, quota=1924 octets` (=
924 + 1000 and 924 + 900 exactly, confirming the computation above); `upf`'s log independently
confirms the exact same values applied to its own map: `applied Update URR for TEID 0x1:
threshold=1824 quota=1924 octets`. The same pair repeated for the second (VOLQU) report
(`threshold=1912 quota=2012`, i.e. 1012 + 1000 / 1012 + 900).
**Real, disclosed finding, not a bug:** the VOLQU report (at total=1012, exceeding the *original*
quota=1000) fired only ~100ms after the VOLTH report -- before the first re-authorization's real
CHF-call-plus-PFCP-round-trip (which itself took ~101ms) could land in UPF's map. This is exactly
the race TS 29.244 §5.2.2.2.1 NOTE 3/4's own real design intent warns about: the gap between Volume
Threshold and Volume Quota exists specifically to give the OCS round-trip time to complete before
quota exhaustion. This test's artificially tiny quota (1,000 octets, needed to make live
verification practical without sending gigabytes of real traffic) left only a ~100-octet
(~2-packet) window -- nowhere near enough real headroom for a ~100ms multi-hop round trip. A real
deployment sizes this gap in megabytes for exactly this reason; the race is a property of this
test's scale choice, not of the implementation's correctness -- both Modifications still landed and
were applied correctly, just after their respective quota had already been momentarily exceeded by
a couple of packets (traffic was never stopped either way -- Stage 2's own disclosed gap, forwarding
never halts on quota exhaustion in this build). 140/140 tests pass, zero regressions.

**Consequence:** the full quota-consumption-tracking loop (UPF measures → reports → SMF decodes →
CHF re-authorizes → SMF pushes the new quota back to UPF) is real end to end, including the
feedback path back into the datapath. Stage 6 (a dedicated, larger-quota end-to-end live-
verification pass demonstrating the re-authorized quota being respected with real headroom, plus a
documentation summary closing out this 7-stage effort) remains.

### Stage 6 (2026-08-10): dedicated end-to-end live verification with real headroom -- effort closed

No new code -- this stage is a dedicated live-verification pass, deliberately sized to give the
real CHF-call-plus-PFCP-Modification round trip (Stage 5's own live verification measured it at
~100-104ms) genuine headroom to complete before a quota is exhausted, closing the one honestly-
disclosed gap Stage 5's live verification left open (its artificially tiny 1,000-octet test grant
raced the round trip and hit the original Volume Quota before the first re-authorization landed).

**Real grant, real headroom, real packet size.** Seeded via `bss/product-catalog`: 200,000-octet
grant (threshold=180,000, the same 90%/100% ratio every stage has used), sent as 260 real GTP-U
G-PDUs carrying a realistic ~1400-byte T-PDU each (MTU-sized, not the earlier stages' 44-byte test
payload), 30ms apart -- a real ~14-packet (~420ms) window between the original Volume Threshold and
Volume Quota, several times the measured round-trip latency.

**Result: the re-authorized quota was genuinely respected, not raced.** `upf`'s log: real VOLTH at
total=180,600 (crossing the real 180,000 threshold) -> real re-authorization landed 104ms later
(`applied Update URR for TEID 0x1: threshold=360600 quota=380600 octets`) -> **zero VOLQU reports
at any point** -- traffic sailed straight through the ORIGINAL quota=200,000 mark because the
datapath's own map already held the new, far higher threshold/quota by the time cumulative usage
reached it, not because forwarding was ever stopped (Stage 2's own disclosed non-enforcement still
applies, but was never even reached here) -- then a second, real VOLTH fired at total=361,200,
matching the NEW threshold=360,600 almost exactly, and re-authorized again
(threshold=541,200/quota=561,200). All 260 real T-PDUs were delivered to `upf-tun0` throughout,
uninterrupted. `smf`'s and `chf`'s logs independently confirm the same two real
Update-then-Modification cycles. 140/140 tests pass, zero regressions -- final full-suite run for
this effort.

**Consequence, and this 7-stage effort's real, honest final state:** ADR-0048's quota-consumption-
tracking/re-authorization gap is closed, end to end, for real: UPF measures real usage in-kernel,
reports it unsolicited to SMF, SMF calls CHF's real Update endpoint, CHF applies the reported usage
and re-authorizes, and SMF pushes the new quota back into the live datapath -- demonstrated both
under real timing pressure (Stage 5, an honest disclosed race at an artificially tiny scale) and
with real headroom (this stage, at a scale closer to how a real deployment would size the
Threshold/Quota gap). Real, disclosed gaps still standing, none silently dropped: UPF never actually
stops forwarding on Volume Quota exhaustion (Stage 2); CHF applies no real balance/wallet deduction
and re-grants the same catalog amount unconditionally every time, not a remaining-balance-aware
amount (Stage 4); neither SMF nor CHF differentiate a Volume-Threshold report from a
Volume-Quota-exhaustion one via a real `Trigger` (Stage 3/4); `perform_n40_charging_data_release`
still hardcodes its own `invocationSequenceNumber` rather than sharing the per-session counter
(Stage 3); `kSmfCpFunctionPfcpPort` remains a lab-only convention, not an IANA/spec assignment
(Stage 0). None of these block the real capability this effort set out to build; all are recorded
here, not discovered later in review.

