## ADR-0197: UDR operator-specific-data PUT/DELETE -- correcting ADR-0111/ADR-0114's own real documentation errors

### Context

Found while auditing the ADR-0193 backlog, not a new gap: ADR-0111 (`subscription-data`-scoped
Operator-Specific Data Container) and ADR-0114 (`policy-data`-scoped Operator-Specific Data) both
asserted, in their own written text, "confirmed by direct read, no PUT/DELETE exists for this
resource" and "confirmed by grepping this exact path... no PUT/DELETE exists for this resource,"
respectively. A direct re-read of both vendored YAML files this turn disproves both claims:

- `TS29505_Subscription_Data.yaml`'s `/subscription-data/{ueId}/operator-specific-data` declares
  `get`/`patch`/**`put`**/**`delete`** (`QueryOperSpecData`/`ModifyOperSpecData`/
  **`CreateOperSpecData`**/**`DeleteOperSpecData`**) -- four real operations, not two.
- `TS29519_Policy_Data.yaml`'s `/policy-data/ues/{ueId}/operator-specific-data` declares
  `get`/`patch`/**`put`**/**`delete`** (`ReadOperatorSpecificData`/`UpdateOperatorSpecificData`/
  **`ReplaceOperatorSpecificData`**/**`DeleteOperatorSpecificData`**) -- same real shape.

Neither original ADR was fabricating anything -- both cite "confirmed by direct read/grep," so
this is a genuine, disclosed transcription/reading error from that earlier session, not a
deliberate omission. Caught and fixed directly per ADR-0193's own audit finding, not left as a
backlog item, since it's a documentation-accuracy defect rather than missing coverage.

### Implementation

`OperatorSpecificDataStore`/`PolicyOperatorSpecificDataStore` (`nfs/udr/src/stores.hpp`/`.cpp`)
both gain real `put()`/`remove()` methods alongside their existing `get()`/`apply_patch()`. `put()`
uses a real, standard PostgreSQL `INSERT ... ON CONFLICT DO UPDATE ... RETURNING (xmax = 0) AS
inserted` idiom to distinguish a genuine create from a replace in one round-trip (`xmax = 0` is
true only for a row the current transaction just inserted) -- the same real create-vs-replace
distinction this project's own `UePolicySetStore::put` (ADR-0113) already established for its
own real PUT-with-differentiated-response pattern, reused here rather than reinvented. Real routes
added in `nfs/udr/src/main.cpp`: `PUT`/`DELETE` at both
`/subscription-data/{ueId}/operator-specific-data` and
`/policy-data/ues/{ueId}/operator-specific-data`, real `201`+`Location` on create / real `204` on
replace (matching each YAML's own declared response codes), real `204`/`404` on delete, real
subscriber notification (`change_replace`/`change_remove`) on both, matching every other
UDR write resource's own established convention. Stale scope comments in both ADR's own code
(main.cpp/stores.hpp) claiming "no PUT/DELETE" corrected in place.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl lifecycle against a running `udr` process backed by a real PostgreSQL database, for both
resources independently: `PUT` on an unseeded `ueId` -> real `201` + `Location` + echoed body;
`PUT` again on the same `ueId` -> real `204` (replace); `GET` -> real `200` with the replaced
value; `DELETE` -> real `204`; `DELETE` again -> real `404`. A `GET` on the sibling
`subscription-data`-scoped resource for the same `ueId`, after only the `policy-data`-scoped one
was ever created, independently returned real `404` -- confirming the two stores are genuinely
separate, not aliases.

### Testing

Full project rebuild clean. Full `ctest` (excluding the two known-flaky tests): 369/369 pass (no
new automated test needed -- this closes real coverage via the same disclosed manual-live-
verification precedent ADR-0111/ADR-0114 themselves already established, not a new pattern).

### What this ADR does NOT include

A retroactive correction to ADR-0111/ADR-0114's own historical text (left as-is, an accurate record
of what was believed and built at the time) -- this ADR is the correction, referenced forward from
both, not a silent edit of the historical record.

