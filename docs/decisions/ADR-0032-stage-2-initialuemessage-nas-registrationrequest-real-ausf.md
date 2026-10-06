## ADR-0032: Stage 2 (InitialUEMessage -> NAS RegistrationRequest -> real AUSF call -> AuthenticationRequest), three more real Aligned PER bugs, and a hand-rolled minimal NAS-5GS codec

**Date:** 2026-08-07
**Status:** Accepted

**Context:** Stage 2 of the staged NGAP/NAS plan: AMF decodes a real `InitialUEMessage` (containing
a NAS-PDU with a `RegistrationRequest`), extracts the SUPI from the null-protection-scheme SUCI in
the 5GS Mobile Identity IE, calls real AUSF's `POST /nausf-auth/v1/ue-authentications` with it,
gets back a 5G-AKA vector, encodes a NAS `AuthenticationRequest` (RAND/AUTN), wraps it in
`DownlinkNASTransport`, and sends it to the gNB. Grounding for the real NGAP IE names/codes, the
real NAS-5GS wire format, and AUSF's real SBI schema was gathered via a research pass (real
`specs/NGAP/ngap-17.9.asn` IE definitions; `simulators/ransim/vendor/UERANSIM/src/lib/nas` read
as a reference oracle per ADR-0016/ADR-0031's arms-length policy; `nfs/ausf/src/main.cpp`'s
already-implemented `/ue-authentications` endpoint) before any code was written.

**New library: `nfs/amf/src/nas_codec.{hpp,cpp}`, a minimal hand-rolled NAS-5GS (TS 24.501)
codec.** Unlike NGAP, NAS-5GS is TLV-encoded, not ASN.1 -- there is no `asn1c`-equivalent codegen
tool to generate this from, so hand-writing it is not the "hand-rolled partial parsing" ADR-0031
explicitly rejected for NGAP (that concern was about skipping a *generatable* codec; no such thing
exists for NAS-5GS in any real 5GC implementation either). Scope is deliberately narrow --
`decode_registration_request` extracts only the SUPI from a null-protection-scheme SUCI (returns
`std::nullopt`, not a guess, for anything else: ciphered NAS, GUTI-based registration, a real
protection scheme, a padded 1-octet routing indicator); `encode_authentication_request` builds
exactly the fields needed (ngKSI, ABBA, RAND, AUTN). Every byte layout (header format, IE ordering,
the SUCI's exact field layout for the null scheme, RAND/AUTN's IE encodings) is cited against real
files, not memory -- see the file's own comments.

