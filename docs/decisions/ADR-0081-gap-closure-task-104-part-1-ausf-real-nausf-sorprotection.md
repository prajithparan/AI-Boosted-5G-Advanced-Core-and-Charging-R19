## ADR-0081: gap-closure task #104 (part 1) -- AUSF real Nausf_SoRProtection (SoR-MAC-IAUSF/IUE)

### Context

`docs/CAPABILITY_GAP_ANALYSIS.md`'s AUSF section named `Nausf_SoRProtection` as a real gap
(free5GC-only, `internal/sbi/api_sorprotection.go`) -- protects Steering-of-Roaming (SoR) list/
CMCI data against tampering by a compromised VPLMN, TS 33.501 clause 6.14.2. Implementing it for
real required a cryptographic MAC derivation (SoR-MAC-IAUSF) this project did not have spec
material for at the start of this pass -- a genuine blocker, not a minor gap, since fabricating a
security-critical MAC's FC value or parameter construction would be a real, serious integrity
failure, not a cosmetic one.

### Real, verified spec material -- corrected clause numbering, confirmed against a primary source

The user initially referenced "Annex E.2" and supplied a research document
(`specs/MAC-AUSF.TXT`) locating the real derivation at **Annex A.17 (SoR-MAC-IAUSF) / A.18
(SoR-MAC-IUE, SoR-XMAC-IUE)** instead -- that document itself flagged the clause-number
correction and cited a third-party clause browser for the FC table, not the primary spec text.
Per this project's own crypto-verification discipline (cross-process independent re-derivation
catches bugs self-consistency tests miss -- the same discipline already applied to every other
AKA/EAP-AKA' derivation in `libs/aka-crypto`), this was independently verified against a real
local copy of **3GPP TS 33.501 v19.6.0 (Release 19 -- this project's own target release)**, found
at `/home/mastermind/Downloads/TS_33_501.pdf`, before any code was written: Annex A.17 (page 242),
Annex A.18 (page 243), and clause 6.14.2.3's own CounterSoR state-machine text (pages 122-123).
Every claim in the supplied research document -- FC=0x77/0x78, the exact P0/P1/P2 parameter
construction, the 128-LSB truncation, and the CounterSoR initial-value/reset/wrap-around rules --
matched the primary spec text exactly. (Bonus, found in the same PDF pass: Annex A.19/A.20 give
the real FC values for `Nausf_UPUProtection`'s own UPU-MAC-IAUSF/IUE, FC=0x7B/0x7C -- not built
this pass, since `Nausf_UPUProtection` wasn't part of task #104's original scope, but now
independently confirmed and ready for a future turn without needing to re-derive it.)

### Real crypto: `libs/aka-crypto`, reusing the existing generic KDF

`derive_sor_mac_iausf`/`derive_sor_mac_iue` (`kdf.hpp`/`kdf.cpp`) reuse the SAME `generic_kdf`
primitive every other Annex A derivation in this codebase already uses (KAUSF/KSEAF/KAMF/
KNASenc/KNASint) -- no new crypto machinery, just the real, spec-correct FC value and parameter
list for this derivation. Unlike A.7/A.8's own existing header comment (which discloses those two
were a *reconstruction* cross-checked against UERANSIM source, not a direct spec citation, since
this project had no local TS 33.501 copy at the time), A.17/A.18 here are a real, direct citation
against the primary text -- disclosed as a meaningfully stronger verification basis than A.7/A.8's
own, not silently presented as equivalent.

Verified via `tests/conformance/test_sor_mac.cpp` (7 new tests, all pass): determinism, a direct
structural cross-check against `generic_kdf` called independently with the same real FC/params
(not just testing the function against itself), sensitivity to each real KDF input (header,
counter, presence/absence of the optional P2 steering list), and that SoR-MAC-IAUSF/SoR-MAC-IUE
never collide even under the same CounterSoR value (different FC, different P0). 3GPP does not
publish official test vectors for these two derivations (confirmed while researching this gap --
unlike MILENAGE, which `test_milenage.cpp` verifies against a real published TS 35.207 vector),
so this self-consistency-plus-structural-cross-check bar is the real, honestly-disclosed
verification ceiling available here, not a corner cut.

