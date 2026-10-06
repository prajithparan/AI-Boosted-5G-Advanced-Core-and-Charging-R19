## ADR-0012: jwt-cpp + ES256 for NRF's OAuth2 token issuance/verification

**Date:** 2026-08-05
**Status:** Accepted

**Context:** `TS29510_Nnrf_AccessToken.yaml`'s `AccessTokenClaims` schema (lines 361-406) requires
`iss`, `sub`, `aud`, `scope`, `exp` -- real signed JWTs, not stub-nrf's Phase 0 unsigned placeholder
(ADR-0009 already flagged that as needing to close). Needed: a JWS/JWT library.

**Decision:** `jwt-cpp` (header-only, MIT, vcpkg-available, uses OpenSSL under the hood -- no new
crypto backend). Algorithm: ES256 (ECDSA P-256), not RS256. The lab PKI
(`scripts/gen-lab-pki.sh`) already generates P-256 keys for every NF's mTLS cert, so ES256 keeps
the codebase to one curve/algorithm family instead of two; EC keys are also smaller and faster to
verify than RSA at an equivalent security level. NRF's JWT signing keypair
(`certs/nrf-jwt/{private,public}.pem`) is generated separately from any NF's mTLS transport
cert -- reusing a transport key for JOSE signing mixes usages that real PKI practice (and
TS 33.501) keeps apart.

**Verification (not just chosen, checked to work):** a standalone round-trip test
(issue -> verify valid, issue -> tamper one byte -> verify rejects with "invalid signature", issue
-> verify against a Verifier expecting a different issuer -> rejects with "claim value does not
match expected value") passed before this was wired into NRF's request handlers. Re-verified live
against the running NRF over the real TLS+mTLS transport: a request with `Authorization: Bearer
<token>tampered` gets a real 401 with `ProblemDetails{"detail":"invalid signature", ...}`, not a
silent accept.

**Rejected alternative:** RS256. No functional reason against it, but see above -- would have
added a second key type/algorithm family to the codebase for no benefit over ES256 in a lab
context with no interop requirement forcing RSA specifically.

**Consequence:** `sbi_core::jwt::Verifier` lives in `libs/sbi-core` (not `nfs/nrf`), specifically
because every future NF needs to verify incoming bearer tokens NRF issued, not just NRF itself --
NRF only needs the `Issuer` side.

---

