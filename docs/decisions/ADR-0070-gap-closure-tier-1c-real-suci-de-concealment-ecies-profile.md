## ADR-0070: gap-closure Tier 1c -- real SUCI de-concealment (ECIES Profile A/B, TS 33.501 Annex C)

### Context

The free5GC/open5gs comparison's single most significant security finding: both real references
genuinely decrypt SUCI to recover SUPI; this project's UDM/AUSF previously passed `supiOrSuci`
straight through untouched, so only SUPI-formatted test input ever worked, never a real UE's real
SUCI. This project has no local TS 33.501 copy at the time the gap-closure sequence reached this
item; a research fork could only reach secondary sources (a NIST white paper, an Ericsson-authored
academic paper, a GitHub README) and found no official test vectors -- a real, disclosed shortfall
against this project's own "cross-process independent re-derivation" crypto-verification
discipline (the same rigor Milenage was held to, ADR-0026). Rather than implement security-critical
crypto from secondary-source characterization, the user was asked and chose to supply the real
spec directly (same pattern as TAP-SPEC.pdf earlier); `specs/TS_33_501.pdf` (ETSI TS 133 501
V19.6.0, 2026-04, ETSI/3GPP Release 19) was supplied and used as the primary source for everything
below.

### Real algorithm (TS_33_501.pdf Annex C.3/C.3.4, read directly, not recalled)

ECIES with two real, standardized profiles:
- **Profile A**: Curve25519/X25519, no point compression (ephemeral public key is 32 raw octets).
- **Profile B**: secp256r1 (NIST P-256), always point-compressed (ephemeral public key is 33
  octets: a 0x02/0x03 prefix + 32-octet X coordinate). Uses the Elliptic Curve Cofactor
  Diffie-Hellman primitive, but the spec's own text (C.3.4.0) confirms this equals plain ECDH for
  any curve with cofactor h=1 -- secp256r1's own real cofactor -- so both profiles reduce to the
  same plain-ECDH shared-secret computation in practice.
- **KDF**: ANSI-X9.63-KDF with SHA-256; SharedInfo1 = the ephemeral public key octet string
  exactly as transmitted; SharedInfo2 = the empty string. Output keying material: 64 octets, split
  EK (leftmost 16, AES-128 key) || ICB (middle 16, AES-CTR initial counter block) || MK (rightmost
  32, HMAC key).
- **ENC**: AES-128 in CTR mode. **MAC**: HMAC-SHA-256, truncated to the leftmost 8 octets (64
  bits). **Wire format** (Scheme Output): ephemeral public key || ciphertext || MAC-tag.
- Real MSIN encoding for an IMSI-based SUPI (C.3.1): packed BCD, first digit in the low nibble,
  0xF filler in the high nibble of the final octet if the digit count is odd -- confirmed
  identical to this project's own already-existing `libs/tbcd-core` codec (TS 23.003 clause 2.2),
  reused directly rather than duplicated.

### Real, independent verification (matching the Milenage precedent's own rigor)

Before writing any library code, both profiles' full decrypt pipeline (ECDH -> KDF -> AES-CTR
decrypt -> HMAC verify) was implemented in a standalone throwaway program and run against all four
of the spec's own real, officially-published implementers' test vectors (Annex C.4.3.1/C.4.3.2
Profile A IMSI/NAI, C.4.4.1/C.4.4.2 Profile B IMSI/NAI) -- every one byte-for-byte matched the
spec's own published Plaintext block and MAC-tag value. Only after this independent confirmation
was the real library code (`libs/aka-crypto/suci.{hpp,cpp}`) written, and `tests/conformance/
test_suci.cpp` commits all four real test vectors as permanent regression tests (plus two real,
deliberate negative tests: tampered MAC, too-short input -- both must fail closed).

### Real, disclosed engineering choice: modern vs. deprecated OpenSSL API

