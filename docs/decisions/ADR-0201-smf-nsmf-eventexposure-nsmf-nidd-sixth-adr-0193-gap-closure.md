## ADR-0201: SMF `Nsmf_EventExposure` + `Nsmf_NIDD` -- sixth ADR-0193 gap-closure

### Context

Found during ADR-0193's audit: neither `TS29508_Nsmf_EventExposure.yaml` nor
`TS29542_Nsmf_NIDD.yaml` (real filename confirmed via `ls specs/5G_APIs-REL-19/` -- not
`TS29578_Nsmf_NIDD.yaml`, an earlier guess based on filename-numbering pattern, corrected before
any code was written) had ever been added to the sbi-codegen pilot set -- a real Tier-A gap,
explicitly named and deferred in `nfs/smf/src/main.cpp`'s own file header since this project's
original SMF turn. Real, separate api roots confirmed from each YAML's own `servers:` block:
`/nsmf-event-exposure/v1`, `/nsmf-nidd/v1`.

`Nsmf_EventExposure`'s own `NsmfEventExposure` schema (the request/response body for 3 of its 4
operations) has a large real cross-file dependency fan-out -- `TS29503_Nudm_EE.yaml`,
`TS29554_Npcf_BDTPolicyControl.yaml`, `TS29564_Nupf_EventExposure.yaml`,
`TS29514_Npcf_PolicyAuthorization.yaml`, `TS29517_Naf_EventExposure.yaml`,
`TS29522_TrafficInfluence.yaml`, `TS29520_Nnwdaf_EventsSubscription.yaml` -- none of which are
pilot files themselves. Verified by reading `tools/sbi-codegen/sbi_codegen/loader.py`'s own
`resolve_ref`/`load_file` (lines 51-85): external `$ref`s are resolved lazily against any file in
`specs_dir`, regardless of pilot-set membership -- only the two entry-point files
(`Nsmf_EventExposure`/`Nsmf_NIDD` themselves) needed adding to the pilot set for their referenced
schemas from those 7 other files to generate correctly. Confirmed, not assumed: full reconfigure +
rebuild produced real, correct types with the expected real collision (`EventSubscription`,
`EventNotification`, `NotificationMethod` -- all declared by both `Nsmf_EventExposure` and
`TS29503_Nudm_EE.yaml`/elsewhere) disambiguated by the codegen to `*_Nsmf_EventExposure`.

### Implementation

- **`Nsmf_EventExposure`** (4 operations), backed by a new `EventSubscriptionStore`
  (`event_subscription_store.hpp` -- same real assign-id/store/remove shape as
  `sm_context_store.hpp`'s `SmContextStore`, kept as a separate type since an event subscription is
  a genuinely distinct resource from an SM context): `CreateIndividualSubcription` real structural
  validation of `NsmfEventExposure`'s required `notifId`/`notifUri`/`eventSubs`, assigns a real
  server-side `subId`, real `201`+Location. `GetIndividualSubcription` real get/404-then-200.
  `ReplaceIndividualSubcription` real get/404 then real structural validation and update, real
  `200`. `DeleteIndividualSubcription` real get/404-then-remove, real `204`. Disclosed: no real
  event notification delivery exists -- subscriptions are stored/read/replaced/removed for real,
  but none of the real `SmfEvent` values (`AC_TY_CH`, `UP_PATH_CH`, `PDU_SES_REL`, ...) have a
  trigger path wired to them anywhere in this SMF, same disclosed gap class as AMF's own
  subscription types (ADR-0199/ADR-0200).
- **`Nsmf_NIDD`** (1 operation): `Deliver`, real multipart-only structural validation of
  `DeliverReqData`'s required `mtData`, then checks whether the path's `pduSessionRef` matches a
  live entry in the existing `SmContextStore`. Disclosed, deliberate simplification: this
  project's only real concept of a "live PDU session" is `SmContextStore`'s own
  `smContextRef`-keyed resource (`TS29502_Nsmf_PDUSession.yaml`) -- `Nsmf_NIDD`'s own
  `pduSessionRef` path parameter is treated as referring to that same id space, since there is no
  separate real `/pdu-sessions` resource anywhere else in this build to check against instead (the
  real inter-SMF `/pdu-sessions` collection from `TS29502_Nsmf_PDUSession.yaml` was itself
  deliberately deferred at this project's original SMF turn -- still deferred, unaffected by this
  ADR). Real `404` if no match; real `204` if found -- disclosed: no real NAS/5G-SM Non-IP-Data-
  Delivery pipeline exists to actually push the MT data to a UE, same class of gap as every other
  NAS-adjacent simplification already disclosed in this file (n1SmMsg never decoded, PduSessionType
  fixed-default, ...).

### Live verification (real, live mTLS + OAuth2, not self-consistency)

New `tests/integration/test_smf_event_exposure_nidd.cpp`, real spawned `nrf`+`smf` (and, for the
NIDD test, `pcf` too) processes, real HTTP/2 client over TLS 1.3 + mTLS: `Nsmf_EventExposure` ->
real `201` with a verified `notifId`/server-assigned `subId` -> real `200` `GET` with the same
`notifId` -> real `404` `GET` on an unknown `subId` -> real `200` `PUT` with a verified-changed
`notifId` -> real `204` `DELETE` -> `DELETE` again -> real `404`; a separate test proves real `400`
on a body missing `eventSubs`. `Nsmf_NIDD` `Deliver` -> real `404` against a nonexistent
`pduSessionRef`, then a real, PCF-backed `CreateSMContext` (same real dependency chain as
`test_smf_pdu_session.cpp`'s own PCF-backed tests) to produce a genuine `smContextRef`, followed by
real `204` against that same ref used as `pduSessionRef`.

### Testing

Full reconfigure + rebuild against the complete pilot set clean (`EXIT=0` verified directly from
the build log, single non-overlapping `cmake --build` invocations throughout). All 7 SMF
integration tests pass (4 pre-existing `Nsmf_PDUSession` + 3 new), verified with a suite-name-
anchored `ctest -R` -- no strays before or after (`ps aux` checked).

### What this ADR does NOT include

No real event notification delivery for `Nsmf_EventExposure`'s subscriptions. No real NAS/5G-SM
delivery pipeline for `Nsmf_NIDD`'s `Deliver` -- the data is validated and accepted, never actually
sent to a UE. `Nsmf_NIDD`'s `pduSessionRef` is not a real, independent resource -- it is mapped
onto `SmContextStore`'s existing `smContextRef` id space, a disclosed simplification, not a
literal implementation of a separate `/pdu-sessions` collection. `TS29502_Nsmf_PDUSession.yaml`'s
own real `/pdu-sessions` collection (inter-SMF roaming) remains deliberately deferred, unaffected
by this ADR. The real `oneOf`/`allOf`/`not` structural constraints inside `NsmfEventExposure`'s
own nested schemas (e.g. `StateTransitionInfo`, `TrafficCorrelationNotification`) are not enforced
beyond what the generated DTO's own required-field list captures, same standing simplification as
every other NF's own generated-DTO validation in this project.

