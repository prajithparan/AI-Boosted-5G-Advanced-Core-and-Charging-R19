## ADR-0457: the AMF IRI-POI end to end through the real AMF process

**Date:** 2026-10-05. **Status:** accepted. LI increment 4's remaining harness -- the one ADR-0393
and ADR-0440 both named as "its own increment" ("needs an AMF-process harness with the POI on,
which no existing test has").

**Correction to the LI status memory and the option text the user picked:** it described "5 event
hooks still need wiring". In fact ADR-0440 (IdentifierAssociation, LocationUpdate) and ADR-0393
(Deregistration, IdentifierDeassociation) had already wired five of the six approved events; what
was missing was proof through the real process. That is what this adds.

**Decision.** `tests/integration/test_li_amf_e2e.cpp`, its own binary
(`li_amf_e2e_integration_tests`, li_core + ngap_generated in one executable -- the same
coexistence the amf binary itself relies on, ADR-0364). It writes a temp copy of `config/amf.json`
with only the `li_poi` block changed, selects it via nf_config's existing `AMF_CONFIG_FILE`
override (no new config mechanism), spawns NRF/UDR/UDM/AUSF/PCF/SMF/AMF, provisions an
`IdentifierAssociationEventsGenerated=All` warrant over real LI_X1, drives register -> PDU session
-> normal deregistration over real NGAP/SCTP + NAS, and decodes what a loopback MDF2 receives over
real LI_X2.

**What it proves.** Exactly four xIRIs, each matched to the target IMSI: AMFRegistration and
AMFIdentifierAssociation at RegistrationAccept, AMFDeregistration (direction UE-initiated,
switch-off NormalDetach) and AMFIdentifierDeassociation at deregistration. Every gUTI carries the
5G-TMSI the UE actually decoded from its own RegistrationAccept, and IdentifierAssociation's
mandatory location is the gNB driver's real NGAP ULI (PLMN 999/70, TAC 000001, NR cell 1) --
parsed by the AMF, not supplied by the test. Passes locally (14 s); the AMF's own log shows the
four "delivered ... xIRI" lines.

**Still not covered, disclosed.** LocationUpdate end to end (it fires on N2 PathSwitchRequest /
HandoverNotify, which this flow does not drive) and StartOfInterceptionWithRegisteredUE (not wired:
it needs the POI to see AMF registration state at X1-activation time). Fixed LI_X1 port 19821.

