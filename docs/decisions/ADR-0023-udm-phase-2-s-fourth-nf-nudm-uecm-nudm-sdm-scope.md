## ADR-0023: UDM (Phase 2's fourth NF) -- Nudm_UECM + Nudm_SDM scope

**Date:** 2026-08-06
**Status:** Accepted

**Context:** UDM implements `Nudm_UECM` (`TS29503_Nudm_UECM.yaml`) and `Nudm_SDM`
(`TS29503_Nudm_SDM.yaml`), two of UDM's ten Nudm services. Scope agreed with the user before
implementation: `Nudm_UECM`'s AMF 3GPP-access registration group (`3GppRegistration`,
`Update3GppRegistration`, `Get3GppRegistration`, `deregAMF`) and SMF registration group
(`GetSmfRegistration`, `Registration`, `RetrieveSmfRegistration`, `UpdateSmfRegistration`,
`SmfDeregistration`) -- the operations AMF and SMF actually call during registration and PDU
session establishment -- plus `Nudm_SDM`'s `GetAmData`/`GetSmfSelData`/`GetSmData`/`Subscribe`/
`Unsubscribe`. Deferred, disclosed in `nfs/udm/src/main.cpp`'s file header: `Nudm_EE`, `Nudm_MT`,
`Nudm_NIDDAU`, `Nudm_PP`, `Nudm_RSDS`, `Nudm_SSAU`, `Nudm_UEAU`, `Nudm_UEID` (separate services);
UECM's non-3GPP-AMF/SMSF(3GPP+non-3GPP)/IP-SM-GW/NWDAF registration groups; SDM's remaining ~25
GET operations (LCS/V2X/ProSe/MBS/UC data, shared-data operations, `GetSupiOrGpsi`, Sor/Upu Ack,
`GetGroupIdentifiers`, ...).

**No multipart wall this time:** checked before proposing scope -- neither `TS29503_Nudm_UECM.yaml`
nor `TS29503_Nudm_SDM.yaml` uses `multipart/related` anywhere. Both codegen pilot files added
directly; this is also the turn that surfaced ADR-0022's topo-sort bug (see that entry for the
generator fix required before UDM's types would even compile).

**RFC 7396 JSON Merge Patch, not RFC 6902 JSON Patch:** `Update3GppRegistration` and
`UpdateSmfRegistration` both use `application/merge-patch+json` (confirmed by grep across the
whole YAML file -- all 6 PATCH operations in `Nudm_UECM` use it consistently), unlike NRF's
`UpdateNFInstance` which uses RFC 6902 JSON Patch (`application/json-patch+json`). Implemented via
`nlohmann::json::merge_patch()` (built-in, does exactly RFC 7396 semantics) rather than NRF's
`nlohmann::json::patch()` (RFC 6902) -- a different standard for a different operation, not an
inconsistency. Both `Amf3GppAccessRegistrationModification` and `SmfRegistrationModification`
mark their own primary key field (`guami`, `smfInstanceId` respectively) as spec-`required` even
though the request is a partial-update patch -- checked directly against the YAML's `required:`
list rather than assumed to be a codegen artifact; it is what the spec actually says.

**Disclosed simplification, stated up front:** UDM normally proxies subscriber-provisioned data
from UDR (`Nudr_DataRepository`), which doesn't exist yet in this build order (UDR is next).
`GetAmData`/`GetSmfSelData`/`GetSmData` therefore return a schema-valid but empty/default response
for any `supi` -- there is no UDR-backed store yet to return real provisioned data from.
`Nudm_UECM`'s registration operations are real bookkeeping (an AMF or SMF really did register),
not a UDR-dependent gap -- the two services have genuinely different honesty postures and that
distinction is stated explicitly in code, not left for the reader to infer.

**Verification:** manual `curl` end-to-end for all 14 operations (including RFC 7396 merge-patch
actually merging rather than replacing, confirmed by patching only `guami`/`smfSetId` and checking
untouched fields like `amfInstanceId`/`pduSessionId` survive) plus 4 new real
subprocess-to-subprocess integration tests (`tests/integration/test_udm_uecm_sdm.cpp`): AMF
registration full lifecycle (create, idempotent-replace 200-not-201, get, merge-patch, deregister,
404-after-deregister), SMF registration full lifecycle (create, list-for-ue collection GET,
retrieve, merge-patch, delete, 404-after-delete), SDM data retrieval + subscribe/unsubscribe
(double-unsubscribe correctly 404s), and the 404/401 error paths. All 25 project tests pass,
stable across repeated runs.

**Rejected alternative:** also implement `Nudm_UEAU` (`Nudm_UEAuthentication_Get`, the operation
AUSF calls for authentication vectors) in this same turn, since it's arguably just as central to
UE registration as UECM/SDM. Rejected -- AUSF doesn't exist yet in this build order (comes after
UDM, UDR), so `Nudm_UEAU` would be unreachable/unverifiable by anything in this repository until
then, the same reasoning that kept AMF's turn from reaching into PCF/UDM territory prematurely.
Tracked as a natural addition once AUSF's own turn needs it.

---

