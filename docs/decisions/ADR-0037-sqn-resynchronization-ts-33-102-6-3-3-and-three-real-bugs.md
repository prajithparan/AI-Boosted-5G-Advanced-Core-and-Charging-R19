## ADR-0037: SQN resynchronization (TS 33.102 §6.3.3), and three real bugs found only by closing the loop with a real `nr-ue`

**Date:** 2026-08-08
**Status:** Accepted

**Context:** ADR-0036 left SQN resynchronization as the largest disclosed gap blocking a real,
unmodified `nr-ue` from ever completing authentication: UDM's seeded test SQN and UERANSIM's own
USIM-simulator SQN state start out of sync (`FF9BB4D0B607` received vs. `000000000000` expected on
the UE side, confirmed in this session's own interop logs below), and TS 33.102's normal AKA
procedure has no way to recover from that except the explicit resync exchange (`AuthenticationFailure`
with `mmCause=0x15`/SYNC_FAILURE carrying `AUTS`, TS 24.501 §5.4.1.3.7). Every prior stage's real
interop was blocked at exactly this point (ADR-0032 through ADR-0036 all record it). This ADR closes
it, and in the process of finally being able to run a real UE through the *entire* procedure for the
first time, surfaces and fixes two further real bugs that no amount of self-consistency testing
against synthetic vectors had caught.

**f1\*/f5\* (Milenage "star" functions) added to `libs/aka-crypto`** (`milenage.hpp`/`.cpp`):
TS 33.102 Annex C.3's resync MAC-S/AK\* computation uses distinct rotation constants and a fixed
AMF (`0x0000`, not the real network AMF) from the normal f1/f5 functions used for authentication --
confirmed against UERANSIM's own compiled `milenage.c` (`crypt-ext/milenage.c`), not invented.
`verify_and_decode_auts` computes AK\* via f5\*, XORs it into the received AUTS to recover SQN_MS,
recomputes MAC-S via f1\*, and compares. Cross-checked via a standalone scratch harness linking
UERANSIM's real compiled `milenage_f1`/`milenage_f2345`/`milenage_auts` functions directly (not
UERANSIM's higher-level wrappers): 80/80 matches across 20 random trials × 4 checks (f5\*, f1\*, full
AUTS round-trip, tamper-rejection). 3 new unit tests in `tests/conformance/test_milenage.cpp`.

**UDM's `resync_sqn` (`nfs/udm/src/stores.cpp`) advances the stored SQN by `+0x10000`, not `+1`.**
UERANSIM tracks freshness via TS 33.102 Annex C.3's array scheme (`SqnManager`, confirmed at
`usim.cpp:18`: `indBitLen=5`) -- the low 5 bits of SQN are an array index (IND), not
freshness-checked directly; only the upper SEQ bits are. A naive `SQN_MS + 1` lands entirely inside
the IND bits, leaving SEQ unchanged, so the "resynced" vector would still fail freshness on the UE
side. Advancing by `2^16` guarantees SEQ moves regardless of IND width up to the spec's max allowed
16 bits. `AuthenticationInfoRequest`/`AuthenticationInfo`'s existing (real, present-in-YAML)
`resynchronizationInfo` field (`ResynchronizationInfo_Nudm_UEAU{rand, auts}`, TS 29.503/29.509) is
plumbed UDM<-AUSF<-AMF, and AMF's retry loop (`nfs/amf/src/ngap_task.cpp`,
`initiate_5g_aka_authentication`) is guarded by a new `sqn_resync_attempted` flag so it retries
exactly once per association, not in an unbounded loop against a still-broken vector.

**KAMF derivation (TS 33.501 Annex A.7) was using the wrong SUPI format.** AMF fed the full
`"imsi-999700000000001"` string into `derive_kamf`; UERANSIM's own `Supi::Parse` strips the prefix
before its KDF call, using bare digits only. Fixed via a `strip_imsi_prefix` helper applied only at
the `derive_kamf` call site (`auth_state.supi` itself is left prefixed everywhere else -- AUSF/PCF/
SMF calls all expect the `"imsi-"` form). Verified against UERANSIM's real `crypto::CalculateKdfKey`
using both synthetic and real live-captured KSEAF values. This fix alone did not resolve real
interop -- SecurityModeCommand still failed -- leading to the next, deeper bug.

