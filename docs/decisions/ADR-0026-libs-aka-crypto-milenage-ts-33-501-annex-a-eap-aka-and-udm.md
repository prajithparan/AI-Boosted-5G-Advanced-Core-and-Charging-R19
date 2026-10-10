## ADR-0026: libs/aka-crypto (Milenage + TS 33.501 Annex A + EAP-AKA') and UDM's Nudm_UEAU

**Date:** 2026-08-06
**Status:** Accepted

**Context:** Phase 2 build order is NRF -> AMF -> SMF -> UDM -> UDR -> AUSF -> PCF; AUSF (next NF)
cannot do anything real without a UDM that can generate authentication vectors, and AUSF's own
Nausf_UEAuthentication/EAP-AKA' handshake needs a from-scratch MILENAGE + TS 33.501 key-derivation
implementation regardless of which NF calls it first. Rather than build that crypto inline inside
AUSF's turn (a second full NF plus a from-scratch cryptographic primitive in one turn), this turn
extracted the crypto into its own library (`libs/aka-crypto`) and used it to add Nudm_UEAU to the
already-committed UDM (ADR-0023's rejected alternative -- deferred then, done now that AUSF's turn
actually needs it) -- same precedent as UDR's turn extending nothing but adding a new NF; here it's
the reverse, one new library plus one extension to an existing NF, no second full NF. AUSF itself
remains a future turn: `TS29509_Nausf_UEAuthentication.yaml` was added to `libs/sbi-generated`'s
codegen pilot files this turn (so its generated DTOs exist and were exercised for compile-cleanliness)
but no `nfs/ausf` binary was created -- confirmed by `CMakeLists.txt` not gaining an
`add_subdirectory(nfs/ausf)` line.

**Source:** `specs/5G_APIs-REL-19/TS29503_Nudm_UEAU.yaml` (Nudm_UEAU's GenerateAuthData/ConfirmAuth/
DeleteAuth operations), commit `bca84b60a37773133bcae97e5c6c0d10a93b47b6`. The crypto itself --
MILENAGE (TS 35.205/35.206), the generic KDF (TS 33.220 Annex B.2.0), and the 5G-specific key
derivations (TS 33.501 Annex A) -- has no OpenAPI YAML representation (it's stage-2/stage-3 crypto,
not an SBI schema); implemented from the public algorithm definitions rather than any generated
DTO, per CLAUDE.md's carve-out for "where stage-3 YAML is genuinely missing... implement against
stage-2 and mark the gap explicitly." EAP-AKA' packet framing is RFC 5448 (key derivation) + RFC
4187 Section 8 (attribute TLV format), not 3GPP YAML at all -- AUSF's own future turn will wire
`libs/aka-crypto/include/aka_crypto/eap_aka_prime.hpp` to `TS29509_Nausf_UEAuthentication.yaml`'s
`EapSession`/`UEAuthenticationCtx` schemas (base64 `EapPayload` string fields).

**Scope agreed with the user before implementation:** `libs/aka-crypto` implements MILENAGE's f1
(network authentication, MAC-A) and f2/f3/f4/f5 (RES, CK, IK, AK) -- deliberately NOT f1*/f5*
(resynchronisation MAC-S/AK*), so there is no AUTS/SQN-resynchronisation support anywhere in this
build; `AuthenticationSubscriptionStore`'s SQN is a bare monotonically-incrementing 48-bit counter
with no windowing, the simplest possible SQN scheme that still produces a fresh, valid vector on
every call. On top of MILENAGE: the TS 33.501 Annex A derivations AUSF/UDM need (KAUSF, CK'/IK',
RES*/XRES*, HRES*/HXRES*, KSEAF) and RFC 5448/4187's EAP-AKA' Challenge-only packet exchange
(Request/Response/Success/Failure/Client-Error; not AKA-Identity, Notification, Reauthentication).
UDM's Nudm_UEAU scope is GenerateAuthData (5G-AKA and EAP-AKA' vector generation), ConfirmAuth, and
DeleteAuth only -- GetRgAuthData (5G-RG), GenerateAv (EPS/IMS/HSS via `TS29562_Nhss_imsUEAU.yaml`'s
merged schemas), GenerateGbaAv, and GenerateProseAV are out of this build's Tier-1 5G-AKA scope and
deferred, not dropped.

**Verified against real published vectors, not self-consistency alone:** `tests/conformance/
test_milenage.cpp` checks the from-scratch MILENAGE implementation against 3GPP TS 35.207 Test Set
1 (OPc, MAC-A, RES, CK, IK, AK all match the published values exactly), cross-checked this turn
against two independent sources (the `milenage` Rust crate's docs.rs page and the
`mitshell/CryptoMobile` Python test suite) before being trusted, per the project's fabrication-is-
the-worst-failure-mode rule -- a subtle rotation-amount or constant-byte bug in a hand-rolled MILENAGE
port would otherwise silently produce self-consistent-but-wrong keys, the worst possible failure
mode for authentication crypto (looks like it works; every derived key is wrong). The TS 33.501
Annex A FC values (0x6A/0x20/0x6B/0x6C) and each derivation's parameter list were cross-checked
against free5GC's `github.com/free5gc/util/ueauth` (Apache-2.0, consulted as reference reading only
per CLAUDE.md's build-vs-fork policy, not vendored) rather than trusted from memory alone.
`tests/conformance/test_eap_aka_prime.cpp` verifies PRF'/MK derivation determinism and key-
sensitivity, full Request/Response packet round-trips including real AT_MAC computation and
verification, and that both a tampered MAC and a wrong K_aut are correctly rejected.

