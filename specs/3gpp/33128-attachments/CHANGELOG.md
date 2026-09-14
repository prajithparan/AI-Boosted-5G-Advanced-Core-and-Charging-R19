# Changelog for 3GPP 33.128 Release 19

This page describes various changes to the ASN.1 and XSD schema
provided with 3GPP TS 33.128 Release 19.

Conventions used within the Change description are:
| Term          | Purpose |
| ----          | ------ |
| *Breaking*    | Breaking change to the schema. |
| *Fix*         | Fix to the schema to permit compilation. |
| *Note*        | Inconsisent versioning, or potential issue in repository such as missing git tag. |
| *Warning*     | Compatibility issue with a different Release or Version. |


# Table of Contents

[[_TOC_]]

# 33128/r19/TS33128Dictionaries.xml

| Release   | Item                          | Change |
| -------   | ----                          | ------ |
| V19.0.1   | LocationAcquisition           | Change text of Meaning. |
| V19.0.1   | DictionaryEntry               | Add LocationInformation. |
| V19.0.1   | ReqCurrentLoc                 | Change text of Meaning. |
| V19.0.1   | DictionaryEntry               | Add ECIDMeasurements. |

# 33128/r19/TS33128IdentityAssociation.asn

| Release   | Item                          | Change |
| -------   | ----                          | ------ |
| V19.0.1   | {Module}                      | Initial publication, baselined from V18.9.1. *Note*: OIDs not changed from V18.9.1 (0.4.0.2.2.4.20.17.1). |
|           |                               | |
| V19.4.0   | {All OIDs}                    | Change to 0.4.0.2.2.4.20.18.0. |

# 33128/r19/TS33128Payloads.asn

| Release   | Item                          | Change |
| -------   | ----                          | ------ |
| V19.0.1   | {Module}                      | Initial publication, baselined from V18.9.1. All OIDs set to 0.4.0.2.2.4.19.19.0. |
| V19.0.1   | IMSMessage                    | Add tag 9. |
|           |                               | |
| V19.1.0   | {All OIDs}                    | Change to 0.4.0.2.2.4.19.19.1. |
| V19.1.0   | IMPORTS                       | Change IPIRIPacketReport to OID 0.4.0.2.2.5.3.18. |
| V19.1.0   | XIRIEvent                     | Add tags 162-175. |
| V19.1.0   | IRIEvent                      | Add tags 162-175. |
| V19.1.0   | UAStarParams                  | Add tag 3. |
| V19.1.0   | AMFRegistration               | Deprecate tag 20. |
| V19.1.0   | NASTransportInitialInformation | Add tag 7. |
| V19.1.0   | InitialRANUEContextSetup      | Add tags 17-18. |
| V19.1.0   | MMEAttach                     | Deprecate tag 15. |
| V19.1.0   | MMEStartOfInterceptionWithEPSAttachedUE | Deprecate tag 16. |
| V19.1.0   | EUTRALocation                 | Add tag 12. |
| V19.1.0   | NRLocation                    | Add tag 12. |
| V19.1.0   | CellRadioRelatedInformation   | Add tag 3. |
|           |                               | |
| V19.2.0   | {All OIDs}                    | Change to 0.4.0.2.2.4.19.19.2. |
| V19.2.0   | XIRIEvent                     | Add tags 176-185. |
| V19.2.0   | AMFRegistration               | Add tag 31. |
| V19.2.0   | AMFDeregistration             | Add tag 13. |
| V19.2.0   | AMFLocationUpdate             | Add tag 10. |
| V19.2.0   | AMFStartOfInterceptionWithRegisteredUE | Add tag 21. |
| V19.2.0   | AMFUnsuccessfulProcedure      | Add tag 10. |
| V19.2.0   | AMFPositioningInfoTransfer    | Add tag 9. |
| V19.2.0   | SMFPDUSessionModification     | Add tag 23. |
| V19.2.0   | SMFStartOfInterceptionWithEstablishedPDUSession | Add tag 29. |
| V19.2.0   | SMFUnsuccessfulProcedure      | Add tag 20. |
| V19.2.0   | AMFIdentifierAssociation      | Add tag 8. |
|           |                               | |
| V19.3.0   | {All OIDs}                    | Change to 0.4.0.2.2.4.19.19.3. |
| V19.3.0   | XIRIEvent                     | Add tags 186-190. |
| V19.3.0   | IRIEvent                      | Add tags 186-190. |
| V19.3.0   | UAStarParams                  | Add tags 4-5. |
| V19.3.0   | RATType                       | Add values 21-33. |
| V19.3.0   | Location                      | Add tag 7. |
|           |                               | |
| V19.4.0   | {All OIDs}                    | Change to 0.4.0.2.2.4.19.19.4. |
| V19.4.0   | XIRIEvent                     | Add tags 191-196. |
| V19.4.0   | IRIEvent                      | Add tags 191-196. |
| V19.4.0   | CCPDU                         | Add tags 8-9. |
| V19.4.0   | AMFRegistration               | Add tag 32. |
| V19.4.0   | AMFUnsuccessfulProcedure      | Add tag 11. |
| V19.4.0   | AMFUEConfigurationUpdate      | Add tag 9. |
| V19.4.0   | SMFPDUSessionEstablishment    | Add tags 30-31. |
| V19.4.0   | SMFPDUSessionModification     | Add tag 24-25. |
| V19.4.0   | SMFPDUSessionRelease          | Add tag 16. |
| V19.4.0   | SMFUnsuccessfulProcedure      | Add tag 21. |
| V19.4.0   | SMFPDUtoMAPDUSessionModification | Add tag 19. |
| V19.4.0   | SMFMAPDUSessionEstablishment  | Add tag 28. |
| V19.4.0   | SMFMAPDUSessionModification   | Add tag 23. |
| V19.4.0   | SMFMAPDUSessionRelease        | Add tag 15. |
| V19.4.0   | SMFMAUnsuccessfulProcedure    | Add tag 18. |
| V19.4.0   | PositioningMethod             | Add values 16-19. |
|           |                               | |
| V19.5.0   | {All OIDs}                    | Change to 0.4.0.2.2.4.19.19.5. |
| V19.5.0   | XIRIEvent                     | Add tag 197. |
| V19.5.0   | IRIEvent                      | Add tag 197. |
| V19.5.0   | CCPDU                         | Add tag 10. |
| V19.5.0   | PTCStartOfInterception        | Add tag 10. |
| V19.5.0   | PTCIdentifiers                | Deprecate tag 1. |
| V19.5.0   | TargetIdentifier              | Add tag 18. |

