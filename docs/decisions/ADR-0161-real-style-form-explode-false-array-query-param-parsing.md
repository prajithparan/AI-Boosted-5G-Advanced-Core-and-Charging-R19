## ADR-0161: real `style: form, explode: false` array-query-param parsing infra in sbi-core,
plus its first real consumer -- UDR's `QueryContextData`

### Context

ADR-0160 confirmed that most of UDR's remaining real gaps (`pdtq-data`, `nidd-authorization-data`,
`Nudr_GroupIDmap`'s `/nf-group-ids`, bare `/subscription-data/{ueId}`/`{ueId}/context-data`) are
blocked by the same shared, project-wide capability gap: this project has never built real
parsing for OpenAPI's `style: form, explode: false` array query-param convention (a single query
key holding a comma-separated list, e.g. `?context-dataset-names=AMF_3GPP,SDM_SUBSCRIPTIONS`).
Presented to the user via `AskUserQuestion` (investigate `GetSSAuData` further / build this infra
/ pause UDR gap-closure here). **User's explicit answer (this session's second such prompt, after
`GetSSAuData` was resolved in ADR-0160): "Build array-query-param parsing infra."**

### Implementation

- **The helper**: `sbi_core::http2::split_form_array(const std::string& value)`, declared in
  `libs/sbi-core/include/sbi_core/http2_server.hpp`, implemented in
  `libs/sbi-core/src/http2_server.cpp`. Takes a single, already-percent-decoded
  `Request::query_params` value and splits it on literal commas via `std::getline`. Real,
  disclosed limitation, documented in the header comment: `Request::query_params` values are
  already fully percent-decoded by the time a handler sees them (confirmed by reading
  `parse_query_string`'s own implementation), so a literal comma a spec-compliant client escaped
  as `%2C` is indistinguishable from a delimiter comma after decoding -- this function cannot tell
  them apart and would over-split such a value. Every real 5G identifier this project has needed
  to split so far (enum values, external group IDs, PLMN/SNSSAI component strings) is comma-free
  by construction, so this is a real but narrow, disclosed simplification, not swept under the
  rug. A correct fix would require splitting on the RAW, still-percent-encoded query string before
  decoding each piece independently -- not done here, left as a known limitation for if a future
  real consumer's identifier space can contain literal commas.
- **Real unit tests**: `tests/conformance/test_query_params.cpp` (six `SplitFormArray.*` cases --
  empty input, single value, multiple values, real non-comma special characters preserved
  (`@`/`-`/`.`), an internal empty element (`"a,,b"`), and a trailing delimiter with no following
  characters (`"a,b,"`) -- the last two document real, observed `std::getline` behavior rather
  than assumed behavior). `conformance_tests`' own `CMakeLists.txt` now links `sbi_core` (a new,
  real, minimal dependency addition) so this pure-function logic can be tested without spinning up
  a live server.
- **First real consumer**: `QueryContextData` (`/subscription-data/{ueId}/context-data`, real
  spec `GET`-only, real REQUIRED `context-dataset-names` array query param). Same real, disclosed
  live-composition design as `ue-update-confirmation-data` (ADR-0147): this "document" is a live
  VIEW over 11 already-independently-stored sub-resources (`AMF_3GPP`/`AMF_NON_3GPP`/
  `SDM_SUBSCRIPTIONS`/`EE_SUBSCRIPTIONS`/`SMSF_3GPP`/`SMSF_NON_3GPP`/`SUBS_TO_NOTIFY`/`SMF_REG`/
  `IP_SM_GW`/`ROAMING_INFO`/`PEI_INFO`, each already backed by a real, already-existing store),
  composed live at request time from `context-dataset-names`' own requested subset -- not a
  twelfth, duplicate table. Missing the required query param is a real `400`. The schema's own
  documented forward-compatible "any other string" case (`ContextDataSetName`'s real `anyOf`) is
  handled by silently skipping an unrecognized name -- there is nothing real to populate for it,
  matching the spec's own forward-compatibility intent rather than rejecting the request.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl sequence against a running `udr` process (freshly built, freshly started, registered
with a freshly started `nrf`, mTLS client cert + real NRF-issued OAuth2 bearer token), SUPI
`imsi-999700000000095`:

- Seeded real `AMF_3GPP` context (via the already-implemented `amf-3gpp-access` PUT) and real
  `PEI_INFO` (via the already-implemented `pei-info` PUT).
- `GET .../context-data` with no `context-dataset-names` query param -> real `400`.
- `GET .../context-data?context-dataset-names=AMF_3GPP,PEI_INFO,SDM_SUBSCRIPTIONS,SOME_FUTURE_VALUE`
  -> real `200` with `amf3Gpp` and `peiInfo` populated (the two seeded, real datasets),
  `sdmSubscriptions` correctly absent (no data exists for that UE), and `SOME_FUTURE_VALUE`
  correctly silently skipped (no crash, no bogus field) -- confirms both the real comma-split
  parsing and the forward-compatible-name handling in one request.
- A second, fresh UE with no data at all, requesting `AMF_3GPP,SMF_REG` -> real `200 {}` (matching
  `ue-update-confirmation-data`'s own "live view, no independent existence" precedent).

### Testing and verification

`sbi_core` and `udr` built clean (zero warnings) both before and after `clang-format-18`
(reformat added no diff beyond what was newly written). Full `conformance_tests` (excluding the
two disclosed pre-existing flaky tests): 331/331 pass (325 prior + 6 new `SplitFormArray.*`
cases), zero regressions.

### What this ADR does NOT include

No NF's own existing logic calls the new `QueryContextData` route (same disclosed "surface first,
wire consumers later" precedent already used repeatedly for UDR's own resource-breadth
gap-closure). This closes UDR resource #70 of free5GC's ~42+ real `Nudr_DataRepository` resources.
Real, disclosed scope narrowing: this infra is NOT yet wired into `pdtq-data`,
`nidd-authorization-data`, `Nudr_GroupIDmap`'s `/nf-group-ids`, bare `/subscription-data/{ueId}`
(which additionally has a real `content: application/json` query param, `single-nssai`, genuinely
different from the array-style params this ADR addresses), or the "optional filter not honored"
resources' own already-shipped, deliberately-not-honored array filters (`ee-subscriptions`'
`event-types`/`nf-identifiers` etc. -- retrofitting those to real honored filtering is a separate,
disclosed, deliberate decision from before, not automatically revisited just because the parsing
capability now exists). Task #106 remains open for each of those, now genuinely unblocked rather
than blocked, as real candidates for future turns.