**AUSF client wiring**: `run_ngap_lifecycle` (in `nfs/amf/src/ngap_task.cpp`) now takes
`amf_instance_id`/`nrf_base` and constructs a dedicated `http2::Client`/`OAuth2Client` pair for
AUSF (scope `"nausf-auth"`, matching AUSF's own `kApiRoot`), living on the NGAP thread itself --
not shared with `run_nrf_lifecycle`'s pair or any ioc-thread client, since `http2::Client` is
synchronous/not thread-shared (same discipline ADR-0006/ADR-0027 already established, extended
here to a *third* dedicated-thread client after `run_nrf_lifecycle`'s own). SUPI is passed straight
through as `AuthenticationInfo.supiOrSuci` -- AUSF/UDM have no separate SUCI-deconcealment logic
yet (confirmed by reading `nfs/ausf/src/main.cpp`: `supiOrSuci` is forwarded verbatim into UDM's
URL path), so this is a faithful, disclosed simplification consistent with what's already built,
not a new gap introduced here.

**Real config gap found and fixed, unrelated to any of this project's own code**:
`simulators/ransim/config/ue.yaml` was missing three fields (`integrityMaxRate`, `uacAic`,
`uacAcc`) that UERANSIM's `nr-ue` refuses to start without -- this file had never actually been run
against real `nr-ue` before Stage 2 (Stage 0/1 only ran `nr-gnb`). Added with UERANSIM's own
reference `config/custom-ue.yaml` values, not invented.

**Three more real Aligned PER bugs found via the same "real gNB, real UE, real reference-decoder
oracle" methodology ADR-0031 used** -- Stage 2's real message content (a non-zero `S-NSSAI.sST`
inside `NGSetupResponse`'s `PLMNSupportList`, and a real `RAN-UE-NGAP-ID` value from a real gNB)
exercised code paths Stage 1's own testing never touched (Stage 1's BIT STRINGs were all-zero, and
no field in Stage 1's fixed test message needed a >16-bit value-field encoding):

1. **A short (<=2 octet) fixed-size OCTET/BIT STRING got wrongly octet-aligned on encode.** Found
   because `S-NSSAI.sST` (a 1-octet `OCTET STRING`, set to `1`) silently became `0` on the wire --
   `PLMNSupportList` correctly *decoded* structurally (Stage 1's own verification), but the
   gNB rejected AMF's `NGSetupResponse` for real ("Could not find a suitable AMF" -- `nr-gnb`'s own
   slice-selection logic reads `sST` and found no match), a failure mode Stage 1's decode-only
   testing couldn't have caught. Root cause: `OCTET_STRING_encode_uper`'s
   `csiz->effective_bits >= 0 && !inext` branch had a single, unconditional content-alignment call
   added after its if/else (originally written only for the `effective_bits > 0`,
   AMFName-style variable-length case) that also fired for the `effective_bits == 0` fixed-size
   case -- wrongly aligning (and thus byte-shifting) any short fixed-size field, while the
   already-correct `effective_bits > 0` case needed exactly that call. Fixed by moving the call
   inside the `else` branch only. The *decode* side never had this bug (its fixed-size branch
   returns early, structurally unable to reach the shared align call) -- confirmed via a standalone
   `S-NSSAI` encode/decode round-trip test before touching the real pipeline again.
2. **A >31-bit constrained-whole-number VALUE field got re-aligned mid-value by ADR-0031's own
   alignment fix.** Found because `RAN-UE-NGAP-ID` (`INTEGER(0..4294967295)`, exactly 32 bits, so
   *always* hits `uper_get/put_constrained_whole_number`'s pre-existing >31-bit recursive split --
   `per_get_few_bits` itself caps at 31 bits per call) failed to decode from a real gNB's
   `InitialUEMessage`. ADR-0031's `aper_align_value_get/put_nbits` call was applied *inside* the
   `nbits<=31` recursive base case, which is *also* the code path the >31-bit recursion's leftover
   fragment hits -- wrongly re-aligning partway through an already-in-progress value. Fixed by
   splitting each function into a `_raw` variant (pure recursive bit-splitting, no alignment) and a
   public wrapper that aligns exactly once, up front, using the caller's true (pre-split) `nbits`.
3. **The real X.691 rule for value fields needing MORE than 2 octets (`nbits>16`) is not a
   fixed-width encoding at all -- it's a small unaligned octet-count selector, then alignment,
   then the value in the minimum octets it actually needs.** This directly contradicted ADR-0031's
   own (wrong) assumption that all value fields round up to a fixed whole-octet width based on the
   *range*. Found the same way as everything else in this saga: a real gNB's `RAN-UE-NGAP-ID=1`
   arrived as 2 bytes (`00 01`), not the 4 bytes ADR-0031's rule predicted. Confirmed byte-for-byte,
   not guessed, by compiling a standalone encoder against
   `simulators/ransim/vendor/UERANSIM/src/asn/asn1c`'s vendored reference codec (same read-only
   oracle pattern as ADR-0031) with `ASN_EMIT_DEBUG` tracing: encoding `RAN-UE-NGAP-ID=1` (range
   32 bits, so `max_octets=ceil(32/8)=4`) produces a 2-bit selector (`ceil(log2(4))=2` bits, value
   `0` meaning "1 octet used", *not* octet-aligned itself), then 6 bits of alignment padding, then
   exactly 1 octet (`0x01`) -- `"00 01"` over the wire. Implemented in
   `uper_get/put_constrained_whole_number` as a genuinely new code path for `nbits>16`, replacing
   the wrong fixed-width assumption; the `nbits<=16` path (procedureCode, ProtocolIE-ID, both
   already verified in Stage 1) is untouched.
4. **A NAS-5GS IE encoding mistake, not an ASN.1/PER bug**: `AuthenticationParameterRand` (NAS
   `AuthenticationRequest`'s RAND field) was given a length octet like `AuthenticationParameterAutn`
   (TS 24.501's genuinely length-prefixed AUTN field). RAND is actually a **Type-3** IE (fixed
   16-octet value, no length octet at all -- confirmed against
   `simulators/ransim/vendor/UERANSIM/src/lib/nas/ie3.hpp`'s `IEAuthenticationParameterRand` base
   class, `InformationElement3`, vs. AUTN's `ie4.hpp`/`InformationElement4`). This one byte of
   drift shifted every subsequent field, and real `nr-ue` didn't just reject the message -- it threw
   an *uncaught* C++ exception (`"Bad constructed NAS message"`) and **crashed the process**,
   confirmed by compiling and running UERANSIM's own `DecodeNasMessage` directly against the exact
   bytes AMF sent as a standalone oracle test (same arms-length read-only pattern). Fixed by
   dropping the length octet for RAND only.

**Verification:** the full Stage 2 goal -- AMF (freshly rebuilt from a *clean*
`scripts/setup-asn1c.sh` run) receiving a real `InitialUEMessage` from real `nr-gnb`/`nr-ue`,
correctly extracting SUPI `imsi-999700000000001` from the null-scheme SUCI, making a real,
successful `POST /nausf-auth/v1/ue-authentications` call to real AUSF (which itself made a real
call to real UDM), and sending back a real NAS `AuthenticationRequest` -- succeeds end-to-end,
reproducibly. Real `nr-ue` logs confirm it **decoded** AMF's `AuthenticationRequest` correctly
(`"Authentication Request received"`) and extracted the real RAND/AUTN/SQN from it; it then sent a
NAS `AuthenticationFailure` with cause "SQN out of range" -- this is `nfs/udm/src/main.cpp`'s own
seeded TS 35.207 test SQN (`ff9bb4d0b607`, a large fixed value chosen as test data, not derived
from any counter) legitimately exceeding a fresh UE's initial `SQN-MS=0` by design, exactly the
real TS 33.102 synchronization-failure procedure a real UE is supposed to trigger in this
situation -- not a bug, and not this stage's concern (SQN resync, and handling whatever
`AuthenticationResponse`/`AuthenticationFailure` the UE actually sends, is Stage 3's territory).
All 58 pre-existing tests still pass unchanged. `scripts/patches/asn1c-aligned-per.patch` was
regenerated to capture bugs 1-3 above (applies cleanly against a fresh `asn1c-0.9.29` extraction,
verified).

**Rejected alternative:** treat the SQN-out-of-range `AuthenticationFailure` as a Stage 2 blocker
and implement the resync procedure now. Rejected: Stage 2's own scope (per the approved staged
plan) ends at "AMF sends `AuthenticationRequest`, UE receives it" -- the plan's Stage 3 is
specifically "Authentication Response -> AUSF confirmation -> KAMF derivation", and handling
whichever real NAS message the UE sends back (a success `AuthenticationResponse` *or* a
spec-correct `AuthenticationFailure`) is exactly that stage's job, not something to pull forward.

**Consequence:** Stage 2 is complete and empirically verified, including the crash-on-decode NAS
bug that a purely self-consistent round-trip test (encode+decode with only this project's own
codec) could never have caught -- every one of this stage's four bugs was found only because the
*other* side of the wire was a real, independent implementation. Stage 3 (Authentication Response
decode -- including a real `AuthenticationFailure` path now that one's been observed for real --
AUSF confirmation, KAMF derivation) can proceed knowing the transport and RAND/AUTN encoding are
now genuinely interoperable, not just self-consistent.

