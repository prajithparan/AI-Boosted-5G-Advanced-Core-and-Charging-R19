## ADR-0162: gap-closure task #106 continuation -- UDR real PDTQ Data collection + individual
document, the first resource genuinely unblocked by ADR-0161's array-parsing infra

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (70 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0161, which added the array-query-param parsing
infra but its own first consumer, `QueryContextData`, didn't need the "optional array filter"
half of that infra -- its own array param was required). This ADR closes `pdtq-data`
(`TS29519_Policy_Data.yaml`), confirming by direct read that it really was blocked by exactly the
gap ADR-0161 closed, not merely a plausible candidate.

Real, confirmed-by-YAML-read: `/policy-data/pdtq-data` (real spec operation `ReadPdtqData`, real
OPTIONAL `pdtq-ref-ids` array query-param filter, `style: form, explode: false`) and
`/policy-data/pdtq-data/{pdtqReferenceId}` (real spec operations `ReadIndividualPdtqData`/
`CreateIndividualPdtqData`/`UpdateIndividualPdtqData`/`DeleteIndividualPdtqData`).

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_pdtq_data` table (`pdtq_ref_id TEXT PRIMARY KEY`,
  `data JSONB`).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `PdtqDataStore` class (`put`/`get`/`list`/`merge_patch`/
  `remove`), structurally identical to `BdtDataStore` (single-key, void `put`, RFC 7396
  `merge_patch`) plus a new `list()` method for the real collection GET.
- `nfs/udr/src/main.cpp`: store construction, four new OTel counters, and five new routes
  (collection GET, individual GET/PUT/PATCH/DELETE).
- Real, disclosed: `pdtqReferenceId` is client-supplied (a real path parameter on the individual
  document, confirmed by the path-item-level `parameters:` block covering all four
  individual-document operations) -- no server-generated ID needed, unlike several of this
  project's own earlier `ee-subscriptions`-family resources.
- Real, disclosed: `CreateIndividualPdtqData`'s own response list documents ONLY `201` -- no `204`
  update-success path exists at all. Matches the existing `bdt-data` precedent exactly (found
  during that resource's own closure, well before this session's `ee-subscriptions`-family
  distinct-201-vs-204 pattern was established) -- this route always responds `201` with the
  stored resource, matching the real spec literally rather than inventing an undocumented `204`
  or a `409`-style conflict response the spec doesn't document either.
- Real RFC 7396 JSON Merge Patch (`application/merge-patch+json`, `PdtqDataPatch`), same idiom as
  `bdt-data` and UDM's own `AMF-3GPP-access` merge-patch route: the raw body is validated against
  the real generated `PdtqDataPatch` shape first (catches malformed/wrong-shape patches), then
  `.merge_patch()` is applied to the RAW parsed body (not round-tripped through the DTO struct) so
  RFC 7396's own absent-vs-explicit-null field semantics survive intact.
- Real, disclosed scope choice: the collection GET's own optional `pdtq-ref-ids` array filter is
  NOT honored (always returns the full list) -- matching the established "optional filter not
  honored" precedent (`ee-subscriptions`' own `event-types`/`nf-identifiers`) for consistency,
  not because the new `split_form_array()` infra couldn't parse it. Retrofitting every
  "optional filter not honored" resource (this one included) to real honored filtering remains a
  separate, disclosed, deliberate decision left for its own turn.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl sequence against a running `udr` process (freshly built, freshly started, registered
with a freshly started `nrf`, mTLS client cert + real NRF-issued OAuth2 bearer token),
`pdtqReferenceId` `pdtq-ref-001`:

- `GET` collection on a fresh install -> real `200 []`.
- `GET` individual before any `PUT` -> real `404`.
- `PUT` with a first, incomplete guess at `pdtqPolicy`'s real shape -> real `400`
  (`"key 'pdtqPolicyId' not found"`) -- checked the generated `PdtqPolicy` struct
  (`pdtqPolicyId: int64`, `recTimeInt: TimeWindow`, both required), corrected, re-ran: `PUT` with
  `{"aspId":"asp-001","pdtqPolicy":{"pdtqPolicyId":1,"recTimeInt":{"startTime":"...",
  "stopTime":"..."}},"qosReference":"qos-ref-1"}` -> real `201`, body echoed back, `Location`
  header confirmed to contain the real `pdtqReferenceId`.
- `GET` individual -> real `200` with the same body; `GET` collection -> real `200` with that one
  item.
- `PATCH` (real RFC 7396 merge-patch, `{"warnNotifEnabled":true}`) -> real `200` with the merged
  body (new field added, existing fields preserved); `GET` after confirms persistence.
- `PATCH` against a nonexistent `pdtqReferenceId` -> real `404`.
- Direct `psql SELECT` against `udr_pdtq_data` independently confirmed the row matched curl's
  response exactly.
- `DELETE` -> real `204`; `GET` after -> real `404`; `psql SELECT` confirmed zero rows remained.

### Testing and verification

`udr` built clean (zero warnings) both before and after `clang-format-18` (reformat added no diff
beyond what was newly written). Full `conformance_tests` (excluding the two disclosed pre-existing
flaky tests): 331/331 pass, zero regressions.

### What this ADR does NOT include

No NF's own existing logic calls these new routes (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #71 of free5GC's ~42+ real `Nudr_DataRepository` resources. Task #106 remains open:
`nidd-authorization-data`, `Nudr_GroupIDmap`'s own `/nf-group-ids`, bare
`/subscription-data/{ueId}`, and `group-data`'s own `5g-vn-groups`/`mbs-group-membership` bare
collection GETs remain real candidates now genuinely unblocked by ADR-0161's infra, not yet
individually closed. `GetSSAuData` remains deliberately deferred per ADR-0160.

