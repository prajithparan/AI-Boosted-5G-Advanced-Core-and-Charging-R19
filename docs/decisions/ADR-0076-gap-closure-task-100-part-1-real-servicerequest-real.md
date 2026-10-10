## ADR-0076: gap-closure task #100 (part 1) -- real ServiceRequest, real persistent NAS security context, real 5G-GUTI assignment

### Context

`docs/CAPABILITY_GAP_ANALYSIS.md`'s AMF section named the single highest-impact finding of the
whole free5GC/open5GS sweep: `ServiceRequest` (TS 24.501 §5.6.1), the dominant real NAS procedure
for CM-IDLE -> CM-CONNECTED transitions, was entirely unimplemented, and this project's AMF had
zero N2 handover support. This ADR closes the `ServiceRequest` half of task #100. N2 handover
(NGAP `HandoverRequired`/`HandoverRequestAcknowledge`/etc., SMF's own coupled `UpdateSMContext`
gap, task #101) is NOT addressed here -- a separate, still-larger piece of work, disclosed as
still open.

### The real, load-bearing architectural prerequisite this surfaced

Every NAS security context this project had built until now (`ngap_task.cpp`'s own `UeAuthState`)
lived ONLY in per-NG-association memory, destroyed the moment the SCTP association tore down --
correct for the single-registration-per-association scope every prior NGAP/NAS stage disclosed,
but it meant a UE reconnecting on a FRESH association (exactly what `ServiceRequest` is for) had
nothing to reconnect to. Closing `ServiceRequest` therefore required building real, persistent
security-context storage first, not just a new decode function:

1. **`nfs/amf/src/ue_security_context_store.hpp`/`.cpp`** (new): `UeSecurityContextStore`,
   Redis-backed (AMF's first-ever real Redis dependency -- `AMF_REDIS_URL`, same getenv/fail-fast
   pattern as every other NF's own Redis connection), keyed by 5G-TMSI. Stores KAMF (the real
   TS 33.501 root key; KNASint/KNASenc are re-derived from it on load, not separately persisted)
   plus a real, PERSISTENT, monotonically-incrementing NAS uplink/downlink COUNT (TS 24.501
   §4.4.3.1) -- replacing every prior stage's own hardcoded per-association literal
   (`downlink_count=0/1/2`) now that a security context genuinely survives across multiple NG
   associations.
2. **Real 5G-GUTI assignment** (`encode_registration_accept`, extended): a UE has no TMSI to
   present in a later `ServiceRequest` without one. Real TS 24.501 §9.11.3.4 GUTI structure (PLMN
   + AMF Region ID + AMF Set ID (10 bit) + AMF Pointer (6 bit) + 5G-TMSI), byte layout confirmed
   against UERANSIM's own `IE5gsMobileIdentity::Encode` (arms-length reference, ADR-0016/-0031).
   **Real consistency bug caught before it shipped**: the AMF Region/Set/Pointer values initially
   chosen for the GUTI didn't match the all-zero AMF Region/Set/Pointer this AMF's own
   `NGSetupResponse` GUAMI already broadcasts to the gNB (existing, live code) -- fixed to `0/0/0`
   to match, not an independently-chosen value.
3. **Real, cascading consequence of adding a GUTI, found and handled correctly, not
   discovered-and-ignored**: TS 24.501's real UE behavior (confirmed via UERANSIM source, already
   cited in this project's own prior comments) is that a UE sends `RegistrationComplete` if
   `RegistrationAccept` carries a 5G-GUTI. This project's own `RegistrationAccept` never had before
   -- meaning a real UE now sends a message this AMF previously had no phase/handler for at all.
   Skipping it would have desynchronized the NAS uplink COUNT every later secured message's MAC
   verification depends on, not just missed a log line. Fixed properly: a new
   `AwaitingRegistrationComplete` phase, a new `handle_uplink_nas_transport_registration_complete`
   handler (reusing the already-built, already-tested-but-previously-unreachable
   `decode_registration_complete`, ADR-0031's own "kept for when a future turn adds GUTI
   reassignment" -- that turn is this one), and the PDU Session Establishment Request's own
   `uplink_count` shifted from `1` to `2` to account for the new message in between.

### `ServiceRequest`/`ServiceAccept`/`ServiceReject` codec (`nfs/amf/src/nas_codec.hpp`/`.cpp`)

Real, load-bearing protocol property (TS 24.501 §4.4.4.3, confirmed against UERANSIM source):
`ServiceRequest` is integrity-protected but NEVER ciphered, specifically so the network can read
the plaintext 5G-TMSI it carries to look up which security context to verify the MAC against --
solving the real "can't decrypt until we know who this is, can't know who this is until we
decrypt" problem. Decode is split into two real steps: `peek_service_request_tmsi` (no key
needed, reads the always-plaintext TMSI) then `decode_service_request` (real MAC verification
once the caller has looked up a context). `encode_service_accept`/`encode_service_reject_plain`
close the response side. Full real handling wired into `ngap_task.cpp`'s
`handle_service_request`: unknown TMSI -> real `ServiceReject` (cause
`UE_IDENTITY_CANNOT_BE_DERIVED_FROM_NETWORK`); ngKSI mismatch -> real, disclosed
security-context-desync rejection; success -> `ServiceAccept`, CM-IDLE->CM-CONNECTED, UE
re-registered in `NgapUeRegistry` (so a later `Namf_Communication` N1N2 delivery can still reach
it after a reconnect). Real, disclosed scope boundary: closes the CM-IDLE->CM-CONNECTED
transition itself; does NOT drive real N2 PDU Session Resource Setup for any PDU session
`uplinkDataStatus` reports as pending -- that's SMF's own `UpdateSMContext` real N2SmInfo
dispatch, task #101, a separate, already-tracked gap, logged as a warning when observed rather
than fabricated.

### Testing and verification -- both real interop AND a real bug caught by unit tests, neither alone would have been enough

**Real, live interop** (full lab stack + UERANSIM's real `nr-gnb`/`nr-ue`, not simulated): the
core registration-flow changes (the highest-risk part -- modifying already-working, already-tested
sequencing) were verified end-to-end against a genuinely interoperating UE. Real UE log: "Sending
Registration Complete" (confirming the GUTI-triggered behavior change actually happens with real
UE software, not just in theory); real AMF log: "RegistrationComplete verified OK", "AM Policy
Association established with PCF", "SM context established with SMF" -- the full chain held
through the new `RegistrationComplete` step and the shifted `uplink_count=2` for PDU Session
Establishment. `RegistrationAccept`'s own real wire size (26 bytes, `tmsi=00000001`) matched this
ADR's own byte-count math exactly, independent corroboration. A real, separate finding: gNB-side
`ue-release` (the natural way to test `ServiceRequest` via idle-mode re-entry) sends a real NGAP
`UEContextReleaseRequest` this AMF cannot yet decode at all -- itself part of the still-open N2
gap (task #101's own NGAP-coverage half) -- so `ServiceRequest` specifically could not be
naturally exercised via this same interop run.

**Real unit tests, added because the interop run above could not reach this code path**
(`tests/conformance/test_nas_codec.cpp`, 6 new + 1 updated): found and fixed two real bugs neither
self-consistency nor the interop run would have caught:
1. An off-by-one in the short TMSI-identity decode: read `off+2..off+5` instead of `off+3..off+6`,
   silently folding the packed AMF-Set-ID/Pointer byte into the TMSI's own high byte and dropping
   the real last TMSI byte.
2. The short TMSI-identity value length itself was wrong -- assumed 6 octets, the real value
   (confirmed against UERANSIM's own `IE5gsMobileIdentity::Encode` TMSI case) is 7
   (identity-type + 2 packed octets + 4-octet TMSI). Both bugs were in the DECODE path
   specifically -- the ENCODE path (GUTI in `RegistrationAccept`) that the real interop run did
   exercise uses a different, longer value shape and was unaffected, which is exactly why the
   interop run's success didn't also prove the decode path correct.

Full `conformance_tests` suite: 261/261 pass (up from 255, the 6 new `ServiceRequest`/
`ServiceAccept`/`ServiceReject` tests), zero regressions. `AmfIntegration.*` (5 tests, HTTP/SBI
level): pass unchanged -- confirms the new hard Redis dependency (AMF's `redis->ping()`
fail-fast-at-startup, same pattern as CHF/PCF) doesn't break the existing test harness, since the
same Redis instance CHF's own tests already use satisfies it.

### What this ADR does NOT include

N2 handover (NGAP `HandoverRequired`/`HandoverRequestAcknowledge`/`HandoverCommand`/
`HandoverNotify`/`PathSwitchRequest`, SMF's own coupled `UpdateSMContext` N2SmInfo dispatch) --
task #101, a separate, still-larger piece of gap-closure #100 work, not started. Real N2 PDU
Session Resource Setup triggered by a `ServiceRequest`'s own `uplinkDataStatus` -- logged when
observed, not implemented (couples to the same task #101 gap). NGAP `UEContextRelease{Request,
Complete}` -- found, during this ADR's own live-verification attempt, to be a real, additional gap
this AMF cannot decode at all; not fixed here, flagged for task #101's own NGAP-coverage scope.
Rate-limiting/replay-window enforcement on the persisted NAS COUNT (a real TS 24.501 concern for
a long-lived context) -- this project's own single-registration-per-UE lab scope, same disclosed
simplification every prior NGAP/NAS stage already carries.

