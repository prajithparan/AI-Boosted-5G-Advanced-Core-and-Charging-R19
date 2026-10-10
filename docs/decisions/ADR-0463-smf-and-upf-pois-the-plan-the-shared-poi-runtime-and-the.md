## ADR-0463: SMF and UPF POIs -- the plan, the shared POI runtime, and the SMF xIRI codec

**Date:** 2026-10-06. **Status:** stages 1, 2a, 2b and 2c done; stages 3 and 4 not started. User-approved (event list shown
first, as every LI POI increment): shared library over copying (decision 1), and CC scope "everything except the eBPF
capture" (decision 2).
**What TS 33.127 6.2.3 / TS 33.128 6.2.3 require.** The SMF hosts an IRI-POI generating xIRI for PDU session
establishment, modification, release, start-of-interception-with-an-established-session and unsuccessful procedure
(the ProSe events and MA-PDU variants are out of scope here), matching on SUPI, PEI or GPSI independently. CC needs a
CC-TF in the SMF that triggers a CC-POI in the UPF when the SMF sends the N4 PFCP Session Establishment for a target;
**LI_T3 is the same X1 protocol** (33.128 5.2.5/6.2.3.3.1: the TF sends ActivateTask to the UPF's X1 server with UPF
target identifiers -- GTP F-TEID, UE IP, ports, PFCP session id), so li_core's X1 client/server already cover it. The
CC-POI sends xCC as TS 103 221-2 X3 PDUs (payload formats 5/6/7 or 12 = GTP-U, the last mandatory) to an MDF3, which
delivers CC over HI3.
**Two findings that shaped the plan.** (1) The UPF datapath is eBPF/XDP uplink GTP-U decapsulation ONLY: there is no
downlink path, and it cannot attach in this sandbox or in CI (no CAP_NET_ADMIN -- only PFCP runs there), so real packet
capture cannot be verified here and would cover one direction at best. (2) The AMF's POI carried ~560 lines of generic
machinery (X1 listener, task store, keepalive monitor, X2 delivery, per-XID sequencing) that every later POI would
have copied.
**Staged plan (one commit each).** 1 shared `libs/li-poi` + migrate the AMF [done]; 2 SMF IRI-POI: (2a) xIRI codec for
the five records [done], (2b) the SMF POI + hooks + registered-session registry [done], (2c) ADMF provisioning of the SMF POI
and PEI/GPSI target mapping [done]; 3 CC chain: CC-TF in the SMF, CC-POI in the UPF behind a packet-tap seam (fed by tests),
X3, `nfs/li-mdf3`, HI3 CC delivery, ADMF `cc_capable`; 4 the eBPF capture itself, deferred to a privileged lab host and
NOT enabled until verified there (so `cc_capable` stays false in a real deployment until then).
**Stage 1 -- `libs/li-poi` (`PoiRuntime`).** Owns the LI_X1 listener, the warrant store, the clause-6.6.2 keepalive
monitor, per-XID sequence numbers, LI_X2 framing/delivery and the worker that runs a POI's "targets added" callback off
the X1 request path. A POI supplies hooks: the target kinds it can match, an identity normaliser and the callback.
Behaviour change worth recording: a task naming a target kind the POI cannot match is now refused with X1 3010
(UnsupportedTargetIdentifier) at provisioning -- before, the AMF accepted it and it silently never matched. The AMF keeps
only what is AMF-specific (identity matching, registered-UE registry, the six `report_*` records).
**Proof (stage 1).** The pre-existing guard suites pass unchanged on the refactored AMF: `li_amf_poi_integration_tests`
(8), the four real-AMF end-to-end tests including the full ADMF chain; plus `LiPoiRuntime.*` (7 new tests: X1 lifecycle,
3010, per-kind/normalised matching with one match per task, hook off the X1 thread and only for new targets, X2 framing
with per-XID sequence numbers, strict UUID parsing).
**Stage 2a -- the SMF xIRI facade** (`li_core::xiri`): `SmfPduSessionEstablishment`, `Modification`, `Release`,
`SmfStartOfInterceptionWithEstablishedPduSession`, `SmfUnsuccessfulProcedure`, with the shared building blocks FTEID,
S-NSSAI, UEEndpointAddress, PEI (IMEI/IMEISV), GPSI (MSISDN/NAI). Mandatory members as in the ASN.1: Establishment
{pDUSessionID, gTPTunnelID, pDUSessionType, dNN, requestType}; Modification {requestType}; Release {sUPI, pDUSessionID};
StartOfInterception adds a MANDATORY uEEndpoint list (OPTIONAL in Establishment -- the ASN.1 differs between the two);
Unsuccessful {failedProcedureType, failureCause, initiator}. **Not modelled, disclosed:** MA PDU sessions, ProSe, EPS
interworking, PCC rules, UP path change, satellite, hSMF URI, mapped-HPLMN slice values and the other C/O members.
`XiriSmf.*` (6 tests): every modelled member round-trips, minimal forms, and the ASN.1 constraints (IMEI 14 / IMEISV 16
digits, MSISDN 1..15, IMSI 6..15, non-empty DNN) are enforced on encode.
**Stage 2b -- the SMF POI** (`nfs/smf/src/li_poi.cpp`, on `li_poi::PoiRuntime`, config block `li_poi`, off by default).
Matches SUPI, PEI and GPSI independently. Hooks: CreateSMContext (establishment once the N1N2 transfer carrying the
accept succeeded; otherwise UnsuccessfulProcedure on a non-201), SMContext release, UpdateSMContext on path switch and
on `hoState` COMPLETED (modification), and start-of-interception for sessions already established when a warrant is
activated (registry of established sessions). Directions: establishment FromTarget, unsuccessful by initiator, the rest
NotApplicable. **Proof:** four real-SMF end-to-end tests in `test_li_amf_e2e.cpp` (`LiSmfEndToEnd.*`); a negative control
with the release/modification hooks disabled failed as expected.
**Disclosed gaps / simplifications (2b):** this SMF allocates no UE IP, so `uEEndpoint` is omitted; the AMF does not send
pei/gpsi/requestType/ueLocation in CreateSMContext, so PEI/GPSI matching is implemented but not exercised by a real AMF
and `requestType` defaults to InitialRequest; the failureCause mapping from HTTP status (31, or 26 for 503) is
implementation-defined; modification is reported only for path switch and handover completion; no MA PDU, ProSe or
EPS-interworking records. After an N2 handover this AMF does not process a deregistration on the target association
(pre-existing, unrelated).
**Stage 2c -- ADMF provisioning.** Each network element declares `target_elements`; the LIPF sends a POI only the
targets it can match (skipping a POI that would get none), and a task is infeasible only when a target is carried by no
POI. An empty list means the AMF set (supiimsi, supinai, imsi, nai). HI1 PEI/GPSI formats (PEIIMEI, PEIIMEISV,
GPSIMSISDN, GPSINAI) map to the X1 elements, and GETCSPCONFIG advertises the union of the POIs' formats. **Proof:**
`LiAdmfLipf.EachPoiIsToldOnlyTheTargetsItCanMatch` and two further new LIPF tests; the li-admf suite is 45/45. **Not
done:** an ADMF-level end-to-end test through a real SMF POI (needs a UPF in the harness); the SMF compose service
exposing X1 port 7809 is configured in the overlay but not run.

