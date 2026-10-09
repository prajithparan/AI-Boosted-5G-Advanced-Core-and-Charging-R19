## ADR-0475: LI increment 7a -- the UDM IRI-POI: event list for approval (TS 33.127 7.2.2.4, TS 33.128 7.2.2.3)

**Date:** 2026-10-09. **Status:** Event list APPROVED by the user on 2026-10-09 ("Approved", given for the list as proposed: events 1-4 build, 5 and 7 stay open, 6 and 8 out of scope). No code written yet. Questions (b)-(d) were not answered individually; the proposal text is taken as approved, so event 4 still requires reading Table 7.2.2.3.8-1 in full before coding and event 5 stays deferred.

**Context.** The LI plan (ADR-0364) step 7 starts with the UDM. TS 33.127 V19.7.0 clause 7.2.2.4 lists the events the UDM/UDR IRI-POI generates xIRI for; TS 33.128 V19.7.0 clause 7.2.2.3 gives each event's record and its M/C/O fields. The UDM is reached by the ADMF over LI_X1 (target identities SUPI, PEI, GPSI, IMPU/IMPI per 7.2.2.2) and sends xIRI over LI_X2 to MDF2, exactly like the AMF POI (ADR-0378/0457) on the shared `libs/li-poi` runtime.

**Event list (TS 33.127 7.2.2.4) with the trigger and the UDM operation that exists in this repo (`nfs/udm`).**

| # | Event / record (TS 33.128) | Trigger (TS 33.128 clause) | This UDM | Proposed |
|---|---|---|---|---|
| 1 | Serving system / `UDMServingSystemMessage` (M: sUPI, servingSystemMethod, roamingIndicator; C: pEI, gPSI, gUAMI, gUMMEI, pLMNID, serviceID) | amf3GPPAccessRegistration / amfNon3GPPAccessRegistration received (Nudm_UECM_Registration, TS 29.503 5.3.2.2.2/.3) | UECM registration exists | **Build** |
| 2 | Start of interception with target already registered / `UDMStartOfInterceptionWithRegisteredTarget` (M: sUPI, uDMSubscriptionDataSets; C: gPSI) | an X1 task activated for a UE that already has a registration context at the UDM | needs the UDM's registration store to be queryable by SUPI | **Build** (mirrors AMF ADR-0461) |
| 3 | Cancel location / `UDMCancelLocation` | a deregistration notification sent/received (Nudm_UECM Deregistration) | UECM deregistration exists | **Build** |
| 4 | UE authentication response / `UDMUEAuthenticationResponse` | AuthenticationInfoRequest from the AUSF (or HSS) answered (Nudm_UEAuthentication) | UEAU exists (ADR-0383) | **Build** -- note: the record carries authentication data; what may be reported (no K/OPc/RAND-derived secrets) must be read from Table 7.2.2.3.8-1 before coding, and is a question below |
| 5 | Subscriber record change / `UDMSubscriberRecordChangeMessage` | the GPSI/SUPI/PEI of the target changes or is de-provisioned, or its S-NSSAIs/CAG are modified | provisioning goes through the BSS -> UDR; the UDM sees changes only via UDR/SDM subscriptions | **Defer** until we agree where the change is observed (UDM has no provisioning API of its own) |
| 6 | Location information request / result (`UDMLocationInformationResult`) | LocationInfoRequest from an HSS / another PLMN | HSS interworking is not built | **Not applicable now** (needs the HSS/IWF) |
| 7 | UE information response (`UDMUEInformationResponse`) | ProvideUeInfo request answered | Nudm_MT exists (`/nudm-mt/v1`) | **Decide**: build if Nudm_MT ProvideUeInfo is implemented here (to be checked) |
| 8 | ProSe target identifier deconcealment / ProSe target authentication | 5G PKMF / ProSe remote UE | ProSe is not built | **Not applicable now** |

**Proposal (not decided):** build events 1-4 as one increment (3 stages: codecs for the four records from TS33128Payloads, the POI wiring in `nfs/udm` on `li_poi::PoiRuntime`, and an end-to-end test through the real UDM like `test_li_amf_e2e.cpp`), with a SUPI/GPSI/PEI target match and UDM X1 task handling. Events 5, 7 stay open; 6 and 8 are out of scope until the HSS-IWF / ProSe exist.

**Questions for the user.** (a) Approve events 1-4 as the first slice? (b) Event 4: confirm that the authentication record reports only what Table 7.2.2.3.8-1 lists (to be read in full and quoted before coding) and never key material. (c) Event 5: is the UDR change feed (a Nudr subscription) acceptable as the observation point, or should it wait? (d) The UDM instance is single-process with state in the UDR -- LI warrant state follows the AMF POI pattern (in-process, ADMF re-pushes on restart); confirm.

**Disclosed.** TS 33.127/33.128 were read from `specs/3gpp/*.docx` converted to text; only 7.2.2.3.2 and 7.2.2.3.9 were read field by field so far. Other records' fields are NOT yet extracted and nothing here is a design of them.

**Rejected alternatives.** Building all eight events at once (four need interfaces that do not exist here, so they would be untestable stubs). Starting with the SMSF/NEF/CHF POIs (the UDM is the next row in ADR-0364 and its events map onto operations that already exist).

**Status update, slice 1 (2026-10-09).** Event 1 codec built: `UdmServingSystemMessage` (TS 33.128 7.2.2.3, ASN.1 `UDMServingSystemMessage`) in `libs/li-core` (struct, fill/extract, `Event` variant member, encode/decode dispatch); `Xiri.UdmServingSystemMessage*` (2 tests) pass in `conformance_tests` together with the 9 existing Xiri tests. Modelled: sUPI, servingSystemMethod (M); pEI, gPSI, pLMNID, roamingIndicator. NOT modelled: gUAMI, gUMMEI, serviceID. Which of the modelled members are C vs O in the table was taken from the ASN.1 OPTIONAL markers, not from a field-by-field read of the table text. **Not built yet:** the UDM POI itself (X1 listener, X2 client, SUPI/GPSI/PEI target match, hook in Nudm_UECM registration), events 2-4 codecs and triggers, the end-to-end test.

**Status update, slice 2 (2026-10-09).** Event 2 codec built: `UdmStartOfInterceptionWithRegisteredTarget` (sUPI, optional gPSI, `SBIType` reference/value as verbatim UTF-8 strings); round-trip test passes (`conformance_tests` Xiri.*, 12 pass). Which UDM subscription data sets are reported and how their SBI reference/value are rendered is not decided and is a UDM-POI design point still open. Events 3 and 4 codecs, the UDM POI wiring and the e2e test remain.
