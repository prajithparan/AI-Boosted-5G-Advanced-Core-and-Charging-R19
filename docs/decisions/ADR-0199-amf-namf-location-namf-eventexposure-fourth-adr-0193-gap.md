## ADR-0199: AMF `Namf_Location` + `Namf_EventExposure` -- fourth ADR-0193 gap-closure

### Context

Found during ADR-0193's audit: both `TS29518_Namf_Location.yaml` and
`TS29518_Namf_EventExposure.yaml` were already in `libs/sbi-generated/CMakeLists.txt`'s pilot set
(their types compiled into `sbi_gen` all along) but had zero routes wired in
`nfs/amf/src/main.cpp` -- a Tier-A gap (whole API never wired), not a Tier-B one. Both are real,
separate api roots from `nfs/amf`'s existing `Namf_Communication` surface: `/namf-loc/v1` and
`/namf-evts/v1` respectively, confirmed via each YAML's own `servers:` block, not assumed to share
`kApiRoot`.

### Implementation

**`Namf_Location`** (3 operations, all real structural validation via generated DTOs):
- `ProvidePositioningInfo` (`POST /{ueContextId}/provide-pos-info`): validates `RequestPosInfo`
  (real required fields `lcsClientType`+`lcsLocation`), then a real, disclosed `501` -- same class
  of gap as LMF's own `DetermineLocation` (ADR-0191): no real LPP/GNSS/PRU positioning capability
  exists anywhere in this build to answer from.
- `ProvideLocationInfo` (`POST /{ueContextId}/provide-loc-info`): validates `RequestLocInfo` (every
  field in this real schema is optional, so any parseable JSON body succeeds structurally), then
  checks `ue_contexts.get(ueContextId)` -- real `404` if absent (mirrors `ReleaseUEContext`'s own
  established get/404 pattern), else a real but honestly-empty `ProvideLocInfo{}` `200`, because
  `UeContextStore` only ever persists `{"ueContextId": ...}` (see `CreateUEContext`,
  `ue_context_store.hpp`'s own header) -- there is no real RAT-type/location/timezone data
  anywhere in this AMF to populate the response from.
- `CancelLocation` (`POST /{ueContextId}/cancel-pos-info`): validates `CancelPosInfo` (real
  required fields `supi`+`hgmlcCallBackURI`+`ldrReference`), then a real `404` -- since
  `ProvidePositioningInfo` above always `501`s, no real `ldrReference` is ever issued, so no
  cancellation request can ever match one. Same precedent as LMF/GMLC's own cancel-path gaps.

**`Namf_EventExposure`** (6 operations, real full subscription CRUD): `CreateSubscription`
(`POST /subscriptions`), `ModifySubscription` (`PATCH /subscriptions/{id}`, real RFC 6902 JSON
Patch via `nlohmann::json::patch()` -- `AmfUpdateEventSubscriptionItem`/`AmfUpdateEventOptionItem`
are real `op`/`path`/`value`-shaped per the YAML, so no special-case parsing is needed beyond
standard `.patch()`), `DeleteSubscription` (`DELETE /subscriptions/{id}`), and their exact
`CreateAMFSetLevelBulkSubscription`/`ModifyAMFSetLevelBulkSubscription`/
`DeleteAMFSetLevelBulkSubscription` counterparts at `/set-subscriptions[/{id}]`. Both families
share the same real `AmfEventSubscription`/`AmfCreateEventSubscription`/
`AmfCreatedEventSubscription`/`AmfUpdatedEventSubscription` schemas per the YAML, but are kept as
two separate `IdKeyedStore<sbi_gen::AmfEventSubscription>` instances (`nfs/amf/src/
subscriptions.hpp`) with distinct id prefixes -- genuinely separate resource collections/id spaces
per `TS29518_Namf_EventExposure.yaml`, same "distinct resource, not a rename" precedent as UDR's
`OperatorSpecificDataStore` vs `PolicyOperatorSpecificDataStore` (ADR-0197). Verified live that the
two id spaces are genuinely isolated (an individual-family `DELETE` against a set-level-only id
correctly `404`s, not accidentally succeeding).

Same disclosed notification-delivery gap as this file's other 3 subscription types
(`subscriptions.hpp`'s own header): subscriptions are stored/modified/removed for real, but
nothing in this lab build ever fires one (no NGAP/N2, no multi-AMF deployment, no real trigger for
`AmfEventSubscription`'s own `eventList` -- registration/connectivity/reachability/etc. -- to
originate from).

### Live verification (real, live mTLS + OAuth2, not self-consistency)

New `tests/integration/test_amf_namf_location_eventexposure.cpp`, real spawned `nrf`+`amf`
processes, real HTTP/2 client over TLS 1.3 + mTLS: `ProvidePositioningInfo` -> real `501` on a
structurally valid body, real `400` on a body missing `lcsLocation`; `ProvideLocationInfo` -> real
`404` before any UE context exists, real `200` with an honestly-empty `ProvideLocInfo` (verified
`ratType`/`location` both absent) after a real multipart `CreateUEContext`; `CancelLocation` ->
real `404`; `Namf_EventExposure`'s individual family -> real `201`-with-generated-fields ->
`PATCH` `200` with a verified-changed `notifyCorrelationId` -> `PATCH` against a nonexistent id
-> real `404` -> `DELETE` `204` -> `DELETE` again -> real `404`; the set-level family -> same
create/modify/delete lifecycle, plus the cross-family isolation check above.

### Testing

Full project rebuild clean (`cmake --build build`, real `EXIT=0` checked directly from the build
log, not the background-task wrapper's own reported code). All 10 AMF integration tests pass
(5 pre-existing `Namf_Communication` + 5 new). Full-suite `ctest` was not re-run end-to-end this
pass: a broad `-R "amf|Amf|AMF"` regex incidentally caught `UdrIntegration.AmfContextLifecycle`
(matches on "Amf" as a substring) and that test hung indefinitely -- reproduced in isolation with a
bounded `timeout 60`, confirmed pre-existing (this session made no changes to `nfs/udr`) and
unrelated to this ADR's scope. Disclosed, not fixed here; tracked as a new backlog item for
separate root-causing.

### What this ADR does NOT include

No real LPP/GNSS/PRU positioning capability (same standing gap as GMLC ADR-0189, LMF ADR-0191).
No real per-UE RAT-type/location/timezone persistence in AMF (`UeContextStore` only ever stores
`{"ueContextId": ...}`) -- `ProvideLocationInfo`'s honestly-empty response is a direct consequence,
not a new simplification introduced here. No real event notification delivery for
`Namf_EventExposure`'s subscriptions (same disclosed gap class as this file's other subscription
types). The pre-existing `UdrIntegration.AmfContextLifecycle` test hang is not investigated or
fixed by this ADR.