### Real, new architectural prerequisite: persistent per-SUPI KAUSF (same shape as ADR-0076, different NF)

Every KAUSF this project computed before this pass lived only in the short-lived, per-in-flight-
authentication `AuthContextStore`, discarded once the 5G-AKA-confirmation/eap-session exchange
completed. `Nausf_SoRProtection` is invoked LATER, by UDM, well after authentication finishes (TS
33.501 clause 6.14.2, step 8-9: "The UDM shall select the AUSF that holds the latest KAUSF of the
UE") -- the same "a root key must outlive the request that produced it" architectural gap AMF's
own `UeSecurityContextStore` closed for `ServiceRequest` (ADR-0076), now closed for AUSF via a new
`KausfStore` (`nfs/ausf/src/kausf_store.hpp/.cpp`, Redis-backed, keyed by SUPI, storing KAUSF +
CounterSoR + a suspended flag). Hooked into both existing real success paths (5G-AKA confirmation,
EAP-AKA' success) -- TS 33.501's own "when the newly derived KAUSF is stored" trigger.
`use_counter` implements the real CounterSoR state machine atomically (Redis `HINCRBY`): the value
handed out for computation N is the value stored before increment (0x0001 for the first real use,
matching the spec's own AUSF-side initial value exactly, distinct from the UE's own 0x0000 initial
value); 0xFFFF is the real last usable value, and the call that hands it out marks the context
suspended for every call after, until a fresh KAUSF resets it -- the real, spec-mandated
wrap-around protection, not a simplification of it.

Also the first new DB dependency added since ADR-0077 (the standing "no hardcoded config in
source" decision): `nfs/ausf/src/main.cpp` retrofitted onto `libs/nf-config`/`config/ausf.json` in
the same pass, alongside the new Redis dependency -- `port`, `metrics_bind_address`,
`nrf_base_url`, `udm_base_url`, `redis_url` all moved out of in-source literals, matching AMF's own
ADR-0077 precedent exactly.

### Real, disclosed scope narrowing (not fabrication)

The real request schema (`SorInfo`, `TS29509_Nausf_SoRProtection.yaml`) lets the caller either
supply the already-encoded SOR header directly, or (per clause 6.14.2 NOTE 2) omit it and let the
AUSF construct it itself from the ACK indication and steering list, per TS 24.501 §9.11.3.51's own
NAS-layer bit encoding -- a DIFFERENT spec section this project doesn't have in hand. Only the
"received from requester" branch is implemented; a request without `sorHeader` is rejected (400)
rather than the AUSF fabricating a header encoding it doesn't actually know. Same real reasoning
for the optional P2 (Steering Info List) parameter: the real `steeringContainer` field is a
`oneOf` of a `SecuredPacket` (opaque base64 bytes, used directly as P2 -- no NAS encoding needed)
or a structured array of `SteeringInfo` objects (would need the SAME TS 24.501 §9.11.3.51 encoding
to turn into P2's own real "octets beyond octet 22" bytes) -- only the `SecuredPacket` form is
supported for real byte-exact P2 inclusion; the structured form logs a real, disclosed warning and
omits P2 (falls back to the real, spec-permitted 2-parameter MAC variant) rather than guessing an
encoding. `SoR-XMAC-IUE` is computed and returned per spec when `ackInd=true`, but no later real
endpoint exists yet in this lab to verify a UE's own returned `SoR-MAC-IUE` against it -- computed,
not yet consumed downstream, same shape as other real-but-not-fully-wired values elsewhere in this
project.

### Real codegen bug found and fixed while wiring this (schema-name collision)

`SorInfo` is defined twice in the real R19 YAML under the same bare name -- once in
`TS29503_Nudm_SDM.yaml` (a UDM subscription-data object: `sorMacIausf`/`provisioningTime`/
`sorCmci`/...) and once in `TS29509_Nausf_SoRProtection.yaml` (this ADR's own real request body:
`steeringContainer`/`ackInd`/`sorHeader`/...) -- two genuinely different schemas that happen to
share a name, the same real collision class ADR-0017 already found and fixed for
`SubscriptionData`/`NFProfile`/etc. Initially appeared broken (only `TS29503_Nudm_SDM.yaml`'s
`SorInfo` was being generated, with none of the real SoR-protection-specific fields) -- root-caused
via a direct Python repro of `tools/sbi-codegen`'s own `Converter`, which confirmed the
disambiguation logic (`_disambiguate`, ADR-0017) works correctly and was never the bug: `TS29509_
Nausf_SoRProtection.yaml` simply hadn't been added to `libs/sbi-generated/CMakeLists.txt`'s own
pilot-file list yet (its types were only reachable transitively, as dependencies of other pilot
files, which the codegen's own `convert_files` deliberately does NOT enqueue as standalone
top-level types -- correct behavior, not a bug). Added to the pilot list; regeneration then
correctly produced `SorInfo_Nausf_SoRProtection`/`SorInfo_Nudm_SDM` as two distinct, disambiguated
types. A real, if ultimately negative, finding -- confirmed rather than assumed, consistent with
this project's own "verify, don't guess" discipline extending to its own tooling, not just 3GPP
crypto.

### Verification -- live, cross-process, not just unit tests

Real live HTTP verification, deliberately using a SEPARATE AUSF process from the one that
performed the original authentication (proving genuine cross-process persistence, the same bar
AMF's own `UeSecurityContextStore` was held to in ADR-0076): ran the real `AusfIntegration.
FiveGAkaSuccessfulAuthenticationCrossChecksHxresAndKseaf` test (real MILENAGE-derived RES*,
real UDM round trip), confirmed via direct Redis inspection (`ausf:sorctx:imsi-999700000000001`)
that a real 64-hex-char KAUSF and `counter_sor=1` were persisted; started a brand-new AUSF process
against the same Redis and called `POST /nausf-sorprotection/v1/imsi-999700000000001/ue-sor`
directly: first call → 200, `counterSor=0001`, real `sorXmacIue` present (ackInd=true); second call
→ `counterSor=0002` (confirmed monotonic increment via Redis `HGETALL` between calls); a
`SecuredPacket`-form `steeringContainer` → 200, different MAC (P2 inclusion verified indirectly by
the MAC changing); a structured-array `steeringContainer` → 200 with the real disclosed warning
logged; missing `sorHeader` → 400; unknown SUPI → 404. Full `conformance_tests`: 268/268 pass (up
from 261, the 7 new `SorMac.*` tests). All 6 pre-existing `AusfIntegration.*` tests still pass
unchanged with the new hard Redis dependency.

### What this ADR does NOT include

ProSe authentication (task #104's other named half) -- turns out to need its OWN, separate
cryptographic derivation (`KNR_ProSe`, TS 33.503, a different spec document from TS 33.501) this
project also doesn't have material for; not addressed in this ADR, tracked as its own remaining
scope. `Nausf_UPUProtection` -- a related, real AUSF service found while researching this gap
(same Annex A family, FC=0x7B/0x7C now independently confirmed), not part of task #104's original
scope, not built here, flagged for a future turn. AUSF-side SOR header construction (TS 24.501
§9.11.3.51) and the structured-`SteeringInfo`-array form of P2 -- both real, disclosed scope
narrowings above, not silently dropped. Real UE-side `SoR-MAC-IUE` verification against the
cached `SoR-XMAC-IUE` -- no caller/trigger exists in this lab yet.

