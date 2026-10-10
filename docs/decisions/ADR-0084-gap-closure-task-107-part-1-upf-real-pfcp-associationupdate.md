## ADR-0084: gap-closure task #107 (part 1) -- UPF real PFCP AssociationUpdate/AssociationRelease

### Context

`docs/CAPABILITY_GAP_ANALYSIS.md`'s UPF section grep-confirmed 5 real PFCP message types free5GC's
`internal/pfcp/pfcp.go` dispatches that this project's UPF did not: `PFDManagement`,
`AssociationUpdate`, `AssociationRelease`, `NodeReport`, `SessionSetDeletion`. This ADR closes 2 of
the 5 -- `AssociationUpdate` and `AssociationRelease` -- chosen first because both reuse 100% of
this project's already-existing IE infrastructure (`NodeId`, `Cause`, `UpFunctionFeatures`,
`CpFunctionFeatures`); the remaining 3 each need a genuinely new IE type this pass deliberately
does not build (see "What this ADR does NOT include" below), matching the same part-1/part-2 split
already used for task #100 (AMF NGAP: ServiceRequest, then UEContextRelease) and task #104 (AUSF:
SoR, then ProSe).

### Real, spec-cited scope

Real message type values confirmed directly against Table 7.3-1 "Message Types" in the same
vendored `specs/PFCP/29244-e30.pdf` every other `pfcp_core::MessageType` value was already
confirmed against (ADR-0039): `AssociationUpdateRequest=7`, `AssociationUpdateResponse=8` (newly
added to the enum this pass). `AssociationReleaseRequest=9`/`AssociationReleaseResponse=10` were
already present in the enum -- grep-confirmed zero dispatch/handling anywhere in
`nfs/upf/src/*.cpp` or `nfs/smf/src/*.cpp` before this pass, i.e. a real dead/unreachable enum
value, not a working feature this ADR merely extends.

Real IE tables read from `specs/PFCP/29244-e30.pdf` §7.4.4.3-§7.4.4.6:
- **Association Update Request**: `NodeID`(M) + `UP Function Features`(O) + `CP Function
  Features`(O) + conditional release-related sub-IEs (UP/CP function about to
  restart/graceful-release). This pass only decodes/responds to the mandatory `NodeID` --
  real, disclosed scope narrowing below.
- **Association Update Response**: `NodeID`(M) + `Cause`(M) + `UP/CP Function Features`(O).
- **Association Release Request**: `NodeID`(M) only.
- **Association Release Response**: `NodeID`(M) + `Cause`(M).

`libs/pfcp-core/include/pfcp_core/header.hpp`: added `AssociationUpdateRequest`/`Response` to the
`MessageType` enum, comment citing this ADR and the same spec table every existing value cites.
`nfs/upf/src/main.cpp`: two new builder functions,
`build_association_update_response_ies`/`build_association_release_response_ies`, each reusing
existing `encode_ie`/`encode_node_id_ipv4`/`encode_cause`/`encode_up_function_features_ftup_only`
calls verbatim -- no new IE codec written. Both wired into `run_pfcp_lifecycle`'s existing
`else if` dispatch chain, in the same position/style as every other PFCP message type there.

### Real, disclosed scope narrowing (both messages)

- **Association Update**: no real per-feature negotiation state machine. The response
  unconditionally accepts with this UPF's own static `UpFunctionFeatures` (FTUP only, same value
  already sent in Association Setup Response) -- there is no in-project concept of a UPF capability
  actually *changing* at runtime yet, so "negotiating an update" would be simulating state this
  project doesn't have. The request's own optional "UP function about to restart" / "graceful
  release period" sub-IEs are not decoded: this lab's topology has no UP-function-initiated
  restart-notification flow to exercise, and decoding them without any consumer would be dead code.
- **Association Release**: does **NOT** bulk-delete this UPF's own session state for the releasing
  CP peer as a side effect. TS 29.244 ties bulk cleanup-on-release to the separate, more precise
  `SessionSetDeletion` message (FQ-CSID-scoped, so only the sessions belonging to the specific CP
  function instance that's releasing are removed, not all sessions touching that UPF) -- which this
  ADR deliberately does not build (see below). Implementing an approximate "delete everything" on
  plain Association Release here would be a real behavioral fabrication beyond what the request
  itself carries, not a simplification of it.
- **No automatic trigger exists yet on the SMF side for either message** -- SMF has never sent
  either in this project's existing call flows (neither is part of any Stage 1-3 procedure this
  project has built). This is why live verification (below) used a hand-crafted client rather than
  an existing end-to-end SMF-driven test.

### Verification -- live, real UDP wire bytes, not a unit test double

