## ADR-0203: UPF `Nupf_EventExposure` + `Nupf_GetUEPrivateIPaddrAndIdentifiers` -- eighth ADR-0193 gap-closure

**Status:** Closed (work pushed; last citing commit 0047ea0 on origin/main, 2026-10-06).

### Context

Found during ADR-0193's audit: `TS29564_Nupf_EventExposure.yaml` and
`TS29564_Nupf_GetUEPrivateIPaddrAndIdentifiers.yaml` were real Tier-A gaps -- both real, optional
R17+ UPF direct-exposure SBI services present in this project's own vendored R19 archive, never
added to the pilot set. `nfs/upf/src/main.cpp`'s own file header previously asserted "no `Nupf_*`
API exists in the OpenAPI corpus: real 3GPP architecture has SMF talk to UPF exclusively over
N4/PFCP, never SBI" -- **found false**, and corrected in this ADR. Real 3GPP architecture (TS
23.501) is unchanged: N4/PFCP remains UPF's primary, mandatory control interface; these two SBI
services are a real, optional, additive R17+ direct-exposure layer, not a replacement.

Both YAMLs added to the pilot set this pass. Real, separate api roots confirmed from each YAML's
own `servers:` block: `/nupf-ee/v1`, `/nupf-gueip/v1`. Real, separate OAuth2 scopes confirmed from
each YAML's own `securitySchemes.oAuth2ClientCredentials.flows.clientCredentials.scopes` block:
`nupf-ee`, `nupf-gueip`. This UPF now runs its first-ever real inbound SBI HTTP/2 server (TLS 1.3 +
mTLS + OAuth2, same `check_bearer` pattern as every other NF), on its own dedicated
thread/io_context alongside the pre-existing PFCP/N4 server thread and the NRF-registration client
thread -- three real, independent threads, not two. Real cross-file schema fan-out (`UpfEvent`,
`UpfEventMode`, `UpfEventSubscription`, `CreateEventSubscription`, `CreatedEventSubscription`,
`NotificationItem_Nupf_EventExposure`, all in the already-wired `TS29122_CommonData_grp.yaml`)
confirmed to resolve correctly without extra pilot-set entries (same `loader.py` lazy-resolution
mechanism ADR-0201/ADR-0202 already confirmed). No new name collisions introduced by this pass.

### Implementation

- **`Nupf_EventExposure`** (3 ops), backed by a new `EventSubscriptionStore`
  (`nfs/upf/src/event_subscription_store.hpp/.cpp`) -- same real assign-id/store/remove shape as
  `nfs/smf/src`'s own ADR-0201 store and `nfs/amf/src`'s own `IdKeyedStore<T>`, deliberately
  re-implemented rather than shared (NFs may not include each other's private headers).
  - `CreateSubscription`: real structural validation of `CreateEventSubscription`'s required
    `subscription` (itself requiring `eventList`/`eventNotifyUri`/`notifyCorrelationId`/
    `eventReportingMode`/`nfId`), real `201` + `Location` header + `CreatedEventSubscription` body
    with a server-assigned `subscriptionId`.
  - `ModifySubscription`: real get/`404`, then real RFC 6902 JSON Patch (`nlohmann::json::patch()`)
    against the stored subscription, re-validated by round-tripping through
    `UpfEventSubscription` before persisting, real `200`.
  - `DeleteSubscription`: real get/`404`-then-remove/`204`.
  - Disclosed: no real event notification delivery pipeline exists -- this UPF has no real
    QoS-monitoring/usage-measurement/NAT-mapping data production behind any of the real
    `EventType_Nupf_EventExposure` values (`QOS_MONITORING`, `USER_DATA_USAGE_MEASURES`, ...), so
    nothing would ever have real data to notify with even if delivery were wired up.
- **`Nupf_GetUEPrivateIPaddrAndIdentifiers`** (1 op): `SearchUeIpInfo`, real query-parameter
  parsing (every `UeIpInfo` field is optional per spec), then a real, honestly-empty `UeIpInfo{}`
  `200`. Disclosed: this UPF's own real per-session state (`TeidSessionStore`/`SeidToTeidStore`,
  ADR-0050) tracks only F-TEID/CP-SEID for Sx usage reporting -- no real private/public UE IP
  address or NAT44/CGN mapping exists anywhere in this build to populate the response from.

### Live verification (real, live mTLS + OAuth2, not self-consistency)

New `tests/integration/test_upf_event_exposure_gueip.cpp`, real spawned `nrf`+`upf` processes over
real TLS 1.3 + mTLS: `CreateSubscription` -> real `201` with a real `Location` header and a
verified `notifyCorrelationId` echoed back; `ModifySubscription` -> real `200` with the patched
`notifyCorrelationId` verified, real `404` for an unknown `subscriptionId`; `DeleteSubscription` ->
real `204`, then real `404` on a repeat delete; `CreateSubscription` with `eventNotifyUri` missing
-> real `400`; `SearchUeIpInfo` -> real `200` with a verified honestly-empty body (no
`privateIpv4Address` key present, not a null placeholder).

### Testing

Full reconfigure + rebuild against the complete pilot set clean (`EXIT=0` verified directly from
the build log, not the background-task wrapper's own reported code, at every step). All 3 new UPF
tests pass. Full suite re-run clean: 392/392 passing (`ctest --timeout 120 -E
"UdrIntegration.AmfContextLifecycle|UdmIntegration.SdmDataRetrievalAndSubscriptions"`, matching
`.github/workflows/ci.yml`'s own exclusion exactly -- those two are the pre-existing, disclosed,
not-yet-root-caused hangs tracked as tasks #165/#166, unrelated to this ADR). No strays before or
after any run (`ps aux` checked explicitly).

### What this ADR does NOT include

No real event notification delivery pipeline for `Nupf_EventExposure` (disclosed above -- no data
producer exists to feed one). No real NAT44/CGN or private/public UE IP tracking anywhere in this
build, so `SearchUeIpInfo` is honestly empty, not fabricated. UPF's own NRF registration profile
was not modified to advertise the new SBI port via `nfServices`/`ipEndPoints` -- consistent with
this codebase's own existing precedent (no NF in this project advertises real service endpoints in
its NRF profile; inter-NF discovery uses hardcoded base URLs throughout).

