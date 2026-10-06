## ADR-0194: NRF `Nnrf_Bootstrapping` -- first ADR-0193 gap-closure

### Context

The literal Tier-A gap that triggered ADR-0193's project-wide audit: `Nnrf_Bootstrapping`
(TS29510_Nnrf_Bootstrapping.yaml v1.3.0, TS 29.510 clause 6.4, one real operation --
`GET /bootstrapping` -> `BootstrappingInfo`) existed in the vendored spec tree but was never added
to the sbi-codegen pilot set, unlike its three siblings (`Nnrf_NFManagement`/`Nnrf_NFDiscovery`/
`Nnrf_AccessToken`, all already wired for NRF). Closed first per ADR-0193's own
smallest-and-clearest-first prioritization.

### Implementation

`TS29510_Nnrf_Bootstrapping.yaml` added to `libs/sbi-generated/CMakeLists.txt`'s pilot set (its
cross-file `$ref`s onto `TS29571_CommonData.yaml` were already wired). Generated DTO
(`sbi_gen::BootstrappingInfo`/`sbi_gen::Status`, standalone header
`TS29510_Nnrf_Bootstrapping.hpp`) used directly -- no hand-written struct. Real route registered
in `nfs/nrf/src/main.cpp`, path exactly as the vendored YAML declares it: bare `/bootstrapping`,
no service-name/version prefix (unlike `NFManagement`'s own `{apiRoot}/nnrf-nfm/v1`) -- the real
YAML's own `servers: url: '{nrfApiRoot}'` has no such prefix, since Bootstrapping is meant to be
reachable before an NF knows anything about this NRF's other service paths; no prefix was invented
to match the other three.

Response population, each field grounded in real project state, nothing fabricated:
- `nrfInstanceId`: this NRF's own real, fixed `kNrfInstanceId`.
- `status`: `OPERATIVE` (real -- the instance is serving requests when this handler runs).
- `oauth2Required`: `{"nnrf-nfm": false, "nnrf-disc": false, "nnrf-oauth2": false}` -- honestly
  `false` for all three, since every existing NRF route's own `check_bearer` only validates a
  bearer token if one is present (the YAML's own `security: [{}, oAuth2ClientCredentials]`
  explicitly permits the anonymous alternative); this NRF does not actually require OAuth2 today.
- `_links`: real, disclosed gap -- TS 29.510 clause 6.4.6.3.3 (the real link-relation vocabulary
  for this required map) is stage-3 prose this project's vendored material doesn't include, only
  the OpenAPI schema itself, which places no enum on the map's own keys. `"self"` (RFC 8288) is
  used to satisfy the schema's real `minProperties: 1` structural requirement -- a defensible,
  universal HAL relation, not a claim to the full real TS 29.510 relation set.
- `nrfFeatures`/`nrfSetId`: deliberately omitted (both optional per the YAML) -- no real
  supported-features bitmask tracking or NRF Set concept exists in this project; populating either
  would be fabricated data, not a real simplification.
- Real `Cache-Control: max-age=60` and a real content-hash `ETag` are emitted; the YAML declares an
  `If-None-Match` request parameter but never declares a `304` response for this operation, so no
  conditional-request short-circuit is implemented -- disclosed, not a silent gap.

New conformance test `tests/conformance/test_nrf_bootstrapping_dtos.cpp` (2 round-trip tests over
the real generated DTOs), wired into `tests/conformance/CMakeLists.txt`. NRF's own file-header
scope comment updated to list this operation as in-scope and to correct a stale disclosure
(`/scp-domain-routing-info*` still said "SCP isn't built yet" -- SCP has existed since ADR-0186;
the wiring itself remains a real, separate, still-open gap, now correctly described as stale
rather than current).

### Live verification (real, live process, not self-consistency)

Real `nrf` process, real `GET /bootstrapping` over mTLS: real `200`, real
`content-type: application/3gppHal+json`, body:
`{"_links":{"self":{"href":"/bootstrapping"}},"nrfInstanceId":"5ba9a927-1d31-4c8e-8a10-000000000001","oauth2Required":{"nnrf-disc":false,"nnrf-nfm":false,"nnrf-oauth2":false},"status":"OPERATIVE"}`.
Process killed by explicit PID afterward.

### Testing

Full project rebuild clean. Full `ctest` (same exclusions as ADR-0192): 363/363 pass, including
both new `NrfBootstrappingDtos` tests.

### What this ADR does NOT include

The real TS 29.510 clause 6.4.6.3.3 link-relation vocabulary (disclosed above -- not vendored).
`304 Not Modified` conditional-request handling (the YAML never declares this response for this
operation). The other three NRF gaps found in the same ADR-0193 audit
(`OptionsNFInstances`/`RetrieveStoredSearch`/`RetrieveCompleteSearch`/`RetrieveKeyRequest`, and the
stale `/scp-domain-routing-info*` wiring) -- tracked in `docs/CAPABILITY_GAP_ANALYSIS.md`, not
closed by this ADR.