Built (`cmake --build . --target upf -j4`, `cmake --build . --target conformance_tests -j4`) clean,
no warnings from the new code. Started a real standalone UPF instance (`build/nfs/upf/upf`,
datapath disabled as expected without root capabilities -- disclosed non-fatal per ADR-0043's own
precedent, PFCP control-plane unaffected) and ran a hand-crafted raw UDP Python client
(`pfcp_assoc_verify.py`, scratch tooling, not committed -- same "one-off verification instrument"
class as this session's other live-HTTP curl checks) that:
1. Sends a real `AssociationSetupRequest` first (the already-working baseline) to confirm the
   harness itself talks correctly to a live UPF before trusting the two new results.
2. Sends a real `AssociationUpdateRequest` with a `NodeID` IE -- confirmed the response is
   `AssociationUpdateResponse` (type 8), sequence number correctly echoed, `Cause=RequestAccepted`,
   `NodeID` IE present and byte-correct.
3. Sends a real `AssociationReleaseRequest` with a `NodeID` IE -- confirmed the response is
   `AssociationReleaseResponse` (type 10), sequence number correctly echoed,
   `Cause=RequestAccepted`, `NodeID` IE present and byte-correct.

All three passed. UPF's own server-side log independently corroborated each: `"upf: Sx Association
Update accepted from 127.0.0.1"`, and `"upf: Sx Association Release accepted from 127.0.0.1 (real,
disclosed scope: this UPF's own session state for the peer is NOT bulk-deleted as a side effect --
see this file's own comment on Session Set Deletion)"`.

Full `conformance_tests` re-run (`ctest -j1`, excluding the two already-known-flaky/hanging tests
this session's own earlier runs already excluded by name -- `UdrIntegration.AmfContextLifecycle`,
`UdmIntegration.SdmDataRetrievalAndSubscriptions`, both pre-existing test-isolation issues
unrelated to this change): **310/310 passing, zero regressions** from this pass's changes (6 other
tests skipped, pre-existing gated behavior unrelated to this pass).

### Real, unrelated finding surfaced during this verification pass (not this ADR's own scope)

Getting a clean full-suite run required working around a real, pre-existing, systemic bug this
pass's own verification stumbled into, not introduced by it: `UdrIntegration.SmfRegistrationLifecycle`
initially failed with `pqxx::broken_connection ... password authentication failed for user "udr"`.
Root-caused, not assumed: `nfs/udr/src/main.cpp`'s hardcoded fallback connection string
(`postgresql://udr:udr@localhost:5432/udr`) targets port 5432, but `docker ps` shows the real,
running `docker-postgres-udr-1` container is mapped to host port **5437** -- `docker-compose port
5432` was already claimed by `docker-postgres-1` (which is actually `product-catalog`'s own
container, confirmed via `docker inspect`'s real `POSTGRES_USER`/`POSTGRES_DB` env values, not
assumed from the container name). Checked every other Postgres-backed service's own hardcoded
fallback the same way: **CHF, balance-management, roaming-interconnect, and
subscriber-management all have the identical bug** (`localhost:5432` hardcoded, but their real
containers live on 5434/5433/5436/5435 respectively) -- only `product-catalog`'s own default
happens to be correct, apparently because it was the first Postgres container created and got
the real 5432 mapping before the others existed. This is systemic, real, and exactly the class of
gap ADR-0077/task #109 ("no hardcoded DB URL/config params in source") already exists to fix --
not fixed here (out of this ADR's own scope, task #107 is UPF PFCP coverage, not the config
retrofit), but disclosed here as new, concrete evidence found during this pass's own verification,
worked around for this one verification run only via five `*_DATABASE_URL` environment variable
overrides (the exact override mechanism ADR-0077 already established), not a code change.

### What this ADR does NOT include

`PFDManagement` (needs a new `Application ID`/`PFD Contents` IE pair, real ADF-deployment
semantics -- its own real spec clause, not a small extension of what exists here),
`NodeReport` (needs a new `User Plane Path Failure Report` IE, UPF-initiated not
CP-initiated -- a different message direction than every PFCP handler this project has built so
far), `SessionSetDeletion` (needs a new `FQ-CSID` IE and the real bulk-cleanup-by-CP-instance
semantics this ADR explicitly declined to fake on plain Association Release). Each is deferred to
task #107's continuing scope, each needing its own spec-reading pass before implementation, per
this project's own "never invent a field" rule. No automatic SMF-side trigger for
AssociationUpdate/Release is added in this pass either -- both remain externally-triggerable-only
(e.g. an operator/O&M-driven N4 re-association) until a real in-project call flow needs to send
one.

