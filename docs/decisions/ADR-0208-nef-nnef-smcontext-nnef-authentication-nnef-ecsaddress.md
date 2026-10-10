## ADR-0208: NEF `Nnef_SMContext` + `Nnef_Authentication` + `Nnef_ECSAddress` -- thirteenth ADR-0193 gap-closure (second NEF slice)

### Context

Second of NEF's own Tier-A gap-closure slices (task #164, continuing directly from ADR-0207's
first slice per the user's explicit "continue one after the other" instruction -- no
per-slice confirmation pause). Closes 3 more of the remaining 9 real NEF YAML files:
`TS29541_Nnef_SMContext.yaml` (4 ops, `/nnef-smcontext/v1`), `TS29256_Nnef_Authentication.yaml`
(1 op, `/nnef-authentication/v1`), and `TS29591_Nnef_ECSAddress.yaml` (5 ops,
`/nnef-ecs-addr-cfg-info/v1`) -- 10 real operations, all api roots confirmed directly from each
YAML's own `servers:` block. 6 of NEF's remaining 9 real Tier-A gaps close after this slice
(6 remain: `Nnef_EventExposure`, `Nnef_Inference`, `Nnef_TrafficInfluenceData`, `Nnef_Training`,
`Nnef_VFLInference`, `Nnef_VFLTraining`).

Confirmed while reading `TS29541_Nnef_SMContext.yaml`: its own real api root/scope
(`/nnef-smcontext/v1`, `nnef-smcontext`) is exactly the scope name `TS29541_Nnef_SMService.yaml`
declared in its own `securitySchemes` block (ADR-0207's disclosed "quirk"). Both files share the
same TS 29.541 source document -- the "mismatch" flagged in ADR-0207 was `Nnef_SMService` pointing
at its sibling service's real scope name, not a spec error; left as-is (already used verbatim in
ADR-0207, no further action needed).

