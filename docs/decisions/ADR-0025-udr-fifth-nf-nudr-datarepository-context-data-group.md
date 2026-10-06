## ADR-0025: UDR (fifth NF) -- Nudr_DataRepository context-data group

**Date:** 2026-08-06
**Status:** Accepted

**Context:** Fifth NF in the agreed Phase 2 build order (NRF -> AMF -> SMF -> UDM -> UDR -> AUSF ->
PCF). Source: `specs/5G_APIs-REL-19/TS29505_Subscription_Data.yaml` (the file `TS29504_Nudr_DR.
yaml`'s paths `$ref` into for their request/response schemas -- TS29504 itself defines almost no
schemas of its own, just path aggregation), commit `bca84b60a37773133bcae97e5c6c0d10a93b47b6`.

**Scope agreed with the user before implementation:** the `context-data` group only --
`QueryAmfContext3gpp`/`CreateAmfContext3gpp`/`AmfContext3gpp` (AMF 3GPP-access context, singular
per UE -- confirmed by reading the YAML that no delete operation exists for this resource, not
assumed) and `QuerySmfRegList`/`QuerySmfRegistration`/`CreateOrUpdateSmfRegistration`/
`UpdateSmfContext`/`DeleteSmfRegistration` (SMF registration context, one per UE+pduSessionId).
User explicitly declined ("UDR standalone this turn") to wire UDM's existing `AmfRegistrationStore`
/`SmfRegistrationStore` to call through to UDR in this same turn, even though that is UDR's real
intended role as 3GPP's data-repository NF behind UDM -- kept as a separate, deliberate future turn
touching already-committed UDM code, not silently done here.

**Deliberately deferred, not dropped:** the `provisioned-data` group (am-data/smf-selection-
subscription-data/sm-data) is GET-only in this spec with no way to provision it through the API at
all, so implementing it now would just be another permanently-empty stub, no more useful than
UDM's existing disclosed `GetAmData`/`GetSmfSelData`/`GetSmData` stub. Also deferred:
authentication-data (AUSF doesn't exist yet in this build order), ue-update-confirmation-data
(SoR/UPU), context-data's other sub-resources (non-3gpp-access, smsf-3gpp/non-3gpp, ip-sm-gw, mwd,
roaming-information, pei-info, ee-subscriptions, sdm-subscriptions, nidd-authorizations),
operator-specific-data, lcs-*, pp-data, group-data, shared-data, subs-to-notify, and all of
`TS29504_Nudr_GroupIDmap.yaml`.

**RFC 6902 JSON Patch, not RFC 7396 Merge Patch:** `AmfContext3gpp` and `UpdateSmfContext` both use
`application/json-patch+json` (confirmed by reading the YAML directly), unlike UDM's
`Update3GppRegistration`/`UpdateSmfRegistration` which use `application/merge-patch+json`. This is
a real, spec-mandated difference between the two NFs' PATCH semantics, not an inconsistency to
paper over -- applied via `nlohmann::json::patch()` (matching NRF's own `UpdateNFInstance`), not
`.merge_patch()`. `nfs/udr/src/stores.hpp`'s `AmfContextStore`/`SmfRegistrationStore` are therefore
deliberately NOT the same classes as `nfs/udm/src/stores.hpp`'s same-shaped stores, despite the
near-identical field layout, because the two NFs' patch application semantics differ. Both PATCH
responses always return 204 (the spec permits either 204-no-body or 200 with a `PatchResult` report
body listing per-operation outcomes; 204 is simpler and doesn't require fabricating report items
with no real per-op tracking behind them -- a disclosed, deliberate choice, not an oversight).

**Decision:** implemented as `nfs/udr` (port 7781, metrics 9468, `kApiRoot = "/nudr-dr/v2"`),
following the same structural pattern as every prior NF: NRF registration/heartbeat on a background
thread (`run_nrf_lifecycle`, reusing the pattern from ADR-0006/ADR-0019), OAuth2 JWT verification
against NRF's fixed `kNrfInstanceId` (ADR-0018), TLS 1.3 + mTLS via the shared lab PKI, Prometheus
metrics, ProblemDetails error responses per TS 29.500. 8 routes total across the two resource
groups; PUT returns 201+body+Location on create, 204 no-body on replace (idempotent-replace
semantics, verified by re-PUTting the same resource and confirming 204 not a second 201).

**Verification:** manual `curl` end-to-end for all 8 operations, including a real
`[{"op":"replace","path":"/guami/amfId","value":"FEDCBA"}]` RFC 6902 patch call confirmed to change
only the targeted field while `amfInstanceId`/`deregCallbackUri` survive untouched, and Prometheus
metrics confirmed via `curl http://127.0.0.1:9468/metrics`. 3 new real subprocess-to-subprocess
integration tests (`tests/integration/test_udr_context_data.cpp`): AMF context full lifecycle
(create, idempotent-replace 204-not-201, get, RFC 6902 patch, get-after-patch confirming only the
patched field changed), SMF registration full lifecycle (create, list-for-ue collection GET via the
real generated `sbi_gen::SmfRegList` type, retrieve individual, RFC 6902 patch adding a new field,
delete, 404-after-delete), and the 404/401 error paths (nonexistent ueId, tampered bearer token).
All 28 project tests pass, stable across repeated runs.

**Rejected alternative:** wire UDM's stores to UDR in this same turn (making UDR the real backing
store 3GPP intends it to be). Rejected per explicit user decision -- kept UDR's turn scoped to
standing up the NF's own API surface and verifying it independently first, deferring the
UDM<->UDR integration to a dedicated future turn where it can be reviewed on its own.

**Consequence:** adding this turn's codegen pilot file (`TS29505_Subscription_Data.yaml`) surfaced
one more real `sbi-codegen` bug before UDR's own code could be written -- see ADR-0024
(pure-`$ref`-only schema re-export handling). A generator-level fix, not a UDR-specific workaround,
benefiting every NF generated from this point forward.

---

