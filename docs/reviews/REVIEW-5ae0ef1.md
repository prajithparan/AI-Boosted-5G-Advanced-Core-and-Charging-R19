# Code and spec review: 5G-Advanced (R19) Core + Charging at commit `5ae0ef1`

Read-only review of `main` at `5ae0ef1` (AAnF increment 3, ADR-0497). Nothing in the repository
was changed. This file is written to be handed to a Claude Code session that will fix the findings.

> **Status (maintained; everything below this box is the review as written, unchanged).**
> Reviewer: Claude Opus 5.5, max effort, 2026-10-16. Every finding was re-checked against the code,
> YAML and TS text at `5ae0ef1` on 2026-10-16: 41 confirmed, none found wrong; SUS-01..04 remain
> suspected (code reads as described, not run). Not re-measured: the 974-route total and the
> DEP-02 per-NF peer list.
>
> | Finding | Status | Where |
> |---|---|---|
> | SEC-05, SEC-10, SEC-13 | Fixed | `3049778` (ADR-0494, ADR-0491) |
> | SEC-01 | Partly fixed: N32-c `sender` must be a client-certificate dNSName SAN; per-PLMN trust anchors and the lab's shared SEPP certificate remain | `3049778` (ADR-0491) |
> | SEC-02 | Fixed (unit tests pass and fail on the old code; SEPP e2e 4/4 pass) | `0d8683c` (local), ADR-0491 review fix |
> | SBI-01, SBI-02, SEC-03 | Fixed (2026-10-17): tokens required by default on 25 NFs (SEPP: no OAuth2-protected API); NRF issues tokens only to registered, certificate-bound consumers (`config/nrf.json` check on); SEC-03: AAnF register/remove only from the AUSF, `afId` bound to the calling AF | ADR-0499 (phase table) |
> | The PCF ModAppSession wrapper bug (§0, "already known") | Fixed | `4255e69` (ADR-0498) |
> | Not in the review: the NRF issues any token to anyone (no registration, certificate or NF-type check) | Fixed: on in `config/nrf.json` (ADR-0499 phases 2, 5) | `nfs/nrf` |
> | Not in the review: most inter-NF calls send no token (e.g. NEF→PCF/UDM/UDR, AMF→UDM) | Fixed: every outbound SBI call carries a token for its target service (`sbi_core::TokenSource`) | `b076331` (ADR-0499 phase 3) |
> | Not in the review: TSan CI job red on `main` (3 `LiChfEndToEnd` -- the CHF exits with no CDR sink in that job) | Fixed (`873b284`, on `main`): the TSan leg's empty `CHF_CDR_EVENT_BUS_BROKERS` overrode the test's broker; the test now keeps the CHF's direct-insert sink there (reproduced: 3/3 pass with the TSan env) | `tests/integration/test_li_chf_e2e.cpp` |
> | TSCTSF (`4255e69`, after this review) inherits SBI-01, SBI-02, SBI-05 | Fixed: tokens required (`7402cf6`, ADR-0499 phase 4); absolute `Location` via sbi-core (SBI-05, below) | `nfs/tsctsf` |
> | Prevention: the review's bug classes are a mandatory pre-commit checklist | Done | skill `new-nf-checklist` "Never repeat these"; CLAUDE.md |
> | SPEC-02, SPEC-03, SPEC-04, SPEC-11 | Fixed (2026-10-17): `sbi_core::http2::apply_json_patch_body` applies an `application/json-patch+json` body as RFC 6902 `PatchItem[]` (415 with `Accept-Patch` for another content type, 400 `INVALID_MSG_FORMAT` otherwise); NEF Monitoring Event and NSACF slice-EE PATCH use it (NSACF answers `CreatedSACEventSubscription`, the invented `SACSubscriptionPatch` comment is gone); NEF 5GLAN applies `5gLanParams` / `5gLanParamsPatch` to the stored `5gLanParams`; PCF AM PolicyAuthorization PATCH applies `evSubsc` (merge-patch: null removes) | `tests/conformance/test_sbi_json_patch.cpp`, `PatchConformance.*`, `AmPolicyAuthorizationIntegration.*` |
> | SEC-08, SEC-09, SBI-04 | Fixed (2026-10-18; CI green on PR #9, all six jobs incl. ASan/UBSan and TSan): the client gives up on a silent peer (connect 5 s, request 30 s, `SBI_CLIENT_*_TIMEOUT_MS`); the server answers a body over 16 MiB (`SBI_MAX_BODY_BYTES`) with 413 `MAX_JSON_SIZE_EXCEEDED`, discarding it as it arrives; an unserved method is 501, a path served for other methods 405 with `Allow`, an unknown path 404 (TS 29.500 5.2.7.2) | `libs/sbi-core`; `SbiRobustness.*` |
> | SBI-05, SEC-07 | Fixed (2026-10-18; CI green on PR #9): sbi-core makes every bare-path `Location` absolute, `https://<request :authority><path>` (TS 29.501 4.4.1), for all NFs at once; the AAnF uses an absolute `Location` as is and logs a failed EE DELETE. Disclosed: behind an SCP/load balancer the apiRoot is the authority that reached the NF; the lab host is an IP literal, not the operator FQDN TS 29.501 recommends | `libs/sbi-core`, `nfs/aanf`; `SbiRobustness.ABarePathLocationIsMadeAbsoluteWithTheApiRoot` |
> | SBI-03 | Partly fixed (2026-10-20): `problem_response` takes a required `cause`; the parse helpers answer `INVALID_MSG_FORMAT` / `MANDATORY_IE_MISSING` / `MANDATORY_IE_INCORRECT`; 440 of 986 calls carry a Table 5.2.7.2-1 cause. Open: 488 404s whose causes are API-specific (TS 29.5xx texts, per NF), `invalidParams` (NOTE 1). The 964 per-handler 401 checks the Authorizer made unreachable are removed (6 kept 401s still lack `WWW-Authenticate`) | ADR-0502; `SbiJsonBody.*` |
> | SPEC-01 | Fixed in 2 increments (2026-10-23): the NEF's Npcf_PolicyAuthorization create carries the UE address, one media component and an event subscription, AF PUT/PATCH reach the PCF (ADR-0519); the PCF's `/notify` and `/terminate` callbacks reach the AF as `UserPlaneNotificationData`, `_npcfEventsNotification` removed (ADR-0520). Open, separate gap: the PCF itself sends no events yet | ADR-0519, ADR-0520 |
> | DOC-02 | ADR-0318 corrected (now true); ADR-0317 corrected to state the monitoring notification is not conformant until SPEC-06 | ADR-0520 |
> | SPEC-05 | Fixed (2026-10-24): PCF posts to `{notificationUri}/update`; SMF serves `/update` and `/terminate`. Open: the SMF does not release the PDU session on `/terminate` (TS 23.502 4.3.4.2 not implemented) | ADR-0521 |
> | CHG-02 | Fixed (2026-10-24): mapping questions decided by the user and recorded; header refreshed; CHARGING_PROMPT.md aligned. Open, separate work: TMF622 ProductOrder (after CHG-01) | ADR-0524 |
> | Everything else | Open | |
>
> **Fix order (one subsystem per turn):** SEC-02 → SBI-01 phases 1-5 + SEC-03 (ADR-0499) → TSan CI
> → PATCH helper (SPEC-02/03/04/11) → sbi-core robustness (SBI-03/04/05, SEC-08/09) → deployment
> (DEP-01..04, SEC-04) → NEF mapping (SPEC-01/06, DOC-02) → SPEC-05 → charging (CHG-01/02) → lows.
> Awaiting user decisions: SEC-01 trust anchors, SPEC-01/06 mappings, CHG-01, DEP-02, CHG-02.

---

## 0. How to use this report (for Claude Code)

- **Line numbers are at `5ae0ef1`.** Re-read the lines and re-check the cited YAML/TS text before
  changing anything; the code may have moved.
- **Follow `CLAUDE.md`.** In particular:
  - One NF or one subsystem per turn.
  - Architectural changes need an ADR (skill `adr-workflow`; never edit `docs/DECISIONS.md` by hand).
  - Test-first for protocol logic.
  - Never invent a field, path, enum or cause. Where the spec leaves a choice, ask the user
    (see §11, "Decisions for the user").
  - No attribution lines in commits (guardrail 9; run `git config core.hooksPath .githooks` once).
  - Look up spec shapes with skill `spec-lookup` instead of reading YAML or generated headers whole.
- **Already known; do not re-report or fix as part of this:**
  - The PCF `Npcf_PolicyAuthorization` ModAppSession handler merging the whole
    `AppSessionContextUpdateDataPatch` into `ascReqData` (fixed locally, not yet pushed).
  - A TSCTSF NF that exists only locally.
- **Finding IDs** (`SPEC-01`, `SEC-01`, …) are stable, so you can say "fix SEC-01".
- **Verified vs suspected.** "Verified" means the code and the cited spec lines were both read
  (one item was also reproduced). "Suspected" items (§9) need confirmation before any fix.
- **Each finding ends with "Done when":** the test or check that shows it is fixed.

### Spec sources used

- 3GPP OpenAPI YAML: `specs/5G_APIs-REL-19/`.
- 3GPP TS text: `specs/3gpp/TS_*.txt` (text renderings of the `.docx` specs).
- Citations below give the file and line, e.g. `TS_29.500_j70.txt:2044` or
  `TS29512_Npcf_SMPolicyControl.yaml:88-89`.

---

## 1. Summary of findings

| ID | Severity | Title | Primary location |
|---|---|---|---|
| SEC-01 | **Critical** | SEPP never binds the N32-c `sender` to the TLS peer: PRINS context hijack and teardown | `nfs/sepp/src/main.cpp:391-481` |
| SEC-02 | High | SEPP N32-f correlation applies the multi-context rule across different peers, no certificate check | `nfs/sepp/src/n32.cpp:337-358` |
| SEC-03 | High | AAnF accepts anonymous anchor-key registration/removal; `afId` not bound to the caller | `nfs/aanf/src/main.cpp:224-385` |
| SEC-04 | High | AKMA anchor keys in plaintext in a shared, unauthenticated, host-published Valkey | `deploy/docker/docker-compose.yml:1118-1121` |
| SBI-01 | High | Producers never check token `aud`/`scope`/`sub`; tokenless requests accepted everywhere | `libs/sbi-core/src/jwt.cpp:53` |
| SPEC-01 | High | NEF QoS notification to the AF: wrong schema plus invented field | `nfs/nef/src/main.cpp:7885-7892` |
| SPEC-02 | High | NEF Monitoring Event PATCH applies merge-patch to a JSON Patch document | `nfs/nef/src/main.cpp:8154-8168` |
| DEP-01 | High | Helm charts cannot start their NFs (no config, no certs mounted) | `deploy/helm/*/templates/deployment.yaml` |
| DEP-02 | High | Compose: AAnF/AUSF exit at startup; 19 NFs keep a loopback NRF URL | `config/aanf.json:6`, `config/ausf.json:7` |
| CHG-01 | High (feature off by default) | AI quota multiplier changes the per-octet charge (guardrail 7) | `nfs/chf/src/charging_engine.cpp:345-349` |
| SPEC-03 | Medium | NEF 5GLAN PATCH stores the wrapper member `5gLanParamsPatch` | `nfs/nef/src/main.cpp:4861-4880` |
| SPEC-04 | Medium | NSACF slice-EE PATCH: wrong patch type, invented schema name, wrong 200 body | `nfs/nsacf/src/main.cpp:868-907` |
| SPEC-05 | Medium | PCF→SMF SM-policy notification URI lacks `/update`; `/terminate` unserved | `nfs/pcf/src/main.cpp:542`, `nfs/smf/src/main.cpp:2966-2968` |
| SPEC-06 | Medium | NEF monitoring notification: invented `_nudmEeReport`, no `monitoringEventReports` | `nfs/nef/src/main.cpp:7966` |
| SBI-02 | Medium | 401 responses carry no `WWW-Authenticate` (957 sites) | all NFs except UDSF |
| SBI-03 | Medium | ProblemDetails almost never carry `cause` (~1,930 sites) | `libs/sbi-core/src/json_body.cpp:9` |
| SEC-05 | Medium | PRINS sequence counter wraps silently: AES-GCM nonce reuse | `nfs/sepp/src/main.cpp:382-387` |
| SEC-06 | Medium | AAnF roaming check fails open on any UDM failure | `nfs/aanf/src/main.cpp:299-328` |
| SEC-07 | Medium | AAnF DELETE URL = `udm_base + Location`: EE-subscription leak with a conformant UDM | `nfs/aanf/src/main.cpp:311-315` |
| SEC-08 | Medium | SBI HTTP client has no timeouts | `libs/sbi-core/src/http2_client.cpp:137-200` |
| SEC-09 | Medium | Request bodies unbounded; no 413 | `libs/sbi-core/src/http2_server.cpp:665-676` |
| SEC-10 | Medium | SEPP starts a detached thread for every failed N32-f message | `nfs/sepp/src/main.cpp:528-547` |
| DEP-03 | Medium | Helm charts missing for 17 NFs and all of `bss/` | `deploy/helm/` |
| DEP-04 | Medium | No Makefile, no `make lab-up` | repo root |
| DOC-01 | Medium | COMPONENT_STATUS overstates the SEPP | `docs/COMPONENT_STATUS.md:53, 116-117` |
| DOC-02 | Medium | ADR-0317 and ADR-0318/0319 claim AF notifications are in TS 29.122 shape | `docs/decisions/…` |
| CHG-02 | Medium | `CHARGING_MAPPING.md` contradicts itself; open mapping questions | `docs/CHARGING_MAPPING.md:3-7` |
| SBI-04 | Low-medium | Router returns 404 for every unmatched method/path; no 405/501/415 | `libs/sbi-core/src/http2_server.cpp:475-484` |
| SBI-05 | Low-medium | ~100 `Location` headers are bare paths, not `{apiRoot}/…` | e.g. `nfs/udm/src/main.cpp:3450-3452` |
| DOC-03 | Low-medium | Two tests verify less than their ADRs say | `tests/…` |
| SPEC-07 | Low | 14 NEF PUT handlers create-on-PUT with 201 where YAML allows 200/204 | `nfs/nef/src/main.cpp:2391…` |
| SPEC-08 | Low | UDR serves GET on three resources that have no GET in YAML | `nfs/udr/src/main.cpp:9391, 9502, 9613` |
| SPEC-09 | Low | UDR implements a YAML pseudo-operation | `nfs/udr/src/main.cpp:10315` |
| SPEC-10 | Low | UDSF invented cause `INSUFFICIENT_SCOPES` | `nfs/udsf/src/service.cpp:117-121` |
| SPEC-11 | Low | PCF AM PolicyAuthorization PATCH silently drops `evSubsc` | `nfs/pcf/src/main.cpp:1503-1545` |
| SEC-11 | Low | SEPP `Authorization` checks are case-sensitive | `nfs/sepp/src/main.cpp:607` |
| SEC-12 | Low | SEPP telescopic table grows without bound | `nfs/sepp/src/n32.cpp:564-569` |
| SEC-13 | Low | SEPP `Location` rewrite throws on uppercase hosts | `nfs/sepp/src/main.cpp:1005-1008` |
| SEC-14 | Low | SEPP handshake ids from a non-cryptographic RNG | `nfs/sepp/src/main.cpp:117-122` |
| SEC-15 | Low | AAnF: SUPI unvalidated; race in `AkmaStore::put` | `nfs/aanf/src/main.cpp:242-250` |
| RULE-01 | Low | Raw `new`/`delete` (4 sites) | see finding |
| RULE-02 | Low | ~90 `throw`s for recoverable errors | see finding |
| DOC-04 | Low | Stale test names in ADR-0493 and ADR-0270 | see finding |
| DOC-05 | Low | COMPONENT_STATUS totals and date wrong | `docs/COMPONENT_STATUS.md:14, 112` |
| DOC-06 | Low | SEPP `main.cpp` header says PRINS forwarding not built | `nfs/sepp/src/main.cpp:24-27` |
| DOC-07 | Low | "700M-CDR scale" title is a design claim, not a measurement | `docs/DECISIONS.md:474` |
| SUS-01 | Medium (suspected) | Long `Via` header can crash the SEPP (`std::regex` stack overflow) | `nfs/sepp/src/n32.cpp:43-52` |
| SUS-02 | Medium (suspected) | Uncapped, non-expiring in-memory subscription stores | `nfs/nef/src/stores.hpp` |
| SUS-03 | Low (suspected) | PRINS `open()` can be made to allocate a huge array | `nfs/sepp/src/prins_codec.cpp:571-572` |
| SUS-04 | Low (suspected) | AUSF compares RES* in non-constant time | `nfs/ausf/src/main.cpp:570` |

---

## 2. Spec fidelity (verified)

### SPEC-01: High. NEF sends the AsSessionWithQoS notification to the AF in the wrong schema, with an invented field

- **Where:** `nfs/nef/src/main.cpp:7859-7920`, route `POST /nnef-callback/v1/qos-notify/{afId}/{subscriptionId}`. The payload is built at 7885-7892.
- **Problem:** the body sent to the AF's `notificationDestination` is
  `{"subscription": <AF resource link>, "_npcfEventsNotification": <raw PCF EventsNotification>}`.
- **Violates:**
  - `TS29122_AsSessionWithQoS.yaml:125-134`: the callback body is `UserPlaneNotificationData`.
  - `:987-1001`: `UserPlaneNotificationData` requires `transaction` (Link) and `eventReports` (array of `UserPlaneEventReport`, minItems 1).
  - `subscription` is not a member of that schema, and `_npcfEventsNotification` exists in no YAML (CLAUDE.md guardrail 1).
- **Failure scenario:** an AF that validates the callback rejects every QoS event (400) or finds no `eventReports`. The PCF→NEF→AF QoS event path delivers nothing usable.
- **Fix hint:** build `UserPlaneNotificationData`, with `transaction` = the AF subscription link and `eventReports` mapped from the PCF `EventsNotification`. Ask the user where the PCF→TS 29.122 event mapping is ambiguous.
- **Done when:** a test posts a PCF `EventsNotification` to the callback and the body forwarded to the AF validates against `UserPlaneNotificationData` (extend `tests/conformance/validate_structural_conformance.py`). Also update the ADR text in DOC-02.

### SPEC-02: High. NEF Monitoring Event PATCH applies RFC 7396 merge-patch to a JSON Patch document

- **Where:** `nfs/nef/src/main.cpp:8154-8168`; store `nfs/nef/src/stores.cpp:606-616` (`AfMonitoringSubStore::merge_patch` → `nlohmann::json::merge_patch`).
- **Problem:**
  - The body is parsed with an unguarded `json::parse(req.body)`.
  - It is applied as a merge patch.
  - The content type is not checked.
- **Violates:**
  - `TS29122_MonitoringEvent.yaml:354` (patch), `:372-383` (`application/json-patch+json`, array of `PatchItem`, minItems 1).
  - RFC 7396 §2: a non-object patch replaces the whole target.
  - TS 29.500 §5.2.7.2: 400 `INVALID_MSG_FORMAT` for a malformed body; 415 for an unsupported content type.
- **Failure scenario:**
  - An AF sends the conformant `[{"op":"replace","path":"/maximumNumberOfReports","value":5}]`. The stored subscription is replaced by that array, the 200 response returns the array, and every later GET/DELETE works on a corrupted resource.
  - Malformed JSON throws, and the server's catch-all answers 500 `SYSTEM_FAILURE` instead of 400.
- **Fix hint:** apply RFC 6902 (`json::patch`), as `nfs/lmf`, `nfs/nrf` and the UDR handlers already do. Reject non-arrays (400) and other content types (415).
- **Done when:** a test shows a `PatchItem` array updates the field, while a merge-patch object, or a JSON Patch sent with the wrong content type, is rejected.

### SPEC-03: Medium. NEF 5GLAN PATCH stores the wrapper member instead of applying it

- **Where:** `nfs/nef/src/main.cpp:4861-4880`; the merge is at 4873-4875.
- **Problem:** the whole `5GLanParametersProvisionPatch` body is merge-patched onto the stored `5GLanParametersProvision`.
- **Violates:**
  - `TS29522_5GLANParameterProvision.yaml:408-417`: the patch schema has `5gLanParamsPatch` (`5GLanParametersPatch`) and `5gLanParams`.
  - The resource (`:394-405`) holds `5gLanParams`.
  - This is the same class of bug as the known PCF `ascReqData` one.
- **Failure scenario:** `PATCH {"5gLanParamsPatch":{"gpsis":{…}}}` returns 200. The resource gains a stray top-level `5gLanParamsPatch`, `5gLanParams` is unchanged, and GET returns a non-schema member.
- **Fix hint:** merge `5gLanParamsPatch` into the stored `5gLanParams`, and replace it when `5gLanParams` is given.
- **Done when:** a test that patches `gpsis` through `5gLanParamsPatch` sees them under `5gLanParams` and no `5gLanParamsPatch` member.

### SPEC-04: Medium. NSACF slice-EE PATCH: wrong patch type, invented schema name, wrong 200 body

- **Where:** `nfs/nsacf/src/main.cpp:868-907`. The comment is at 889-891, the merge at 893, the response at 906.
- **Problem:**
  - The handler applies merge-patch. Its comment justifies this as "what SACSubscriptionPatch is", but `SACSubscriptionPatch` appears nowhere in `specs/5G_APIs-REL-19` (guardrail 1).
  - It returns the bare merged `SACEventSubscription`.
- **Violates:**
  - `TS29536_Nnsacf_SliceEventExposure.yaml:127` (patch).
  - `:141-146` (`application/json-patch+json`, `PatchItem[]`, minItems 1).
  - `:149-153` (200 = `CreatedSACEventSubscription`, required `subscription` and `subscriptionId`).
- **Failure scenario:** for any conformant `PatchItem` array, `current.merge_patch(array)` replaces the document with the array. `get<SACEventSubscription>()` then throws and the client gets 400. PATCH can never succeed for a conformant consumer, and a non-conformant merge object is accepted.
- **Fix hint:** apply JSON Patch and answer with `CreatedSACEventSubscription{subscription, subscriptionId}`, as the POST handler already does at 832-836. Remove the invented name.
- **Done when:** a test with a `PatchItem` array changes the threshold and the 200 body validates as `CreatedSACEventSubscription`.

### SPEC-05: Medium. PCF→SMF SM-policy update notifications go to the wrong URI; `/terminate` is not served

- **Where:**
  - `nfs/pcf/src/main.cpp:486-563` posts `SmPolicyNotification` to the bare `notificationUri` (line 542).
  - `nfs/smf/src/main.cpp:2432-2433` sets `notificationUri = …/sm-contexts/{ref}/pcf-notify`.
  - `nfs/smf/src/main.cpp:2966-2968` serves only `…/pcf-notify`.
- **Violates:**
  - `TS29512_Npcf_SMPolicyControl.yaml:87-89`: `SmPolicyUpdateNotification` → `'{$request.body#/notificationUri}/update'`.
  - `:149-150`: `SmPolicyControlTerminationRequestNotification` → `'…/terminate'`.
- **Failure scenario:** this PCF and SMF only work with each other.
  - A conformant PCF POSTs `…/pcf-notify/update` and gets 404 from this SMF.
  - This PCF POSTs to another vendor's SMF at its bare `notificationUri`, so the spending-limit-driven policy change (ADR-0286/0328) is lost.
  - A PCF-initiated SM policy termination cannot be delivered at all.
- **Fix hint:** the PCF appends `/update`; the SMF serves `…/pcf-notify/update` and `…/pcf-notify/terminate`.
- **Done when:** an SMF test covers both callback paths, and a PCF test asserts the target URL ends with `/update`.

### SPEC-06: Medium. NEF monitoring notification to the AF carries an invented `_nudmEeReport` and no `monitoringEventReports`

- **Where:** `nfs/nef/src/main.cpp:7926-7990`, line 7966.
- **Violates:** guardrail 1; `TS29122_MonitoringEvent.yaml` `MonitoringNotification` (reports belong in `monitoringEventReports`). ADR-0317 discloses the carry-through, but the field on the wire is still invented.
- **Failure scenario:** the AF receives notifications with no parseable `MonitoringEventReport`, and a strict validator may reject the extra member.
- **Fix hint:** map the Nudm_EE `MonitoringReport` to `MonitoringEventReport` for the supported event types, and drop the private field. Ask the user about ambiguous mappings.
- **Done when:** the notification body validates against `MonitoringNotification`, with at least one `monitoringEventReports` entry for a LOSS_OF_CONNECTIVITY / UE_REACHABILITY style event.

### SPEC-07: Low. 14 NEF PUT handlers create the resource and return 201 where the YAML allows only 200/204

- **Where:** route registrations in `nfs/nef/src/main.cpp` at 2391, 2451, 2602, 2724, 2921, 3137, 3339, 3421, 3772, 3905, 4018, 4303, 4404 and 4526. Example body: 2400-2416, with the comment "PUT on this API creates the resource".
- **Violates:**
  - e.g. `TS29122_CpProvisioning.yaml:189+`: the PUT responses are 200, 204, 307, 308, 400, 401, 403, 404, 411, 413, 415, 429, …; there is no 201.
  - None of these 201 responses sets `Location`.
- **Related:**
  - 3217 (NIDD rds-ports PUT) returns 200 where the YAML has only 201/202.
  - 3654 (AM PolicyAuthorization events-subscription PUT) returns 201 without `Location`.
- **Failure scenario:** an AF creates resources under ids it chooses, bypassing POST id allocation. A client expecting 404 for an unknown id gets 201.
- **Fix hint:** answer 404 for a missing resource unless the YAML defines 201, and set `Location` on every 201.
- **Done when:** a test sees PUT on an unknown id return 404; the cited YAMLs' response sets match.

### SPEC-08: Low. UDR serves GET on three individual application-data resources that have no GET in the YAML

- **Where:** `nfs/udr/src/main.cpp:9391` (`bdtPolicyData/{bdtPolicyId}`), `:9502` (`iptvConfigData/{configurationId}`), `:9613` (`serviceParamData/{serviceParamId}`).
- **Violates:**
  - `TS29519_Application_Data.yaml:1062-1203`, `1361-1508` and `1717-1868` define only PUT, PATCH and DELETE on these items; reads are collection GETs with filters.
  - ADR-0254:10-12 lists "GET/PUT/PATCH/DELETE item" as YAML coverage.
- **Fix hint:** remove the extra GETs, or record them in ADR-0254 as a disclosed project extension.
- **Done when:** the route set equals the YAML method set for these paths.

### SPEC-09: Low. UDR implements a YAML pseudo-operation

- **Where:** `nfs/udr/src/main.cpp:10295-10330` (route at 10315), `POST /nudr-dr/v2/data-restoration-events`.
- **Violates:** `TS29504_Nudr_DR.yaml:259-261`, "This is a pseudo operation, clients shall NOT invoke this method!" (it exists only to describe the callback).
- **Fix hint:** drop the route, or label it explicitly as non-standard in the code and its ADR.

### SPEC-10: Low. UDSF sends an invented ProblemDetails cause, `INSUFFICIENT_SCOPES`

- **Where:** `nfs/udsf/src/service.cpp:117-125`.
- **Violates:** guardrail 1. `TS_29.500_j70.txt:3080-3084` asks for 403 plus `WWW-Authenticate: Bearer … error="insufficient_scope"`. The header is sent correctly, but the cause value is defined nowhere.
- **Fix hint:** drop the cause, or use a defined one.

### SPEC-11: Low. PCF `Npcf_AMPolicyAuthorization` PATCH silently ignores `evSubsc`

- **Where:** `nfs/pcf/src/main.cpp:1503-1545`. Only `termNotifUri`, `expiry`, `highThruInd`, `covReq`, `asTimeDisParam` and `sliceReplReq` are applied.
- **Violates:** `TS29534_Npcf_AMPolicyAuthorization.yaml` `AppAmContextUpdateData` (includes `evSubsc: AmEventsSubscDataRm`).
- **Failure scenario:** an AF updates its event subscription by PATCH and gets 200, but nothing changes.
- **Fix hint:** apply `evSubsc` (create, modify or null-remove) as the events-subscription PUT/DELETE handlers do (`main.cpp:1585+`).

### No findings in this area

- **Reference points.** The code cites only N1, N2, N3, N4, N6, N26, N28, N32, N40, N41 and N42. All appear in `TS_23.501_j80.txt`, and no unassigned number (N25, N39, N53-54, N64, N69, N72-79, N98) appears anywhere.
- **TS numbers.** All cited TS numbers are real. TS 29.251 was confirmed via the in-repo TS 29.244 V14.3.0 PDF (references at lines 8324-8330 of its text extraction); TS 29.369 and TS 29.506 have YAML in the repo.
- **`3gpp-Sbi-*` header names.** Every one used in code appears in TS 29.500 or the YAML.
- **Route paths.** 974 of the code's routes match a REL-19 YAML method+path exactly. The rest are the project's own callback, admin, OAM-provisioning and LI X1 endpoints, plus the SEPP relay wildcards.
- **Hand-written JSON keys.** All keys used on the wire are YAML property names, except the NEF relay fields in SPEC-01 and SPEC-06. The remaining non-YAML keys are internal storage or config keys.

---

## 3. SBI conformance (verified)

### SBI-01: High. Producers never check token audience, scope or subject; tokenless requests are accepted everywhere

- **Where:**
  - `libs/sbi-core/src/jwt.cpp:53` (`Verifier::verify`) checks only the ES256 signature, `iss` and `exp`.
  - Every NF's `check_bearer` (e.g. `nfs/pcf/src/main.cpp:192`) returns "no result" when the `Authorization` header is absent, and handlers reject only `auth && !auth->valid`.
- **Coverage across the tree:**
  - `aud` is checked nowhere.
  - `scope` is checked only by the AAnF (`naanf-akma:applicationkeyget:supi-access`, `nfs/aanf/src/main.cpp:334-335`) and the UDSF (`authorize`, `nfs/udsf/src/service.cpp:77-127`).
  - `sub` is never compared with the client certificate.
  - Only the UDSF has a "require token" setting (`oauth2_required`).
- **Violates:**
  - `TS_33.501_j70.txt:4022-4028` (§13.4.1.2): the producer shall check `sub` against the consumer's TLS certificate (direct communication), `aud` against its own identity or NF type, `scope` against the requested operation, and expiry.
  - `TS_29.500_j70.txt:3073` allows tokenless requests only "based on local configuration".
  - Not disclosed in ADR-0012.
- **Failure scenario:** any process holding any lab-CA certificate can call any operation on any NF, with no token or with a token issued for a different NF or service. Examples: read UDR authentication-subscription data, delete AKMA contexts, change PCF policies.
- **Fix hint:** one central sbi-core authorizer:
  - a per-NF `require_token` setting;
  - `aud` = own NF type or instance;
  - `scope` ⊇ the operation's YAML scope (from each YAML's `securitySchemes`);
  - `sub` = the client certificate's SAN.
- **Done when:** tests show that the wrong `aud` gets 403, a missing scope gets 403 with `WWW-Authenticate … error="insufficient_scope"`, and no token with `require_token=true` gets 401 with `WWW-Authenticate`. Needs an ADR.

### SBI-02: Medium. 401 responses carry no `WWW-Authenticate` header

- **Where:** 957 `problem_response(401, …)` call sites across `nfs/`. Only the UDSF (`nfs/udsf/src/service.cpp`, `dr_routes.cpp`) sends the header.
- **Violates:** `TS_29.500_j70.txt:3074-3078`: 401 together with `WWW-Authenticate` (scheme Bearer, `realm` = API URI, `error="invalid_token"` when a token was present).
- **Failure scenario:** consumers cannot apply the rule at `TS_29.500_j70.txt:3086` (do not repeat the request with the same token), so retries loop.
- **Fix hint:** emit the header from one shared 401 helper.

### SBI-03: Medium. Error ProblemDetails almost never carry `cause`

- **Where:**
  - `libs/sbi-core/src/json_body.cpp:9` `problem_response(status, title, detail)` (declared at `json_body.hpp:25`) never sets `cause`.
  - `parse_json_body` (`json_body.hpp:40-42`) returns 400 "Malformed JSON" or "Missing or invalid mandatory IE" without one.
  - About 1,930 call sites: NEF 641, UDR 515, UDM 186, PCF 85, AMF 73, SMF 52, AUSF 48, …
- **Violates:** `TS_29.500_j70.txt:2044` ("shall include … ProblemDetails … with the cause attribute") for the errors in table 5.2.7.2-1:
  - `INVALID_MSG_FORMAT` (`:2052`)
  - `MANDATORY_IE_INCORRECT` (`:2067`)
  - `MANDATORY_IE_MISSING` (`:2073`)
  - `SUBSCRIPTION_NOT_FOUND` (`:2106`)
  - and the rest of the table.
- **Failure scenario:** consumers that decide between retry and abort by `cause` behave wrongly.
- **Fix hint:** make `cause` a required parameter of the helper. Map parse failures to `INVALID_MSG_FORMAT` and DTO failures to `MANDATORY_IE_INCORRECT` / `MANDATORY_IE_MISSING`.

### SBI-04: Low-medium. The router answers 404 for every unmatched method or path; no 405, 501 or 415

- **Where:** `libs/sbi-core/src/http2_server.cpp:475-484`. There is no 405/`Allow`, 501, or 415/`Accept-Patch` anywhere in `nfs/` or `libs/`.
- **Violates:**
  - `TS_29.500_j70.txt:2028`: 501 for a method no resource supports.
  - `:2031`: 405 plus `Allow`.
  - `:2040`: 415, with `Accept-Patch` when the PATCH document type is unsupported.
- **Failure scenario:**
  - A wrong-method call gets a misleading 404.
  - A PATCH with the wrong document type is applied instead of rejected with 415, which is what lets SPEC-02 and SPEC-04 corrupt data.
- **Fix hint:** the router distinguishes "path matched, method didn't" from "no path", and PATCH handlers declare the media type they accept.

### SBI-05: Low-medium. Most `Location` headers are bare paths, not `{apiRoot}/…`

- **Where:** about 100 sites (98 matched by a simple grep), e.g. `nfs/udm/src/main.cpp:3450-3452`: `std::string(kEeApiRoot) + "/" + ue_identity + "/ee-subscriptions/" + id`. Only one site builds an absolute URI.
- **Violates:** the YAML `Location` descriptions, e.g. `TS29503_Nudm_EE.yaml:65-66`: "{apiRoot}/nudm-ee/v1/{ueIdentity}/ee-subscriptions/{subscriptionId}".
- **Failure scenario:** a consumer that treats `Location` as an absolute URI fails. SEC-07 is the concrete case inside this project.
- **Fix hint:** prefix the NF's own apiRoot (its advertised base URL).

---

## 4. Correctness and security (verified)

### SEC-01: Critical. SEPP never binds the N32-c `sender` to the TLS peer, so any CA-trusted client can hijack or tear down another partner's N32 context

- **Where:**
  - `nfs/sepp/src/main.cpp:391-426` (exchange-capability; the NONE teardown is at 415-417).
  - `main.cpp:428-481` (exchange-params; the peer is looked up by the claimed `sender` at 436-446).
  - `main.cpp:482-508` (n32f-terminate).
  - `nfs/sepp/src/n32.cpp:134-246` (`negotiate`, using only `request.sender` via `find_peer` at 137).
  - `n32.cpp:361-364` (`PrinsStore::put` replaces the existing context for that peer).
  - `main.cpp:89-95`: the N32 server trusts a single lab CA.
- **Violates:**
  - `TS_33.501_j70.txt:1464`: the SEPP shall perform mutual authentication with the SEPP in the other network.
  - `:3510-3512`: per-PLMN trust anchors; map the PLMN-ID of the remote SEPP certificate to its trust anchor.
  - `:3515`: N32-f certificate PLMN-IDs must be contained in the N32-c certificate.
  - Not disclosed in ADR-0491 to 0494.
- **Failure scenario A (PRINS partner, full impersonation):**
  1. An attacker with any certificate the SEPP's CA trusts (any NF certificate in the lab; in production, any roaming partner) POSTs `exchange-params` with `{"sender":"<partner A FQDN>","n32fContextId":"…","jweCipherSuiteList":["A128GCM"]}`.
  2. The SEPP looks up A's PRINS capability context and creates a new N32-f context whose master key is the TLS exporter of the **attacker's** connection. The attacker can compute that value as well.
  3. That context replaces A's real one, and the response hands the attacker a fresh `n32fContextId`.
  4. The attacker sends JOSE-protected N32-f messages, which are accepted and forwarded into this PLMN as PLMN A. A's genuine traffic gets 403 `CONTEXT_NOT_FOUND`.
- **Failure scenario B (any partner, denial of service):** `exchange-capability` with `{"sender":"<A>","supportedSecCapabilityList":["NONE"]}` deletes A's context.
- **Fix hint:**
  - Before acting on any N32-c request, require that the `sender` FQDN appears in the client certificate's dNSName SANs, and that the certificate's PLMN-ID maps to that peer's trust anchor.
  - Record the N32-c certificate identity in the context, for use by SEC-02.
- **Done when:**
  - A test (unit or SEPP e2e) sends exchange-capability and exchange-params with `sender=A` from a certificate that does not name A and gets 403.
  - A's contexts remain unchanged.
  - Needs an ADR (trust-anchor model), and DOC-01 must be updated.

### SEC-02: High. SEPP N32-f correlation applies the multi-context rule across different peers and skips the certificate check

- **Where:**
  - `nfs/sepp/src/n32.cpp:312-359`; the multi-context branch is at 337-358.
  - The store keeps one context per peer (`n32.cpp:90-94`), so `contexts.size() > 1` always means several partners.
  - Caller: `nfs/sepp/src/main.cpp:700-721`.
  - `tests/conformance/test_sepp_n32.cpp:166-171` encodes the behaviour: a request with an **empty** certificate-name list is correlated to a peer by handshake id alone.
- **Violates:**
  - `TS_29.573_j70.txt:760-764` (§5.3.3.2.1): the handshake-id/purpose rule applies to multiple N32-c contexts "between a pair of SEPPs", i.e. after the peer is identified.
  - `TS_33.501_j70.txt:3515`.
- **Failure scenario:**
  - (a) With two or more partners and handshake ids exchanged, a request is mapped to a context by the `3gpp-Sbi-N32-Handshake-Id` header alone, whoever presented the certificate.
  - (b) If any partner exchanged no handshake id, correlation falls back to purpose. Two partners that both negotiated ROAMING (the default) are then always "ambiguous", so every N32-f TLS request gets 403: adding a second roaming partner breaks roaming.
- **Fix hint:** first filter contexts to those whose peer FQDN is in the client certificate (Via-header rule for common certificates), then disambiguate by handshake id or purpose among that peer's contexts. Fix the unit test together with the code.
- **Done when:** a test with two peers A and B shows:
  - B's certificate carrying A's handshake id is refused;
  - each peer's requests correlate to its own context without handshake ids.

### SEC-03: High. AAnF accepts anonymous anchor-key registration and removal, and doesn't bind `afId` to the caller

- **Where:**
  - `nfs/aanf/src/main.cpp:224-254` (`register-anchorkey`) and `:362-385` (`remove-context`) accept requests with no token and don't check the consumer type (only an AUSF should register).
  - `:276-279`: `allowed_af_fqdns` is checked against the **claimed** `afId`, not the authenticated AF.
  - File header lines 26-27.
- **Violates:**
  - `TS_33.535_j00.txt:247` (an AF shall be authenticated and authorized before getting KAF).
  - `:382` (the AAnF checks by local policy or by the OAuth2 token).
  - `:408` (the NEF verifies the AF_ID).
  - `TS_33.501_j70.txt:4022-4028`.
- **Failure scenario:**
  - **Key injection:** any lab-CA certificate holder registers an A-KID with a KAKMA of its own choice for any SUPI. Every AF then gets KAFs derived from a key the attacker knows.
  - **Wipe:** the same holder deletes every UE's AKMA context.
  - **Cross-AF key:** an internal AF that learns a UE's A-KID requests the KAF for **another** AF's `afId`, which breaks AKMA's per-AF key separation.
- **Disclosure:** ADR-0496 discloses the NEF half (AF_ID verification not built); the AAnF half is undisclosed.
- **Fix hint:**
  - Require a token with the `naanf-akma` scope from an AUSF (consumer NF type) for register and remove.
  - Authorize `afId` against the authenticated AF identity (token `sub` or certificate).
  - Depends on SBI-01.
- **Done when:** tests show anonymous register/remove gets 401, a non-AUSF token gets 403, and an AF requesting another AF's `afId` gets 403.

### SEC-04: High. AKMA anchor keys are stored in plaintext in a shared, unauthenticated, host-published Valkey

- **Where:**
  - `deploy/docker/docker-compose.yml:1118-1127`: the single `valkey` service has no `requirepass`/ACL and publishes `"6379:6379"` on the host. Nearly every NF uses it.
  - `nfs/aanf/src/akma_store.cpp:18-29`: `kakma` is stored as hex at 25-26.
- **Violates:** `TS_33.117_j20.txt:508-515` (§4.2.3.2.3: read access to sensitive data in storage shall be restricted; authentication data shall not be stored in the clear).
- **Failure scenario:** anyone who can reach host port 6379, and every NF container, can read every UE's KAKMA and derive any AF's KAF, or plant contexts.
- **Fix hint:**
  - Valkey AUTH/ACL per NF, and no host port publish.
  - Encrypt key material at rest (or keep KAKMA in a dedicated, access-restricted store).
- **Done when:** `valkey-cli -h <host> -p 6379 keys 'aanf:*'` from outside the compose network fails, and stored values are not raw keys.

### SEC-05: Medium. The PRINS sequence counter wraps silently, which reuses AES-GCM nonces

- **Where:**
  - `nfs/sepp/src/main.cpp:382-387`: `std::map<std::string, std::uint32_t> seqs`, incremented with `seqs[...]++` and no check.
  - `nfs/sepp/src/prins_codec.cpp:366-370`: nonce = IV salt (8 octets) ∥ SEQ (32-bit).
- **Violates:**
  - `TS_33.501_j70.txt:3756`: SEQ is a 32-bit counter starting at zero.
  - NOTE at `:3740`: IV reuse reveals the integrity key (Joux's forbidden attack).
  - NIST SP 800-38D: at most 2³² invocations per key with this construction.
- **Failure scenario:** after 2³² messages in one direction of a long-lived N32-f context (days at production rates), nonces repeat under the same key and forgery becomes possible.
- **Fix hint:** stop and re-establish the N32-f context (new context id, hence new keys) before SEQ reaches 2³²−1.
- **Done when:** a unit test with the counter set near the limit gets a refusal or rekey instead of a repeated nonce.

### SEC-06: Medium. AAnF roaming check fails open on any UDM failure

- **Where:** `nfs/aanf/src/main.cpp:299-328`.
- **Problem:** any non-201 status, unreachable UDM, timeout or malformed reply leaves `roaming` unset, and AKMA is provided (logged only).
- **Disclosure:** ADR-0497 discloses only the "UE not registered, no report" case.
- **Failure scenario:** with `deny_roaming_ues` on, a UDM outage grants AKMA to roaming UEs.
- **Fix hint:** fail closed (or configurably) when the UDM call itself fails; keep "provided" only when the UDM answers 201 without a report.
- **Done when:** a test with the UDM returning 500 or unreachable gets the configured refusal.

### SEC-07: Medium. AAnF builds the DELETE URL as `udm_base + Location`, which leaks EE subscriptions on a conformant UDM

- **Where:** `nfs/aanf/src/main.cpp:311-315`.
- **Problem:** it works only because this project's UDM returns a bare path (`nfs/udm/src/main.cpp:3450-3452`). With the YAML's absolute `{apiRoot}` `Location` (`TS29503_Nudm_EE.yaml:65-66`), the URL becomes `https://udm…https://udm…/…`, and the failure is discarded (`(void)udm_client.send(del)`).
- **Failure scenario:** one orphaned EE subscription at the UDM for every KAF request with `deny_roaming_ues` on.
- **Fix hint:** use `Location` as an absolute URI when it is one, and log a failed DELETE. Fix SBI-05 on the UDM side.
- **Done when:** a test with an absolute `Location` sees the DELETE hit the right URL.

### SEC-08: Medium. The SBI HTTP client has no timeouts

- **Where:** `libs/sbi-core/src/http2_client.cpp:137-200`; there is no `CURLOPT_TIMEOUT`/`_MS` or `CURLOPT_CONNECTTIMEOUT` anywhere in `libs/` or `nfs/`.
- **Problem:** handlers make blocking outbound calls on a pool of only `max(2, hardware_concurrency)` threads (`libs/sbi-core/src/io_context_pool.cpp:105`).
- **Failure scenario:** a peer that accepts the connection and never answers stalls the whole NF. Examples: the UDM for the AAnF, the AAnF for the NEF, a partner SEPP for the SEPP.
- **Fix hint:** default connect and total timeouts in the client, configurable per NF, mapped to 504.
- **Done when:** a test against a black-hole server returns within the timeout with an error.

### SEC-09: Medium. Request bodies are unbounded; there is no 413

- **Where:** `libs/sbi-core/src/http2_server.cpp:665-676`: `on_data_chunk_recv` appends DATA frames to the body with no cap. No `nghttp2_option` limits are set.
- **Violates:** `TS_29.500_j70.txt:2042` (413 when the content is larger than the NF can process).
- **Failure scenario:** any client with a valid certificate streams an arbitrarily large body until the NF runs out of memory.
- **Fix hint:** a configurable maximum body size; above it, reset the stream or answer 413.
- **Done when:** a test sending more than the limit gets 413 or a stream reset, and the NF's memory stays bounded.

### SEC-10: Medium. The SEPP starts a detached OS thread and a new TLS client for every failed N32-f message

- **Where:** `nfs/sepp/src/main.cpp:528-547` (`report_n32f_error`).
- **Problem:** nothing bounds the threads, and with SEC-08 (no timeouts) they can pile up.
- **Failure scenario:** a peer that knows a context id (it travels in clear JWE metadata) sends many malformed N32-f messages and causes thread and connection exhaustion.
- **Fix hint:** a bounded queue served by one worker thread with a reused client, plus rate-limited error reports.

### SEC-11: Low. The SEPP's `Authorization` checks are case-sensitive

- **Where:** `nfs/sepp/src/main.cpp:607` (`if (name != "authorization" …)`); `nfs/sepp/src/prins_codec.cpp:542-588` (`open()` keeps header names as received; the policy check compares exact names).
- **Violates:** `TS_29.573_j70.txt:693` (step 6: check the consumer PLMN ID in the Bearer token).
- **Failure scenario:** a peer that sends the header as `Authorization` bypasses both the consumer-PLMN check and the POLICY_MISMATCH check.
- **Fix hint:** lower-case header names in `open()` before any check.

### SEC-12: Low. The SEPP's telescopic table grows without bound

- **Where:** `nfs/sepp/src/n32.cpp:564-569` (`TelescopicTable::label_for`).
- **Problem:** a label is added on every `GET /nsepp-telescopic/v1/mapping?foreign-fqdn=` (`main.cpp:766-789`, any local client) and for every remote `Location` host (`main.cpp:1000-1010`, which the remote side controls).
- **Fix hint:** the label is a pure function of the FQDN (SHA-256 based). Store only FQDNs from configured peers' domains, or cap the table with LRU eviction.

### SEC-13: Low. The SEPP's `Location` rewrite throws on uppercase host names

- **Where:** `nfs/sepp/src/main.cpp:1005-1008`; `host_of()` lower-cases the host (`n32.cpp:72`), but `loc->second.find(host)` is case-sensitive.
- **Failure scenario:** the search returns `npos`, `std::string::replace` throws `out_of_range`, and the NF gets a 500 after the remote operation already succeeded.
- **Fix hint:** search case-insensitively, or rebuild the URI from its parsed parts.

### SEC-14: Low. N32 handshake ids come from a non-cryptographic RNG

- **Where:** `nfs/sepp/src/main.cpp:117-122` (`std::mt19937_64`).
- **Why it matters:** with SEC-02, the handshake id is the only correlation key in the multi-peer case.
- **Fix hint:** use `RAND_bytes`, as `random_n32f_context_id()` already does (`n32.cpp:406-418`).

### SEC-15: Low. AAnF: SUPI not validated; race in `AkmaStore::put`

- **Where:** `nfs/aanf/src/main.cpp:242-250` (SUPI accepted as given), `:291` and `:341` (SUPI put into UDM URL paths); `nfs/aanf/src/akma_store.cpp:18-29` (the old A-KID is read outside the MULTI).
- **Failure scenario:**
  - A SUPI containing `/` or `..` changes the UDM URL path.
  - Two simultaneous registrations for one SUPI can leave an orphaned A-KID that still resolves to an old KAKMA, against TS 33.535 §6.1 NOTE 1a ("replaces").
- **Fix hint:** validate the SUPI format (`imsi-<digits>` / `nai-…`), and use WATCH or a Lua script for the replace.

---

## 5. CLAUDE.md engineering rules (verified)

### RULE-01: Low. Raw `new`/`delete`

- **Where:**
  - `nfs/upf/src/advertised_address.cpp:87` (`new int(::socket(...))`) and `:19` (`delete fd` in a custom deleter).
  - `libs/nas-alg-provider/src/loader.cpp:93` (`new Provider()`).
  - `libs/li-core/src/x2x3_server.cpp:284` (`new std::atomic<bool>`).
- **Note:** all results go straight into a `unique_ptr`, but the rule is "no raw new/delete".
- **Fix hint:** use `std::make_unique`, or an fd wrapper class without heap allocation.

### RULE-02: Low. About 90 `throw` statements report recoverable conditions inside stores and libraries

- **Where:**
  - `nfs/adrf/src/record_store.cpp` and `model_store.cpp` (26× "data store unreachable").
  - `nfs/udsf/src/store.cpp:431, 510` (write contention).
  - `nfs/nwdaf/src/feature_store.cpp`, `nfs/chf/src/cdr_event_producer.cpp`, `libs/event-bus/src/producer.cpp`.
- **Problem:** these rely on the server's catch-all (`libs/sbi-core/src/http2_server.cpp:521-542`), which turns them into 500 `SYSTEM_FAILURE`.
- **Violates:** CLAUDE.md, "tl::expected for recoverable errors; exceptions only at API boundaries". ADR-0360 accepted the catch-all as a safety net.
- **Fix hint:** return `tl::expected` from stores; the handler maps it to 503 or 500 with a cause.

### No findings in this area

- No NF includes another NF's private headers. The SMF's NGAP use goes through the shared `ngap_core` library.
- No NF reads another NF's datastore keys (each uses its own Valkey prefix).
- The non-SBI interfaces in use (PFCP/N4, NGAP/N2, Diameter Gy/Rf, CAP, LI X1/X2/X3) are the spec-defined ones.

---

## 6. Deployment (verified by reading; nothing was run)

### DEP-01: High. None of the existing Helm charts can start its NF

- **Where:**
  - The runtime images contain only the binary (e.g. `deploy/docker/aanf.Dockerfile:70-82`).
  - The binaries need `CONFIG_DIR=/build/config` and `CERTS_DIR=/build/certs` (e.g. `nfs/aanf/CMakeLists.txt:6`).
  - Of the 11 NF charts (`aanf`, `amf`, `ausf`, `li-admf`, `nrf`, `pcf`, `sepp`, `smf`, `udm`, `udr`, `upf`), 10 mount neither config nor certs. `upf` mounts its config from a ConfigMap but no certs.
- **Failure scenario:** each pod exits at "nf_config: could not open config file" or a TLS certificate load failure. This is the failure class ADR-0453 fixed for compose, now in Helm.
- **Fix hint:** each chart ships a ConfigMap for `config/<nf>.json` and a Secret for the NF's certificates and CA, mounted at `/build/config` and `/build/certs`.
- **Done when:** `helm template` shows both mounts for every chart, and a kind/minikube install reaches "listening".

### DEP-02: High. In compose, AAnF and AUSF exit at startup, and most NFs can never reach the NRF or their peers

- **Where:**
  - `config/aanf.json:6` and `config/ausf.json:7` set `"redis_url": "tcp://127.0.0.1:6379"`.
  - The compose `aanf` (`docker-compose.yml:361-378`) and `ausf` (`:201-216`) services set no environment variables and don't depend on `valkey`.
  - `connect_redis_or_die` PINGs and calls `fatal()` (`libs/nf-config/include/nf_config/redis.hpp:18-29`).
  - On compose's default bridge network, 127.0.0.1 is the container itself.
- **NRF URL left at loopback:** only AMF, UPF, UDR, UDSF and CHF set `<NF>_NRF_BASE_URL` in compose (the ADR-0077 retrofits, e.g. `docker-compose.yml:63`, `:190`, `:1290`). The other 19 keep `nrf_base_url: https://127.0.0.1:7777`: AAnF, ADRF, AUSF, BSF, DCCF, EIR, GMLC, LMF, MFAF, NEF, NSACF, NSSF, NWDAF, PCF, SCP, SEPP, SMF, SMSF and UDM. They retry NRF registration forever and can't get OAuth2 tokens.
- **Peer URLs left at loopback** (environment variable unset, or the code has no override):
  - AMF → AUSF, PCF, SMF, NSACF, SMSF.
  - SMF → PCF, NSACF, AMF, CHF, UDR.
  - PCF → UDR, CHF.
  - NEF → UDR, PCF, UDM, AAnF.
  - UDM → UDR; AUSF → UDM; SMSF → AMF; NWDAF → SMF; AAnF → UDM.
  - The CHF's catalog and balance URLs are correctly overridden (`:1295-1296`).
- **Fix hint:** a compose-specific config set or per-service environment overrides using service DNS names, plus `depends_on: valkey`.
- **Done when:** `docker compose up` brings every NF to "registered with NRF".

### DEP-03: Medium. Helm charts are missing for 17 NFs and all BSS components; `bss/provisioning` isn't deployable

- **Missing charts:**
  - Tier 1: `chf`, `nef`, `nssf`, `scp`, `bsf`.
  - Others: `adrf`, `dccf`, `eir`, `gmlc`, `li-mdf`, `li-mdf3`, `lmf`, `mfaf`, `nsacf`, `nwdaf`, `smsf`, `udsf`.
  - All of `bss/*`.
- `bss/provisioning` also has no Dockerfile and no compose service.
- **Violates:** CLAUDE.md definition of done, item 7 (Docker image + Compose entry + Helm chart).

### DEP-04: Medium. No Makefile, so no `make lab-up`

- **Where:** no Makefile anywhere in the repo.
- **Violates:** CLAUDE.md mandated stack: "all reproducible from a single `make lab-up`".

### No findings in this area

Every NF and BSS service in `deploy/docker/docker-compose.yml` mounts `- ../../config:/build/config:ro` (ADR-0453); infrastructure images are correctly excluded.

---

## 7. Documentation honesty (verified)

### DOC-01: Medium. COMPONENT_STATUS overstates the SEPP

- **Where:** `docs/COMPONENT_STATUS.md:53` (SEPP row) and `:116-117` ("roaming intermediaries are the remaining N32 gap").
- **Problem:**
  - ADR-0491:50-53 and ADR-0494's Disclosed list more unbuilt items: N32-f keepalive PINGs, TLSCOR OPTIONS, SNDN32F, topology hiding, message filtering and PLMN-ID checks, URI_PARAM and multipart protection, replay detection, renegotiation.
  - The page also doesn't reflect SEC-01 and SEC-02.
- **Fix hint:** list the real remaining gaps.

### DOC-02: Medium. ADR text claims the AF notifications are "rebuilt in the AF's own TS 29.122 shape"

- **Where:** `docs/decisions/_other/adr-0318-0319-qos-event-delivery-and-the-ueid-service.md:9-12` and `docs/decisions/ADR-0317-nef-actually-delivers-notifications.md:25-31`.
- **Problem:** the code does not do this (SPEC-01, SPEC-06).
- **Fix hint:** correct the ADRs when SPEC-01 and SPEC-06 are fixed, or state the non-conformance plainly.

### DOC-03: Low-medium. Two tests verify less than their ADRs say

1. `SeppN32.CorrelationFollowsClause5332` (`tests/conformance/test_sepp_n32.cpp:152-174`) encodes the SEC-02 misreading: lines 166-171 correlate two different peers by handshake id with an empty certificate-name list. ADR-0491 cites it as verification of §5.3.3.2.1.
2. ADR-0495 Verified says the "AAnF's KAF equals the UE-side derivation", but the test's "UE side" calls the same `aka_crypto::derive_kakma`, `derive_a_tid`, `derive_kaf` and `make_a_kid` (`tests/integration/test_akma_e2e.cpp:175-180`). That proves the plumbing, not the KDF inputs.
   - By reading, the inputs do match `TS_33.535_j00.txt` Annex A (FC 0x80/0x81/0x82; P0 "AKMA"/"A-TID"/AF_ID; AF_ID = FQDN ∥ Ua* id).
   - There is no independent vector.
   - **Fix hint:** add a hand-computed vector (HMAC-SHA-256 of the spelled-out S) as an independent check.

### DOC-04: Low. Stale test names in ADRs

- `docs/decisions/ADR-0493-…md:63` names `SeppEndToEnd.PrinsParameterExchangeAgreesOnOneMasterKey`, which doesn't exist. It was replaced by `SeppEndToEnd.PrinsAgreesOnOneKeyAndForwardsJoseProtected` (`tests/integration/test_sepp_e2e.cpp:215`).
- `docs/decisions/ADR-0270-…md:64` names `AmfNgapTestGnb.FullN2HandoverRelayReachesHandoverCommand`, which was renamed to `…ThroughExecutionAndSourceRelease` (archived ADR-0271). The assertion it describes still exists (`tests/integration/test_amf_ngap_handover.cpp:664-672`).
- ADR-0486 and ADR-0488 record their renames in the ADR text itself, so they are fine.

### DOC-05: Low. COMPONENT_STATUS totals and date are wrong

- `docs/COMPONENT_STATUS.md:112` says "23 of 48" NFs and 23 in pipeline. The table has 47 NF rows (13 + 16 + 18) and 22 in pipeline (4 + 18).
- `:14` says "Last updated: 2026-10-14", but the AAnF row cites ADR-0496 and ADR-0497 (2026-10-15).

### DOC-06: Low. The SEPP source header says PRINS forwarding isn't built

`nfs/sepp/src/main.cpp:24-27` still says JOSE-protected forwarding is "NOT built yet: traffic towards a PRINS peer gets 504". ADR-0494 and the code at `main.cpp:842-963` say otherwise.

### DOC-07: Low. "Charging … at 700M-CDR scale" is a design claim, not a measurement

The index title at `docs/DECISIONS.md:474` makes the claim. The body (`docs/decisions/_other/adr-0344-…md:63-73`) describes the physical design and verifies partition pruning on a 4-partition table; nothing was measured at 700M rows. **Fix hint:** "designed for 700M CDRs".

### No findings in this area

- "Production-ready" and "carrier-grade" wording is consistently hedged (e.g. `docs/project-context/commercialization-mandate.md`).
- The NRF and CHF benchmark claims are backed by raw artefacts in `docs/benchmark-2026-09-10/` and `docs/benchmark-chf-*`.

---

## 8. Charging domain (verified)

### CHG-01: High, though the feature is off by default. The AI quota model changes the per-octet charge (guardrail 7)

- **The chain:**
  1. The model sets `grant.totalVolume = base × clamp(predicted/base, 0.5, 2.0)` (`nfs/chf/src/charging_engine.cpp:345-349`).
  2. The money reserved is the fixed ProductOfferingPrice amount (`charging_engine.cpp:288`, `result.cost = price.price`).
  3. The AI-scaled volume is stored as the session's granted volume (`charging_engine.cpp:786-791`, `add_granted_volume`).
  4. At Release, the debit is `reserved × min(used, granted)/granted` (`nfs/chf/src/proportional_debit.cpp:34-45`, called at `nfs/chf/src/main.cpp:1600-1606`).
- **Effect:** with multiplier `m`, the subscriber pays `P/(m·B)` per octet. The same usage costs a different amount depending on the model's output, within a 4x band.
- **Violates:**
  - CLAUDE.md guardrail 7 ("the model informs, the deterministic engine decides").
  - ADR-0074:117-119 asserts compliance, but predates the ADR-0297 proportional debit, which created this interaction.
- **State:** disabled by default (`config/chf.json:24`, `"ai_quota_sizing_enabled": false`).
- **Fix hint:** record the price-configured base grant as the debit denominator, so the AI only changes how much is granted per round, never the price per unit. Needs an ADR amending ADR-0074/0297, and the user should confirm the intended semantics.
- **Done when:** a test with the AI on (multiplier 2.0) and the same usage produces the same debit as with the AI off.

### CHG-02: Medium. `docs/CHARGING_MAPPING.md` contradicts itself and has open mapping questions

- **Contradiction:** the header (`:3-7`) still says "DRAFT … No SID/TMF mapping code exists anywhere in this repo yet". The same file's Next-steps items 3 and 5 record `libs/bss-sid` (2026-08-10) and the real rating engine (2026-10-02).
- **Open, flagged items (questions for the user, not inventions to make):**
  - `:144-161`: are `registrationChargingInformation`, `nSSAAChargingInformation`, `eASDeploymentChargingInformation` and `nSMChargingInformation` a TMF688 `Event` or `ProductUsage`?
  - `:163`: `networkSharingChargingInformation`, possibly dual-mapped to `Agreement` (TMF651) and `ProductUsage`.
  - `:109`, `:181`: Policy/TMF723 field list not confirmed from any public source.
  - `:184`: CustomerOrder and Policy "not asked about yet".
  - `:191`: `multipleUnitInformation`/`grantedUnit` mapping not re-examined against the real rating engine.
  - TMF622 `ProductOrder` is not implemented.
  - `:200-211`: the CHARGING_PROMPT.md vs CLAUDE.md TMF API list divergence is unresolved.
- **Not covered by the mapping:** the CHF now detects and flattens about 25 `*ChargingInformation` types into CDRs and rating attributes (ADR-0344/0345) without mapping rows. No TMF mapping code consumes them yet (checked), so this is a documentation gap, not hidden mapping code.
- **Fix hint:** refresh the header and scope, then put the open items to the user.

### No findings in this area

The LLM agents (`config/mcp-server.json`) have read-only tools only: `get_balance`, `get_recent_charges`, `explain_charge`, `get_offering`, `list_nf_instances`.

---

## 9. Suspected findings (need confirmation before fixing)

### SUS-01: Medium. A long `Via` header can probably crash the SEPP through a `std::regex` stack overflow

- **Where:** `nfs/sepp/src/n32.cpp:43-52` (`via_sepps`, regex `SEPP-([A-Za-z0-9.\-]+)`).
- **When it runs:** `correlate()` calls it when the client certificate names more than one SEPP (`n32.cpp:330-333`). That is the lab's common-certificate setup (ADR-0491 decision 3).
- **Evidence:** the same regex and iterator, compiled standalone with g++ (`-O2`) and run on a default 8 MB thread stack, segfaulted at 30,000 matching characters (exit 139) and worked at 8,000. Repro in Appendix B. The SEPP's server sets no nghttp2 header-size limits.
- **Not done:** an end-to-end run against the SEPP.
- **To confirm:** send an N32-f request with a ~40 KB `Via: 2.0 SEPP-aaaa…` from a common-certificate peer.
- **Fix hint:** parse `Via` with a hand-written tokenizer, or cap the header length before matching.

### SUS-02: Medium. In-memory subscription stores look uncapped and never expire

- **Evidence:** `nfs/nef/src/stores.hpp` defines 18 store classes backed by `std::unordered_map`, with no cap, TTL or eviction (grep for `max_|limit|capacity|expire|ttl|evict` finds none). `nfs/udm/src/stores.hpp` and `nfs/udr/src/stores.hpp` show none either.
- **Why it matters:** combined with anonymous access (SBI-01), any client can grow them without bound.
- **To confirm:** check each store's backing (some UDR data lives in Postgres) and whether any reaper runs.

### SUS-03: Low. PRINS `open()` can be made to allocate a huge array

- **Where:** `nfs/sepp/src/prins_codec.cpp:571-572` (`body[json::json_pointer(p.iePath)] = *v`).
- **Problem:** an `iePath` such as `/x/100000000` makes nlohmann allocate a 100M-element array. It is reachable only after the JWE tag verifies, which SEC-01 makes possible for an attacker.
- **Fix hint:** bound array indices (and nesting) during reconstruction.

### SUS-04: Low. The AUSF compares RES* in non-constant time

- **Where:** `nfs/ausf/src/main.cpp:570` (`*res_star == ctx->xres_star`).
- **Fix hint:** `CRYPTO_memcmp`.

---

## 10. Overall state

**What holds up.** For 28 NFs, the spec work is largely faithful and well documented:
- 974 routes match the REL-19 YAML exactly.
- Hand-written JSON keys stay within the YAML, apart from the NEF relay fields.
- No reference point or TS number is invented.
- The AKMA KDF inputs and the PRINS JWE construction (AAD, nonce, tag handling) match TS 33.535 and TS 33.501 / RFC 7516.
- The ADRs mostly disclose their gaps honestly.

**What doesn't.** The weaknesses are cross-cutting, not per-procedure:
- Authorization is effectively "any certificate from the lab CA" (SBI-01).
- The SEPP doesn't authenticate its N32-c peers (SEC-01, SEC-02).
- Key material sits unprotected in a shared datastore (SEC-04).
- The deployment artefacts would not run the NFs they describe (DEP-01, DEP-02).

**Verdict:** a well-documented reference implementation that has not yet met its production-grade target (ADR-0009).

**What to fix first:**
1. **SEPP peer authentication:** bind the N32-c `sender` and N32-f correlation to the TLS certificate and a per-PLMN trust anchor (SEC-01, SEC-02).
2. **SBI authorization in sbi-core:** enforce `aud`, `scope` and `sub` = certificate SAN, add a "require token" setting, and send `WWW-Authenticate` on 401. Then close the AAnF's anonymous register/remove (SBI-01, SBI-02, SEC-03).
3. **Working deployments:** compose services get their NRF, Valkey and peer URLs; Helm charts mount config and certs; Valkey gets AUTH and no host port (DEP-01, DEP-02, SEC-04).

---

## 11. Suggested work plan and decisions for the user

### Batches (one subsystem per turn, per CLAUDE.md)

1. **SEPP N32 identity binding:** SEC-01, SEC-02, then SEC-11, SEC-14, DOC-01, DOC-06, and the correlation unit test (DOC-03 part 1). Needs an ADR on the trust-anchor model.
2. **sbi-core authorization:** SBI-01, SBI-02, SPEC-10. Needs an ADR, with a rollout flag so lab tests can migrate.
3. **AAnF hardening** (after batch 2): SEC-03, SEC-06, SEC-07, SEC-15.
4. **Deployment:** DEP-02, DEP-01 and SEC-04 together; then DEP-03 and DEP-04.
5. **NEF conformance:** SPEC-02, SPEC-03 and SPEC-07 first (mechanical); then SPEC-01 and SPEC-06 (need mapping decisions), and DOC-02.
6. **NSACF / PCF / SMF callbacks:** SPEC-04, then SPEC-05 (the PCF and SMF together form one N7 callback fix), then SPEC-11.
7. **sbi-core conformance and robustness:** SBI-03, SBI-04, SBI-05, SEC-08, SEC-09.
8. **Charging:** CHG-01 (ADR amending ADR-0074/0297), then CHG-02 (refresh the doc and ask the open questions).
9. **Remaining lows:** SEC-05, SEC-10, SEC-12, SEC-13, SPEC-08, SPEC-09, RULE-01, RULE-02, DOC-03 part 2, DOC-04, DOC-05, DOC-07. Confirm SUS-01 to SUS-04 first if you touch those areas.

### Decisions for the user (ask, don't decide)

- **SEC-01/SEC-02:** the trust-anchor configuration model (per-PLMN CA bundles and PLMN-ID extraction from the certificate), and whether a common certificate for several SEPPs stays allowed.
- **SBI-01:** require tokens by default or per NF? This breaks lab tests that call without tokens.
- **SPEC-01/SPEC-06:** how to map PCF `EventsNotification` and Nudm_EE reports onto TS 29.122 event reports wherever no field corresponds one to one.
- **CHG-01:** should an AI-enlarged grant also enlarge the reservation (the user pays the same rate), or only the volume per round?
- **DEP-02:** a compose-specific config directory, or per-service environment overrides?
- **CHG-02:** the open SID/TMF mapping questions listed in that finding.

---

## Appendix A: How the checks were done

All scripts ran outside the repo. To re-run them after fixes:

- **YAML index.**
  - Parsed every `specs/5G_APIs-REL-19/*.yaml`, resolving path-item `$ref`s (TS29504 points into TS29505/TS29519).
  - Collected all property names, enum values, parameters and operations: base path from `servers.url` with `{apiRoot}` removed, path, method, request content types, response codes.
  - Totals: 2,557 operations, 10,920 property names, 4,700 enum values.
- **Routes.** Extracted every `add_route(method, path)` in `nfs/` and `bss/`, resolving `k…` string constants and local `std::string` path variables, normalising `{param}` segments, and matching method+path. 974 matched; the rest were reviewed by hand.
- **JSON keys.**
  - Extracted the literal keys in `j["k"]`, `.at("k")`, `.contains("k")`, `.value("k", …)` and `{"k", …}` in `nfs/`, `libs/` and `bss/`, skipping the ASN.1/PFCP/LI/Diameter libraries.
  - Compared them with the YAML property and parameter names and the config keys; the 290 left over were triaged by hand.
- **Enums and causes.** UPPER_SNAKE string literals were checked against the YAML enums, the YAML text and the TS text. Apart from environment-variable names, the leftovers were triaged by hand.
- **PATCH.** For every PATCH route, compared the YAML request content type and schema with the handler's tokens (`merge_patch`, `.patch(`, typed `parse_json_body<>`). Also compared each merge-patch schema's members with the resource schema's members (GET 200 / PUT body) to find wrapper members.
- **Callbacks.** Listed every YAML callback whose URL has a suffix after `{$request.body#/…}` (e.g. `/update`, `/notify`, `/terminate`) and checked the senders.
- **Status codes.** Compared the 2xx codes each handler returns with the YAML's response codes; checked that every 201 carries `Location`.
- **SBI cross-cuts.** Counted cause-less ProblemDetails, 401s without `WWW-Authenticate`, and scope/audience use across all NFs; read `jwt.cpp`, `http2_server.cpp` and `http2_client.cpp`.
- **ADR test references.** Extracted every `Suite.Test` name from `docs/decisions/**/*.md`, compared it with the `TEST(Suite, Name)` definitions across the tree, then checked each miss by prefix and by any rename note in the ADR.
- **Deployment.** Parsed `deploy/docker/docker-compose.yml` and checked every `nfs/*` and `bss/*` component for its compose service, config mount, Dockerfile and Helm chart, plus loopback URLs in `config/*.json` without environment overrides.

---

## Appendix B: Repro for SUS-01 (standalone, not the SEPP itself)

```cpp
// Same pattern and iteration as nfs/sepp/src/n32.cpp via_sepps(), on a worker thread with the
// default pthread stack. g++ -std=c++20 -O2 -pthread via.cpp -o via && ./via 30000
#include <cstdio>
#include <cstdlib>
#include <regex>
#include <string>
#include <thread>
int main(int argc, char** argv) {
    const std::size_t n = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 60000;
    std::string via = "2.0 SEPP-" + std::string(n, 'a');
    std::thread t([&] {
        static const std::regex kEntry(R"(SEPP-([A-Za-z0-9.\-]+))");
        std::size_t count = 0;
        for (auto it = std::sregex_iterator(via.begin(), via.end(), kEntry);
             it != std::sregex_iterator(); ++it) {
            ++count;
        }
        std::printf("n=%zu matches=%zu ok\n", n, count);
    });
    t.join();
}
```

Observed (`ulimit -s` 8192): `n=1000` ok, `n=8000` ok, `n=30000` SIGSEGV (exit 139), `n=60000` SIGSEGV.
