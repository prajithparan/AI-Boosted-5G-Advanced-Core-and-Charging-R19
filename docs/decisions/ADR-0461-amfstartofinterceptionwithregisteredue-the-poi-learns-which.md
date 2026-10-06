## ADR-0461: AMFStartOfInterceptionWithRegisteredUE -- the POI learns which UEs are registered

**Date:** 2026-10-06. **Status:** accepted. The last wired-but-untriggered LI increment-4 event
(ADR-0378 / ADR-0457 recorded "no POI<->AMF registration-state coupling"). Design shown first and
the three open decisions taken as recommended by the user.

**Spec.** TS 33.128 6.2.2.2.5: generate the record "when it detects that a new interception for a
UE is activated (i.e. provisioned by the LIPF) and the 5GMM state ... is 5GMM-REGISTERED", one per
access type. M members registrationResult/sUPI/gUTI; C members used here: location (last known)
and timeOfRegistration (REGISTRATION ACCEPT sent, UTC). Payload Direction 5 (not applicable).
Gating follows 6.2.2.2.1: emitted for Absent and All, never for an IdentifierAssociation-only
warrant. Emitted only under the NEWLY activated warrant's XID (the record exists so an additional
warrant is not blind to a UE that registered earlier).

**What the AMF lacked.** No single record of "this SUPI is 5GMM-REGISTERED" (UeContextStore is a
SUPI-keyed JSON blob written only if the PCF call succeeds; UeSecurityContextStore is keyed by
TMSI; the GUTI lived only in per-association auth_state) and no stored location.

**Decisions.**
1. *Registered-UE state is in-process, inside `LiPoi`* (decision 1: in-process now, Valkey with
   the P11 state externalisation). The AMF pushes it in -- `note_registered` at RegistrationAccept
   (GUTI, the Accept-answered UplinkNASTransport's location, UTC accept time), `note_location` at
   HandoverNotify / PathSwitchRequest, `note_deregistered` at deregistration -- rather than the
   POI calling a lookup back into the AMF: the POI stays free of AMF internals and each hook is one
   line. Called for ALL UEs, not only targets, because the target may be provisioned later.
2. *ModifyTask counts for newly added identifiers only* (decision 2): a target identifier the task
   did not carry before is "a new interception for a UE"; ones it already carried are not repeated.
3. *The X2 send is off the X1 path* (decision 3): activate/modify only enqueue a job; a worker
   thread inside LiPoi (started/joined with start()/stop()) looks the UE up, checks the task still
   exists and its gating, and emits. The X1 response never waits on an X2 connect.
4. *Race ordering.* `note_registered` runs BEFORE `is_target`/`report_registration`, and the task
   is stored BEFORE the job is queued, so a warrant activated concurrently with a registration is
   caught by at least one path. A tiny window can produce both a Registration and a
   StartOfInterception for the same UE: an over-report, never a miss. Disclosed, not eliminated.
5. *Codec:* `AmfStartOfInterceptionWithRegisteredUE` gained `location` and `time_of_registration`
   (a GeneralizedTime string; the codec refuses anything not ending in 'Z', per the table's "UTC or
   offset from UTC, not as local time").

**Proof.** `Xiri.AmfStartOfInterceptionCarriesLocationAndUtcTimeOfRegistration` (round-trip, absent
members stay absent, non-UTC refused). `LiAmfEndToEnd.RealAmfEmitsStartOfInterceptionWhenAWarrantIs-
ActivatedOnARegisteredUe` through the real AMF: a UE registers with no warrant (nothing delivered);
warrant A (All) activated afterwards -> exactly one record under A's XID with the UE's real
5G-TMSI, the registration cell, a 15-char UTC time, direction 5; warrant B (IdentifierAssociation-
only) -> nothing; warrant C (gating absent) -> its own record, A's not repeated; after the UE
deregisters, warrant D -> nothing. **Negative control run:** with the registry insert disabled the
test fails at "no StartOfInterception after activating on a registered UE". (A first control
attempt was invalid -- it failed at registration because a CI Test step was using the same loopback
ports; CI run 37402356790, superseded by this push, was cancelled and the control repeated clean.)

**Disclosed, not conformant / simplified.**
- Registered-UE state is lost on AMF restart; a restarted AMF would not start interception for UEs
  that were registered before it came back (until P11 externalises it).
- 3GPP access only (this AMF has no N3IWF/TNGF path), so no per-access-type second record and no
  non3GPPAccessEndpoint.
- `registrationType` is not reported (same lab simplification as AMFRegistration); slice, PEI,
  SUCI, GPSI, TAI list and the other C/O members are not populated.
- No implicit deregistration exists in this AMF, so a UE that vanishes stays "registered".
- The last-known location is only as fresh as the last registration / HandoverNotify /
  PathSwitchRequest the POI was told about.
- Targets are matched by SUPI/IMSI/NAI kinds only (as the rest of this POI); GPSI/IMEI-only
  warrants are not matched.

