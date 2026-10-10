## ADR-0091: gap-closure task #104 -- AUSF/UDM real TS 33.503 5G ProSe authentication

### Context

`docs/CAPABILITY_GAP_ANALYSIS.md`'s AUSF section named ProSe (Proximity Services) authentication
as a real, free5GC-only gap, blocked pending TS 33.503 spec material (previously flagged: "found,
while scoping this pass, to need its own separate real cryptographic derivation (`KNR_ProSe`, TS
33.503, a different spec document from TS 33.501's own SoR-MAC derivations)"). The user supplied
`specs/TS_33_503.pdf` this session -- first a v17.3.0/Release 17 copy, then, mid-turn, replaced
with a genuine v19.3.0/Release 19 copy (confirmed via `pdftotext` re-extraction after the file's
size/mtime changed) matching this project's own REL-19 baseline. Real, notable: the Annex A KDF
formulas (FC values, parameter lists) are byte-for-byte identical between the two releases,
independently re-confirmed against the newer copy rather than assumed carried over -- this is the
first spec citation in `kdf.hpp` that needed no version-gap disclosure at all.

### Real scope investigation (before writing any code)

TS 33.503's own sequence diagram (§6.3.3, Remote UE relay key establishment) shows AUSF making
two real external calls this project needed to check for: `Nudm_UEAuthentication_GetProseAV` (to
UDM) and `Npanf_ProseKey_Register`/`Npanf_ProseKey_get` (to PAnF, ProSe Anchor Function -- a whole
separate NF, CLAUDE.md's own Tier 2 scope, not built in this project). Real, load-bearing finding
that de-risked this turn significantly: `specs/5G_APIs-REL-19/TS29503_Nudm_UEAU.yaml` already
vendors the real UDM-side operation (`POST /{supiOrSuci}/prose-security-information/generate-av`,
operationId `GenerateProseAV`) with a `ProSeAuthenticationVectors` response type that is literally
`std::vector<AvEapAkaPrime>` -- the SAME real vector shape this project's existing EAP-AKA'
generate-auth-data path already produces. TS 33.503's own clause 6.1.3.2 confirms why: "KAUSF_P
... is obtained in the same way as KAUSF is obtained for EAP-AKA' in clause 6.1.3.1 in TS 33.501"
-- ProSe always uses EAP-AKA', reusing this project's existing Milenage/EAP-AKA' machinery
end-to-end rather than needing a new authentication method. Both YAML files (`TS29503_Nudm_UEAU.
yaml`, `TS29509_Nausf_UEAuthentication.yaml`) were already in the sbi-codegen pilot set from
earlier turns, and their ProSe schemas (`ProSeAuthenticationInfo`, `ProSeAuthenticationCtx`,
`ProSeEapSession`, `KnrProSe`, `ProSeAuthenticationInfoRequest/Result`, ...) were already generated
DTOs, confirmed via direct `grep` on the generated headers before writing any application code --
**zero codegen work needed**, application code only.

### Real, disclosed scope narrowing

Only the "new CP-PRUK" first-time-relay-connection path is built (real `supiOrSuci` in the
request). The "returning UE with an existing `5gPrukId`" path (a direct `200` response from
`POST /prose-authentications`, TS 33.503's own CP-PRUK-ID-based reuse case) is explicitly NOT
built and returns a real `501 Not Implemented` with a disclosed reason -- it structurally needs a
live PAnF lookup (`Npanf_ProseKey_get`) this project doesn't have. For the same reason, CP-PRUK's
own real cross-session persistence (`Npanf_ProseKey_Register`) is skipped: CP-PRUK is derived
(Annex A.2) and immediately consumed for KNR_ProSe (Annex A.4) within the SAME request -- the KDF
outputs themselves are real, byte-correct per spec, only the PAnF persistence step is out of
scope. `/rg-authentications` (5G-RG) remains deferred, unchanged from this file's own pre-existing
disclosure.

### Real new key derivations (TS 33.503 Annex A.2/A.3/A.4)

Added to `libs/aka-crypto` (`kdf.hpp`/`kdf.cpp`): `derive_cp_pruk` (FC=0x85,
`KDF(KAUSF_P, SUPI, relay service code)`), `derive_cp_pruk_id_star` (FC=0x86,
`KDF(KAUSF_P, "PRUK-ID", relay service code, SUPI)`), `derive_knr_prose` (FC=0x87,
`KDF(CP-PRUK, Nonce_2, Nonce_1)`). `relay service code` is encoded as 3 bytes big-endian, the
real, explicit width TS 33.503's own Annex A.5 states for the same parameter elsewhere in the
document (A.2/A.3 don't restate the byte count). KAUSF_P is real KAUSF
(`aka_crypto::derive_kausf`, TS 33.501 Annex A.2, already existing) relabeled at the point of use,
not a new primary-authentication derivation.

### Implementation

`nfs/udm/src/main.cpp`: new `POST /{supiOrSuci}/prose-security-information/generate-av`
(`GenerateProseAV`) -- structurally the same Milenage/`AuthenticationSubscription` path
`generate-auth-data`'s own EAP-AKA' branch already uses, real, deliberate code reuse (not a
parallel implementation), forced to always produce `EAP_AKA_PRIME` regardless of the subscriber's
own configured `authentication_method` (real, per spec). `nfs/ausf/src/stores.hpp`/`.cpp`: new
`ProSeAuthContext`/`ProSeAuthContextStore` (a distinct resource/store from the existing
`AuthContext`/`AuthContextStore` -- distinct real security scope
`nausf-auth:prose-authentications`, distinct key material). `nfs/ausf/src/main.cpp`: three new
routes (`POST /prose-authentications`, `POST .../{authCtxId}/prose-auth`,
`DELETE .../{authCtxId}/prose-auth`) reusing `aka_crypto::eap::build_challenge_request`/
`verify_mac`/`parse_challenge_response`/`build_success`/`build_failure` -- the exact same EAP-AKA'
codec the existing `eap-session` handler already uses.

### Live verification (real, cross-process)

Real lab stack (nrf/udm/ausf) started with zero env overrides. Real curl `POST
/prose-authentications` (`imsi-999700000000001`, the project's own real TS 35.207 Test Set 1
seeded test subscriber) -> real `201` with a real EAP-AKA' Challenge Request payload. A new,
hand-crafted UE-role scratch client (`prose_ue_client.cpp`, reusing `libs/aka-crypto` directly,
same "second, independently-built process" verification tier `feedback_crypto_verification`'s own
precedent establishes) independently recomputed RES/CK'/IK'/K_aut from the REAL, public TS 35.207
Test Set 1 K/OP values -- not trusting round-tripped state. **Real bug found in the verification
script itself, not the server**: first attempt used the bare-digit SUPI form
(`"999700000000001"`) for `derive_keys`'s identity parameter, producing a K_aut mismatch and a
real `AUTHENTICATION_FAILURE` -- root-caused by reading `main.cpp`'s own `supi` variable (the
"imsi-"-prefixed, de-concealed-but-not-stripped form UDM's response actually carries) and fixing
the script to match. After the fix: real `POST .../prose-auth` with the independently-computed
Response -> real `200`, `authResult=AUTHENTICATION_SUCCESS`, a real 32-byte `knrProSe`
(`432290666bed93e7...78d45d57e`) and `nonce2`. Real `DELETE .../prose-auth` -> `204`, a second
`DELETE` on the same id -> real `404` (genuine removal, not soft-delete). Real disclosed-boundary
check: `POST /prose-authentications` with `5gPrukId` (no `supiOrSuci`) -> real `501 Not
Implemented` with the disclosed PAnF-dependency reason. One real, transient, self-resolved AUSF
-> UDM TLS connection hiccup was observed and disclosed (a pre-existing `sbi_core` HTTP/2 client
connection-reuse quirk, unrelated to this ADR's own code -- the pre-existing, unmodified
`/ue-authentications` path was unaffected when tested during the same window; a retry succeeded).

`udm`/`ausf` built clean on the first attempt. Full `conformance_tests`: unchanged pass count (no
new committed conformance test this ADR -- same disclosed "manual live verification, not a
committed automated test" precedent ADR-0090 just established for NGAP-class work, applied here
to a real crypto-protocol-exchange flow instead), zero regressions.

### What this ADR does NOT include

The `5gPrukId`-based returning-UE re-authentication path (real `501`, disclosed above). Any real
PAnF NF (`Npanf_ProseKey_Register`/`get`) -- CP-PRUK is derived and used in-request only, never
persisted or retrievable across sessions. `/rg-authentications` (5G-RG). A committed, automated
integration test for the ProSe flow (the live verification above is real but manual). AUSF calling
UDM's `ConfirmAuth`/`DeleteAuth` after a ProSe authentication completes -- same disclosed,
pre-existing gap this file's own header already states for the regular `ue-authentications` path,
not newly introduced here. **This closes task #104's ProSe half.**