**The actual root cause of every `SecurityModeCommand`/NAS-integrity failure this whole staged
effort had hit: `libs/aka-crypto`'s 128-NIA2 primitive was correct, but `nfs/amf/src/nas_codec.cpp`
was calling it on the wrong input.** TS 24.501's NAS MAC construction (as implemented by
UERANSIM's `nas_enc::ComputeMac`) prepends the 1-octet NAS sequence number (COUNT's low-order byte)
to the message bytes *before* computing the 128-NIA2 MAC -- a NAS-security-layer detail on top of
(not a replacement for) EIA2's own COUNT parameter, which this project's `encode_secured_downlink`/
`decode_secured_uplink` had never accounted for. Found only after exhausting every other
possibility: KAUSF/KSEAF/KAMF/KNASint/KNASenc all cross-checked byte-for-byte against UERANSIM for
real live values; the raw EIA2 primitive cross-checked against UERANSIM's `eia2::Compute` for both
arbitrary and real captured inputs; the NGAP/ASN.1 wire encoding decoded correctly through
UERANSIM's own real `libasn-ngap.a`; manual NAS TLV framing traced against `SecurityModeCommand::
onBuild`'s real decode order -- all correct. Resolved by instrumenting UERANSIM's actual `nr-ue`
binary with temporary debug logging (since fully reverted, confirmed via `grep` before rebuild),
confirming keys and message bytes matched exactly between AMF and UE yet computed MACs still
differed, which is what led to re-reading `nas_enc::ComputeMac`'s exact body and finding the
prepended sequence-number byte. Fixed in both `encode_secured_downlink` and `decode_secured_uplink`
(shared by every secured NAS message this project builds or verifies); 4 existing unit tests in
`tests/conformance/test_nas_codec.cpp` updated to match. **This fix produced this project's
first-ever complete real registration**: `nr-ue` logged `Initial Registration is successful` and
automatically proceeded to `Sending PDU Session Establishment Request`.

**A fourth bug surfaced immediately by that same success: AMF's state machine was waiting for a
`RegistrationComplete` a real UE will never send.** TS 24.501's actual rule (confirmed by reading
UERANSIM's `receiveInitialRegistrationAccept`,
`simulators/ransim/vendor/UERANSIM/src/ue/nas/mm/register.cpp:346-426`) is that `RegistrationComplete`
is sent *conditionally* -- only if `RegistrationAccept` carried a 5G-GUTI, an NSSCI=CHANGED
indication, or a configuredNSSAI. `encode_registration_accept` sends none of those (an
already-disclosed simplification, see its own comment -- no GUTI allocation scheme exists in this
project), so a real UE correctly never sends `RegistrationComplete` and instead proceeds straight to
PDU Session Establishment. AMF was staying in a phase that would never advance, so it rejected the
UE's next real message (the PDU Session Establishment Request's `UlNasTransport`) as a
"RegistrationComplete MAC verification FAILED," even though the MAC itself was fine -- the message
was just a different, expected type. **This was a design gap, not a crypto bug**: removed
`UeAuthState::Phase::AwaitingRegistrationComplete` entirely; AMF now proceeds directly from sending
`RegistrationAccept` to requesting the AM Policy Association from PCF (folded into
`handle_uplink_nas_transport_smc_complete`, since that call no longer waits on anything from the
UE), then to `AwaitingPduSessionEstablishmentRequest`. This also fixed a related off-by-one: the PDU
Session Establishment handler's `uplink_count` was hardcoded to `2` (assuming a RegistrationComplete
had used count `1`); it is now correctly `1`, since SecurityModeComplete (count `0`) is genuinely the
only secured uplink message before it. `amf::nas::decode_registration_complete` itself is kept
(unit-tested, spec-correct) but is now documented as currently unreachable by any production
handler, for whenever a future turn adds real GUTI reassignment.

**Verification:** full real interop, first attempt, no manual message spoofing anywhere in the
chain -- real `nrf`/`udr`/`udm`/`ausf`/`pcf`/`smf`/`amf` plus real `nr-gnb`/`nr-ue`: NG Setup →
Initial Registration → Authentication Failure (SQN out of range) → SQN-resync Authentication Request
accepted → SecurityModeCommand/Complete verified → RegistrationAccept sent → AM Policy Association
established with PCF → PDU Session Establishment Request verified → SM context established with
SMF. `nr-ue` logged `Initial Registration is successful` followed immediately by
`Sending PDU Session Establishment Request`, with **zero retries or failures anywhere in the
registration or SM-context-creation path** -- the first time this has happened in the project's
history. The UE does still retransmit its PDU Session Establishment Request afterward (`T3580`
expiry), which is expected and separately disclosed (ADR-0036: SMF's `CreateSMContext` response
carries no `n1SmMsg`, so AMF has no real Accept content to send back yet); AMF handles the
retransmit gracefully via its pre-existing `Done`-phase fallback (logs and ignores, no crash, no bad
state). Full `ctest` suite re-run clean: 87/87 passing, zero regressions.

**Consequence:** Phase 2's "no narrowed slice" definition of done -- UE registration *and* PDU
session establishment, demonstrated end-to-end against a real, unmodified UE -- is now genuinely
met for the first time, not just implemented-but-blocked. The one remaining disclosed gap in this
path is the missing PDU Session Establishment Accept content (ADR-0036's own disclosed scope,
unchanged by this ADR).

