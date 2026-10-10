## ADR-0027: AUSF (sixth NF) -- Nausf_UEAuthentication ue-authentications group

**Date:** 2026-08-06
**Status:** Accepted

**Context:** Sixth NF in the agreed Phase 2 build order (NRF -> AMF -> SMF -> UDM -> UDR -> AUSF ->
PCF). Source: `specs/5G_APIs-REL-19/TS29509_Nausf_UEAuthentication.yaml`, commit
`bca84b60a37773133bcae97e5c6c0d10a93b47b6` (added to `libs/sbi-generated`'s codegen pilot files in
ADR-0026's turn, in preparation for this one). This is the first NF in the build whose own request
handlers make a real synchronous SBI client call to another NF's business API (UDM's
`Nudm_UEAU_GenerateAuthData`) rather than only to NRF for registration/OAuth2 -- every prior NF's
only outbound client role was `run_nrf_lifecycle`.

**Scope agreed with the user before implementation:** the `ue-authentications` resource group only
-- `POST /ue-authentications` (initiate; calls UDM), `PUT .../5g-aka-confirmation`
(Confirm5gAkaAuthentication), `DELETE .../5g-aka-confirmation` (Delete5gAkaAuthenticationResult),
`POST .../eap-session` (EapAuthMethod), `DELETE .../eap-session` (DeleteEapAuthenticationResult),
`POST .../deregister` (UEAuthenticationsDeregister). `/rg-authentications` (5G-RG) and
`/prose-authentications` + `/prose-authentications/{authCtxId}/prose-auth` (ProSe) deliberately
deferred, not dropped -- same Tier-1 5G-AKA-for-a-normal-UE boundary ADR-0026 already drew for
UDM's Nudm_UEAU turn.

**Disclosed simplification, stated up front:** AUSF does NOT call UDM's ConfirmAuth/DeleteAuth
(`Nudm_UEAuthentication_ResultConfirmation`) after an authentication completes, even though UDM's
ConfirmAuth/DeleteAuth were built in ADR-0026 anticipating exactly this caller. Wiring that up is a
real design decision of its own (sync-in-the-response-path vs. fire-and-forget, what to do if UDM
is unreachable at that point) that wasn't part of the scope agreed for this turn -- explicitly
deferred to a dedicated future turn, not silently done or silently skipped. SUCI de-concealment is
also not implemented, same disclosed gap as UDM's GenerateAuthData (ADR-0026):
`AuthenticationInfo.supiOrSuci` is passed straight through to UDM, so a real SUCI-formatted id
404s exactly like it does calling UDM directly.

**KAUSF for EAP-AKA' -- resolved with the user mid-implementation, not assumed:** the file header
comment proposed to the user before coding claimed "KAUSF = EMSK" for the EAP-AKA' path, citing TS
33.501 §6.1.3.1 from memory. Implementing it exposed the claim was likely wrong before any code
was trusted: RFC 5448's EMSK is 64 bytes, but every `Kausf` field across both YAML files is 32
bytes (`pattern: '[A-Fa-f0-9]{64}'`, i.e. 64 hex chars), a dimensional mismatch that should not
exist if EMSK were really used unmodified. Per CLAUDE.md's "if unsure, say so plainly" rule, this
was raised back to the user explicitly (with the size-mismatch evidence) rather than silently
picking a reconciliation (e.g. truncating EMSK) or silently proceeding on the original unverified
claim -- neither of which had a spec citation behind it. Resolved: KAUSF for EAP-AKA' is derived
via the *same* Annex A.2 generic-KDF function already implemented for 5G-AKA
(`aka_crypto::derive_kausf`, FC=0x6A), just keyed on CK'/IK' instead of CK/IK -- dimensionally
consistent (32 bytes) and consistent with Annex A's one-KDF-family-different-key-inputs structure.
This is still not a spec citation (no TS 33.501 text was available locally to confirm either way --
`specs/` only holds the OpenAPI YAML, not the normative spec PDF), and is disclosed as the AUSF's
best-available reconstruction, not a confirmed-correct citation. SQN xor AK for this derivation is
read directly out of AUTN's own first 6 bytes (AUSF never learns SQN any other way either, same as
a real UE/USIM) -- see `nfs/ausf/src/main.cpp`'s EAP-AKA' branch and `nfs/ausf/src/stores.hpp`.