Profile A uses OpenSSL 3.x's modern `EVP_PKEY_new_raw_private_key`/`raw_public_key` (X25519,
correct, non-deprecated). Profile B's P-256 path needed the classic `EC_KEY`/`EC_POINT`/
`ECDH_compute_key` API: the modern `EVP_PKEY_fromdata` interface for constructing an EC `EVP_PKEY`
from a raw private-key octet string alone (no public point supplied) was tried first and does not
succeed against this project's own installed OpenSSL 3.6.3 (confirmed by direct testing). The
classic API is deprecated since OpenSSL 3.0 but remains fully functional and is proven correct
against the real C.4.4.1/C.4.4.2 test vectors; the deprecation warnings are suppressed with a
scoped `#pragma GCC diagnostic ignored "-Wdeprecated-declarations"`, not silently left as
accumulating warning-noise.

### Real UDM/AUSF wiring

`nfs/udm/src/main.cpp`'s `GenerateAuthData` handler now parses `supiOrSuci`: if it isn't
`suci-`-prefixed, it's passed through unchanged (already-correct existing behavior for a real
SUPI). If it is, the real SUCI text format is parsed -- confirmed directly from this repo's own
vendored `specs/5G_APIs-REL-19/TS29571_CommonData.yaml`'s `SupiOrSuci` schema pattern (not
guessed): `suci-<supiType>-<mcc>-<mnc>-<routingIndicator>-<protectionScheme>-
<homeNetworkPublicKeyId>-<schemeOutput>` for the real IMSI-type (`supiType` digit `"0"`) form.
Real, disclosed scope narrowing: only this IMSI-type form is parsed -- the same real YAML
pattern's alternative form for `supiType` 1-7 (NAI/GCI/GLI-based SUCI) uses a free-form realm/
identifier segment that can itself contain `-`, making a simple `-`-split ambiguous; left
unsupported rather than guessed. `protectionScheme` `"0"` (null-scheme, real passthrough per
C.2), `"1"` (Profile A), `"2"` (Profile B) are all handled; a real, deliberate 400 `ProblemDetails`
on any parse/MAC failure, never a silent fallback.

**Real, disclosed lab key material**: the real Home Network private keys used are the SAME
real, officially-published TS 33.501 Annex C.4.3.1/C.4.4.1 test key pairs `test_suci.cpp`
independently verifies against -- reused as this lab's own Home Network key material, matching
this project's own existing precedent (`AuthenticationSubscriptionStore`'s seeded test subscribers
already reuse TS 35.207 Test Set 1's public K/OPc). A real production deployment needs a real,
non-public Home Network key pair and a real provisioning path for it; neither exists in this lab,
disclosed, not silently implied to be production-grade.

`nfs/ausf/src/main.cpp`'s own passthrough of `AuthenticationInfo.supiOrSuci` to UDM is unchanged
and remains correct -- TS 33.501 clause 6.12.5 names UDM as the real home of the Subscription
Identifier De-concealing Function (SIDF), so AUSF needed no code change, only its stale file-header
comment (claiming SUCI de-concealment was unimplemented) corrected.

### Real end-to-end verification

Built a real SUCI string from the verified Profile A IMSI test vector
(`suci-0-274-012-0000-1-1-<scheme output>`) and `POST`ed it to a real running UDM's
`generate-auth-data` over real mTLS HTTP/2: UDM correctly recovered `imsi-274012001002086` (exact
match: MCC=274, MNC=012, MSIN=001002086) and returned a real 404 only because that SUPI isn't a
seeded lab subscriber -- proving the full HTTP-layer de-concealment path, not just the isolated
library test. A tampered MAC-tag on the same SUCI correctly returned a real 400. The pre-existing
plain-SUPI passthrough (`imsi-999700000000001`) was re-verified unchanged: real 200 with a real
auth vector.

`clang-format-18 --dry-run --Werror` clean. Full rebuild + `ctest`: 281/281 pass, zero regressions.

### What this ADR does NOT include

NAI/GCI/GLI-based SUCI (`supiType` 1-7) parsing (real, disclosed, deferred -- see above). AUSF
still doesn't call UDM's `ConfirmAuth`/`DeleteAuth` (a real, separate, already-disclosed gap from
ADR-0026, unrelated to this ADR). No real Home Network key provisioning path -- the key material
is real spec test data reused as lab-only key material, not a production secret or a production
provisioning mechanism.

