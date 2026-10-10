## ADR-0018: NRF's own nfInstanceId is a fixed constant, not randomly generated per run

**Date:** 2026-08-05
**Status:** Accepted

**Context:** Discovered while wiring AMF's `sbi_core::jwt::Verifier` for incoming bearer tokens.
`sbi_core::jwt::Verifier`'s constructor requires the exact expected issuer id up front (checked
against every token's `iss` claim). NRF's own `main.cpp` previously called
`sbi_core::generate_uuid_v4()` for its own `nfInstanceId` at every process start -- fine for NRF
itself (it passed its own freshly-generated id to both its `Issuer` and its own `Verifier` in the
same process, so it always agreed with itself), but a real bootstrapping gap for every other NF:
AMF has no way to know what random id NRF picked this run before constructing its own `Verifier`.
Previously latent because NRF was the only NF that both issued and verified tokens; AMF is the
first NF to need to verify tokens NRF issued from outside NRF's own process.

**Decision:** `nfs/nrf/src/main.cpp`'s `kNrfInstanceId` is now a fixed compile-time constant
(`"5ba9a927-1d31-4c8e-8a10-000000000001"`, an arbitrary but validly-shaped UUID) instead of
`generate_uuid_v4()`. Every other NF that verifies NRF-issued tokens (starting with AMF) hardcodes
the identical constant. This is arguably the more correct design independent of the bug it fixes:
NRF is the trust anchor every other NF's OAuth2 flow depends on, and a real deployment's root of
trust having a stable, well-known identity (not one that changes every restart) is the normal
expectation, not a lab shortcut.

**Verification:** rebuilt `nrf`, reran the full test suite (all existing tests, unaffected) --
confirmed nothing depended on the identity being random. `grep` for `nrf_instance_id`/the removed
`generate_uuid_v4()` call site across the repo found only NRF's own file referencing it.

**Disclosed gap, not fixed here:** the constant is duplicated by hand in every NF that needs it
(`nfs/nrf/src/main.cpp` and `nfs/amf/src/main.cpp` so far) rather than coming from one shared
source of truth (e.g. a config file, environment variable, or `libs/sbi-core` constant). Acceptable
for two NFs; worth revisiting (shared config) once several more NFs need the same value -- tracked
as follow-up, not silently left as an accepted-forever hand-copy pattern.

**Rejected alternative:** have `Verifier` accept any issuer and expose the actual `iss` claim in
`VerifyResult` for the caller to check itself against a dynamically-discovered value. Rejected --
weakens the security-by-default property `Verifier` currently has (ADR-0012's tamper-rejection
test relies on issuer mismatch being rejected unconditionally), and there is no discovery mechanism
for "NRF's own identity" today anyway (NRF does not register itself in its own `NfRegistry`), so
this would have traded a real security check for solving a problem the fixed-constant approach
solves more simply.

---

