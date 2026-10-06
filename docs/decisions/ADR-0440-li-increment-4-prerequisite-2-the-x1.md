## ADR-0440: LI increment 4, prerequisite 2 -- the X1 IdentifierAssociationExtensions gating, and the AMF IdentifierAssociation + LocationUpdate hooks

**Date:** 2026-09-26. **Status:** accepted (partial: IdentifierAssociation and LocationUpdate are
wired; Deregistration, IdentifierDeassociation and StartOfInterceptionWithRegisteredUE are still
blocked, see below). This continues ADR-0378. Pinned sources: TS 33.127/33.128 V19.7.0
(`specs/3gpp/TS_33.128_j70.txt`), ETSI TS 103 221-1 V1.23.1 and TS 103 221-2 V1.10.1, and the
TS 33.128 X1 extension schema already vendored with the other attachments by ADR-0364
(`specs/3gpp/33128-attachments/urn_3GPP_ns_li_3GPPX1Extensions.xsd`, target namespace
`urn:3GPP:ns:li:3GPPX1Extensions:r19:v4`).

**Decision 1: parse the gating parameter in `li_core::x1`, taking element names from the XSD.**
TS 33.128 table 6.2.2.1.1-1 gives the field as "TaskDetailsExtensions/IdentifierAssociationExtensions",
and table 6.2.2.1.1-2 calls its one member "TaskDetailsExtensions/EventsGenerated", with values
IdentifierAssociation and All. The vendored XSD names that member `IdentifierAssociationEventsGenerated`
and restricts it to the same two enumerators. The XSD is authoritative for element names (the
YAML-prime rule, ADR-0250, applied to XML), so the code uses the XSD name and records the naming
gap here. The clause 6.2.2.2.1 prose also misspells the parameter as "IdentifierAssocationExtensions".
The XSD declares two global elements of type `IdentifierAssociationExtensions`: the element of that
name, which is the path the TS table spells out, and `X1Extensions` with its `IdentifierAssociation`
choice branch. Both are schema-valid inside the ETSI `Extension` wildcard, so both are parsed.
Elements are matched on namespace URI plus local name. `TaskDetails.identifier_association_events`
is `nullopt` when the parameter is absent.

**Decision 2: the 3GPP schema joins the X1 validation set, and the images ship it.** The ETSI
`Extension` type is `<xs:any namespace="##other"/>`, whose default `processContents` is strict.
Before this change, any ActivateTask that carried a 3GPP extension failed validation and came back
as a TopLevelError. `x1-validation.xsd` now imports the 3GPP schema, after the X1 namespace that
the 3GPP schema imports without a schemaLocation. The import path
(`../../3gpp/33128-attachments/...`) points outside `specs/etsi`, so `amf.Dockerfile` and
`li-mdf.Dockerfile` now copy that one file too. Without it the schema set would not load in the
container, and every X1 request would fail closed, including requests to the already-shipped MDF2.

**Decision 3: gating is a per-task decision table in the POI.** Clause 6.2.2.2.1 reduces to
`xiri_record_enabled(gating, record)`:

| Gating | Records generated |
|---|---|
| Absent | every AMF record except Identifier(De)Association (table 6.2.2.1.1-1: absent means "shall not be generated") |
| IdentifierAssociation | only IdentifierAssociation, IdentifierDeassociation and LocationUpdate ("No other record types shall be generated for that target") |
| All | every AMF record type |

So gating does not simply switch the POI on or off. With the parameter absent, Registration and
LocationUpdate are still emitted. In IdentifierAssociation mode, the Registration hook from
ADR-0378 is now suppressed. Emitting it there would be non-conformant.

The target store keeps the gating per XID, and ModifyTask replaces it. `matches()` returns every
task that targets the SUPI. The old lookup returned the first hit from an unordered_map, so when
two warrants with different gating targeted one UE, which gating applied was nondeterministic.
Each warrant now gets its own xIRI under its own XID and sequence number.

**Decision 4: wire only the triggers the AMF really has.**
- *IdentifierAssociation* (6.2.2.2.7): this fires when a REGISTRATION ACCEPT "has been sent by the
  AMF towards a target UE", "regardless of whether [it] is subsequently successfully completed".
  That is the existing RegistrationAccept site in `handle_uplink_nas_transport_smc_complete`.
  - The mandatory `location` is the `UserLocationInformation` of the UplinkNASTransport being
    handled. TS 38.413 makes that IE mandatory, per `specs/NGAP/ngap-17.9.asn`
    UplinkNASTransport-IEs.
  - It is encoded as `Location.locationInfo.userLocation`, as the table requires.
  - Payload Direction is 5, as the clause states.
  - If the ULI is an unmodelled branch (N3IWF or an extension), no record is emitted and the event
    is logged. Emitting the record without its mandatory member would be worse.
