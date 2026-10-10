## ADR-0393: AMF UE-originating Deregistration (TS 23.502 §4.2.2.3.2) -- LI programme prerequisite 3

**Date:** 2026-09-27. **Status:** accepted; the common register -> establish a PDU session ->
deregister path is implemented and covered by a real NGAP/SCTP+NAS integration test; several real,
disclosed scope boundaries remain (below).

**Why now.** ADR-0440's own "still blocked" section named this exact gap: AMFIdentifierDeassociation
"needs an identifier deassociation without a new association within the same UE context. The AMF
has no UE deregistration procedure. ... The only conformant trigger is deregistration, which is
ADR-0378 prerequisite 3." This closes that prerequisite.

**Decision 1: implement UE-originating Deregistration only; network-initiated is out of scope.**
This AMF has no trigger anywhere in its own code that would decide, on its own, to deregister a
UE -- inventing one to reach the network-initiated LI trigger bullet would be fabricating a
procedure this build has no other reason to have. TS 23.502 §4.2.2.3.2's UE-initiated case is real,
common, and is the one a real deregistering UE (airplane mode, SIM removal, manual "forget
network") actually sends.

**Decision 2: real byte layouts from the same vendored oracle every earlier NAS stage used.**
`simulators/ransim/vendor/UERANSIM/src/lib/nas/msg.cpp`'s `DeRegistrationRequestUeOriginating::
onBuild` (`mandatoryIE1(&ngKSI, &deRegistrationType); mandatoryIE(&mobileIdentity);`) and
`DeRegistrationAcceptUeOriginating::onBuild` (empty -- no IEs at all) fix the wire shape exactly.
`ie1.cpp`'s `IEDeRegistrationType::Decode`/`Encode` and `utils/bits.hpp`'s real `Ranged8`/
`Bmp4Dec112` bit order fix the one packed byte's layout: ngKSI in the high nibble (same convention
`decode_registration_request`'s own ngKSI/registrationType byte already established), then
switchOff (bit3) | reRegistrationRequired (bit2, spare in the UE->network direction, not decoded --
this project has no CONFIGURATION UPDATE COMMAND procedure for it to matter to) | accessType
(bits0-1, always THREEGPP_ACCESS -- ADR-0031's single-access-type scope) in the low nibble.
`nas_codec.cpp` gained message-type constants 0x45/0x46 (`kMessageTypeDeregistrationRequestUeOriginating`/
`...AcceptUeOriginating`), `decode_deregistration_request`, and `encode_deregistration_accept`.

**Decision 3: the mobile identity IE is walked past, not decoded.** A real UE sends its assigned
5G-GUTI here (TS 24.501 §5.5.2.2.1); this AMF already knows which UE this is from the
association's own `UeAuthState` (unlike ServiceRequest, which genuinely needs identity from the
wire because it arrives on a FRESH association with no prior state to consult). Decoding a
specific identity kind here would add real parsing work with no caller that needs the result;
skipping the TLV by its own length field is correct regardless of what identity type it carries.
The test driver still sends a REAL 5G-GUTI (extracted from its own RegistrationAccept via the new
`extract_guti_from_registration_accept`, the mirror-decode of `encode_registration_accept`'s own
GUTI IE), matching what a real UE actually sends -- a fabricated identity would have been a weaker
test of the framing even though AMF is provably indifferent to its content.

**Decision 4: the uplink NAS COUNT is tracked on `auth_state`, not hardcoded.** Every earlier
NGAP/NAS stage hardcodes its own uplink_count literal (0/1/2) because this project's phase machine
was, until now, strictly linear -- each phase has exactly one real message type and one real count.
`Phase::Done` breaks that: it is reached both by a real PDU session establishment (next real count
3) and by a ServiceRequest reconnect (next real count is that persisted store's own post-increment
value) -- two different real counts for the one phase a Deregistration can arrive in. Hardcoding
either would silently fail the MAC on whichever path did not use it. `UeAuthState` gained
`next_uplink_count`, set for real by both paths that reach `Phase::Done`
(`handle_uplink_nas_transport_pdu_session_establishment` sets 3;
`handle_service_request` sets its own `uplink_count + 1`, the exact post-increment value
`UeSecurityContextStore::next_uplink_count`'s own atomic counter already holds).

**Decision 5: the downlink NAS COUNT for DEREGISTRATION ACCEPT belongs to `NgapUeRegistry`, not a
local literal either.** SMF's own asynchronous `Namf_Communication` N1N2MessageTransfer callback
(landing on the SBI server's own thread) can advance a UE's `next_downlink_count` between this
association's own NGAP-thread reads of it, exactly the same reason `send_dl_nas_transport` already
owns that counter instead of a caller. `NgapUeRegistry::send_deregistration_accept` mirrors
`send_dl_nas_transport`'s own get-and-increment-under-the-registry's-own-mutex shape, just sealing
a plain 5GMM message (`amf::nas::encode_deregistration_accept`) instead of wrapping an N1 SM
container.

**Decision 6: a real, AMF-INITIATED NGAP UEContextReleaseCommand, factored out of the existing
RAN-initiated one.** `handle_ue_context_release_request`'s own header comment used to say
"this does NOT implement the AMF-INITIATED direction ... this lab has no such trigger yet." It now
does: `send_ue_context_release_command(assoc, amf_ue_id, nas_cause)` is the same PDU-building code
that function always had, factored out and parameterized by cause so both call sites share it --
`CauseNas_normal_release` for the pre-existing RAN-initiated round trip, `CauseNas_deregister` for
this one. The gNB's own `UEContextReleaseComplete` confirms either through the SAME
`handle_ue_context_release_complete`, not a second confirmation path.

**Decision 7: real cleanup of the persisted stores, but only for a REAL deregistration.**
`UeAuthState` gained a `deregistered` flag, set only by
`handle_uplink_nas_transport_deregistration` just before it triggers the release. Only when that
flag is set does `handle_ue_context_release_complete` remove the tmsi-keyed
`UeSecurityContextStore`/`AmfUeIdIndexStore` entries -- the RAN-initiated round trip (radio-link
failure, O&M intervention) reaches the SAME confirmation handler and must NOT trigger this removal:
that UE is still registered, and a real ServiceRequest reconnect still needs those entries. Getting
this wrong either way was a real risk this decision exists to name: unconditionally removing on
every release would break ServiceRequest reconnection; never removing would leave a deregistered
UE's old 5G-TMSI silently still honoured by a later ServiceRequest, the opposite of what
"identifier deassociation" means.

**Decision 8: real PCF AM Policy Association termination, closing a gap found while building
this.** Auditing what a UE-initiated deregistration owes (TS 23.502 §4.2.2.3.2) surfaced that
`handle_uplink_nas_transport_registration_complete` had been discarding PCF's own `Location` header
since the AM Policy Association was first created (ADR-0075) -- the same class of gap ADR-0249
found and fixed for SMF's `smContextRef`. `polAssoId` is now captured the identical way and stored
on `auth_state`; deregistration calls PCF's real `DELETE .../npcf-am-policy-control/v1/policies/
{polAssoId}` (confirmed against `nfs/pcf/src/main.cpp`'s own route, 204 success / 404 already-gone),
best-effort (a missing PCF token or an unreachable PCF is logged, not allowed to hang the UE's own
teardown) -- the same discipline SMF's own `ReleaseSMContext` route already applies to ITS
best-effort PCF/CHF calls, which this AMF's own `Nsmf_PDUSession_ReleaseSMContext` calls (one per
`smContextRef` this UE has) follow too.

**Decision 9: the LI IRI-POI's `AMFDeregistration`/`AMFIdentifierDeassociation` codecs gained their
real C members.** `libs/li-core`'s `AMFDeregistration_t`/`AMFIdentifierDeassociation_t` (asn1c-
generated from `TS33128Payloads.asn`) already carried every field TS 33.128 table 6.2.2.2.3-1/
6.2.2.2.7-2 names; only `fill()`/`extract()` had been written for the two M-only members
(`li_poi.hpp`'s own header comment said as much: "added when the AMF POI that produces this event
is wired against real AMF state" -- this is that wiring). Added: `supi`, `guti`, `location` (all
optional/C, populated when the call site has them) and `switchOffIndicator` (tag `[10]`, real
`SwitchOffIndicator ::= ENUMERATED { normalDetach(1), switchOff(2) }` from the vendored ASN.1,
populated from the exact bit this procedure already decodes to decide whether to send an ACCEPT at
all). Not populated: `sUCI`/`pEI`/`gPSI`/`cause`/`reRegRequiredIndicator`/
`unavailabilityPeriodDuration`/`additionalUserIdentifiers` -- this AMF retains no SUCI past
registration, captures no PEI, has no GPSI mapping, and this increment's scope is a UE-initiated
ACCEPT (no reject cause applies; re-registration-required is spare in the UE->network direction
anyway). `AmfXiriRecord` gained `Deregistration`; `xiri_record_enabled`'s existing decision table
needed no logic change (Deregistration is not an identifier-association record, so it follows
Registration/LocationUpdate's own rule -- verified by an extended `RecordMatrix` test case, not
assumed). `LiPoi::report_deregistration`/`report_identifier_deassociation` mirror
`report_registration`'s own per-matched-task, gating-checked, best-effort-emit shape exactly.
Covered by two new `LiAmfPoi` unit tests (direction FromTarget for Deregistration per table
5.3.2-1's UE-initiated case, direction 5/not-applicable for Deassociation per clause 6.2.2.2.7,
both switchOff values, and the C-members-stay-absent-when-not-given case) -- 8/8 passing locally.

**Verification.**
- `li_amf_poi_integration_tests` (LiPoi unit scope, no AMF process): 8/8 passing, including the two
  new tests above and the extended `RecordMatrix`.
- `test_amf_deregistration.cpp` (real NGAP/SCTP+NAS, the full NF fleet -- NRF, UDR, UDM, AUSF, AMF,
  PCF, SMF): `NormalDeregistrationTearsDownSessionAndUeContext` drives register -> establish a PDU
  session -> deregister (switchOff=false), asserting a real DeregistrationAccept (MAC-verified
  against the UE's own KNASint at downlink_count=3) followed by a real AMF-INITIATED
  UEContextReleaseCommand (Cause=nas/deregister, parsed for real, not just "some release arrived"),
  then confirms it with a real UEContextReleaseComplete.
  `SwitchOffDeregistrationSendsNoAccept` drives the same setup with switchOff=true and asserts the
  FIRST message back is the NGAP release itself, not a DownlinkNASTransport.
- The peer-NF calls this procedure makes (SMF `ReleaseSMContext`, PCF
  `DeleteIndividualAMPolicyAssociation`) are exercised for real over real mTLS HTTP/2 by these
  tests, but their own HTTP-level correctness is covered by their own dedicated test files; these
  tests assert that AMF orchestrates them and completes the real NAS+NGAP procedure.
- `amf`/`integration_tests` build clean throughout this increment's own incremental builds.

**Real, disclosed scope boundaries (not silently narrowed).**
- **Network-initiated deregistration** is not implemented -- no trigger for it exists anywhere in
  this AMF. `AmfDirection` is always encoded `UeInitiated` in the emitted xIRI as a direct
  consequence, not a simplification of a case this build could reach.
- **A Deregistration arriving in `Phase::AwaitingPduSessionEstablishmentRequest`** (before this UE
  ever established a PDU session) is not handled -- this dispatch switch only reaches
  `handle_uplink_nas_transport_deregistration` from `Phase::Done`, matching this project's existing
  single-fixed-order-per-association scope (ADR-0031). A UE that deregisters before any PDU session
  exists is a real gap this build cannot exercise yet, named here rather than discovered later.
- **A Deregistration immediately following a ServiceRequest reconnect** runs its real NAS/NGAP/
  SMF-release procedure correctly (the COUNT tracking in Decision 4 covers it), but its own LI
  xIRIs carry no gUTI -- `UeSecurityContext` (the persisted, cross-association store
  ServiceRequest reads) does not itself store GUTI components, so `auth_state.guti` is left unset
  on that path rather than reconstructed from partial information. SUPI is still present in that
  case's xIRIs; only the C ("if available") gUTI member is genuinely unavailable there. Not
  covered by either integration test (both use the common register -> PDU session -> deregister
  path).
- **Nudm_UECM has no registration/deregistration call anywhere in this AMF** -- a separate,
  pre-existing gap this procedure does not touch: nothing was ever registered with UDM for this
  procedure to deregister.
- **LI is not exercised end to end for this procedure.** The AMF process both integration tests
  spawn runs with LI disabled (`config/amf.json`'s default) -- `report_deregistration`/
  `report_identifier_deassociation`'s call sites inside
  `handle_uplink_nas_transport_deregistration` are compile-verified only, the same disclosed limit
  ADR-0440 already recorded for its own NGAP-side hooks (`ngap_task.cpp`/`ngap_handover.cpp`'s
  PathSwitchRequest/HandoverNotify hooks). Building an LI-enabled AMF process harness is its own
  increment, not attempted here.
- **T3346 back-off timer handling for a rejected re-registration**, IMEISV request, and any
  reject-cause path for DeregistrationRequest itself (this procedure only ever accepts) are out of
  scope, consistent with this project's existing "the happy path first, real gaps disclosed" NAS
  coverage discipline.

**Fixed while here.** `ue_context_store.hpp`'s header comment claiming "nothing currently calls
put()" was stale (ADR-0249's own `CreateSMContext`/AM-Policy-Association response handling has
called it since that ADR) -- corrected to say what actually calls it and why the CreateUEContext-
specific gap it originally described is still real but separate.

**Rejected alternatives.**
- *A generic "peek the message type after decrypting, dispatch on that" mechanism inside
  `Phase::Done`*: `Phase::Done` has exactly one real, implemented message type reachable from it
  today (Deregistration); building a dispatcher for a set of one would be speculative generality
  with no second case to justify it. If a second post-establishment procedure is ever added, this
  is the natural place to introduce one.
- *Reconstructing `auth_state.guti` for the ServiceRequest-reconnect path from
  `AmfUeIdIndexStore`/`UeSecurityContextStore`*: neither store persists GUTI components (only
  `tmsi`, which is the 5G-TMSI half of a GUTI, not the full region/set/pointer/PLMN). Doing this
  right would mean extending `UeSecurityContext` to also persist those fields at registration time
  -- a real, reasonable follow-up, but out of this ADR's own scope (it would touch the registration
  path's own persisted-context shape, not just deregistration).
- *A single combined `report_deregistration_event` call instead of two separate `LiPoi` methods*:
  rejected for the same reason `report_registration`/`report_identifier_association` are already
  separate calls at the REGISTRATION ACCEPT site -- each record has its own gating decision
  (`xiri_record_enabled`), own Payload Direction, and TS 33.128 defines them as two distinct record
  types with two distinct trigger clauses (6.2.2.2.3 vs 6.2.2.2.7), even though this AMF happens to
  emit both from the same real event.

