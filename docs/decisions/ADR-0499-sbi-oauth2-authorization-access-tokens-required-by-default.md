## ADR-0499: SBI OAuth2 authorization -- access tokens required by default, phased rollout (REVIEW-5ae0ef1 SBI-01/02, SEC-03)

**Date:** 2026-10-16. **Status:** Accepted (decision by the user, 2026-10-16: "Require tokens by
default"); implemented, phases 1-5 done (2026-10-17) -- see the phase table.

**Context.** `docs/reviews/REVIEW-5ae0ef1.md` SBI-01: no producer checks the token's `aud`,
`scope` or `sub`, and a request with no token is accepted everywhere (each NF's `check_bearer`
returns "no result" for a missing header; handlers reject only an invalid token). TS 33.501
13.4.1.2 (TS_33.501_j70.txt:4022-4028): the producer shall check `sub` against the consumer's TLS
certificate, `aud` against its own identity or NF type, and `scope` against the requested
operation. TS 29.500 (TS_29.500_j70.txt:3073) lets a producer accept tokenless requests "based on
local configuration"; that choice is now made: required. SBI-02: 401s carry no `WWW-Authenticate`.

Found while planning (not in the review):
- **The NRF issues any token to anyone.** `/oauth2/token` signs whatever `scope`/`targetNfType`
  is asked, without checking that the requester is registered, that its `nfInstanceId` matches its
  client certificate, or that its NF type may use the service (TS 33.501 13.4.1.1.2). Producer-side
  checks are only as strong as this.
- **Most inter-NF calls send no token.** 62 `OAuth2Client`s exist; e.g. the NEF has none for the
  PCF/UDM/UDR, the AMF none for the UDM, the SMF and TSCTSF none for the UDM. Enforcing first would
  break those flows.

**Decision.** Access tokens are required by default on every SBI producer, reached in phases so no
flow breaks in between:

| Phase | Content | Status |
|---|---|---|
| 1 | sbi-core server-side authorizer, before any handler: no token -> 401 + `WWW-Authenticate: Bearer` (`realm`, `error="invalid_token"` when one was sent); `aud` not own NF type/instance -> 401 `invalid_token`; `scope` lacking the API's service name -> 403 `insufficient_scope`. Exempt: notification/callback routes (TS 33.501 13.4.1.0: "The OAuth 2.0 framework does not apply to the notification operation"), NRF `/oauth2/token`, NRF bootstrapping (no security scheme in its YAML). Per-NF `oauth2_required`. Unit tests. | Done (sbi-core only, no NF switched on): `sbi_core/authorizer.hpp`, `Server::set_authorizer`, `RouteAuth` per route (default `OAuth2`). Also: a token missing a required AccessTokenClaims claim -> 401 `ACCESS_TOKEN_CLAIM_MISSING` with `invalidParams` (TS 29.500 Table 5.2.7.2-1). Tests: `SbiAuthorizer.*` (10), `Http2Authorizer.*` (1) |
| 2 | NRF token issuance: requester registered, `nfInstanceId` bound to the client certificate, NF type allowed the requested service (TS 33.501 13.4.1.1.2). | Done, switch off in `config/nrf.json`: `nfs/nrf/src/token_authz.cpp`. Unregistered requester, `nfType` differing from its profile, or a client certificate not naming its NF type -> 400 `invalid_client`; a service no registered producer of `targetNfType` offers to the consumer (`allowedNfTypes`/`Domains`/`Plmns`/`Nssais`, service- then profile-level, `nfServiceList` or deprecated `nfServices`) -> 400 `invalid_scope`; errors are now `AccessTokenErr` with `Cache-Control: no-store` (TS 29.510 Table 6.3.6.3-1, RFC 6749 5.2). Config `access_token_authorization` (env `NRF_ACCESS_TOKEN_AUTHORIZATION`), default true. Tests: `NrfTokenAuthz.*` (8); with the env on, `HelloNfIntegration` is refused, as expected (below) |
| 3 | Consumers: one sbi-core token provider cached per (target NF type, service); every outbound SBI call in all NFs carries a token for its target service. | Done: `sbi_core::TokenSource` picks the scope and `targetNfType` of a request's API from `api_catalog` (generated from the YAMLs, the producer NF type from the file name) and caches one `OAuth2Client` per pair; `http2::Client` asks it for every request sent without an `Authorization` header. `install_default_token_source` makes it the process-wide default; each NF installs it in `run_nrf_lifecycle` (23 NFs), the SCP and SEPP attach it to their NRF client only (a relay must not put its own token on forwarded requests). NFRegister now goes without a token (hello-nf fetches its token after registering). Tests: `Http2Authorizer.*` (3), `SbiApiCatalog.*` (1). Verified with the NRF check on: a live PCF's pre-registration token request is refused (`invalid_client`, not registered), its NFRegister is accepted without a token (201) and its heartbeat 15 s later carries one. `HelloNfIntegration` still fails with the check on: hello-nf registers as an AMF with the `hello-nf` certificate, which the phase-2 binding rightly refuses (phase 5) |
| 4 | `oauth2_required: true` NF by NF as its consumers are migrated, each verified end to end; ends with every NF enforcing. | Done: 25 NFs enforcing, the AAnF last with SEC-03 in phase 5 (below, "Phase 4 status") |
| 5 | Tests: helpers fetch tokens; negative tests (no token 401, wrong `aud` 401, missing scope 403, notification without token accepted). Then SEC-03 (AAnF: register/remove only from an AUSF token; `afId` bound to the caller). | Done (2026-10-17): (a) after NFRegister every NF clears the token back-off (`TokenSource::clear_backoff`, `sbi_core::clear_default_token_backoff`), so a token refused before registration is asked for again at once; (b) the tests' identity is a registered lab AF (`tests/integration/test_tokens.hpp`: `certs/af`, a fixed NfInstanceId, tokenless NFRegister first) -- the 67 hand-built `test-client` token requests and the TokenSource go through it; CI generates `af`, `af.example.com`, `gpsi-af.example.com`; (c) hello-nf, which registers as an AMF placeholder, presents the lab AMF certificate; (d) SEC-03: the AAnF enforces tokens, takes register-anchorkey/remove-context only from a peer whose certificate names the AUSF (403 otherwise; no TS 29.500/29.535 cause fits, so none is sent), and serves retrieve-applicationkey only for the afId whose FQDN the calling AF's certificate names, or from the NEF (TS 33.535 6.3 step 2) -- `AanfAuthorization.*`; (e) `config/nrf.json` `access_token_authorization: true`. Negative tests: each NF's `*Authorization.*` test (phase 4; the wrong-`aud` token is now one issued for the test AF, as the NRF issues none towards an NF type nothing registered). Verified with the NRF check on and a local Doris: full ctest 1078/1117, then the 8 failures this check exposed fixed and rerun (ADRF 5/5, UDSF 17/17, LI-SMSF 4/4, LI-NEF 2/2, AKMA + `AanfAuthorization` 2/2, hello-nf); the rest are the local BSS schema gap (26) and the open `NwdafMtlf` re-training test. Test stand-ins register like the NFs they stand in for (the LI-SMSF fake AMF) |

**Northbound APIs (user decision, 2026-10-17: "Enforce NRF tokens on all").** TS 33.122 gives AFs their tokens from a CAPIF core function, which this repo does not have; the NEF therefore accepts an NRF-issued token for itself on its northbound APIs too. Where the YAML declares OAuth2 with no scope (`scopes: {}`, most of TS 29.122/29.522), the catalog row has an empty scope and only the token (signature, claims, `aud`) is checked -- no scope is invented. Not the CAPIF model; disclosed.

**Required scope per API.** The service-level scope is read from each API's YAML, not assumed to be
its apiName: `tools/sbi-codegen/gen_api_catalog.py` writes `libs/sbi-core/src/api_catalog.inc`
(359 APIs, 217 of them declaring OAuth2 with no scope, which get an empty scope). Four differ from their apiName, e.g. `nnef-smservice` -> `nnef-smcontext`
(TS29541_Nnef_SMService.yaml), `3gpp-akma` -> `nnef-akma:gpsi-access` (TS29522_AKMA.yaml). An
apiName not in the table (a project-owned API, e.g. the UDR OAM API) requires the apiName itself.
The table is checked in; nothing yet fails CI when the YAMLs change and it is not regenerated.

**Phase 4 status.** Each enforcing NF reads `oauth2_required` from `config/<nf>.json` (env
`<NF>_OAUTH2_REQUIRED`), default true, shown in the GUI schema. Each has a test driving real nrf +
NF processes through `tests/integration/oauth2_enforcement.hpp`: no token 401 with a bare Bearer
challenge, another NF type's token 401 `invalid_token`, another service's token 403
`insufficient_scope`, its own token reaches the handler. Each test also fails with the NF's switch
off. Every API below lists 401/403 and admits tokenless requests in its YAML (`security: - {}`);
requiring a token is the local configuration TS 29.500 6.7.3 allows.

| NF | APIs enforced | Exempt | Consumers in this repo | Test |
|---|---|---|---|---|
| 5G-EIR | n5g-eir-eic | -- | none | `EirAuthorization.*` |
| GMLC | ngmlc-loc | -- | none | `GmlcAuthorization.*` |
| LMF | nlmf-loc, nlmf-dataexposure | nlmf-broadcast: TS29572_Nlmf_Broadcast.yaml declares no security scheme, so there is no scope to require; mTLS only | none | `LmfAuthorization.*` |
| NSSF | nnssf-nsselection, nnssf-nssaiavailability | -- | none | `NssfAuthorization.*` |
| BSF | nbsf-management | -- | PCF, TSCTSF (explicit `nbsf-management` tokens) | `BsfAuthorization.*`; `TsctsfEndToEnd.*` passes against the enforcing BSF |
| NSACF | nnsacf-nsac, nnsacf-slice-ee | -- | AMF, SMF, DCCF (default token source) | `NsacfAuthorization.*`; `NsacfIntegration.*`, `Dccf.*`, `AmfNgapTestGnb.*` pass |
| SMSF | nsmsf-sms | -- | AMF (default token source) | `SmsfAuthorization.*`; `amf_sms_integration_tests`, `li_smsf_e2e_integration_tests` pass |
| TSCTSF | ntsctsf-qos-tscai | its PCF notification route (`/ntsctsf-qos-tscai/v1/pcf-notify/**`, TS 33.501 13.4.1.0) | NEF (default token source) | `TsctsfAuthorization.*` (also: the notification needs no token); `TsctsfEndToEnd.*` passes |
| DCCF | ndccf-datamanagement, ndccf-contextmanagement | -- | NWDAF, ADRF (default token source) | `DccfAuthorization.*`. `Dccf.*` needs Kafka and Doris (its NWDAF exits without Doris), so it is verified only in CI |
| MFAF | nmfaf-3dadatamanagement, nmfaf-contextmanagement | `/mfaf-inbound/v1/notifications/{id}` (notification target) and `/mfaf-inbound/v1/fetch`, a callback of TS29576_Nmfaf_3caDataManagement.yaml's notification declared without security | DCCF (default token source) | `MfafAuthorization.*`; `Mfaf.*` passes with a local Kafka |
| UDSF | nudsf-dr, nudsf-timer | -- | none in this repo | its own check (scope only, no `aud`) is replaced by the sbi-core authorizer, and `config/udsf.json` now sets `oauth2_required: true` (was false); `udsf_integration_tests` 17/17 pass with test tokens, including its own 401 `invalid_token` / 403 `insufficient_scope` cases |
| UPF | nupf-ee, nupf-gueip | -- | none in this repo | `UpfAuthorization.*`; `UpfEventExposureIntegration.*`, `UpfGetUeIpInfoIntegration.*` pass |
| AUSF | nausf-auth, nausf-sorprotection, nausf-upuprotection | -- | AMF (default token source) | `AusfAuthorization.*`; `AusfIntegration.*`, `AusfUpuProtectionIntegration.*` (now with an `nausf-upuprotection` token, it used the `nausf-auth` one), `amf_sms_integration_tests`, `AkmaEndToEnd.*` pass |
| UDR | nudr-dr, nudr-group-id-map | `/oam-provisioning/v1/...`: not an SBI API, keeps its mTLS allow-list (ADR-0382) | UDM, PCF, NEF, ... (default token source / explicit tokens) | `UdrAuthorization.*` |
| UDM | the ten nudm-* APIs | -- | AMF, AUSF, SMF, ... | `UdmAuthorization.*` (the authorized read reaches the enforcing UDR with the UDM's own token) |
| PCF | the ten npcf-* APIs | the CHF spending-limit notification (`.../sm-policies/{id}/spending-limit-notify/notify`) | SMF, AMF, NEF, BSF ... | `PcfAuthorization.*` |
| SMF | nsmf-pdusession, nsmf-event-exposure, nsmf-nidd | the PCF SM policy notification (`.../sm-contexts/{ref}/pcf-notify`) | AMF, NEF | `SmfAuthorization.*` |
| AMF | the seven namf-* APIs | -- | SMF, NEF, GMLC, ... | `AmfAuthorization.*` |
| NEF | the nnef-* SBI APIs (YAML scope) and the northbound 3gpp-* APIs (token for the NEF; most of their YAMLs declare OAuth2 with `scopes: {}`, so no scope is checked) | the two `/nnef-callback/v1/...-notify` notification routes | AF (northbound), PCF, UDM, ... | `NefAuthorization.*`; the 23 other NEF suites, `li_nef_e2e`, `tsctsf_e2e`, `akma_e2e` pass |
| NRF | nnrf-nfm, nnrf-disc | NFRegister (PUT of a new instance: registration is the pre-requisite of a token, TS 33.501 13.4.1.1.2); `/oauth2/token`, `/oauth2/retrieve-key` and `/bootstrapping` (no security scheme in their YAMLs). An NFUpdate (PUT of a registered instance) is checked in the handler | every NF | `NrfAuthorization.*` (register without a token 201, update without one 401, with one 200) |
| CHF | nchf-convergedcharging, nchf-spendinglimitcontrol, nchf-offlineonlycharging; the project-owned `/chf-admin/v1`, `/internal-billing/v1` with their apiName as scope | -- | SMF, PCF, AMF | `ChfAuthorization.*`; with a local Doris: N28, CHF ceilings, Diameter RAR, PCF-CHF N28, LI-CHF, chaos charging pass |
| NWDAF | the nnwdaf-* APIs | the `/nwdaf-inbound/v1/...` callback routes (notifications, fetch, ml-models, qos-mon) | AMF, SMF, NEF, ... | `NwdafAuthorization.*`; with Doris: Phase A, Phase C, ML training sub, LI-NWDAF pass |
| ADRF | nadrf-datamanagement, nadrf-mlmodelmanagement | `/adrf-inbound/v1/notifications/{id}`; the model-file URL (`/adrf-mlmodel-files/v1/{id}`) keeps its own check -- a valid token whose subject is an allowed consumer of the model -- as no YAML scope covers that URL | NWDAF, DCCF | `AdrfAuthorization.*`; `Adrf.*` passes with Doris + its Postgres |
| SCP | nscp-ee (TS29570_Nscp_EventExposure.yaml) | -- | none in this repo (the SCP forwards no messages here) | `ScpAuthorization.*` |
| SEPP | nothing: every API it serves (TS29573 N32 handshake, JOSE-protected forwarding, telescopic FQDN mapping) declares no security scheme -- N32 is secured by TLS/PRINS, not OAuth2 | all | the PLMN-B producer behind it | `SeppEndToEnd.*` unchanged |

With 18 NFs enforcing, the integration suite (minus the suites that need Doris, Kafka or the BSS schemas locally) passes 255 tests and every separate test target passes except the two NWDAF ones (their ADRF exits without Doris); the tests fixed on the way all sent a token for the wrong service or NF type, or none: the UDM SDM/UECM, UDM MAP-server, NEF traffic-influence, N28, AUSF UPU, NSACF slice-EE and AMF Namf tests. Compose mounts `config/` for all of them. Open, not caused by this ADR: `NwdafMtlf.AccuracyBelowThresholdIsReportedAndDrivesRetraining` fails at its re-training step (test_nwdaf_mtlf.cpp:861) with every NF's `oauth2_required` off as well; it first ran locally on 2026-10-17 (it needs Doris), runs 168 s, and CI's ctest `--timeout 120` would cut it -- not investigated here. Only the TSCTSF has a Helm chart, and it mounts no config (DEP-02, open); the default (true) applies there. Tests that call an enforcing NF without a token of their own get one from `tests/integration/test_tokens.hpp` (`nf_test::attach_test_tokens`, the phase 3 `TokenSource` as `test-client`).

**Phase 2 findings.**
- **NFs fetch a token before registering.** With the NRF check on, hello-nf is refused
  (`invalid_client`, not registered): it requests its token before its NFRegister, while TS 33.501
  13.4.1.1.2 makes registration a pre-requisite. Phase 3 must order registration first in every NF;
  the switch stays off until then.
- **Certificate binding is to the NF type, not the instance.** The lab certificates carry
  `CN=<nf>`/`DNS:<nf>` (`scripts/gen-lab-pki.sh`), so the NRF checks that the certificate names the
  consumer's registered NF type (TS 29.510 5.4.2.2.1 allows checking attributes "e.g. NF type"
  against the certificate). A holder of one NF type's certificate can still claim another instance
  of the same type.
- **(Resolved in phase 5.) Test clients requested tokens as an unregistered `test-client` with the `hello-nf` certificate**
  (66 places). Phase 5 replaces them with registered identities.
- **Discovery reads only the deprecated `nfServices`.** `profile_discoverable_by`
  (`nfs/nrf/src/discovery_authz.cpp`) ignores `nfServiceList` (TS29510_Nnrf_NFManagement.yaml
  NFProfile); the new token check reads both. Not fixed here.
- An omitted `targetNfType` still defaults to NRF, and `targetNfInstanceId` (TS 33.501 13.4.1.1.2
  1b) is ignored, as TS 29.510 5.4.2.2.1 permits for unsupported parameters: the token's `aud` is
  the NF type.

**Phase 3 findings.**
- **A refused or failed token fetch is not retried for 5 s,** and the request goes without a token;
  the producer then decides. So the first NFRegister of a cold start is tokenless (as allowed), and
  a heartbeat 15 s later gets the token.
- **A 401 `invalid_token` is not acted on:** a token the producer rejects (e.g. the NRF restarted
  with a new signing key) stays cached until it expires.
- **Not every API names its producer.** 6 of 142 catalog rows have no NF type, so requests to them
  go without a token: the file name names no NF (`TS29522_AKMA.yaml`, `TS29522_VFLInference.yaml`,
  `TS24558_Eees_AppContextRelocation.yaml`), names none in the NFType enum (`TS29309_Nbsp_GBA.yaml`),
  or matches one only by prefix (`Niwmsc` / `Nrouter` vs `SMS_IWMSC` / `SMS_ROUTER`, not guessed).
  Names that equal an enum value once `_` is removed are matched (`Nmbsmf` -> `MB_SMF`, also
  `IMS_AS`, `IP_SM_GW`, `SOR_AF`). Project-owned APIs (e.g. `udr-oam`) are not in the catalog.
- **A request to a callback URI whose first path segment is a catalog apiName gets a token** for
  that API; harmless (the notification route ignores it) but a needless token request.
- The 62 explicit `OAuth2Client`s stay; their header wins over the default.

**Disclosed.**
- **`sub` cannot be checked with the current lab PKI.** TS 33.501 matches `sub` (NF instance ID)
  to the NF instance ID in the client certificate's subjectAltName; this repo's NFs generate their
  instance ID at startup and the lab certificates (`scripts/gen-lab-pki.sh`) do not carry one. Needs
  fixed instance IDs and certificates carrying them; open until then.
- Until phase 4 completes, NFs not yet switched still accept tokenless requests (today's behaviour).
- Only the service-level scope is enforced; additional (resource/operation) scopes are not.

**Rejected alternatives.**
- *Opt-in per NF, default off* -- the user chose required by default.
- *Editing each NF's `check_bearer` and its ~957 call sites* -- 25 copies of the same logic; one
  authorizer in sbi-core is enforced the same way everywhere and cannot be forgotten on a new route.
- *Switching enforcement on before consumers send tokens* -- breaks most inter-NF flows at once.
