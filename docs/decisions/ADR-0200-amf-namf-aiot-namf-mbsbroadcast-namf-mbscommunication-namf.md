## ADR-0200: AMF `Namf_AIoT` + `Namf_MBSBroadcast` + `Namf_MBSCommunication` + `Namf_MT` -- fifth ADR-0193 gap-closure

### Context

Found during ADR-0193's audit: all 4 of `TS29518_Namf_AIoT.yaml`, `TS29518_Namf_MBSBroadcast.yaml`,
`TS29518_Namf_MBSCommunication.yaml`, `TS29518_Namf_MT.yaml` were real Tier-A gaps -- never added
to `libs/sbi-generated/CMakeLists.txt`'s pilot set at all (unlike ADR-0199's two, which were wired
but unrouted). All 4 added to the pilot set this pass; reconfigure + full rebuild against the
complete pilot set confirmed real, collision-free standalone headers per file, with the one
expected real collision (`N2MbsSmInfo`, declared separately by both `Namf_MBSBroadcast` and
`Namf_MBSCommunication`) disambiguated by the codegen itself to
`N2MbsSmInfo_Namf_MBSBroadcast`/`N2MbsSmInfo_Namf_MBSCommunication` -- confirmed by reading the
actual generated headers, not assumed.

### Implementation

- **`Namf_AIoT`** (api root `/namf-aiot/v1`, 1 op): `MessageDelivery` accepts its real declared
  dual encoding (`application/json` and `multipart/related`), dispatched on the real Content-Type
  header via `sbi_core::multipart::is_multipart_related` (already used by
  `parse_multipart_json_body` internally); real structural validation of `AiotMessageReq`'s
  required fields (`ranNodeId`/`aiotMessage`/`aiotMessageType`/`notifUri`/`aiotfId`/
  `correlationId`), then a real `204`. Disclosed: TS 23.369 Ambient IoT is a Tier 3 R19 feature
  per CLAUDE.md and this lab has no real device/NG-RAN stack behind it -- the message is honestly
  accepted-for-processing, not actually forwarded anywhere.
- **`Namf_MBSCommunication`** (api root `/namf-mbs-comm/v1`, 1 op): `N2MessageTransfer`, real
  multipart/related-only structural validation (`MbsN2MessageTransferReqData`'s required
  `mbsSessionId`+`n2MbsSmInfo`), then a real `200` with `result=N2_INFO_TRANSFER_INITIATED` --
  same disclosed "no real N2 delivery pipeline" simplification as `Namf_Communication`'s own
  `NonUeN2MessageTransfer`.
