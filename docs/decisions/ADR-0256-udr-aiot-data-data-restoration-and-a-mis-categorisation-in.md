## ADR-0256: UDR AIoT data + data-restoration -- and a mis-categorisation in ADR-0252's own audit

### What was built

| Path | Methods | Source |
|---|---|---|
| `/aiot-data/aiot-device-profile-data/{aiotDevPermId}` | GET, PATCH (RFC 6902) | `TS29506_Aiot_Data.yaml` |
| `/aiot-data/aiot-device-profile-data` | GET (bundled), PATCH (**501, see below**) | same |
| `/aiot-data/af-authorization-data` | GET | same |
| `/data-restoration-events` | POST | `TS29504_Nudr_DR.yaml` |

**UDR: 5 -> 1 unrouted**, and the 1 that remains is a disclosed deferral, not a gap.

`TS29506_Aiot_Data.yaml` (TS 29.506 V19.2.0) defines the paths and **zero schemas of its own** --
every DTO comes from `TS29369_Nadm_DM.yaml` (TS 29.369 V19.2.0). Only the schema file was added to
the codegen list; generating a schemaless paths-only file produces an empty header. `Nadm_DM`'s own
5 service paths belong to a different NF (ADM) and are deliberately **not** implemented here.

Both AIoT resources are GET/PATCH-only in UDR's spec -- no create, replace or delete operation
exists anywhere in the file -- so rows are seeded at startup, the same "no live provisioning path"
shape as the `provisioned-data` group (ADR-0069). Because every `udr` start re-seeds, the new
integration tests are idempotent against a persistent database.

### The one operation deliberately left unimplemented, and why 501 rather than a guess

`UpdateBundledAiotDeviceProfileData` (PATCH on the bundled collection) is registered, parses and
validates its body, and then returns **501 with `ProblemDetails`** stating the reason.

The YAML defines the operation but not what it operates on. Its sibling GET on the same path makes
`requester-aiot-devices-id` **required** to select which devices form the bundle; this PATCH has no
device selector at all -- its only parameter is `supported-features`. Two independent consequences:

- the set of devices modified is unbounded and undefined; and
- RFC 6902 paths into an `AiotDevProfileDataList` document are **index-based**
  (`/aiotDevProfileDataList/0/...`), so index 0 names a different device depending on row ordering.

Applying the patch to every stored profile would be a mass-mutation endpoint invented here, not
read from the spec. It is not shipped. This is the "implement against what is there and mark the
gap explicitly" case, not the "pick a reading" case -- and it is **an open question for the user**,
not a closed decision.

Two further YAML defects in the same pair of operations, reproduced as read rather than smoothed:

- The **item** PATCH is `application/json-patch+json` but `$ref`s a **bare** `PatchItem`, while RFC
  6902 bodies are arrays. The array reading is implemented -- one contract, not a superset that
  accepts both -- and a non-array body is a 400.
- The **bundled** PATCH is labelled `application/merge-patch+json` but carries an **array of
  `PatchItem`**, i.e. RFC 6902 semantics under an RFC 7396 content type.

Item PATCH returns 204 rather than a 200 + `PatchResult` execution report -- the same deliberate
choice this file already makes for `AmfContext3gpp`/`UpdateSmfContext`, for the same reason:
fabricating report items with no per-op tracking behind them would be inventing content.

### `/data-restoration-events`: 204, not 201 -- and why that is the more honest reading

This is the thinnest operation in UDR's API surface. Its request body schema is literally `{}` --
no fields at all -- and it defines **no 2xx response**, only `default` (ProblemDetails).

- The body is persisted **opaquely** under a server-assigned id. There is no shape to validate
  against, and inventing one would break this project's first rule.
- The success code is **204**. 201 is the TS 29.500 convention for a creating POST, but it obliges
  a `Location`, and the spec defines **no individual-subscription resource path** -- any `Location`
  emitted here would resolve to nothing. 204 claims less. Both readings are defensible; this one is
  flagged for the user rather than silently settled.
- There is consequently **no unsubscribe**: nothing in the API can address a stored row.
- The `restorationNotification` callback (`DataRestorationNotification`) is **not delivered** --
  the same disclosed gap class as every other subscription resource in this file.
- The spec's own operationId typo, `CreateIndividualSubcription`, is cited verbatim, not tidied.

### ADR-0252's audit got a row wrong, and this ADR corrects it rather than closing it

`/subscription-data/{ueId}/service-specific-authorization-data/{serviceType}` (`GetSSAuData`) was
listed by ADR-0252 as part of UDR's "large, real, undisclosed coverage gap". **That was wrong.**

**ADR-0160 investigated this resource in depth and the user explicitly chose to defer it -- twice.**
Once on which response schema to return (`ServiceSpecificAuthorizationData`, treating the
`AuthorizationData` citation as a spec typo), and again, after a deeper finding, on whether to build
it at all: the already-implemented CRUD sibling stores a third, structurally unrelated schema
(`ServiceSpecificAuthorizationInfo`), `GetSSAuData` has no PUT counterpart, and implementing it
would mean either fabricating an undocumented field mapping or shipping a permanently-empty store.
The user's answer was "Leave this resource deferred."