- *LocationUpdate* (6.2.2.2.4): the clause names "the N2 Path Switch Request (... TS 23.502 clause
  4.9.1.2)" and "the N2 Handover Notify (... clause 4.9.1.3)". The AMF implements both procedures.
  - The PathSwitchRequest hook fires only on the success path, after the acknowledge and the
    registry re-point. It never fires on the ErrorIndication branch.
  - The HandoverNotify hook fires after the registry re-point.
  - In both cases the location is the message's mandatory ULI, which is the target cell.
  - `ngap_handover.cpp` reaches the POI through a new `amf::ngap::li_poi()` accessor, not a second
    global.
- `parse_user_location` (prerequisite 1) is now used. `user_location_from_ies` wraps it: it finds
  IE 121, PER-decodes it, parses it and frees it. `li_location.cpp` joins the `amf` target. The
  NGAP decode runs only when the POI exists and the SUPI is a target, so the LI-off path costs
  nothing.

**Disclosed interpretation: LocationUpdate direction.** Clause 6.2.2.2.4 gives no Payload
Direction. Table 5.3.2-1 sets the direction "based on the initiator of the network procedure".
N2 and Xn handovers are initiated by the NG-RAN. They are not sent by the target UE and not
addressed to it, so neither 2 (to target) nor 3 (from target) describes them. This slice uses 5
(not applicable), the value 6.2.2.2.7 and 6.2.2.2.5 prescribe for the other network-detected AMF
records. This is an interpretation, not a quoted rule. A reviewer who reads "initiator = network"
as 2 has a defensible case.

**Still blocked, and why (no trigger fabricated).**
- *IdentifierDeassociation*: this needs "an identifier deassociation without a new association
  within the same UE context". The AMF has no UE deregistration procedure. A re-registration with a
  new 5G-TMSI is a new association, not a deassociation, and UEContextRelease keeps the GUTI
  (CM-IDLE). The only conformant trigger is deregistration, which is ADR-0378 prerequisite 3.
- *IdentifierAssociation via CONFIGURATION UPDATE COMMAND (5G-GUTI)*: the AMF does not implement
  that NAS procedure, so it has no second trigger.
- *LocationUpdate via PDU Session Resource Modify Indication (Dual Connectivity, TS 37.340)*: the
  AMF has no handler for that NGAP message. Wiring it is NGAP work that comes before LI work. The
  optional operator-policy triggers (RRC Inactive Transition Report, Namf_Location/EventExposure
  sources) are not implemented either.
- *Deregistration* and *StartOfInterceptionWithRegisteredUE*: no change from ADR-0378.

**Verification.**
- `test_li_x1_gating` (11 cases): both element forms, both values, absent, an enumeration
  violation, a missing child, an unrelated SMSF extension, a same-named element in a foreign
  namespace, several extensions, ModifyTask setting and clearing the gating, and the NE server
  passing the value to the store callback. Every document goes through `parse_request`, so these
  cases also prove the combined ETSI+3GPP schema set loads in libxml2.
- `test_li_amf_location` +2: IE-container decode.
- `test_li_amf_poi` +5 (`LiAmfPoiGating.*`): the decision table; the absent, IdentifierAssociation
  and All modes over real LI_X1 provisioning and real LI_X2 to a loopback MDF2, with the mandatory
  members (sUPI, gUTI, location) and direction 5 asserted on the decoded xIRIs; and the non-target
  case.
- *Not tested*: the NGAP-side hooks in `ngap_task.cpp`/`ngap_handover.cpp` are compile-verified
  only. No test drives a PathSwitchRequest or HandoverNotify through the AMF with LI enabled.
  That needs an AMF-process harness with the POI on, which no existing test has.

**Rejected alternatives.**
- *Emitting IdentifierAssociation without the parameter, or LocationUpdate only when gated*: both
  misread 6.2.2.2.1.
- *Using the TS table's "EventsGenerated" element name*: it is not declared in the schema, so a
  conformant ADMF could never send it.
- *Relaxing the wildcard (`processContents="lax"`) in a local copy of the ETSI schema instead of
  importing the 3GPP one*: that would edit a normative ETSI file and stop validating the extension
  content.
- *Taking location from the stored InitialUEMessage*: the UplinkNASTransport's own mandatory ULI
  is the more recent of the two, and it is already in scope.

