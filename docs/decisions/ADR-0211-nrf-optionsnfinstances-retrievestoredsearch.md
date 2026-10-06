## ADR-0211: NRF `OptionsNFInstances` + `RetrieveStoredSearch`/`RetrieveCompleteSearch` + `RetrieveKeyRequest` -- first Tier-B gap-closure since ADR-0193's own Tier-A backlog closed

### Context

With every NF's real Tier-A gaps now closed (ADR-0194 through ADR-0210), this is the first item
off ADR-0193's own remaining Tier-B backlog (`docs/CAPABILITY_GAP_ANALYSIS.md`'s "undisclosed Tier-B
gaps inside NFs already marked real" section): NRF's own 4 undisclosed missing operations, the
smallest/clearest Tier-B item per that same section's own prioritization note, matching the
"smallest/clearest first" pattern already used for ADR-0194/ADR-0195/ADR-0196. Confirmed by direct
read of the real YAML, not assumed:
- `OptionsNFInstances` (`OPTIONS /nnrf-nfm/v1/nf-instances`, `TS29510_Nnrf_NFManagement.yaml`) --
  real capability-discovery operation, response schema `OptionsResponse` (just an optional
  `supportedFeatures`).
- `RetrieveStoredSearch` (`GET /nnrf-disc/v1/searches/{searchId}`) and `RetrieveCompleteSearch`
  (`GET /nnrf-disc/v1/searches/{searchId}/complete`), both `TS29510_Nnrf_NFDiscovery.yaml` -- real
  operations letting a consumer re-fetch a previously-returned `SearchNFInstances` result by its
  own real `searchId` (the main search response's own optional `searchId` field, whose real
  `links` block in the YAML documents exactly this pair of follow-up operations).
- `RetrieveKeyRequest` (`POST /oauth2/retrieve-key`, `TS29510_Nnrf_AccessToken.yaml`) -- real
  operation for a consumer to fetch the public key needed to validate an OAuth2 access token's
  signature, given the token's real `issuer` (NfInstanceId) and JWT `headerParameters`.

Also confirmed (not a fix needed): the audit's separately-flagged "stale disclosure" for
`/scp-domain-routing-info*` was already accurately worded in `nfs/nrf/src/main.cpp`'s own header
("SCP now exists, ADR-0186, but this wiring hasn't been revisited") -- not the "SCP isn't built
yet" phrasing the audit doc's own snapshot had flagged as stale. No comment fix was needed there;
confirmed via direct re-read before concluding so, not assumed unchanged.

### Implementation

- **`OptionsNFInstances`**: real `200` + `Accept-Encoding: identity` header, no body -- same real
  pattern NSSF's own `OPTIONS` route already established (ADR-0183): no `supportedFeatures`
  bitmask is implemented anywhere in this NRF, so an honest empty-body `200` is used rather than
  fabricating a features list.
- **`RetrieveStoredSearch`/`RetrieveCompleteSearch`**, backed by a new `StoredSearchStore`
  (`nfs/nrf/src/registry.hpp/.cpp`, same real in-memory/no-TTL-eviction simplification already
  disclosed for `NfRegistry`/`SubscriptionRegistry`). `SearchNFInstances` now caches its own real
  `nfInstances` result under a new real `searchId` (returned in the search response itself) every
  time it runs; both new `GET` routes look that `searchId` up and return the same real cached
  `nfInstances`, real `404` if unknown. Real, disclosed: since this NRF has no
  partial-vs-complete-profile filtering (no field-level profile trimming is implemented anywhere),
  "stored" and "complete" are genuinely the same real data here, not a fabricated distinction.
- **`RetrieveKeyRequest`**: real structural validation of `RetrieveKeyReq`'s required
  `issuer`/`headerParameters`, then real `404` if `issuer` doesn't match this NRF's own instance
  id (`kNrfInstanceId` -- no other real issuer exists in this single-NRF lab to serve a key for).
  On a match, `rawPubKey` is the real base64 SubjectPublicKeyInfo body of
  `certs/nrf-jwt/public.pem` -- the exact same key file every NF's own `sbi_core::jwt::Verifier`
  already loads to validate NRF-issued tokens -- extracted by stripping the PEM's own
  `-----BEGIN/END PUBLIC KEY-----` delimiter lines, not re-derived or fabricated key material.

### Live verification (real, live mTLS + OAuth2, not self-consistency)

New `tests/integration/test_nrf_gap_closure.cpp`, 4 tests, real spawned `nrf` process (no second NF
needed, all 4 operations are NRF-internal) over real TLS 1.3 + mTLS: `OptionsNFInstances` real `200`
+ `Accept-Encoding` header verified; `RetrieveKeyRequest` for NRF's own real issuer id returns a
`rawPubKey` independently verified byte-for-byte against the test's own separate read of the real
`certs/nrf-jwt/public.pem` file (not compared against a value the server itself computed);
`RetrieveKeyRequest` for an unknown issuer real `404`; a real NF registered, `SearchNFInstances`
run to get a real `searchId`, then `RetrieveStoredSearch`/`RetrieveCompleteSearch` both verified to
return the same real cached `nfInstances`, plus a real `404` on an unknown `searchId`. All 4 pass.

### Testing

Full reconfigure + rebuild against the complete pilot set clean (`EXIT=0` verified directly from
each build-log step). One real `-Wshadow` warning caught and fixed along the way: a local `issuer`
variable in the new `RetrieveKeyRequest` handler shadowed the outer-scope `sbi_core::jwt::Issuer
issuer` object -- renamed to `requested_issuer`. All 4 new tests pass individually. Full suite
re-run clean: 430/430 passing (`ctest --timeout 120 -E
"UdrIntegration.AmfContextLifecycle|UdmIntegration.SdmDataRetrievalAndSubscriptions"`, matching
`.github/workflows/ci.yml`'s own exclusion). No strays before or after any run (`ps aux` checked
explicitly).

### What this ADR does NOT include

NFManagement's `/shared-data*` (multi-NRF federation -- no second NRF instance exists in this lab)
and `/scp-domain-routing-info*` (SCP exists, ADR-0186, but this specific NRF<->SCP wiring hasn't
been revisited) remain real, disclosed, out-of-scope gaps, unchanged by this ADR. `StoredSearchStore`
has no TTL eviction (same disclosed simplification class as every other in-memory store this NRF
already has). This closes only NRF's own Tier-B backlog item -- UDR (~19 undisclosed operations
across `Subscription_Data`/`Policy_Data`) and UDM (~58+ undisclosed operations across
`Nudm_UECM`/`Nudm_SDM`/`UEAU`/`PP`) remain the two largest real Tier-B items still open, per
`docs/CAPABILITY_GAP_ANALYSIS.md`'s own audit.