- **`Namf_MBSBroadcast`** (api root `/namf-mbs-bc/v1`, 3 ops), backed by a new
  `MbsBroadcastContextStore` (`IdKeyedStore<sbi_gen::ContextCreateReqData>`, `subscriptions.hpp`
  -- reusing the existing generic id-keyed-store template for a non-subscription resource, same
  template CLAUDE.md's "three similar lines" bar already justified reusing repeatedly this
  session): `ContextCreate` does real structural validation (multipart-only
  `ContextCreateReqData`'s required `mbsSessionId`/`n2MbsSmInfo`/`notifyUri`/`snssai`; the real
  `oneOf[mbsServiceArea, mbsServiceAreaInfoList]` constraint is NOT enforced -- disclosed, same
  class of un-enforced `oneOf`/`not` constraint as elsewhere in this project), assigns a real
  server-side `mbsContextRef`, real `201`. `ContextDelete` is a real get/404-then-remove/204
  (mirrors `ReleaseUEContext`'s own precedent). `ContextUpdate` does real get/404 against the
  stored context, real structural validation of the update body, then a real `204` -- disclosed:
  the update body's fields are validated but NOT merged into stored state, since (same as Create)
  there is no real N2 consumer downstream that would ever read the merged result in this lab
  build.
- **`Namf_MT`** (api root `/namf-mt/v1`, 3 ops): `ProvideDomainSelectionInfo` (GET) checks the
  real `UeContextStore` -- real `404` if absent, else an honestly-empty `UeContextInfo{}` `200`
  (same disclosed no-VoPS/RAT-data gap as `Namf_Location`'s `ProvideLocationInfo`, ADR-0199).
  `EnableUeReachability` checks `UeContextStore` -- real `404` if absent, else a real ack (echoes
  the requested `reachability` value back in `EnableUeReachabilityRspData`) -- disclosed: no real
  paging/reachability state machine exists to actually change UE reachability, this only
  acknowledges the request. `EnableGroupReachability` does real structural validation
  (`ueInfoList`+`tmgi` required) then a real `200` with an honestly-empty `ueConnectedList` -- no
  real per-UE reachability tracking exists to populate it from.

### Live verification (real, live mTLS + OAuth2, not self-consistency)

New `tests/integration/test_amf_namf_aiot_mbs_mt.cpp`, real spawned `nrf`+`amf` processes, real
HTTP/2 client over TLS 1.3 + mTLS: AIoT `MessageDelivery` -> real `204` via both
`application/json` and `multipart/related` bodies, real `400` on a body missing `correlationId`;
MBS `N2MessageTransfer` -> real `200` with a verified `N2_INFO_TRANSFER_INITIATED`; MBS Broadcast
`ContextCreate` -> real `201` with a verified `mbsSessionId.ssm.sourceIpAddr.ipv4Addr` echoed back
-> `ContextUpdate` `204` -> `ContextUpdate` against a nonexistent ref -> real `404` ->
`ContextDelete` `204` -> `ContextDelete` again -> real `404`; MT `ProvideDomainSelectionInfo` ->
real `404` before a UE context exists, real `200` with a verified-empty `UeContextInfo` after a
real multipart `CreateUEContext`; `EnableUeReachability` -> real `404` before a UE context exists,
real `200` with a verified-echoed `reachability` after; `EnableGroupReachability` -> real `200`
with a verified-absent `ueConnectedList`, real `400` on a body missing `tmgi`.

### Testing

Full reconfigure + rebuild against the complete pilot set clean (`EXIT=0` verified directly from
the build log, both the pilot-set-addition rebuild and the subsequent handler-code rebuild run as
single non-overlapping `cmake --build` invocations per this session's own established discipline).
All 16 AMF integration tests pass (10 pre-existing + 6 new), verified with a narrowed
suite-name-anchored `ctest -R` this time (not the broad substring regex that incidentally caught
`UdrIntegration.AmfContextLifecycle` during ADR-0199's pass) -- no strays before or after (`ps aux`
checked).

### What this ADR does NOT include

No real Ambient IoT device/NG-RAN forwarding capability (AIoT `MessageDelivery` always honestly
`204`s without delivering anything). No real N2 delivery pipeline for MBS broadcast/communication
(same standing gap class as every other N2-adjacent operation in this AMF). No real merge of
`ContextUpdate`'s fields into `MbsBroadcastContextStore`'s stored state. No real UE reachability
state machine (`EnableUeReachability` is an ack, not an actual paging trigger;
`EnableGroupReachability` never populates `ueConnectedList`). The real `oneOf`/`not` structural
constraints in `ContextCreateReqData`/`ContextUpdateReqData` are not enforced beyond what the
generated DTO's own required-field list captures. `Namf_MT`'s own `ProvideDomainSelectionInfo`
query parameters (`info-class`, `supported-features`, `old-guami`) are accepted but not used to
filter the response, since the response is honestly empty regardless. `Namf_OAM` (a real free5GC
capability with no corresponding YAML found in this project's own `specs/5G_APIs-REL-19/` archive,
checked directly this pass -- see `docs/CAPABILITY_GAP_ANALYSIS.md`) remains out of scope, not
silently assumed unavailable.

