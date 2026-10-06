## ADR-0044: Phase 4 Stage 0/1 -- CHF (Nchf_ConvergedCharging_Create), real N40 wiring, and a genuine sbi-codegen bug fix

**Date:** 2026-08-09
**Status:** Accepted.

**Context:** Phase 4 (Charging + TM Forum SID/BSS layer) begins. Scope for this turn, shown to and
approved by the user before any code was written (per CLAUDE.md's "show the procedure list, get
approval first" rule): a new `nfs/chf/` skeleton plus real `Nchf_ConvergedCharging_Create`
(`POST /chargingdata`) only, wired to SMF over N40 at PDU Session Establishment -- the same trigger
point Stage 3's real N4 Session Establishment uses (ADR-0042). Update/Release, the
`chargingNotification` callback, `Nchf_OfflineOnlyCharging`, `Nchf_SpendingLimitControl`, and the
TM Forum SID/BSS mapping layer are deliberately deferred to separate future turns -- the SID/BSS
layer specifically needs its own turn because CLAUDE.md requires `docs/CHARGING_MAPPING.md` (an
explicit 3GPP-CDR-field -> SID-entity -> TMF-API-resource table) to exist *before* any mapping
code, which this turn's pure-3GPP-side scope doesn't need yet.

**Real API surface, confirmed from the actual R19 YAML, not assumed.** None of the three CHF-
related YAML files (`TS32291_Nchf_ConvergedCharging.yaml`, `TS32291_Nchf_OfflineOnlyCharging.yaml`,
`TS29594_Nchf_SpendingLimitControl.yaml`) use an `operationId` field at all -- confirmed by
grepping all three (zero matches), a genuine property of how these particular charging specs are
authored, not a search miss. `Nchf_ConvergedCharging`'s real paths: `POST /chargingdata` (Create),
`POST /chargingdata/{ChargingDataRef}/update`, `POST /chargingdata/{ChargingDataRef}/release`, plus
a `chargingNotification` callback. This turn implements only the first.

**A genuine, pre-existing `tools/sbi-codegen` bug, found and fixed before CHF's DTOs would even
compile.** Adding `TS32291_Nchf_ConvergedCharging.yaml` to the codegen pilot set (`libs/sbi-
generated/CMakeLists.txt`) caused a real compile failure unrelated to CHF's own schema: `TS28541_
NrNrm.hpp`'s `using TaiList = std::vector<Tai>;` referenced a `Tai` type that no longer existed
under that bare name. Root cause: `schema_to_ir.py`'s `Converter._disambiguate()` correctly renames
every `ObjectType` field's `TypeRef` when a schema name collides across multiple files (real,
already-handled cases per ADR-0017) -- but `AliasType.cpp_underlying` (used for the array-of-named-
type case, e.g. `std::vector<Foo>`) was flattened to a plain string at construction time, *before*
disambiguation could know the element type's final name, and was never rewritten afterward. CHF's
YAML transitively pulled in a second/third file also defining `Tai` (`TS28623_GenericNrm.yaml`,
which `TaiList` actually `$ref`s), triggering a collision that hadn't existed in the smaller pilot
set before -- disambiguation correctly renamed every colliding `Tai` to `Tai_<file>`, but `TaiList`'s
alias string still said the now-nonexistent bare `Tai`. Fixed by giving `AliasType` a structured
`element_ref: TypeRef | None` field (populated alongside the existing flattened string in the array
branch of `_convert_one`), having `_disambiguate` rewrite that ref and regenerate `cpp_underlying`
from it when the element type gets renamed, and having `render.py`'s `_referenced_names` walk the
structured ref instead of regex-tokenizing the string. Real, reproducible, fixed at the generator
level (never hand-patch generated code) -- see `tools/sbi-codegen/sbi_codegen/{ir,schema_to_ir,
render}.py`.

**CHF (`nfs/chf/`).** New NF, port 7784, mirrors `nfs/pcf/src/main.cpp`'s skeleton shape (NRF
registration/heartbeat lifecycle, OAuth2, TLS 1.3+mTLS). `ChargingDataRefAllocator`
(`nfs/chf/src/stores.hpp`) is deliberately just a mutex-protected ID generator, not a full resource
store: Create doesn't need to read anything back this turn (unlike PCF's `AmPolicyStore`, which
backs a real `GET`), so a full store is deferred to the Update/Release turn that will actually need
one. Disclosed simplifications (stated in `nfs/chf/src/main.cpp`'s own file header too): no real
rating/quota engine (`multipleUnitInformation` is never populated -- schema-valid, not a real
charging decision, same category of gap as PCF's fixed-default policy, ADR-0028);
`invocationSequenceNumber` in the response echoes the request's value rather than assigning an
independent CHF-side sequence, because the YAML carries no field-level description distinguishing
the two and no normative TS 32.291 text is vendored in this repo to check -- echoing is the least-
invented choice, disclosed rather than picked silently; no persistence across restarts.

**A new shared utility, not CHF-private.** `ChargingDataRequest`/`Response`'s `invocationTimeStamp`
needed OpenAPI's `format: date-time` (RFC 3339) -- a genuinely different wire format from
`sbi_headers.hpp`'s existing `format_sender_timestamp` (RFC 7231 IMF-fixdate, for the
`3gpp-Sbi-Sender-Timestamp` *header*, not a JSON body field). New `libs/sbi-core/include/sbi_core/
datetime.hpp` + `src/datetime.cpp` (`format_rfc3339`), same small-single-purpose-utility precedent
as `uuid.hpp` -- kept in `sbi_core` rather than private to CHF since any future NF with a
`DateTime`-typed JSON field needs the identical formatting.

**SMF wired as a real N40 client (`nfs/smf/src/main.cpp`).** A `chf_client`/`chf_oauth` pair (same
one-client-per-NF-per-thread pattern as the existing `pcf_client`/`amf_client`), using a hardcoded
base URL (`kChfBase`) -- matching this file's own existing convention for PCF/AMF, not the
`Nnrf_NFDiscovery` path Stage 2's UPF discovery used (that was explicitly called out as the "first
real use of this NRF capability" in ADR-0041; every other NF-to-NF call in this file uses a
hardcoded base URL, before and after). `perform_n40_charging_data_create` sends only the three
mandatory `ChargingDataRequest` fields plus `subscriberIdentifier` -- `pDUSessionChargingInformation`
is deliberately left unset, since CHF's own rating engine doesn't exist yet to use it, so sending
it would be padding, not real content. Called right after the existing N4 Session Establishment
call in `CreateSMContext`'s handler, with the same best-effort/non-fatal discipline (logged on
failure, doesn't block the SM context's own 201) -- there is no real billing/quota dependency yet
for a charging-data failure to correctly block on, the same reasoning already applied to N4 and
N1N2MessageTransfer.

**Live verification, real interop between two independently-built processes.** Full stack (nrf,
udm, udr, ausf, pcf, chf, upf, smf, amf) started, then a real `nr-gnb`/`nr-ue` run performed Initial
Registration (including a real SQN resynchronization) and PDU Session Establishment. `smf`'s log:
`N4 Session Establishment succeeded ... allocated uplink F-TEID=0x1` immediately followed by
`Nchf_ConvergedCharging_Create succeeded for pduSessionId 1`. Verified independently on CHF's own
side too (not just trusting SMF's side of the claim), via each NF's own Prometheus counter,
scraped directly: `chf_charging_data_create_total{otel_scope_name="chf"} 1` and
`smf_chf_charging_data_create_total{otel_scope_name="smf"} 1` -- both real, both 1, confirming CHF
genuinely received and answered exactly one real `Nchf_ConvergedCharging_Create` call.

**Consequence:** Phase 4 Stage 0/1 is real, live-verified, and complete for its approved scope. Next
turns (separate, each needing its own approval per CLAUDE.md): Update/Release wired to session
modification/teardown, then `docs/CHARGING_MAPPING.md` before any TM Forum SID/BSS mapping code.

