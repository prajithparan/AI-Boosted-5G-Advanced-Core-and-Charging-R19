## ADR-0235: UDM `Nudm_SDM` -- shared-data family (6 ops: GetIndividualSharedData/GetSharedData/
GetGroupIdentifiers/SubscribeToSharedData/UnsubscribeForSharedData/ModifySharedDataSubs)

### Context

Confirmed by direct read of `TS29503_Nudm_SDM.yaml`: this real 6-op group is genuinely NOT per-UE,
unlike every other `Nudm_SDM` resource closed so far (ADR-0228/0230/0232/0233/0234) -- shared data
and group identifiers are global resources, keyed by `sharedDataId`/`extGroupId`/`intGroupId`, not
by `supi`.

Checked UDR first before assuming new UDR-side work was needed (the lesson ADR-0233 itself
re-learned after ADR-0230's own scoping mistake): `nfs/udr/src/main.cpp` already has
`GetIndividualSharedData` fully live (`SharedDataStore`, real Postgres-backed, ADR-0110, seeded
`"10000-default"`) and `GetGroupIdentifiers` fully live (`GroupIdentifiersStore`, real
Postgres-backed, ADR-0140, seeded `extgroupid-group1@example.com`/`A1B2C3D4-001-01-AB`). ADR-0110's
own comment in `nfs/udr/src/main.cpp` explicitly discloses UDR never built the real sibling
collection resource, `GetSharedData` (a required comma-separated `shared-data-ids` array query
parameter) -- that gap is closed here at the UDM layer instead of adding a new UDR-side bulk
endpoint.

### Implementation

- **`GetIndividualSharedData`** (`GET /shared-data/{sharedDataId}`) and **`GetGroupIdentifiers`**
  (`GET /group-data/group-identifiers?ext-group-id=...|int-group-id=...`): real, direct proxies to
  UDR's own already-live routes -- real `404`/`200` passthrough, real `400` if neither
  `ext-group-id` nor `int-group-id` is given (matching UDR's own real validation).
- **`GetSharedData`** (`GET /shared-data?shared-data-ids=...`): real bulk retrieval, composed at
  the UDM layer via one real UDR call per id in the required `shared-data-ids` array (parsed with
  the pre-existing `sbi_core::http2::split_form_array` helper, same `style: form, explode: false`
  convention already used elsewhere). An id that 404s or fails to parse is silently omitted from
  the result array rather than fabricated or surfaced as a partial error -- disclosed as valid
  since the response schema has no `minItems` constraint. Real `400` if `shared-data-ids` is
  absent.
- **`SubscribeToSharedData`/`UnsubscribeForSharedData`/`ModifySharedDataSubs`**
  (`/shared-data-subscriptions[/{subscriptionId}]`): backed by a brand-new UDM-local
  `SharedDataSubscriptionStore` (`nfs/udm/src/stores.hpp`/`.cpp`), structurally identical to the
  pre-existing `SdmSubscriptionStore` (real `SdmSubscription` schema, create/id-assign/merge-patch
  shape) but genuinely without a `ue_id` ownership field, since shared-data subscriptions are
  global. Real `201` with `Location` header on create, real `204`/`404` on delete, real RFC 7396
  merge-patch `200`/`404` on modify.

### Live verification (real, live mTLS + OAuth2, not self-consistency)

New `tests/integration/test_udm_sdm_gap_closure_235.cpp`, 1 test
(`SharedDataAndGroupIdentifiersReturnRealUdrSeededData`): real `200` for `GetIndividualSharedData`
against UDR's seeded `"10000-default"`; real `404` for an unknown id; real `200` for `GetSharedData`
with a mixed known+unknown id list, confirming the unknown one is silently omitted (result array
has exactly 1 entry); real `400` for `GetSharedData` with no `shared-data-ids`; real `200` for
`GetGroupIdentifiers` by both `ext-group-id` and `int-group-id` against UDR's seeded group; real
`400` for neither param; a full real `SharedDataSubscriptionStore` CRUD lifecycle
(subscribe->modify->unsubscribe->second-delete-404s).

A real bug was caught and fixed before this passed: the test's own `SubscribeToSharedData` POST
body initially only set `nfInstanceId`, omitting the real schema's other two required fields
(`callbackReference`, `monitoredResourceUris`) -- UDM correctly `400`-rejected it (server-side
route behaved correctly throughout), but the test's own `ASSERT_TRUE(result.contains(
"subscriptionId"))` then failed against that `400` `ProblemDetails` body, short-circuiting the test
function before its `kill()`/`waitpid()` cleanup ran. This reproduced this project's own documented
leaked-process pipe-hang pattern (task #170): the orphaned `nrf`/`udr`/`udm` children inherited the
shell's output pipe and held it open indefinitely, and since the manual repro command used `| tail
-60` (which cannot print anything until it sees EOF), the failure looked like a genuine multi-hour
hang with zero output rather than an instant test-body failure. Root-caused by manually re-running
every one of the 6 new routes individually against a fresh `nrf`+`udr`+`udm` with short `curl`
timeouts (all 6 routes responded correctly and instantly on a clean process, isolating the bug to
the test file, not the server code), then fixing the POST body to the real required shape. No
server-side code changed as a result of this investigation.

Passes.

### Testing

Full reconfigure + rebuild clean. New test passes individually (no hang, no strays, `ps aux`
checked explicitly before and after). Full suite re-run clean at `-j1`.

### What this ADR does NOT include

This closes `Nudm_SDM`'s entire shared-data family. Still open: 2 identifier-lookup ops
(`GetSupiOrGpsi`/`GetMultipleIdentifiers`), `Update SOR Info`, and `Nudm_PP` (11 ops, ADR-0082).

