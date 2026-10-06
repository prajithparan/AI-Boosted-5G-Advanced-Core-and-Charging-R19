## ADR-0462: `nfs/li-admf` (LI increment 5) -- plan, decisions, and step 1: the X1 client codec

**Date:** 2026-10-06. **Status:** accepted; step 1 done, steps 2-6 below not started.

**Spec sources.** ETSI TS 103 120 v1.24.1 (LI_HI1, prose; fetched by `tools/specs/fetch_etsi_specs.py`,
gitignored) and its XSD/JSON schema sets, dictionaries and examples (vendored at
`specs/etsi/103120/`, BSD-3-Clause, forge commit e531df05 -- supplied by the user from the forge
archive after `forge.etsi.org/rep` timed out from this host). **The schemas are v1.23.1** (the forge
README's newest entry) while the prose read is v1.24.1; the diff is editorial plus JSON-signing text
(Annex I, not implemented). TS 33.127 5.3.5 defines the ADMF as LICF + LIPF: HI1 (northbound, from the
LEA) and LI_X1 (southbound, to POIs/TFs/MDFs).

**Decisions (user-approved 2026-10-06).**
1. **XML only first.** TS 103 120 9.2.0 lets a Receiver answer "encoding not supported" for JSON; libxml2
   XSD validation is already in `li_core`. JSON would need a JSON-Schema validator dependency, to be
   evaluated for licence and vcpkg availability before adoption.
2. **State in a dedicated PostgreSQL database**, not Valkey: warrants and legal documents are durable
   records, not rebuildable cache, and TS 33.127 5.6 puts LI functions in a separate security domain
   (a new Compose service, approved).
3. **Scope: the H.5 LI lifecycle workflow profile only.** Out of scope, recorded as gaps: LD/LP/TD
   profiles, traffic and IRI policies, JSON signing/encryption (9.2.3, Annex I), national profiles,
   LI_X0 and virtualised-deployment lifecycle, SMF/UPF provisioning (those POIs do not exist yet), LI
   service discovery (endpoints come from `config/li-admf.json`).
Like `li-mdf`, the ADMF is NOT an SBI NF and does not register with the NRF (TS 33.127 5.5: POIs/TFs/
MDFs are not within NRF service discovery) -- DoD step 2 is N/A, not skipped.

**Build order (one commit each, tests first).** (1) X1 client codec in `li_core` [done below];
(2) HI1 XML codec + validation wrapper (tested on ETSI's own examples); (3) `li-admf` skeleton: config,
mTLS listener, PostgreSQL store; (4) the six H.5 endpoints (`/li/authorisation/{new,extension,
cancellation}`, `/li/task/{addition,cancellation,change-delivery}`) + the LICF warrant->X1 mapping;
(5) Notifications to the LEA and status reflection; (6) end-to-end LEA -> ADMF -> AMF POI -> MDF2,
Compose/config-mount/Helm per the Definition of Done.

**Step 1 -- what was built.** `li_core::x1` grew the ADMF half of the codec: `serialise_request`
(every ADMF->NE request, schema-validated before it is returned -- 7.2.1), `parse_response` (OK, Error
and GetTaskDetails responses with TaskStatus and faults; an X1TopLevelErrorResponse is reported as an
error), and the NE->ADMF `ReportTaskIssue` / `ReportNEIssue` requests (parsed; the NE side answers 1080).
Transport stays outside `li_core`, as on the server side. Facts worth keeping:
- TS 103 280 `IPv6Address` is the fixed `([0-9a-f]{4}:){7}[0-9a-f]{4}` form, no `::` compression: the writer
  expands any textual IPv6 (inet_pton), and a value it cannot parse is passed through so the schema check
  rejects it rather than the codec guessing.
- Destination addresses are written with TCPPort (X2/X3 run over TCP); the reader still accepts either.
- The writer refuses (error, never emits) a request that fails the schema, e.g. a non-UUID `xId`.
**Proof.** `LiX1Client.*` (8 conformance tests: full-fidelity ActivateTask round trip incl. mediation
details/LIID/gating, all other request types and multi-request containers, never-emit-invalid, OK/Error/
GetTaskDetails/top-level-error/XXE response parsing, Report* parsing, 1080 from the NE side). And against
the REAL processes over real X1/mTLS: `li_amf_e2e_integration_tests` and `li_mdf_integration_tests` now
build their X1 requests with `serialise_request` and parse the answers with `parse_response` -- the real
AMF POI and the real MDF2 accept what the codec emits.
**Disclosed.** GetTaskDetails is parsed on the client side but the NE side (AMF POI, MDF2) still answers it
1080; ModifyDestination, GetDestinationDetails, GetNEStatus, GetAll*, ListAllDetails and the generic-object
messages are not modelled.

**ADR-0462, step 2 (2026-10-06): the HI1 XML codec.** `li_core::hi1` (`hi1.hpp`/`hi1.cpp`) parses and builds
TS 103 120 messages: Request/Response with GET, CREATE, UPDATE, LIST, DELIVER and GETCSPCONFIG actions (and
their responses, per-action `ErrorInformation`, and the message-level failure of 9.2.2). Every message is
validated against the schema set, in both directions, through the project-owned wrapper
`specs/etsi/103120/hi1-validation.xsd` (libxml2 needs a schemaLocation per import; the normative files
import by namespace only). Decisions and facts worth keeping:
- **Objects are held as self-contained XML, not re-modelled.** An HI1Object has 15-20 optional members, several
  deep and nationally extensible; modelling each would silently drop what it missed, and a warrant must come
  back from GET exactly as the LEA created it. `Object` gives typed reads of the members the ADMF uses
  (`view_authorisation`, `view_litask`) and ORDERED edits: a replaced or inserted member lands at its XSD-
  sequence position (the order tables in `hi1.cpp` are transcribed from the XSD sequences; the negative control
  below proves the schema check catches a wrong order). An edit to a member the type does not have is an error.
- **xmldsig is a project-owned stub** (`xmldsig-stub.xsd`): the W3C schema has a DOCTYPE referencing DTDs libxml2
  cannot fetch under NONET, and signing (9.2.3, national-profile-defined) is not implemented. A signed message
  is therefore accepted and its signature NOT verified -- disclosed.
- **xsi:type is a QName inside a value**, which libxml2's namespace reconciliation cannot see; `extract_object`
  collects those prefixes first and re-declares them on the copied root, so an object stays self-contained.
- A stored/echoed HI1Object keeps the sender's prefixes; only objects the codec itself builds use its fixed ones.
- 26 of ETSI's 27 example messages validate against the wrapper; the 27th (`request5-XML-Delivery.xml`) carries
  extension content from ETSI's example-only `FooServiceSchema.xsd` and is correctly refused.
**Proof.** `LiHi1.*` (12 conformance tests): every ETSI request/response example parses (>= 15 requests, >= 8
responses); values read back from `request1.xml` match what ETSI printed (LIID1, E.164 442079460223, IRIandCC,
192.0.2.0, Authorisation W000001); examples round-trip through the writer (which validates what it emits);
GETCSPCONFIG response from ETSI's `response_config.xml` reads back; edits land at schema position and a message
carrying the edited object validates; a request of every verb and a response of every kind round-trip; bad
input (garbage, wrong root, schema-invalid with the header recovered, request-as-response, XXE) is refused; the
writer refuses an invalid message. **Negative control run:** with insertion forced to append, the edit test
fails with the schema's own "AuthorisationDesiredStatus: This element is not expected".
**Disclosed.** XML only (decision 1); the Dictionaries element of a GETCSPCONFIG response is not modelled; LD/LP/
TD/TrafficPolicy/IRIPolicy objects parse as `ObjectType::Other` and round-trip but have no typed view.

**ADR-0462, step 3 (2026-10-06): the `li-admf` skeleton.** `nfs/li-admf` (a static `li_admf_core` library plus the `li-admf`
binary): the LI_HI1 receiver over the sbi_core TLS 1.3 + mTLS HTTP/2 server, `Hi1Service` (transport-independent
message handling), `Hi1Store` (the PostgreSQL warrant store on the shared bounded `PgPool`), `config/li-admf.json`,
`nfs/li-admf/schema.sql`, and a dedicated `postgres-li` database (compose + both CI service blocks, port 5439 / 15439).
Behaviour implemented now: transport authentication (the mTLS client certificate CN must be a configured
`lea_bindings` entry, else HTTP 403 before the message layer -- 9.3.4), XML-only encoding (3019 otherwise), schema
validation (3020, the reply addressed to the sender with ITS transaction id, 6.2.5), ETSIVersion check (3021 listing
the supported versions, D.1), ReceiverIdentifier == this CSP and SenderIdentifier == the EndpointID the peer is onboarded
as (Annex D has no code for either; 3007 "improper value" is the closest and is cited in the description), Action
Identifiers 0,1,2,... (6.4.4: duplicate -> 3002, otherwise 3007, top-level, before any action), GETCSPCONFIG (the six
LI workflow endpoints at their table H.0b paths under `public_base_url`, and the ETSI-defined target formats the AMF POI
can match), an append-only `hi1_audit` row for EVERY exchange including refusals, and a 503 -- not an unaudited
answer -- if the audit write fails. HTTP status is 200 for every HI1 outcome (9.3.3), 403 only for an unbound peer.
**Interim, stated plainly:** every action other than GETCSPCONFIG is answered 3001 (feature not supported) until step 4
lands the six H.5 workflows -- a warrant that is acknowledged but not acted on is the worst failure an LI system can
have, so nothing is silently accepted in the meantime.
Facts worth keeping: the `<SERVICE>_CONFIG_FILE` override keeps hyphens (`LI-ADMF_CONFIG_FILE`, the same convention as
`OAM-GUI-BFF_CONFIG_FILE`) -- a test that set `LI_ADMF_CONFIG_FILE` silently ran the real config on the real port; and
pqxx 8 returns proxy types (`field_ref`/`row_ref`), so row helpers must be templates.
**Proof.** `li_admf_integration_tests` (13): 8 service tests (config, unbound peer, JSON, unparseable + schema-invalid,
version, identities, action ids, un-implemented actions refused), 4 store tests against real PostgreSQL (duplicate
identifier, optimistic replace, composable bound-parameter list filters incl. a SQL-looking value, audit append) and
the real process over real HTTPS + mTLS (GETCSPCONFIG, a workflow path, garbage -> 3020 over HTTP 200, a valid-but-
unbound certificate -> 403, and exactly 4 audit rows for the 4 exchanges). **Disclosed.** Step 3's `lea_bindings` is
the whole LEA authorisation model (no per-endpoint ACL by workflow); the GETCSPCONFIG `LastChanged` is the process
start time; no Docker image / compose service / Helm chart for the ADMF process itself yet (step 6).

**ADR-0462, step 4 (2026-10-06): the six LI lifecycle workflows, the LIPF and the reconcile loop.** `Lifecycle`
(`nfs/li-admf/src/lifecycle.{hpp,cpp}`) implements TS 103 120 Annex H.5 on its own two-phase shape; `Lipf`
(`lipf.{hpp,cpp}`) is the X1 client that provisions the POIs and the MDF2; `Hi1Service` routes by path (table H.0b).
- **Request phase (`handle`).** The message must meet the endpoint's requirements (H.5.3.3-H.5.8.3: which verbs, which
  object types, how many) or the whole request is refused top-level (3005/3007) and nothing changes (H.5.2.2.3). Every
  action is then validated BEFORE anything is written: CREATE (Notification -> 3007, Receiver-only; Generation or a
  Receiver-owned Status supplied -> 3007; existing id -> 3010; unresolved link -> 3016, expired target -> 3017, link to
  an object refused in the same message -> 3018; Document ContentType off the H.5.2.3.4 list -> 3007) and UPDATE
  (unknown / another LEA's object -> 3011; Expired/Cancelled/Rejected -> 3012; Status supplied -> 3006; stale Generation
  -> 3008; then the workflow's own tables H.1-H.5). If ANY action is refused, none is applied: the refused ones carry
  their own error and every other action says "not applied" (H.5.2.2.4) rather than looking accepted. Valid requests are
  stored in ONE transaction (`Hi1Store::apply`), Generation = 1 / +1, initial statuses AwaitingApproval, and the
  positive acknowledgement is returned. UPDATE is the field-level merge of 6.4.7 (`Object::merge`).
- **Review-and-action phase (`reconcile_once`).** Driven only by stored state, idempotent, crash-safe (the worker wakes
  on a request and on an interval): an Authorisation is reviewed atomically with its initial tasks -- if any task cannot
  be provisioned in this deployment (no CC-POI, an identifier no POI can match, no destination) the WHOLE authorisation,
  its tasks and documents are Rejected with the reason (H.5.2.2.4's "all the requested changes are rejected"); otherwise
  Approved and the tasks provisioned through the LIPF, Active on success, Error (retried on `retry_interval`) on an NE
  failure. DesiredStatus Cancelled/Rejected/Suspended, an expired EndTime, an extension and a change of delivery are all
  just differences between desired and provisioned state that the same pass resolves. A task with a future StartTime
  is AwaitingProvisioning. Every change set per Authorisation is announced by one NotificationObject (7.4) naming the new
  statuses and, for a rejection, the reason and the request's transaction id (H.5.2.2.4).
- **LIPF.** Per task: the MDF2 destinations (a deterministic DID from XID + address, so a retry and a deprovision find
  the same one), the MDF2 task carrying the XID -> LIID mapping (Annex C.2.2 MediationDetails), then every POI's task with
  the deployment's identifier-association gating. All-or-nothing: a refusal rolls back what the call created; an
  existing XID converges by ModifyTask; deprovisioning is idempotent (X1 2020 counts as done). A task that needs CC is
  REFUSED, never silently downgraded to IRI-only.
- **Access control.** Every object carries its owning LEA (the EndpointID its mTLS peer is onboarded as); GET/LIST/UPDATE
  see only that LEA's objects (a foreign id is "not found", 3014/3011) and LIST is capped (`maximum_list_records`).
- **Mapping choices (implementation-defined, not in the spec).** HI1 TargetIdentifierValue FormatName SUPIIMSI/SUPINAI/
  IMSI/NAI -> the X1 supiimsi/supinai/imsi/nai identifier; TaskDeliveryType IRIOnly/CCOnly/IRIandCC -> X2Only/X3Only/
  X2andX3 (HI2/HI3 on the MDF2); an absent task DesiredStatus is treated as Active once its Authorisation is approved
  (ETSI's own example request carries none); the LITask's ObjectIdentifier is the X1 XID; delivery addresses must be
  IPAddressPort (DestinationReference/URL/FQDN are refused as Invalid).
**Proof.** `li_admf_integration_tests` is now 36 tests: 16 lifecycle tests against real PostgreSQL and in-memory NEs built
on li_core's real X1 server codec (acknowledged-first-then-actioned, shape refusal, all-or-nothing, create rules incl.
3010/3016/3018, CC rejection of the whole authorisation, future start, NE failure and recovery, extension incl. tables H.1/H.2,
authorisation and task cancellation, task addition (and refusal on a cancelled authorisation), change of delivery with the old
destination retired, expiry, LEA isolation, update rules) + 7 LIPF tests + the step-3 tests.
**Disclosed gaps.** Notifications are stored and pollable (GET/LIST) but not yet pushed to the LEA (DELIVER, step 5); the
task Timespan.ProvisioningTime/TerminationTime members and TaskStatus reflection from X1 GetTaskDetails/ReportTaskIssue
are not yet used (step 5); the review is automated -- there is no human-approval hook (a national-profile matter);
TrafficPolicy/IRIPolicy references on a task are stored but ignored; a task that goes Invalid is re-evaluated each pass.

**ADR-0462, step 5 (2026-10-06): the ADMF keeps its network elements alive and hears from them.** Two things the build
could not run in production without:
- **X1 Keepalive (TS 103 221-1 6.6.2).** An NE that hears nothing from its ADMF within TIME_P2 raises a fault and, by default,
  deactivates every task (the AMF POI's config does: p2 180 s, `x1_allow_deactivate_all` true). An ADMF that does not send
  keepalives therefore takes its own interceptions down. `Lipf::keepalive_all` sends one to every NE and
  `Lifecycle`'s worker sends them every `keepalive_interval_seconds` (30), the first immediately, a failure per NE logged at
  error (one NE failing never hides another).
- **`POST /X1/ADMF` (7.2.2.2), the NE -> ADMF direction.** `li_core::x1::handle_request` gained `report_task_issue` /
  `report_ne_issue` callbacks (an NE installs neither and still answers 1080). The ADMF route accepts only a client certificate that
  is a configured network element's `peer_cert_cn` (else HTTP 403), checks `admfIdentifier` (1040) and that `neIdentifier` is THAT
  element's (1060), and audits. `ReportTaskIssue`: TerminatingFault / FullyActionedAndUnsuccessful -> the task goes Error with the NE's
  details as its InvalidReason, the LEA is notified, and the next reconcile pass re-provisions it; ImplicitDeactivation -> Expired;
  AllClear / Warning / NonTerminatingFault / FullyActionedAndSuccessful are logged and change nothing; a task this ADMF never provisioned
  -> X1 2020. `ReportNEIssue` is logged at error (the AMF POI's own sender is not built yet: it still only logs that the report is "due").
**Decision: notifications are polled, not pushed.** TS 103 120 7.4 says the use of NotificationObjects is "subject to national
agreement", and DELIVER (6.4.10) is defined for answering a lawful request with data, so pushing a Notification with it would be
inventing a flow. The LEA reads them with GET / LIST (scoped to its own objects). A national profile that wants push is a later,
profile-driven addition.
**Bug found by the test, not by inspection:** `NetworkElement` gained `peer_cert_cn` BETWEEN two strings that `main.cpp` filled
positionally, so `x1_url` and the CN were swapped and the real ADMF would have posted X1 to a certificate name. The unit tests
use named construction and passed; the process test over real mTLS refused the AMF's own certificate and exposed it. The field is
now last and the order documented.
**Proof.** `LiX1Client.AnAdmfServerHandlesReportsThroughItsCallbacksAndAnNeAnswers1080`; `LiAdmfLipf.Keepalives...`;
`LiAdmfLifecycle` x4 (terminating fault -> Error + notification + recovery on the next pass; implicit deactivation -> Expired and
not resurrected; informational reports change nothing; stray / non-task xid -> 2020); `LiAdmfProcess.TheNesReportToTheAdmf...` over
real mTLS (accepted, 2020, 1060, 403 for a foreign certificate). `li_admf_integration_tests` = 42.
**Disclosed.** GetTaskDetails (a reconciliation audit of what the NEs actually hold) is built on the client side but the AMF POI and
MDF2 still answer it 1080, so the ADMF's view of provisioning is its own record plus the NEs' reports, not a cross-check.

**ADR-0462, step 6 (2026-10-06): the whole chain, end to end, and the deployment artifacts.**
`LiAdmfEndToEnd.AWarrantServedOverHi1InterceptsARealUeAndStopsWhenCancelled` (in `li_amf_e2e_integration_tests`) spawns the real
NRF/UDR/UDM/AUSF/PCF/SMF/AMF fleet (the AMF with its IRI-POI enabled), the real `li-mdf` and the real `li-admf`, plus a loopback LEMF,
and drives it as the actors would: the LEA serves a warrant over HI1 (New Authorisation: an Authorisation, an LITask on the test UE's
IMSI delivering to the LEMF, a Document); the task goes Active on the REAL POI and MDF2 through the ADMF's own X1 client (the LEA reads
`Active` back over HI1); a real UE registers over real NGAP/NAS; the LEMF receives the HI2 record carrying the LEA's own LIID and the
target IMSI; the LEA cancels the authorisation over HI1; the task goes Cancelled; the same UE's PDU session and deregistration then
produce NO further record; and the LEA's Notification objects (>= 2: activation, cancellation) are listable over HI1.
**Negative control run:** with the cancellation not sent, the post-cancel assertion fails (4 records at the LEMF vs the 2 expected).
It needs PostgreSQL and Valkey and skips without them (CI provides both; `postgres-li` was added to both CI service blocks, and
`LI_ADMF_DATABASE_URL`/`li-admf` to the CI env / PKI list). **Deployment (DoD items 7/9, compose + Helm):** `deploy/docker/li-admf.Dockerfile`
(derived from li-mdf's, which documents the li_core/ETSI-schema/config additions), a `li-admf` + `postgres-li` compose service pair with
the mandatory `../../config:/build/config:ro` bind mount and a compose-only overlay (`deploy/docker/li-admf.compose.json`, selected by
`LI-ADMF_CONFIG_FILE`, container-network NE addresses only), `deploy/helm/li-admf` (lab-grade like the others, exactly one replica), and
the README LI row. **Disclosed, not verified here:** the new Dockerfile was NOT built (the host's Docker/vcpkg path has defeated image
builds before, ADR-0442) and the Helm chart is unlinted (no `helm` on this host); the compose file passes `docker compose config`.
Also unfixed: the lifecycle reconcile loop has no leader election, so the ADMF must run as ONE replica.
**CI lessons recorded.** (1) Two earlier CI "failures" of this increment were this session's own local test runs colliding with the
runner's Test step on the same loopback ports (the CI log shows `nrf: bind: Address already in use` at the minute a local run started) --
not defects; (2) `clang-format-18 --dry-run --Werror` over libs/nfs/tests is a CI gate (the `lint` job): format every touched file before
pushing (`clang-format-18 -i $(git diff --name-only <base> | grep -E '\.(cpp|hpp)$')`).

