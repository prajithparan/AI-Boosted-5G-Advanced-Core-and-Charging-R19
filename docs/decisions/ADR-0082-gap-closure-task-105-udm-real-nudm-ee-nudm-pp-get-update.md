## ADR-0082: gap-closure task #105 -- UDM real Nudm_EE + Nudm_PP (Get/Update)

### Context

`docs/CAPABILITY_GAP_ANALYSIS.md`'s UDM section named `Nudm_EE` (Event Exposure) and `Nudm_PP`
(Parameter Provisioning) as real gaps -- entirely missing from this project's UDM and,
grep-confirmed, from open5GS's own UDM too, so both are free5GC-only capabilities per ADR-0075's
own priority framing (lower relative priority than a both-references gap, but still a real one to
close). Unlike task #104's AUSF work, neither has a cryptographic dependency -- both are real,
schema-conformant SBI resource work, no spec-material blocker.

### Real scope, cited against the actual YAML, not assumed

`specs/5G_APIs-REL-19/TS29503_Nudm_EE.yaml` and `TS29503_Nudm_PP.yaml` added to the sbi-codegen
pilot set (clean regeneration, 2136 types, up from 2091, no collisions this time). Implemented,
route-for-route:
- `Nudm_EE`: `CreateEeSubscription` (`POST /{ueIdentity}/ee-subscriptions`),
  `UpdateEeSubscription` (`PATCH .../{subscriptionId}`), `DeleteEeSubscription`
  (`DELETE .../{subscriptionId}`) -- the full real subscription lifecycle, same
  assign-id/store/remove shape this project's own `SdmSubscriptionStore`
  (`nfs/udm/src/stores.hpp`) already established for a UE-scoped subscription resource, kept as a
  distinct `EeSubscriptionStore` rather than shared state (EE and SDM subscriptions are real,
  separate TS 29.503 resources that happen to share a shape, same "don't merge distinct resource
  types" precedent PCF's own `AmPolicyStore`/`SmPolicyStore` already set).
- `Nudm_PP`: `Get PP Data` / `Update` (`GET`/`PATCH /{ueId}/pp-data`) -- the specific operation
  the gap analysis named ("real operator/OAM-driven subscriber parameter updates"). Real,
  disclosed scope narrowing: `TS29503_Nudm_PP.yaml` also defines three larger, more specialized
  resource groups this pass does NOT implement -- `/5g-vn-groups/{extGroupId}` (5G LAN/VN group
  CRUD), `/{ueId}/pp-data-store/{afInstanceId}` (PP Data Entry CRUD), `/mbs-group-membership/
  {extGroupId}` (5G MBS group CRUD) -- found while reading the YAML but not named in the original
  gap-analysis finding; flagged as real, additional, newly-discovered Nudm_PP scope for a future
  turn rather than silently built now or silently left undocumented.

**Real, confirmed-by-reading-the-YAML content-type distinction, not assumed to match**:
`UpdateEeSubscription`'s request body is `application/json-patch+json` (RFC 6902, the same
standard NRF's own `UpdateNFInstance` uses -- `nlohmann::json::patch`), while `Nudm_PP`'s own
`Update` operation is `application/merge-patch+json` (RFC 7396, `nlohmann::json::merge_patch`,
the same real distinction PCF's own `ModAppSession` established for this project in ADR-0080).
Two different real standards for two different real operations in the same file, confirmed by
reading each `requestBody.content` key directly rather than assumed to be uniform.

### Verification -- live, not just build success

Real, live HTTP verification against a running NRF+UDM: `CreateEeSubscription` with a real
`EeSubscription` body (`callbackReference` + `monitoringConfigurations`) -> 201 with real
`Location` header and a `CreatedEeSubscription` wrapping the stored subscription;
`UpdateEeSubscription` with a real RFC 6902 patch op (`replace` on `callbackReference`) -> 200
with the field genuinely changed; `DeleteEeSubscription` with the WRONG `ueIdentity` -> 404
(confirms the real ue-identity-ownership check, not just subscriptionId lookup); the correct
`ueIdentity` -> 204. `GET pp-data` before any provisioning -> 404 (real, correct "no document
yet" behavior, not a fabricated empty success); `PATCH pp-data` with a real merge-patch body ->
200, creating the document on demand (real RFC 7396 behavior: merge-patching a nonexistent
resource starts from an empty object); a subsequent `GET` -> 200 with the change persisted.

Full `conformance_tests`: 268/268 pass, unaffected (no new unit tests -- coverage via the live
HTTP verification above, matching this same session's own established pattern for schema-CRUD-
shaped gap closures). No new NF-level integration test added.

### What this ADR does NOT include

`Nudm_PP`'s three larger resource groups named above (5G VN Group, PP Data Entry, 5G MBS group).
`Nudm_MT`, `Nudm_NIDDAU`, `Nudm_RSDS`, `Nudm_SSAU`, `Nudm_UEID` (separate Nudm services, already
disclosed as deferred in `nfs/udm/src/main.cpp`'s own file header before this ADR, unrelated to
`Nudm_EE`/`Nudm_PP`). Real event-notification delivery for `Nudm_EE` subscriptions (the
`eventOccurrenceNotification` callback) -- created/removed for real, but no trigger path exists
in this lab to ever fire one yet, same disclosed shape as every other proactive-callback gap
already named elsewhere in this project (AMF's own N1N2 notifications, PCF's `PolicyUpdate`).