**Decision:** `libs/aka-crypto` (new static library, links `OpenSSL::Crypto` for AES-128-ECB/HMAC-
SHA-256/SHA-256/RAND_bytes) with `milenage.hpp/cpp`, `kdf.hpp/cpp`, `eap_aka_prime.hpp/cpp`, and a
small `hex.hpp/cpp` helper. `nfs/udm` gains `AuthenticationSubscriptionStore` (seeded at startup
with two fixed test subscribers -- an 5G_AKA one and an EAP_AKA_PRIME one, both using the real TS
35.207 Test Set 1 K/OP/SQN/AMF values, not invented numbers, so an external test client can
independently verify UDM's output against the same published vector) and `AuthEventStore`, plus
three new routes under `/nudm-ueau/v1` (`nfs/udm/src/main.cpp`): `POST .../generate-auth-data`,
`POST .../auth-events` (ConfirmAuth), `PUT .../auth-events/{authEventId}` (DeleteAuth -- a PUT, not
a DELETE, per the YAML: confirmed by reading `TS29503_Nudm_UEAU.yaml` directly, not assumed from the
operationId's name). GenerateAuthData branches on the subscriber's stored `authentication_method` to
return either `Av5GHeAka` (RAND/XRES\*/AUTN/KAUSF) or `AvEapAkaPrime` (RAND/XRES/AUTN/CK'/IK'), both
real generated DTOs from `TS29503_Nudm_UEAU_grp.hpp`, not hand-written structs. AK/SQN⊕AK/AUTN
construction, RES\*/KAUSF/CK'/IK' derivation all real, wired end to end -- not stubbed.

**Disclosed simplification, real gap, not silently hidden:** `AuthenticationSubscriptionStore` is
the same kind of gap `docs/DECISIONS.md`'s prior ADRs already disclosed for UDM/UDR -- real
deployments provision K/OPc/SQN/AMF via UDR's authentication-data group, which UDR's own turn
(ADR-0025) explicitly deferred. Two hardcoded test subscribers, not provisionable through any API.
Real SUCI de-concealment (TS 33.501 Annex C, ECIES over the home network's public key) is NOT
implemented -- `generate-auth-data`'s `supiOrSuci` path parameter must be passed a SUPI-formatted
id; a real SUCI would 404. Disclosed in `nfs/udm/src/main.cpp`'s ProblemDetails error message
itself, not just in this ADR, so it's visible to an actual API caller, not only a reader of this
file.

**Verification:** `tests/conformance/test_milenage.cpp` and `test_eap_aka_prime.cpp` (8 new unit
tests, all against real vectors or real round-trip crypto, not placeholder assertions) plus 4 new
real subprocess-to-subprocess integration tests (`tests/integration/test_udm_ueau.cpp`, following
the same `nrf`+`udm` real-TLS-1.3+mTLS+signed-JWT pattern as every prior NF's integration tests):
GenerateAuthData against the 5G_AKA subscriber confirms two consecutive calls produce distinct
RAND/AUTN/KAUSF (proving the SQN-advance-per-call and fresh-RAND-per-call behaviour is real, not
cached), GenerateAuthData against the EAP_AKA_PRIME subscriber confirms the `AvEapAkaPrime` branch
and correctly-sized CK'/IK'/XRES fields, ConfirmAuth-then-DeleteAuth confirms the full
create/Location-header/delete/404-on-redelete lifecycle, and the 404 (unknown SUPI, with the
SUCI-not-implemented message)/401 (tampered bearer token) error paths. All 45 project tests pass
(37 pre-existing + 4 conformance + 4 integration), stable across repeated runs.

**Rejected alternative:** build AUSF and its EAP-AKA'/5G-AKA crypto together in one turn, treating
UDM's GenerateAuthData as an inline stub returning fabricated key material until AUSF needed real
values. Rejected because a fabricated authentication vector is exactly the kind of thing CLAUDE.md
calls out as the project's worst failure mode -- if UDM's stub keys were ever compared against
AUSF's real MILENAGE computation, they'd silently mismatch in a way that looks like an AUSF bug, not
a UDM shortcut, wasting a future turn's debugging time. Building the real crypto now, backed by
published test vectors, means AUSF's own turn can trust UDM's output immediately, and can focus
its scope purely on the Nausf_UEAuthentication SBI surface and the AUSF-side of the EAP-AKA'
exchange, not on re-deriving MILENAGE from scratch a second time.

**Consequence:** `libs/aka-crypto` is now a shared dependency AUSF's own turn will link against
directly (for `eap_aka_prime.hpp`'s Request/Response packet builders and `kdf.hpp`'s KSEAF
derivation) rather than re-implementing -- one crypto implementation, verified once against
published vectors, used by both NFs that need it, consistent with CLAUDE.md's "never hand-write a
DTO the YAML can generate" spirit extended to "never hand-roll a second copy of already-verified
crypto." Also surfaces the next real decision AUSF's turn will have to make explicitly: `AKAV1_HXRES`
comparison logic (SEAF forwards HRES* for the network to compare against AUSF's HXRES* before AUSF
will release KSEAF) lives partly in AMF/SEAF territory (TS 23.502 §4.2.2.2.2, N12/N8), which this
build hasn't touched yet either -- not resolved by this turn, flagged for when AUSF's own procedure
list is proposed.

---

