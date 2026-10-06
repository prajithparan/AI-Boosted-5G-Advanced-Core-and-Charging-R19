## ADR-0021: SMF (Phase 2's third NF) -- /sm-contexts scope, shared json_body.hpp promotion

**Date:** 2026-08-05
**Status:** Accepted

**Context:** SMF implements `Nsmf_PDUSession` (`specs/5G_APIs-REL-19/TS29502_Nsmf_PDUSession.yaml`).
The full file covers two largely-independent operation groups: `/sm-contexts` (the AMF-triggered
collection real UE PDU Session Establishment uses, TS 23.502 clause 4.3.2.2.1 --
CLAUDE.md's Phase 2 end-state goal) and `/pdu-sessions` (the I-SMF/inter-SMF roaming-scenario
collection), plus `SendMoData`/`TransferMoData` (small-data-over-NAS, multipart-only) and two
entirely separate services (`Nsmf_EventExposure.yaml`, `Nsmf_NIDD.yaml`). Agreed with the user:
this turn scopes to `/sm-contexts` only -- `CreateSMContext`, `RetrieveSMContext`,
`UpdateSMContext`, `ReleaseSMContext` -- deferring `/pdu-sessions`, `SendMoData`/`TransferMoData`,
and the two separate services, all disclosed in `nfs/smf/src/main.cpp`'s file header, not silently
dropped.

**`TS29502_Nsmf_PDUSession.yaml` added as a codegen root file**
(`libs/sbi-generated/CMakeLists.txt`) -- none of SMF's DTOs existed in generated output before this
turn. Regenerating grew the type count from 1104 to 1240; `SmContext{Create,Created,Update,Updated,
Release,Released,Retrieve,Retrieved}Data` all landed with their real field shapes (verified by
direct inspection of the generated header before writing any handler code), no new cross-file name
collisions observed among the schemas this turn's handlers actually use.

**No real PCF/UDM/UPF to talk to, same shape of gap AMF had with AUSF/UDM:** `CreateSMContext` does
real request validation (mandatory `servingNfId`/`servingNetwork`/`anType`/`smContextStatusUri`
per `SmContextCreateData`) and real store bookkeeping, but cannot perform the real procedure's
PCF (SM Policy Association)/UDM (subscription data)/UPF (N4/PFCP, Phase 3) interactions -- it
always succeeds (201) rather than modeling the failure modes those absent dependencies would
produce. `UpdateSMContext` acknowledges (204) without fabricating `SmContextUpdatedData` content
(EBI allocation, N1/N2 info) since there is nothing real behind those fields yet. Both disclosed in
`nfs/smf/src/main.cpp`'s file header.

**`sbi_core::http2::problem_response`/`parse_json_body<T>`/`parse_multipart_json_body<T>` promoted
to shared `libs/sbi-core/include/sbi_core/json_body.hpp` + `src/json_body.cpp`.** This exact
pattern was independently written in `nfs/nrf/src/main.cpp` and then `nfs/amf/src/main.cpp` --
SMF made it a third occurrence, past the point where duplicating it again was the right call
(CLAUDE.md's "three similar lines is better than a premature abstraction" -- this is the fourth).
`nfs/nrf`'s and `nfs/amf`'s own local copies are deliberately left as-is (already tested, already
committed) rather than churned to use the new shared header in the same turn that introduces it --
a disclosed, non-blocking cleanup opportunity for later, not silently left unnoticed. `check_bearer`
(JWT-verification-specific, not JSON-body-parsing) was NOT promoted -- still duplicated a third
time in `nfs/smf/src/main.cpp`, consistent with the existing NRF/AMF pattern; small enough that
promoting it wasn't judged worth the extra churn this turn.

**Verification:** manual `curl` end-to-end (real multipart `CreateSMContext` -> real
`RetrieveSMContext`/`UpdateSMContext`/`ReleaseSMContext` on the resulting `smContextRef`, real
Prometheus counters) plus 2 new real subprocess-to-subprocess integration tests
(`tests/integration/test_smf_pdu_session.cpp`): a full lifecycle test (multipart `CreateSMContext`
constructed via `sbi_core::multipart::encode`, response deserializes as real `SmContextCreatedData`,
`smContextRef` extracted from the real `Location` header, then `RetrieveSMContext` with no request
body -- proving the spec's `required: false` is actually honored -- `UpdateSMContext`, and
`ReleaseSMContext` followed by a second release correctly 404ing) and an error-path test (404 on a
nonexistent context, 401 on a tampered token, 400 when `CreateSMContext` gets a plain JSON body
instead of multipart/related). All 21 project tests pass, stable across repeated runs.

**Rejected alternative:** implement `/pdu-sessions` alongside `/sm-contexts` in the same turn since
both were now unblocked by the multipart codec. Rejected to keep this turn's scope matched to what
was actually agreed with the user (the standard AMF-triggered flow) rather than silently expanding
scope just because the blocker was gone -- `/pdu-sessions` remains a clearly-scoped future addition
rather than something started, then left half-verified alongside everything else this turn already
covers.

---

