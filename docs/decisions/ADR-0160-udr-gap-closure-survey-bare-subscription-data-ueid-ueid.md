## ADR-0160: UDR gap-closure survey -- bare `/subscription-data/{ueId}`/`{ueId}/context-data`
confirmed blocked, `GetSSAuData` investigated in depth and deliberately deferred (no code change)

### Context

With all straightforward CRUD-shaped `Nudr_DataRepository` resources closed as of ADR-0159 (69
real resources, well past free5GC's ~42+), this turn surveyed the remaining real candidates named
in prior ADRs' own "still deferred" lists: bare `/subscription-data/{ueId}`, bare
`/subscription-data/{ueId}/context-data`, and `GetSSAuData`. Presented to the user via
`AskUserQuestion` before investing further effort (three options: investigate `GetSSAuData`
further, start building real array-query-param parsing infra, or pause UDR gap-closure here).
**User's explicit answer: "Investigate GetSSAuData further."**

### Bare `/subscription-data/{ueId}` and `{ueId}/context-data` -- confirmed blocked

Direct read confirms both remain genuinely blocked, matching the same unsupported-parsing class
already disclosed for `pdtq-data`/`nf-group-ids`:
- `/subscription-data/{ueId}` (`QueryUeSubscribedData`): real `style: form, explode: false` array
  query params `dataset-names`, `adjacent-plmns`, `ext-group-ids`.
- `/subscription-data/{ueId}/context-data` (`QueryContextData`): a real, REQUIRED
  `context-dataset-names` array query param (same `style`/`explode` encoding) -- required, so
  this project's own "optional filter not honored" precedent doesn't apply here.

No code change; disclosed in `nfs/udr/src/main.cpp`'s own deferred-list header comment.

### `GetSSAuData` -- investigated in depth, real findings

`GetSSAuData` (`/subscription-data/{ueId}/service-specific-authorization-data/{serviceType}`,
GET-only, distinct from the already-implemented `context-data/service-specific-authorizations/
{serviceType}` CRUD sibling) cites TS29505's own `AuthorizationData` schema for its `200`
response. Cross-reading `TS29503_Nudm_SSAU.yaml` (the real service this UDR resource's own
`serviceType` parameter is scoped by) and `TS29503_Nudm_NIDDAU.yaml` (the service `AuthorizationData`
is actually shaped after) found:

- `Nudm_SSAU`'s own real `ServiceType` enum is `AF_GUIDANCE_FOR_URSP`/`AF_REQUESTED_QOS`/
  `AF_PROVISION_N3GPP_DEV_ID_INFO` -- nothing NIDD-related.
- `Nudm_SSAU` has its own purpose-built `ServiceSpecificAuthorizationData` schema
  (`authorizationUeId{supi,gpsi}`/`extGroupId`/`intGroupId`/`authId`) that semantically matches
  this resource's own name and `serviceType` scoping.
- `Nudm_NIDDAU`'s real API has no `serviceType` parameter at all (`/{ueIdentity}/authorize`) --
  NIDD authorization isn't modeled as a `serviceType` value in that service.
- TS29505's own `AuthorizationData` (the schema `GetSSAuData` actually cites) is explicitly
  described as **"NIDD Authorization Information"**, and its `authorizationData` field literally
  cross-references `TS29503_Nudm_NIDDAU.yaml#/components/schemas/UserIdentifier`.

Presented via `AskUserQuestion` (return `ServiceSpecificAuthorizationData` treating the citation
as a real typo [recommended, same precedent as ADR-0129/ADR-0154]; return the literal
NIDD-shaped `AuthorizationData`; skip and leave deferred). **User's explicit answer: "Return
ServiceSpecificAuthorizationData (Recommended)."**

**A second, deeper finding surfaced before implementing:** the already-implemented PUT/PATCH/DELETE
sibling (`CreateServiceSpecificAuthorizationInfo` etc., under `context-data/
service-specific-authorizations/{serviceType}`) stores a **third, different** schema --
`ServiceSpecificAuthorizationInfo` (TS29505's own type: a map of `authId` ->
`TS29503_Nudm_NIDDAU.yaml#/components/schemas/AuthorizationInfo`, itself NIDD-cross-referencing
but a genuinely different shape from both `AuthorizationData` and `ServiceSpecificAuthorizationData`).
`GetSSAuData` has no PUT counterpart of its own. Returning `ServiceSpecificAuthorizationData` (the
user's just-confirmed choice) from data actually stored as `ServiceSpecificAuthorizationInfo`
would require either fabricating an undocumented field mapping between the two structurally
unrelated schemas (`authorizationUeId`/`extGroupId`/`intGroupId`/`authId` has no documented
correspondence to a map of `authId` -> NIDD `AuthorizationInfo`) -- a real violation of this
project's own never-invent-a-field rule -- or building a second, disconnected,
`ServiceSpecificAuthorizationData`-shaped store with no write path, which would always return
`404` for every real UE since nothing populates it: a stub with no real value.

Presented via a second, focused `AskUserQuestion` (leave deferred [recommended]; build the
disconnected always-empty store anyway, disclosed as a stub). **User's explicit answer: "Leave
this resource deferred (Recommended)."**

### What this ADR does NOT include

No code changed beyond the disclosure comment in `nfs/udr/src/main.cpp`'s own deferred-list
header (rebuilt clean, zero warnings, to confirm the comment-only change compiles). No new
resource closed, no `docs/TRACEABILITY.md` entry (no new test), no
`docs/CAPABILITY_GAP_ANALYSIS.md` resource-count change -- UDR remains at 69 real
`Nudr_DataRepository` resources as of ADR-0159. This ADR exists to preserve the real research
findings and both user decisions above, so a future session doesn't need to re-investigate
`GetSSAuData` or the two bare resources from scratch. Task #106's remaining real, disclosed gaps
after this survey: `group-data`'s genuinely-blocked bare collection GETs, bare
`/subscription-data/{ueId}`/`{ueId}/context-data`, `GetSSAuData` (deliberately deferred per the
findings above), and the other genuinely deferred subsystems (`pdtq-data`,
`mbs-session-pol-data`, `nidd-authorization-data`, `Nudr_GroupIDmap`'s own `/nf-group-ids`).

