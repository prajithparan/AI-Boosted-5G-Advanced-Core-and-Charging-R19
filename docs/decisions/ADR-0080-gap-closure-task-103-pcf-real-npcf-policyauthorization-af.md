## ADR-0080: gap-closure task #103 -- PCF real Npcf_PolicyAuthorization (AF/IMS-facing)

### Context

`docs/CAPABILITY_GAP_ANALYSIS.md`'s PCF section flagged `Npcf_PolicyAuthorization` as a real,
high-impact gap -- both free5GC (`policyauthorization.go`) and open5GS (`npcf-handler.c`, the real
`OGS_SBI_RESOURCE_NAME_APP_SESSIONS` resource) implement it, and it's the real AF-facing interface
an IMS AS (P-CSCF/VoNR call setup) uses to request media/QoS policy authorization -- unlike
`Npcf_UEPolicyControl`/`Npcf_BDTPolicyControl`, which are free5GC-only. This project's PCF
(`nfs/pcf/src/main.cpp`) already self-disclosed this as deferred before this pass; now closed.

### Real spec source and codegen

`specs/5G_APIs-REL-19/TS29514_Npcf_PolicyAuthorization.yaml` added to the sbi-codegen pilot set
(`libs/sbi-generated/CMakeLists.txt`, same mechanism as ADR's own prior `TS29519_Policy_Data.yaml`
addition) -- a clean regeneration (2086 types, up from 2010), no codegen fixes needed this time.
Real operations implemented, route-for-route: `PostAppSessions`, `GetAppSession`, `ModAppSession`,
`DeleteAppSession`, `updateEventsSubsc`, `DeleteEventsSubsc`, `PcscfRestoration`.

**Real, confirmed-by-reading-the-YAML detail, not assumed**: `ModAppSession`'s request body is
`application/merge-patch+json` (RFC 7396) -- NOT RFC 6902 JSON Patch, which is what NRF's own
`UpdateNFInstance` uses (`nlohmann::json::patch`). Implemented with `nlohmann::json::merge_patch`
instead, a real, distinct standard library method for the real, distinct content-type the spec
actually declares here.

### Real, disclosed simplification (same category as PCF's existing AM/SM policy defaults)

This lab has no real PCC-rule/session-rule engine to actually authorize a requested media flow
against. `PostAppSessions` stores the real request and returns a schema-correct
`AppSessionContext` with NO `ServAuthInfo` failure code set. This is not a fabricated "approved"
decision: reading `ServAuthInfo`'s own real schema (`TS29122_CommonData_grp.hpp`'s generated
struct) shows it only enumerates FAILURE reasons (`TP_NOT_KNOWN`, `TP_EXPIRED`,
`ROUT_REQ_NOT_AUTHORIZED`, ...) -- there is no "AUTHORIZED" value defined anywhere in the real
spec, so an absent `servAuthInfo` genuinely IS the correct, real "authorized" outcome, not an
invented one.

`PcscfRestoration` acknowledges (204) without any real per-UE App Session Context inventory to
search -- this lab has no real trigger source for the operation's own real use case (a P-CSCF
actually restoring and needing to terminate stale contexts), same disclosed shape as this file's
other "no real trigger source yet" gaps (e.g. AM/SM policy's own deferred callback notifications).

### Verification -- live, not just build success

Real, live HTTP verification against a running NRF+PCF (not simulated): `PostAppSessions` with a
real `ascReqData` -> 201 with real `Location` header and `ascRespData: {}` (no `servAuthInfo`,
confirming the real "authorized" outcome above); `GetAppSession` retrieves it; `ModAppSession`
merge-patches in a new field (`mcpttId`) while preserving/updating existing ones (`suppFeat`
changed 1->3), confirming `merge_patch`'s real RFC 7396 semantics, not RFC 6902; `updateEventsSubsc`
correctly returns 201 on first PUT and 200 on a second PUT to the same resource, storing the real
subscription nested at `ascReqData.evSubsc`; `DeleteEventsSubsc`, `PcscfRestoration`, and
`DeleteAppSession` (`POST .../delete`, confirmed a real spec quirk -- not an actual HTTP DELETE)
all verified; a missing `ascReqData` -> 400; a nonexistent `appSessionId` -> 404 on both `GET` and
`.../delete`.

**Real bug found and fixed during this same live verification**: the first `updateEventsSubsc`
test request used a bare string array for `events` (`["QOS_MONITORING"]`), which failed with a
real `nlohmann::json` type error ("cannot use at() with string") -- tracing it back confirmed
`AfEventSubscription` (the real element type of `EventsSubscReqData.events`) is a real OBJECT
(`{event: AfEvent, ...}`), not a bare string; the test request was wrong, not the code. Corrected
to `[{"event": "QOS_NOTIF"}]` and re-verified successfully -- included here because it's a real,
concrete confirmation that the generated DTO's own real shape was checked against, not guessed.

Full `conformance_tests` (261/261) and a full rebuild of `integration_tests` both pass, unaffected
(no new unit tests added -- coverage is via the live HTTP verification above, matching this same
pass's own established pattern for NRF's ADR-0079).

### What this ADR does NOT include

AF-pushed callback notifications (`eventNotification`/`terminationRequest`) -- no receiver exists
on the AF side in this lab, same disclosed shape as `Npcf_AMPolicyControl`/`Npcf_SMPolicyControl`'s
own already-deferred `PolicyUpdate`/`TerminationNotification` callbacks. Real, subscriber-specific
authorization decisioning (checking a requested media flow against real subscription data, real
admission control) -- there is no real backing data source for this in the lab (same class of gap
as PCF's other two services' own fixed-default policy responses). `Npcf_UEPolicyControl` (URSP),
`Npcf_BDTPolicyControl`, `Npcf_EventExposure`, and PCF's other free5GC-only sub-services remain
open, per `docs/CAPABILITY_GAP_ANALYSIS.md`'s own priority ordering (this was the highest-priority
item, since both references implement it).

