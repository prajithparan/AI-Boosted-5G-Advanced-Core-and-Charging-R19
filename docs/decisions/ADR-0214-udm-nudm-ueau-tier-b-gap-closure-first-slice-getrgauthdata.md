## ADR-0214: UDM `Nudm_UEAU` Tier-B gap-closure -- first slice (`GetRgAuthData`, `GenerateAv`, `GenerateGbaAv`)

### Context

UDM is now the only NF with a real, known Tier-B gap project-wide (~58+ undisclosed operations
across `Nudm_UECM`/`Nudm_SDM`/`UEAU`/`PP`, per `docs/CAPABILITY_GAP_ANALYSIS.md`, itself only a
rough count with no per-operation detail until this pass). A background audit (fork,
grep/read evidence against `nfs/udm/src/main.cpp`) produced the first real, per-operation
breakdown: `Nudm_UEAU` has exactly 3 missing operations (smallest/clearest, confirmed already
routed: `GenerateAuthData`, `GenerateProseAV`, `ConfirmAuth`, `DeleteAuth`), `Nudm_UECM` has ~24,
`Nudm_SDM` has ~33 (mostly per-SUPI GETs that likely need the existing UDM->UDR proxy pattern
confirmed first, not a UDM-local store), and `Nudm_PP` has the 11 already flagged in ADR-0082 as
deferred (5g-vn-groups/pp-data-store/mbs-group-membership -- unchanged, not new). Per this
project's own established "smallest/clearest first" prioritization, this ADR closes all 3
`Nudm_UEAU` operations, confirmed by direct read of `specs/5G_APIs-REL-19/TS29503_Nudm_UEAU.yaml`:

- **`GetRgAuthData`** (`GET /{supiOrSuci}/security-information-rg`): real SUCI de-concealment
  path param, required query `authenticated-ind`, response `RgAuthCtx` (`authInd` required bool,
  optional `supi`).
- **`GenerateAv`** (`POST /{supi}/hss-security-information/{hssAuthType}/generate-av`): real
  HSS-domain authentication vector generation for EPS/IMS/GBA/EAP-AKA' domains, request
  `HssAuthenticationInfoRequest`, response `HssAuthenticationInfoResult` (`HssAuthenticationVectors`
  is a real `OPAQUE FALLBACK` -- a 3-way `oneOf` array this project's own codegen can't strongly
  type, same disclosed class as other oneOf fields elsewhere in this codebase).
- **`GenerateGbaAv`** (`POST /{supi}/gba-security-information/generate-av`): real GBA-domain
  vector generation, request `GbaAuthenticationInfoRequest`, response
  `GbaAuthenticationInfoResult` (`n3gAkaAv`, real JSON key `"3gAkaAv"` -- the `n`-prefix is only
  the generated C++ member name, since C++ identifiers can't start with a digit).

### Implementation

- **`GetRgAuthData`**: real, disclosed design choice -- since the real spec's own `RgAuthCtx`
  schema doesn't specify UDM-side verification logic for this FN-RG scenario beyond the shape,
  `authInd` is backed by a new `AuthEventStore::has_successful_event(supi)` (new method, iterates
  the existing in-memory event map for a `success: true` entry) -- reusing the exact real
  `ConfirmAuth`-created `AuthEvent` data already written/removed by the already-implemented
  `ConfirmAuth`/`DeleteAuth`, not a new, separate notion of "authenticated". The caller's own
  `authenticated-ind` query param is accepted (real, required) but never echoed back unverified.
- **`GenerateAv`**: reuses the exact same `aka_crypto::f1`/`f2345` Milenage primitives + the
  `auth_subscriptions` per-subscriber SQN-advance already used by `GenerateAuthData`/
  `GenerateProseAV` -- real vectors, generated `numOfRequestedVectors` times (1-5 per spec), not
  stubs, for 4 of the 5 real `hssAuthType` branches: `eap-aka`/`ims-aka`/`gba-aka` (raw
  `AvImsGbaEapAka`, RAND/XRES/AUTN/CK/IK) and `eap-aka-prime` (`AvEapAkaPrime`, CK'/IK' via the
  already-existing `aka_crypto::derive_ck_ik_prime`, serving network name constructed from the
  request's own `servingNetworkId` PLMN per the standard TS 23.003 zero-padded
  `5G:mncXXX.mccXXX.3gppnetwork.org` format -- real transformation, not fabricated, `400` if
  `servingNetworkId` is absent since CK'/IK' derivation genuinely needs it). Real, disclosed gap:
  the `eps-aka` branch's own `AvEpsAka.kasme` needs TS 33.401 Annex A.2 KASME derivation, a
  distinct KDF `libs/aka-crypto` does not yet implement (only the 5G KAUSF/KSEAF chain exists) --
  returns the real, spec-documented `501` rather than fabricating a KASME value.
- **`GenerateGbaAv`**: only one real `GbaAuthType` value exists (`DIGEST_AKAV1_MD5`); `N3GAkaAv`'s
  own real fields (rand/xres/autn/ck/ik) are exactly the same raw Milenage output already used
  above -- a full real vector, no new primitive needed.

### Live verification (real, live mTLS + OAuth2, not self-consistency)

New `tests/integration/test_udm_ueau_gap_closure_214.cpp`, 3 tests, real spawned `nrf`+`udm`
process pairs over real TLS 1.3 + mTLS:
- `GetRgAuthDataReflectsRealConfirmedAuthState`: real `400` when `authenticated-ind` is missing,
  real `200` with `authInd: false` before any confirmed auth event, real `ConfirmAuth` POST, then
  real `200` with `authInd: true` after.
- `GenerateAvProducesRealVectorsForImsGbaEapAkaBranches`: 2 requested vectors for `ims-aka`, both
  real (32-hex RAND/AUTN, distinct RAND per vector, `avType` round-trips), plus real `501` for
  `eps-aka`.
- `GenerateGbaAvProducesRealN3GAkaVector`: real vector (correct RAND/XRES/AUTN/CK/IK hex lengths,
  matching `test_udm_ueau.cpp`'s own established length-assertion precedent), real `404` for an
  unknown subscriber.

All 3 pass. One real bug of my own caught and fixed: the test initially asserted
`got.contains("n3gAkaAv")`, but the generated `to_json` serializes the real schema field name
`"3gAkaAv"` (the `n`-prefix is a C++-identifier-only artifact) -- caught immediately by the actual
JSON response, fixed in the test, not the implementation (which was already correct).

### Testing

Full reconfigure + rebuild clean (`EXIT=0` verified directly from each build-log step). All 3 new
tests pass individually. Full suite re-run clean: 439/439 passing (`ctest --timeout 120 -E
"UdrIntegration.AmfContextLifecycle|UdmIntegration.SdmDataRetrievalAndSubscriptions"`, matching
`.github/workflows/ci.yml`'s own exclusion). No strays before or after any run (`ps aux` checked
explicitly) -- one stray `nrf`/`udm` pair from an earlier interrupted test run was found and
killed before the full-suite run.

### What this ADR does NOT include

`GenerateAv`'s `eps-aka` branch remains a real `501` pending TS 33.401 Annex A.2 KASME derivation
in `libs/aka-crypto` -- not implemented, not stubbed with a fabricated value. This closes only
`Nudm_UEAU`'s own Tier-B backlog. `Nudm_UECM` (~24 undisclosed operations), `Nudm_SDM` (~33,
likely needing the UDM->UDR proxy pattern confirmed first), and `Nudm_PP` (11, already flagged in
ADR-0082 as deferred) remain open, unchanged by this ADR -- UDM is still the only NF with a real
Tier-B gap project-wide.

