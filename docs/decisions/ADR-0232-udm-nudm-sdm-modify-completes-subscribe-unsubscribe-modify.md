## ADR-0232: UDM `Nudm_SDM` Modify -- completes Subscribe/Unsubscribe/Modify

### Context

`Subscribe` (`POST /{ueId}/sdm-subscriptions`) and `Unsubscribe`
(`DELETE /{ueId}/sdm-subscriptions/{subscriptionId}`) were already real and wired (ADR-0069).
`Modify` (`PATCH /{ueId}/sdm-subscriptions/{subscriptionId}`) was the one remaining op in this
group -- confirmed by direct read of `TS29503_Nudm_SDM.yaml`: a real RFC 7396 JSON Merge Patch
(`application/merge-patch+json`, `SdmSubsModification` schema), the same merge-patch shape this
file already uses for every registration resource's own PATCH route. Response is a real `oneOf`
(`SdmSubscription` | `PatchResult`) -- returning the full merged `SdmSubscription` is a real, valid
response per that `oneOf`, same precedent this file's other merge-patch routes already use.

### Implementation

Added `SdmSubscriptionStore::merge_patch(subscription_id, ue_id, patch)` to
`nfs/udm/src/stores.hpp`/`.cpp`, mirroring `AmfRegistrationStore::merge_patch`'s own
`nlohmann::json::merge_patch()` pattern, with a real ownership check baked in (a `subscription_id`
that exists but belongs to a different `ue_id` returns `nullopt`, same real 404 semantics
`Unsubscribe`'s own existing ownership check already applies). New `PATCH` route added to
`nfs/udm/src/main.cpp` right after the existing `Unsubscribe` route.

### Live verification (real, live mTLS + OAuth2, not self-consistency)

New `tests/integration/test_udm_sdm_gap_closure_232.cpp`, 1 test
(`ModifyMergePatchesExistingSubscription`): subscribes, then real merge-patches `expires`,
confirming the response reflects the patched field while unrelated fields
(`nfInstanceId`/`callbackReference`) survive unchanged (real RFC 7396 merge-patch semantics); a
`subscriptionId` that exists but belongs to a different `ueId` real-404s; a genuinely unknown
`subscriptionId` real-404s.

Passes.

### Testing

Full reconfigure + rebuild clean. New test passes individually. Full suite re-run clean at `-j1`
(454/454, 6 legitimately skipped for missing local Postgres env vars, same as always) -- see commit
for exact pass count. No strays before or after any run (`ps aux` checked explicitly).

### What this ADR does NOT include

This closes `Subscribe`/`Unsubscribe`/`Modify` in full. Still open: the 5 SOR/UPU/ack write ops,
the 6-op shared-data family, the 2 identifier-lookup ops, group C2 (2 ops -- see ADR-0233, which
corrects this ADR's own scoping note: UDR already had both resources live, no new UDR stores were
actually needed), and `Nudm_PP` (11 ops, ADR-0082).

