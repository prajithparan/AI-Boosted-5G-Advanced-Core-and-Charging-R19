## ADR-0205: PCF `Npcf_AMPolicyAuthorization` + `Npcf_MBSPolicyAuthorization` + `Npcf_MBSPolicyControl` -- tenth ADR-0193 gap-closure (second PCF slice)

### Context

Second of the PCF Tier-A gap-closure slices (task #163, ADR-0204 was the first). Closes 3 of the
remaining 5 files: `TS29534_Npcf_AMPolicyAuthorization.yaml` (6 ops, `/npcf-am-policyauthorization/v1`,
scope `npcf-am-policyauthorization`), `TS29537_Npcf_MBSPolicyAuthorization.yaml` (4 ops,
`/npcf-mbspolicyauth/v1`, scope `npcf-mbspolicyauth`), and `TS29537_Npcf_MBSPolicyControl.yaml` (4
ops, `/npcf-mbspolicycontrol/v1`, scope `npcf-mbspolicycontrol`) -- all real api roots and scopes
confirmed directly from each YAML's own `servers:`/`securitySchemes` blocks. 2 PCF Tier-A files
remain (`Npcf_PDTQPolicyControl`, `Npcf_BDTPolicyControl`), tracked as the final slice under task
#163.

Real, honest codegen limitation confirmed (not a bug): `AppAmContextRespData` and
`AmEventsSubscRespData` are both real `anyOf`-of-two-full-object-schemas response shapes (either
the full resource representation, or that representation plus an embedded event notification).
`tools/sbi-codegen` has an existing, disclosed "OPAQUE FALLBACK" path for exactly this case --
both typedef to plain `nlohmann::json` rather than a synthesized merge type (confirmed by reading
the generated header's own comment, not assumed). Routes return the real stored representation
directly; this build never has a real event to embed anyway (see Implementation below), so the
`anyOf`'s second arm is never exercised. The same real pattern was already used for
`Npcf_PolicyAuthorization`'s own `EventsSubscPutData` opaque shape (this file's pre-existing code).

Real name collision found and fixed wiring `Npcf_MBSPolicyAuthorization`: `MbsExtProblemDetails`
is independently declared by the already-wired `TS29521_Nbsf_Management.yaml` too (an unrelated
real BSF schema of the same name -- not MBS-related despite the name). Disambiguated by the
codegen to `MbsExtProblemDetails_Npcf_MBSPolicyAuthorization`/`MbsExtProblemDetails_Nbsf_Management`;
`nfs/bsf/src/main.cpp`'s own pre-existing bare-name reference updated to the disambiguated name --
a real, necessary fix required by this pilot-set change, not a functional change to BSF's own
behavior (verified via BSF's own pre-existing DTO conformance tests, unaffected).

### Implementation

- **`Npcf_AMPolicyAuthorization`** (6 ops), backed by a new `AppAmContextStore`
  (`nfs/pcf/src/stores.hpp/.cpp`) -- same `nlohmann::json`-store shape as `AmPolicyStore`/
  `AppSessionStore` above. `PostAppAmContexts`: real structural validation of `AppAmContextData`'s
  required `supi`/`termNotifUri`, real `201` + `Location`. `GetAppAmContext`: real get/`404`.
  `ModAppAmContext`: real merge-patch application of `AppAmContextUpdateData`'s optional fields
  onto the stored context, real `200`. `DeleteAppAmContext`: real get/`404`-then-remove/`204`.
  `updateAmEventsSubsc`/`DeleteAmEventsSubsc`: the `events-subscription` subresource is stored
  nested inside the context's own `evSubsc` field -- same real approach the pre-existing
  `Npcf_PolicyAuthorization` app-sessions events-subscription routes already use, real `201`
  (first PUT) vs `200` (subsequent PUT) distinguished by whether `evSubsc` was already present,
  real `204` delete.
- **`Npcf_MBSPolicyAuthorization`** (4 ops), backed by a new `MbsAppSessionStore`.
  `CreateMBSAppSessionCtxt`: real structural validation of `MbsAppSessionCtxt`'s required
  `mbsSessionId`, real `201` + `Location`. `GetMBSAppSessionCtxt`: real get/`404`.
  `ModifyMBSAppSessionCtxt`: real merge-patch of `MbsAppSessionCtxtPatch`'s only field
  (`mbsServInfo`) onto the stored context, real `200`. `DeleteMBSAppSessionCtxt`: real
  get/`404`-then-remove/`204`.
- **`Npcf_MBSPolicyControl`** (4 ops), backed by a new `MbsPolicyStore`. `CreateMBSPolicy`: real
  structural validation of `MbsPolicyCtxtData`'s required `mbsSessionId`, wrapped into a real
  `MbsPolicyData{mbsPolicyCtxtData}`, real `201` + `Location`. `GetIndMBSPolicy`: real get/`404`.
  `DeleteIndMBSPolicy`: real get/`404`-then-remove/`204`. `UpdateIndMBSPolicy`: real structural
  validation of `MbsPolicyCtxtDataUpdate`, applies `mbsServInfo` onto the stored
  `mbsPolicyCtxtData`, real `200`. Disclosed: `mbsPcrts`/`mbsErrorReport` in the update request
  are accepted (structurally validated) but never applied to any real `mbsPolicies` PCC-rule/QoS
  decision data -- no real MBS PCC-rule/QoS decision engine exists in this build, same class of
  gap as this file's own pre-existing SM/AM policy fixed-default disclosure.

### Live verification (real, live mTLS + OAuth2, not self-consistency)

New `tests/integration/test_pcf_am_mbs_policy_auth_control.cpp`, real spawned `nrf`+`pcf`
processes over real TLS 1.3 + mTLS: `Npcf_AMPolicyAuthorization` create->read->update->delete full
lifecycle plus its own events-subscription sub-lifecycle (real `201` on first PUT, real `200` on
repeat PUT, real `204` delete) -- real `404` on repeat context delete, real `400` on a body
missing `termNotifUri`; `Npcf_MBSPolicyAuthorization` create->read->modify->delete full lifecycle
-- real `404` on repeat delete, real `400` on a body missing `mbsSessionId`;
`Npcf_MBSPolicyControl` create->read->update->delete full lifecycle -- real `404` on repeat
delete, real `400` on a body missing `mbsSessionId`. All 6 new tests pass; all pre-existing PCF
tests (10 across `test_pcf_policy_control.cpp` and `test_pcf_ue_policy_event_exposure.cpp`)
re-verified passing unchanged.

### Testing

Full reconfigure + rebuild against the complete pilot set clean (`EXIT=0` verified directly from
the build log at every step, including after the real BSF collision fix). All 6 new PCF tests
pass; full suite re-run clean: 402/402 passing (`ctest --timeout 120 -E
"UdrIntegration.AmfContextLifecycle|UdmIntegration.SdmDataRetrievalAndSubscriptions"`, matching
`.github/workflows/ci.yml`'s own exclusion). No strays before or after any run (`ps aux` checked
explicitly).

### What this ADR does NOT include

The final 2 PCF Tier-A gaps (`Npcf_PDTQPolicyControl`, `Npcf_BDTPolicyControl`) -- tracked as the
last slice under task #163. No real MBS PCC-rule/QoS decision engine behind
`Npcf_MBSPolicyControl`'s update path. No real event notification delivery for either
`Npcf_AMPolicyAuthorization`'s or `Npcf_MBSPolicyAuthorization`'s own callback shapes (both
deliberately deferred, same disclosed class of gap as every other proactive/callback flow in this
project).

