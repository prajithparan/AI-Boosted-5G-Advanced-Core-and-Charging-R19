## ADR-0477: NRF discovery authorization: allowedNfTypes/allowedPlmns/allowedNssais (finding F4)

**Date:** 2026-10-09. **Status:** Proposed.

**Context.** Finding F4 (docs/SECURITY_COMPLIANCE.md): TS 33.501 13.3.1.3 requires the NRF to return
only producers the requester is authorized to discover. The NRF discovery handler filtered on
`target-nf-type` only. TS 29.510 (Rel-19) defines the authorization attributes in the NF profile and
NF service (`allowedNfTypes`, `allowedPlmns`, `allowedNssais`, `allowedNfDomains`; absent = any) and the
requester query parameters (`requester-nf-type`, `requester-plmn-list`, `requester-snssais`,
`requester-nf-instance-fqdn`); NOTE 12 allows an NRF whose request lacks the requester information to
return only instances whose authorization parameters allow any consumer.

**Decision.** `nfs/nrf/src/discovery_authz.{hpp,cpp}`: a pure filter applied to the search result.
Absent list = unrestricted; present list + missing requester info = not returned (NOTE 12 option, no
400, so existing consumers that omit requester params keep working against unrestricted profiles).
A service's own list prevails over the profile's; a profile is returned if it has no services and its
own lists pass, or at least one service passes. `allowedPlmns` also treats the profile's `plmnList` as
allowed (6.1.6.2.3). S-NSSAI match: SST and SD identical, absent SD never matches present SD (NOTE 10).
Malformed `requester-plmn-list`/`requester-snssais` (not a JSON array) -> 400. Tests:
`nrf_discovery_authz_tests` (5, pure logic, run locally).

**Disclosed gaps / not verified.**
- `allowedNfDomains` / `requester-nf-instance-fqdn` NOT evaluated: the spec does not say which part of
  the FQDN the ECMA-262 pattern is matched against. Needs the architect's decision; not invented.
- `allowedSnpns` / SNPN requester parameters not evaluated.
- Handler wiring (query parsing, 400) compiles but was NOT exercised end-to-end over HTTP; no NF in this
  repo registers any `allowed*` attribute today, so no consumer behaviour changes yet.
- Token-based enforcement (TS 33.501 13.4.1 access-token claims) is separate and unchanged.
- `requester-plmn-specific-snssai-list` not evaluated.

**Rejected alternatives.** Rejecting discovery with 400 when `requester-nf-type` is absent (YAML marks it
mandatory, but every current consumer omits it and would break; NOTE 12 permits the chosen behaviour).
Guessing the allowedNfDomains matching rule.