Real cross-NF codegen name collisions found and fixed (a new class of consequence not previously
seen in this project's own prior 12 gap-closure ADRs): adding `TS29541_Nnef_SMContext.yaml` and
`TS29256_Nnef_Authentication.yaml` to the pilot set collided not just with NEF's own existing
types (the usual, expected pattern from ADR-0204/ADR-0205) but with **other, unrelated NFs'**
already-built, already-shipping code:
- `SmContextCreateData`/`SmContextCreatedData`/`SmContextUpdateData`/`SmContextReleaseData`/
  `SmContextStatusNotification` (defined by `TS29502_Nsmf_PDUSession.yaml`, folded into
  `TS29122_CommonData_grp.hpp`) collided with `Nnef_SMContext`'s own same-named schemas -- both
  sides got suffixed (`_Nsmf_PDUSession` / `_Nnef_SMContext`). Broke 6 real, pre-existing bare-name
  references in `nfs/smf/src/main.cpp` (SMF's own real PDU Session Create/Update/Release routes)
  and 1 in `nfs/amf/src/ngap_task.cpp` (AMF's own real client call into SMF), plus 1 in
  `tests/integration/test_smf_pdu_session.cpp` -- all fixed to the newly-suffixed
  `_Nsmf_PDUSession` names.
- `DeliverReqData` (already suffixed `_Nsmf_NIDD` from ADR-0201's own turn, since it already
  collided once before with `Nsmf_PDUSession`'s own `DeliverReqData`) picked up a third real
  same-named schema from `Nnef_SMContext`'s own `Deliver` operation -- both now
  `_Nsmf_NIDD`/`_Nnef_SMContext`. Fixed 1 bare-name reference in `nfs/smf/src/main.cpp` (SMF's own
  real NIDD MO-data delivery route).
- `AuthResult` (defined by `TS29509_Nausf_UEAuthentication.yaml`, its own real
  `AUTHENTICATION_SUCCESS`/`AUTHENTICATION_FAILURE` enum) collided with `Nnef_Authentication`'s own
  same-named-but-differently-valued `AuthResult` (`AUTH_SUCCESS`/`AUTH_FAIL`) -- both suffixed
  (`_Nausf_UEAuthentication` / `_Nnef_Authentication`). Fixed 8 real, pre-existing bare-name
  references across `nfs/ausf/src/main.cpp` (AUSF's own real 5G-AKA/EAP-AKA' confirmation routes),
  1 in `nfs/amf/src/ngap_task.cpp` (AMF's own real client call into AUSF), and 4 in
  `tests/integration/test_ausf_ue_authentication.cpp`.

All fixes are pure rename-to-the-new-real-generated-name; no behavioral change to SMF/AMF/AUSF's
existing logic. Confirmed via direct `grep` for every bare-name occurrence project-wide before and
after, and via the full `ctest` run below (SMF/AMF/AUSF's own existing test suites all still pass
unchanged).

### Implementation

- **`Nnef_SMContext`**, backed by a new `SmContextStore` (`nfs/nef/src/stores.hpp/.cpp`), keyed by
  an NEF-generated `smContextId`. `Create`: real structural validation of
  `SmContextCreateData_Nnef_SMContext`'s required `supi`/`pduSessionId`/`dnn`/`snssai`/`nefId`/
  `dlNiddEndPoint`/`notificationUri`, real `201` + `Location`, response body echoes only the fields
  the real `SmContextCreatedData_Nnef_SMContext` shares with the request (`rdsSupport`/
  `extBufSupport`/`supportedFeatures`/`maxPacketSize` left unset -- no real small-data-rate/
  feature-negotiation logic exists in this build). `Delete` (`/release`): real get/`404`-then-
  remove/`204` -- an honest `204`, not a fabricated `200` with an empty-but-typed
  `SmContextReleasedData`, since no real small-data-rate-control tracking exists to report.
  `Update`: real partial-field merge via `SmContextStore::update`, `404`/`204`. `Deliver`: real
  `multipart/related` parse (`jsonData` = `DeliverReqData_Nnef_SMContext`, `binaryMoData` matched
  by `Content-Id`), `404` if the SM context doesn't exist, `204` on success -- same disclosed gap
  as `Nnef_SMService`'s `SendSMS` (ADR-0207): no real onward NIDD MT/MO relay exists, so this is
  NEF-level acceptance only.
- **`Nnef_Authentication`**: `POST /uav-authentications`, real structural validation of
  `UAVAuthInfo`'s required `gpsi`/`serviceLevelId`/`nfType`, then a real, honest `403`
  `UAVAuthFailure` (one of only two response bodies this operation's own YAML actually defines,
  unlike every other NEF gap closed so far in this project where an honest `501` was available --
  it is not here) -- this NEF has no real UAS-NF/USS authentication backend to authenticate a UAV
  against (TS 23.256's own Uncrewed Aerial Systems architecture is out of scope for this build), so
  a real, spec-documented failure response is the honest choice, not a fabricated `200` success.
- **`Nnef_ECSAddress`**, backed by a new `EcsAddrCfgInfoSubStore`. `CreateIndividualSubcription`:
  real structural validation of `EcsAddrCfgInfoSub`'s required `notifUri`/`notifCorrId`, real
  `201` + `Location`. `GetIndividualSubcription`: real get/`404`.
  `ReplaceIndividualSubcription` (PUT): real full-replace, `200` + body/`404`.
  `ModifyIndividualSubcription` (PATCH): real RFC 7396 JSON Merge Patch via nlohmann::json's own
  native `merge_patch` -- matches the YAML's own declared `application/merge-patch+json` request
  content type exactly (not RFC 6902 JSON Patch, which several of this project's other merge-style
  UDR routes use instead per their own YAMLs -- confirmed per-operation from each YAML's own
  declared content type, not assumed). Validated against the real generated
  `EcsAddrCfgInfoSubPatch` shape first, then applied as a merge patch, same precedent as PCF/UDR's
  own merge-patch routes. `DeleteIndividualSubcription`: real get/`404`-then-remove/`204`.

### Live verification (real, live mTLS + OAuth2, not self-consistency)

New `tests/integration/test_nef_smcontext_authentication_ecsaddress.cpp`, 5 tests, real spawned
`nrf`+`nef` processes over real TLS 1.3 + mTLS: `Nnef_SMContext` full create->update->deliver(real
multipart)->release lifecycle (real `204` at each step, real `404` on a second release attempt),
plus a real `400` on a create body missing `dlNiddEndPoint`/`notificationUri`; `Nnef_Authentication`
real `403` `UAVAuthFailure` verified end-to-end; `Nnef_ECSAddress` full
create->read->put->patch->delete lifecycle (real RFC 7396 merge-patch verified to leave
untouched fields alone -- `notifUri` survives a patch that only names `notifCorrId`), real `404`
on repeat delete, real `400` on a create body missing `notifCorrId`. All 5 new tests pass.

### Testing

Full reconfigure + rebuild against the complete pilot set clean (`EXIT=0` verified directly from
the build log at every step). One real build error caught and fixed along the way: this
nlohmann::json version's `merge_patch` mutates in place and returns `void`, not a new value --
`EcsAddrCfgInfoSubStore::patch` initially tried `it->second = it->second.merge_patch(...)`, fixed
to a bare `it->second.merge_patch(...)` call. All 5 new NEF tests pass individually; `SmfIntegration`
(9 tests), `SmfNiddIntegration` (1 test), `AmfIntegration` (5 tests), and `AusfIntegration`/
`AusfUpuProtectionIntegration` (7 tests) re-run explicitly to confirm the cross-NF rename fixes
above didn't regress SMF/AMF/AUSF's own existing behavior -- all pass. Full suite re-run clean:
417/417 passing (`ctest --timeout 120 -E
"UdrIntegration.AmfContextLifecycle|UdmIntegration.SdmDataRetrievalAndSubscriptions"`, matching
`.github/workflows/ci.yml`'s own exclusion). No strays before or after any run (`ps aux` checked
explicitly).

### What this ADR does NOT include

The remaining 6 NEF Tier-A gaps (`Nnef_EventExposure`, `Nnef_Inference`,
`Nnef_TrafficInfluenceData`, `Nnef_Training`, `Nnef_VFLInference`, `Nnef_VFLTraining`) -- tracked as
follow-up slices under task #164. No real UAS-NF/USS authentication backend
(`Nnef_Authentication`'s own disclosed consequence). No real onward NIDD MT/MO relay
(`Nnef_SMContext`'s `Deliver` disclosed consequence, same class as `Nnef_SMService`'s `SendSMS`).
No wiring between this NEF-facing `Nnef_SMContext` resource and SMF's own real
`Nsmf_PDUSession`/`Nsmf_NIDD` implementations -- the two remain real, standalone, independently
correct-to-their-own-YAML resource lifecycles, not integrated end-to-end (a real, disclosed
deferral, same class already accepted for `Nnef_PFDmanagement`/UPF PFD management in ADR-0207).