**It was implemented in this turn's working tree before that history was found, and then backed
out** -- route, store, table and seed. What had been written was precisely the option the user
rejected: a `AuthorizationData`-shaped response from a new, disconnected, seed-only store. It is not
in this commit. The resource belongs in the same category as AUSF's `/rg-authentications` and NRF's
`/shared-data` -- rows ADR-0252 correctly marked "disclosed deferred" -- and that audit failed to
check ADR history for this one path while doing so for the others.

**A stale reason was corrected while leaving the deferral intact.** Three places in
`nfs/udr/src/main.cpp` and `nfs/udr/schema.postgres.sql` recorded the blocker as "real required
complex-object query parameters this project has no parsing precedent for". That is no longer
true -- ADR-0165's `GetNiddAuData` established exactly that precedent (`single-nssai` as a
JSON-encoded query value, decomposed to sst/sd). Those comments now say so and point at ADR-0160's
still-valid schema/write-path reason instead. This is the ADR-0250/0251 staleness discipline applied
to a comment that would otherwise have justified the right decision with a wrong fact.

### Verification

All 4 new paths confirmed by **exact-literal `grep -F`** against the built binary; the SSAu path
confirmed **absent** from the binary after the back-out.

Build clean, clang-format clean, conformance 333/333, `ctest` 478/478.
`tests/integration/test_udr_aiot_data.cpp` adds 4 tests driving real `nrf` + `udr` processes over
TLS 1.3 + mTLS against real PostgreSQL, asserting the two deliberate non-implementations explicitly
(bundled PATCH = 501 with the reason in `detail`; data-restoration = 204 **and no `Location`
header**) so that a later change has to be a conscious one.

**Those tests earned their place immediately by catching two real defects in this ADR's own first
draft**, neither of which inspection had found:

1. **An invented field.** The seed wrote `lastKnownAiotfInfo: {aiotfInstanceId: ...}`.
   `LastKnownAiotfInfo`'s only required field is `lastKnownAiotfInfoInd`, a boolean --
   `aiotfInstanceId` does not exist in `TS29369_Nadm_DM.yaml`. The generated `from_json` rejected
   it. This is precisely the failure mode this project's never-invent-a-field rule exists to
   prevent, and generated DTOs caught it exactly as intended.
2. **A dropped connection instead of a response.** That rejection threw out of the bundled-GET
   handler, and the server closed the stream with **no response at all** rather than a
   `ProblemDetails`. The handler now catches `nlohmann::json::exception` and answers 500. Any
   stored row that fails to satisfy its schema is now answered, not silently dropped.

**On the `ctest` total -- resolved, not left open**: 478 here, of which 4 are new; 474 before this
commit, 471 before ADR-0255's 3. ADR-0253 and ADR-0254 both recorded **469**. Checked rather than
guessed: `tests/integration/CMakeLists.txt` listed 47 `.cpp` files at ADR-0254 (`40709b3`) and
lists 49 now, and `git diff 40709b3 -- tests/` shows those two files are the only test-registration
change in between. So nothing untracked added tests: **469 was carried forward stale rather than
re-measured, and the real figure at ADR-0254 was 471.** Corrected here rather than propagated
again.

### Follow-up audit within this ADR: three more of the same defect class

After the two defects above were found by test, the rest of the seed block was re-read against the
YAML rather than assumed correct -- on the reasoning that `aiotfInstanceId` was caught only because
it sat on a *required* field, and the rest of that block was written the same way. Three more real
problems, all in this ADR's own work, all fixed here:

1. **`afAuthData: {"af-app-1": {}}` violated its own schema.** `IndividualAfAuthorizationData`
   requires `afId`. Nothing caught it because the `af-authorization-data` route returns the map
   opaquely and `afAuthData` generates as an opaque `nlohmann::json`, so no round-trip validates
   the entries. Seed corrected; the route now validates each entry through the generated type and
   answers **500** rather than serving a document that violates the spec.
2. **`aiotDevPermId` was a free-form label, not `Bytes`.** `AiotDevPermId` resolves to `Bytes`
   (`format: byte`) in `TS29571_CommonData.yaml` -- base64. The codegen emits it as an opaque
   fallback, so `"aiot-dev-000000000000001"` was accepted by everything and is still wrong. Now
   `"YWlvdC1kZXYtMQ=="`.
3. **The same dropped-connection bug, one route over.** `af-authorization-data` did an unguarded
   `doc->at("afAuthData")`; a row without that key would have thrown out of the handler and closed
   the stream with no response -- identical to the bundled-GET defect fixed above. Guarded.

The generalisable lesson, worth more than the three fixes: **an opaque generated fallback type
silently disables the safety net this project relies on.** `AiotDevPermId` and `afAuthData` both
generate as `nlohmann::json`, so the "generated DTOs catch invented fields" argument -- which is
exactly what caught defect 1 in the previous section -- does not hold for them. Fields behind an
opaque fallback have to be checked by reading the YAML, because nothing else will.

### Open questions for the user

1. **Bundled AIoT PATCH** -- is 501 the right answer, or should it be given a scope this project
   defines (e.g. require `requester-aiot-devices-id` on the PATCH too, mirroring its own GET)? That
   would be inventing a parameter the spec does not have, which is why it was not done unasked.
2. **`/data-restoration-events`** -- 204 or 201? 201 is the TS 29.500 convention but has no
   routable `Location` to offer here.
3. **`GetSSAuData`** -- ADR-0160's deferral still stands and is untouched. Worth revisiting only if
   the schema mismatch it identified is resolvable; nothing new has changed that.