# 33128/r19/urn\_3GPP\_ns\_li\_3GPPIdentityExtensions.xsd

| Release   | Item                          | Change |
| -------   | ----                          | ------ |
| V19.0.1   | {Schema}                      | Initial publication, baselined from V18.9.1. |
| V19.0.1   | xmlns, targetNamespace        | Change to r19:v0. |
| V19.0.1   | xmlns:liqr                    | Change import to r19:v0. |

# 33128/r19/urn\_3GPP\_ns\_li\_3GPPLIQueryExtensions.xsd

| Release   | Item                          | Change |
| -------   | ----                          | ------ |
| V19.0.1   | {Schema}                      | Initial publication, baselined from V18.9.1. |
| V19.0.1   | xmlns, targetNamespace        | Change to r19:v0. |
| V19.0.1   | ListOfGPSI                    | Add complexType. |
| V19.0.1   | ListOfMSISDNs                 | Add complexType. |
| V19.0.1   | ExternalASNType               | Add complexType. |
| V19.0.1   | ASN1OID                       | Add simpleType. |
| V19.0.1   | ExternalASNReference          | Add simpleType. |
| V19.0.1   | ExternalASNValue              | Add complexType. |
| V19.0.1   | AlignedPER                    | Add simpleType. |
| V19.0.1   | BER                           | Add simpleType. |

# 33128/r19/urn\_3GPP\_ns\_li\_3GPPStateTransfer.xsd

| Release   | Item                          | Change |
| -------   | ----                          | ------ |
| V19.0.1   | {Schema}                      | Initial publication, baselined from V18.9.1. |
| V19.0.1   | xmlns, targetNamespace        | Change to r19:v0. |

# 33128/r19/urn\_3GPP\_ns\_li\_3GPPX1Extensions.xsd

| Release   | Item                          | Change |
| -------   | ----                          | ------ |
| V19.0.1   | {Schema}                      | Initial publication, baselined from V18.9.1. |
| V19.0.1   | xmlns, targetNamespace        | Change to r19:v0. |
|           |                               | |
| V19.2.0   | xmlns, targetNamespace        | Change to r19:v1. |
| V19.2.0   | DelegatedTask                 | Add element NEIPAddress. |
|           |                               | |
| V19.3.0   | xmlns, targetNamespace        | Change to r19:v2. |
| V19.3.0   | PINLIX1TargetIdentifier       | Add element and complexType. |
| V19.3.0   | PINClientID                   | Add simpleType. |
| V19.3.0   | IdentityToken                 | Add simpleType. |
| V19.3.0   | PINID                         | Add simpleType. |
| V19.3.0   | X1Extension                   | Add element ChargingDataEvents. |
| V19.3.0   | ChargingDataEventsExtensions  | Add complexType. |
| V19.3.0   | ChargingDataEventsGenerated   | Add simpleType. |
|           |                               | |
| V19.4.0   | xmlns, targetNamespace        | Change to r19:v3. |
| V19.4.0   | UPFLIT3TargetIdentifier       | Add element PDR. |
| V19.4.0   | MCServiceLIX1TargetIdentifier | Add element and complexType. |
| V19.4.0   | MCID                          | Add simpleType. |
| V19.4.0   | MCServiceID                   | Add simpleType. |
| V19.4.0   | MCServiceGroupID              | Add simpleType. |

# 33128/r19/urn\_3GPP\_ns\_li\_3GPPXLAExtensions.xsd

| Release   | Item                          | Change |
| -------   | ----                          | ------ |
| V19.0.1   | {Schema}                      | Initial publication, baselined from V18.9.1. |
| V19.0.1   | xmlns, targetNamespace        | Change to r19:v0. |
| V19.0.1   | xmlns:liqr                    | Change import to r19:v0. |
| V19.0.1   | LocationAcquisitionRequest    | Add element LocationInformation. |
| V19.0.1   | LocationAcquisitionRequest    | Add element ECIDMeasurements. |
| V19.0.1   | LocationResponseDetails       | Add element ECIDMeasurementsOutcomes. |
| V19.0.1   | LocationResponseDetails       | Add element EPCECIDMeasurementsOutcomes. |
| V19.0.1   | ECIDMeasurementsOutcomes      | Add complexType. |
| V19.0.1   | EPCECIDMeasurementsOutcomes   | Add complexType. |
| V19.0.1   | LocationOutcome               | Change element GPSI type to ListOfGPSI (no functional change). |
| V19.0.1   | EPCLocationOutcome            | Change element MSISDNs type to ListOfMSISDNs (no functional change). |
| V19.0.1   | ECIDMeasurementsOutcome       | Add complexType. |
| V19.0.1   | EPCECIDMeasurementsOutcome    | Add complexType. |
