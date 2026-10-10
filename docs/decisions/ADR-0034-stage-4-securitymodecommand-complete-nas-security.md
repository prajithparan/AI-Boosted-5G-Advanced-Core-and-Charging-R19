## ADR-0034: Stage 4 (SecurityModeCommand/Complete -- NAS security activation) and new 128-NEA2/128-NIA2 primitives

**Date:** 2026-08-07
**Status:** Accepted

**Context:** Stage 4 activates real NAS security: AMF derives KNASenc/KNASint from KAMF (TS 33.501
Annex A.8), sends a `SecurityModeCommand` (integrity-protected only, per TS 24.501 -- never
ciphered, since the UE cannot yet be assumed to trust a brand-new KNASenc), and verifies the UE's
`SecurityModeComplete` (integrity-protected **and** ciphered, proving both directions work). This
project's only implemented algorithm pair is 128-NEA2 (AES-128-CTR) / 128-NIA2 (AES-128-CMAC) --
128-EEA0/"null" and the SNOW-3G/ZUC-based NEA1/NIA1/NEA3/NIA3 families are out of scope, a
disclosed simplification matching UERANSIM's own default algorithm selection, not an attempt at
full algorithm-agility.

**New code:** `aka_crypto::derive_knas_enc`/`derive_knas_int` (`libs/aka-crypto/src/kdf.cpp`,
FC=0x69) -- same reconstruction-not-citation disclosure pattern as every other Annex A derivation
in this file, cross-checked against UERANSIM's `DeriveNasKeys`. New
`aka_crypto::nea2_apply`/`nia2_mac` (`libs/aka-crypto/src/nas_security.cpp`, new file) implement
128-NEA2/128-NIA2 directly against OpenSSL's `EVP_aes_128_ctr`/`EVP_MAC` (CMAC) APIs, input formats
(the shared COUNT/BEARER/DIRECTION prefix, padded differently for each algorithm) reconstructed
from `simulators/ransim/vendor/UERANSIM/src/lib/crypt/eea2.cpp`/`eia2.cpp`.
`amf::nas::encode_security_mode_command`/`decode_security_mode_complete`
(`nfs/amf/src/nas_codec.{hpp,cpp}`) build/verify the secured NAS envelope, including a new
`extract_uplink_nas_pdu`/`send_downlink_nas_transport` factoring in `ngap_task.cpp` (previously
duplicated inline across Stage 2/3's own handlers).

**Real interop was blocked by ADR-0033's SQN gap before ever reaching this stage** -- a real run
confirmed the exact same `AuthenticationFailure` outcome, never advancing to `SecurityModeCommand`.
Given this stage's crypto (128-NEA2/128-NIA2) is entirely new, hand-rolled code with no other proof
point, self-consistency-only unit tests were judged insufficient (per this project's own prior
lesson -- a real UB bug in `derive_keys()` shipped past three self-consistency tests before an
independent-re-derivation test caught it, see ADR-0027). **Verification: a standalone scratch
harness** (not committed -- would require linking UERANSIM's vendored `crypt/` sources into the
permanent build, out of proportion to what it's for) compiled UERANSIM's real `eea2.cpp`/`eia2.cpp`
directly and cross-checked `nea2_apply`/`nia2_mac` against them: 20 random trials each of
key/count/bearer/direction/message, **40/40 byte-exact matches, 0 failures**. 17 new/updated unit
tests (`tests/conformance/test_nas_security.cpp`, `test_nas_codec.cpp`) cover determinism,
round-trip, and the SMC/Complete envelope's own MAC-verify/tamper/reject paths.

**Consequence:** Stage 4 complete, 75 tests total. `encode_security_mode_command`/
`decode_security_mode_complete` were refactored onto two new shared low-level helpers
(`encode_secured_downlink`/`decode_secured_uplink`) once Stage 5 needed the identical
envelope-building logic a third time (see ADR-0035) -- confirmed zero behavior change by rerunning
this stage's own unit tests unmodified after the refactor.