**Real bug found and fixed in already-committed `libs/aka-crypto` code (ADR-0026's turn), not new
code from this turn:** `tests/integration/test_ausf_ue_authentication.cpp` -- which plays the
UE/USIM role, independently re-deriving every key from the same TS 35.207 (K, OP) values
`nfs/udm/src/main.cpp` seeds its test subscribers with, then cross-checks AUSF's output against
that independent computation -- caught `aka_crypto::eap::derive_keys()` producing a *different*
K_aut in AUSF's process than in the test's own process, given byte-identical (CK', IK', identity)
inputs (confirmed identical via temporary debug instrumentation on both sides before concluding
this). Root cause: `libs/aka-crypto/src/eap_aka_prime.cpp`'s seed construction was
`std::vector<uint8_t> seed(std::string("EAP-AKA'").begin(), std::string("EAP-AKA'").end());` --
this evaluates `std::string("EAP-AKA'")` **twice**, constructing two distinct temporary objects,
and takes `.begin()` from one and `.end()` from the other. Iterating a range between two unrelated
objects' iterators is undefined behavior, not "the same short string twice" -- in practice this
read whatever stack memory happened to sit between the two (short-string-optimized) temporaries,
producing a seed with the right prefix and suffix but ~24-52 bytes of garbage silently spliced into
the middle, varying by process/stack layout. Every existing unit test in `test_eap_aka_prime.cpp`
missed this because each one only ever called `derive_keys` once and checked self-consistency
(non-zero, keys differ from each other) -- never checked it was *deterministic* for identical
inputs, which is the one property that would have failed. Fixed by storing the label in a named
`static const std::string` before taking iterators from it (`libs/aka-crypto/src/eap_aka_prime.cpp`),
and a regression test added directly to close the gap:
`EapAkaPrime.DeriveKeysIsDeterministicForIdenticalInputs`
(`tests/conformance/test_eap_aka_prime.cpp`) calls `derive_keys` twice with byte-identical inputs
and asserts byte-identical output -- the buggy version would have failed this. Grepped the rest of
the codebase for the same `std::string("...").begin(), std::string("...").end()` two-temporaries
pattern; no other occurrences found.

**Decision:** implemented as `nfs/ausf` (port 7782, metrics 9469, `kApiRoot = "/nausf-auth/v1"`),
following the same structural pattern as every prior NF: NRF registration/heartbeat on a background
thread (`run_nrf_lifecycle`), OAuth2 JWT verification against NRF's fixed `kNrfInstanceId`
(ADR-0018), TLS 1.3 + mTLS via the shared lab PKI (new `certs/ausf/` leaf cert added via
`scripts/gen-lab-pki.sh ausf`), Prometheus metrics, ProblemDetails error responses per TS 29.500.
A second, separate `http2::Client`/`OAuth2Client` pair (distinct from `run_nrf_lifecycle`'s) is used
from route handlers to call UDM -- safe without a mutex because every route handler runs on the
server's single `io_context` thread, never concurrently with each other or with the lifecycle
thread's own separate client instance. `ausf::AuthContextStore` (`nfs/ausf/src/stores.hpp`) holds
per-`authCtxId` state (XRES*/KAUSF for 5G-AKA; K_aut/XRES/KAUSF/MSK for EAP-AKA') between
`POST /ue-authentications` and the later confirmation calls, in-memory only, no expiry -- same
disclosed simplification as every other NF's store so far. UDM's 404 (unknown SUPI) is mapped to
AUSF's own documented 404 ("User does not exist in the HPLMN", the exact YAML response wording, not
paraphrased); UDM unreachable or any other non-200 maps to a 500. `PUT .../5g-aka-confirmation`
returns HTTP 200 for both a matching and a mismatching RES* (the spec documents one response code
covering both outcomes; only the body's `authResult` differs) -- confirmed by reading the YAML, not
assumed from the 5G-AKA convention of "wrong answer = different status".

**Verification:** manual `curl` end-to-end (recorded in `docs/TRACEABILITY.md`) plus 6 new real
subprocess-to-subprocess integration tests (`tests/integration/test_ausf_ue_authentication.cpp`,
spawning real `nrf`+`udm`+`ausf`, real TLS 1.3 + mTLS, real signed JWT): a full 5G-AKA lifecycle
that independently re-derives HXRES* and KSEAF from the same (K, OP) test vector and asserts
byte-for-byte equality with AUSF's own output (not just a 2xx status); a wrong-RES* case asserting
`AUTHENTICATION_FAILURE` with no KSEAF; a full EAP-AKA' lifecycle that independently verifies
AUSF's Request/Challenge AT_MAC, builds a real Response/Challenge packet, and cross-checks the
returned KSEAF/MSK; a wrong-RES EAP-AKA' case asserting a real EAP-Failure packet comes back; a
deregister-then-404-on-second-deregister case; and the 404 (unknown SUPI)/401 (tampered token)
error paths. Plus the crypto library's own regression test (`EapAkaPrime.
DeriveKeysIsDeterministicForIdenticalInputs`) added alongside the bug fix. All 52 project tests
pass, stable across repeated runs (verified twice in a row). Docker image built and verified in
this environment (`docker build -f deploy/docker/ausf.Dockerfile -t 5gc-ausf:test .`); `deploy/
docker/docker-compose.yml` gained an `ausf` service (depends on both `nrf` and `udm` being started,
the first NF whose compose dependency isn't just `nrf`); `deploy/helm/ausf/` mirrors `deploy/helm/
udr/`, including the same disclosed cross-chart shared-PKI gap. `docker compose up` (all six
containers actually mTLS-registering and AUSF actually calling UDM) was NOT run this session --
same disclosed-not-silently-assumed gap ADR-0014 already recorded for NRF alone; `helm install`/
`helm template` also not run, same as every prior chart.

**Rejected alternative:** reconcile the KAUSF/EMSK size mismatch by truncating or hashing EMSK down
to 32 bytes to keep the originally-proposed "KAUSF = EMSK" claim technically satisfiable. Rejected
because that reconciliation step itself would have been invented with no spec citation -- exactly
the fabrication CLAUDE.md's guardrails exist to prevent, just moved one level down (inventing *how*
to make EMSK fit, instead of inventing whether EMSK applies at all).

**Consequence:** the derive_keys bug fix changes EAP-AKA' key output for every caller (there was
only ever one call site before this turn added the second) -- no behavioral compatibility concern
since nothing depended on the old, wrong, non-deterministic values. This turn's cross-process
integration-test pattern (independently re-deriving keys in the test client rather than only
round-tripping the NF's own numbers) found a real bug that three separate unit tests in the prior
turn did not; worth keeping as the default verification bar for any future crypto-adjacent NF work,
not just a one-off for this turn.

---

