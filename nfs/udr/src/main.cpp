// nfs/udr: UDR (Unified Data Repository), Nudr_DataRepository context-data group.
// Source: specs/5G_APIs-REL-19/TS29505_Subscription_Data.yaml (the file TS29504_Nudr_DR.yaml's
// paths $ref into -- TS29504 itself defines almost no schemas of its own), commit
// bca84b60a37773133bcae97e5c6c0d10a93b47b6. Phase 2's fifth NF (PROMPT.md/CLAUDE.md order:
// NRF -> AMF -> SMF -> UDM -> UDR -> AUSF -> PCF).
//
// In scope, agreed with the user before implementation: the `context-data` group --
// QueryAmfContext3gpp, CreateAmfContext3gpp, AmfContext3gpp (AMF 3GPP-access context, singular
// per UE -- no delete operation exists for this resource in the spec, checked not assumed) and
// QuerySmfRegList, QuerySmfRegistration, CreateOrUpdateSmfRegistration, UpdateSmfContext,
// DeleteSmfRegistration (SMF registration context, one per UE+pduSessionId). These mirror
// nfs/udm's UECM registration groups almost exactly -- 3GPP's real intended backing store for
// that data -- but per explicit user decision this turn, UDM's own AmfRegistrationStore/
// SmfRegistrationStore are NOT wired to call UDR yet; that remains a separate, deliberate future
// turn touching already-committed UDM code, not silently done here.
//
// UPDATE (ADR-0069, gap-closure Tier 1b): the `provisioned-data` group (am-data/smf-selection-
// subscription-data/sm-data) is now implemented -- real GET routes, keyed by (ueId,
// servingPlmnId) per the real path shape. Still real, disclosed: this group is genuinely GET-only
// in the spec (no create/update operation exists at all), so there is no live provisioning path;
// data is seeded at startup instead (see main() below), and nfs/udm's own GetAmData/GetSmfSelData/
// GetSmData now call these real routes instead of returning the old permanently-empty stub.
//
// UPDATE (ADR-0072, gap-closure: real N28 end-to-end): the `policy-data` group's SM policy
// resource (/policy-data/ues/{ueId}/sm-data, real schema SmPolicyData -- source
// TS29519_Policy_Data.yaml) is now implemented, real GET+PATCH, keyed by ueId alone. Unlike
// provisioned-data above, this real resource DOES support PATCH (application/merge-patch+json) --
// but the real spec still has no POST/create operation for it, so this project's own store treats
// PATCH as upsert-capable (a disclosed, deliberate choice enabling GUI-driven creation -- see
// stores.hpp's own comment on SmPolicyDataStore). PCF is the real consumer, fetching a
// subscriber's subscSpendingLimits/policyCounterIds per DNN to decide whether to subscribe to
// CHF's Nchf_SpendingLimitControl.
//
// UPDATE (ADR-0083, gap-closure task #106): the Authentication Data group's
// authentication-subscription (real GET+PATCH, RFC 6902) and authentication-status (real
// PUT+GET+DELETE) documents, and the policy-data group's AM policy resource (real GET+PATCH, RFC
// 7396 -- the real UDR-side backing for PCF's own Npcf_AMPolicyControl) are now implemented.
// Real, disclosed architectural note: neither AUSF's own AuthContextStore/KausfStore nor UDM's
// own AuthenticationSubscriptionStore, nor PCF's own AmPolicyStore, were migrated to call these
// new UDR routes in this pass -- that would be a real, separate architectural decision (each of
// those NFs already has its own working, tested store; switching them to be UDR-backed is a
// cross-cutting change touching already-committed code, not a "stand up the resource" change) --
// same "build the surface first, wire consumers in a dedicated later turn" precedent this
// project already used for UDR's own provisioned-data group (ADR-0069) and PCF itself (ADR-0028).
//
// UPDATE (ADR-0093, gap-closure task #106): the AMF non-3GPP-access context group
// (QueryAmfContextNon3gpp/CreateAmfContextNon3gpp, real GET+PUT) is now implemented -- a real,
// distinct resource/table from the 3GPP one above (schema `AmfNon3GppAccessRegistration`, not
// `Amf3GppAccessRegistration`), same "no PATCH/DELETE exists for this resource" scope its 3GPP
// sibling already has.
//
// UPDATE (ADR-0097, gap-closure task #106): the SMSF Registration context-data group
// (CreateSmsfContext3gpp/QuerySmsfContext3gpp/DeleteSmsfContext3gpp and their non-3GPP
// counterparts, real GET+PUT+DELETE) is now implemented -- two real, distinct resources sharing
// the identical `SmsfRegistration` schema, same "no PATCH exists for this resource" scope.
//
// UPDATE (ADR-0098, gap-closure task #106): the IP-SM-GW Registration context-data resource
// (CreateIpSmGwContext/QueryIpSmGwContext/ModifyIpSmGwContext/DeleteIpSmGwContext, real
// PUT+GET+PATCH+DELETE, RFC 6902 JSON Patch) is now implemented -- the richest operation set of
// any context-data resource closed so far.
//
// UPDATE (ADR-0099, gap-closure task #106): the Message Waiting Data (Document) resource
// (CreateMessageWaitingData/QueryMessageWaitingData/ModifyMessageWaitingData/
// DeleteMessageWaitingData, real PUT+GET+PATCH+DELETE, RFC 6902 JSON Patch) is now implemented --
// unlike ip-sm-gw's own always-204 PUT, this one's real PUT genuinely distinguishes 201-Created
// from 204-updated per the YAML.
//
// UPDATE (ADR-0100, gap-closure task #106): the Roaming Information (Document) resource
// (UpdateRoamingInformation/QueryRoamingInformation, real GET+PUT, real distinct 201-vs-204 PUT
// response codes) is now implemented -- same "no PATCH/DELETE exists for this resource" scope as
// the AMF non-3GPP-access context resource.
//
// UPDATE (ADR-0101, gap-closure task #106): the PEI Information (Document) resource
// (CreateOrUpdatePeiInformation/QueryPeiInformation, real GET+PUT, real distinct 201-vs-204 PUT
// response codes) is now implemented -- real schema is an allOf composition
// (TS29503_Nudm_UECM.yaml's base PeiUpdateInfo + this file's own PeiUpdateInfoExt), generated as
// `sbi_gen::PeiUpdateInfo_Subscription_Data` to disambiguate from the base type's own
// `PeiUpdateInfo_Nudm_UECM`.
//
// UPDATE (ADR-0102, gap-closure task #106): the Enhanced Coverage Restriction Data resource
// (QueryCoverageRestrictionData, real GET-only) is now implemented -- same real "no create/update
// operation exists at all, seeded at startup" shape as the provisioned-data group (ADR-0069).
//
// UPDATE (ADR-0103, gap-closure task #106): the LCS Privacy Subscription Data resource
// (QueryLcsPrivacyData, real GET-only) is now implemented -- same real "seeded at startup" shape.
//
// UPDATE (ADR-0104, gap-closure task #106): the LCS Subscription Data resource
// (QueryLcsSubscriptionData, real GET-only) is now implemented -- same real "seeded at startup"
// shape.
//
// UPDATE (ADR-0105, gap-closure task #106): the LCS Mobile Originated Subscription Data resource
// (QueryLcsMoData, real GET-only) is now implemented -- same real "seeded at startup" shape.
//
// UPDATE (ADR-0106, gap-closure task #106): the provisioned-data group's `lcs-bca-data`
// sub-resource (QueryLcsBcaData, real GET-only) is now implemented as a 4th column on
// ProvisionedDataStore -- same real (ueId, servingPlmnId) key shape as am-data/
// smf-selection-subscription-data/sm-data.
//
// UPDATE (ADR-0107, gap-closure task #106): the Parameter Provision (Document) resource
// (GetppData/ModifyPpData, real GET+PATCH, RFC 6902) is now implemented -- same
// "no PUT/DELETE, apply_patch upsert-capable" shape as authentication-subscription.
//
// UPDATE (ADR-0108, gap-closure task #106): the Parameter Provision profile Data (Document)
// resource (QueryPPData, real GET-only) is now implemented -- same real "seeded at startup" shape.
//
// UPDATE (ADR-0109, gap-closure task #106): the Provisioned Parameter Data Entry resource
// (Create/Get/Delete PP Data Entry, real PUT+GET+DELETE) and its real sibling collection resource
// (Get Multiple PP Data Entries) are now implemented -- composite (ueId, afInstanceId) key, same
// real shape as SmfRegistrationStore's own (ueId, pduSessionId).
//
// UPDATE (ADR-0110, gap-closure task #106): the individual Shared Data resource
// (GetIndividualSharedData, real GET-only) is now implemented -- genuinely NOT per-UE, keyed by
// shared_data_id alone. Real, disclosed scope narrowing: the real sibling collection resource
// (GetSharedData, a required comma-separated shared-data-ids array query parameter) remains
// deferred -- this project has no existing precedent for parsing array-shaped query parameters
// yet, a real capability that belongs in its own scoped turn.
//
// UPDATE (ADR-0111, gap-closure task #106): the Operator-Specific Data Container (Document)
// resource (QueryOperSpecData/ModifyOperSpecData, real GET+PATCH, RFC 6902) is now implemented --
// same "no PUT/DELETE, apply_patch upsert-capable" shape as pp-data.
//
// UPDATE (ADR-0112, gap-closure task #106): the Event Exposure Data (Document) resource
// (QueryEEData, real GET-only) is now implemented -- same real "seeded at startup" shape. Real,
// distinct UDR-side resource from this project's own UDM-side Nudm_EE work (task #105).
//
// UPDATE (ADR-0113, gap-closure task #106): the `policy-data` group's UE Policy Set resource
// (ReadUEPolicySet/CreateOrReplaceUEPolicySet/UpdateUEPolicySet, real GET+PUT+PATCH RFC 7396
// merge-patch) is now implemented -- real distinct 201-vs-204 PUT codes; unlike am-data's own
// PATCH, the real spec here only documents 204 (no 200-with-body option).
//
// UPDATE (ADR-0114, gap-closure task #106): the `policy-data` group's Operator-Specific Data
// resource (ReadOperatorSpecificData/UpdateOperatorSpecificData, real GET+PATCH RFC 6902) is now
// implemented -- real, distinct resource from the subscription-data-scoped
// operator-specific-data (ADR-0111), reusing the same real OperatorSpecificDataContainer schema
// via a cross-file $ref.
//
// UPDATE (ADR-0115, gap-closure task #106): the `policy-data` group's Sponsor Connectivity Data
// resource (ReadSponsorConnectivityData, real GET-only) is now implemented -- genuinely NOT
// per-UE, keyed by sponsorId alone. Real, disclosed simplification: the real spec's own distinct
// `204` ("found but no data") vs `404` ("not found") is not modeled -- this store's simple
// existence-based model only distinguishes 200-with-data vs 404-not-provisioned.
//
// UPDATE (ADR-0116, gap-closure task #106): the `policy-data` group's individual BDT
// (Background Data Transfer) Data resource (ReadIndividualBdtData/CreateIndividualBdtData/
// UpdateIndividualBdtData/DeleteIndividualBdtData, real GET+PUT+PATCH+DELETE) is now implemented
// -- real, disclosed: PUT only documents `201` (no update-via-PUT status), so the route always
// responds 201; PATCH is NOT upsert-capable (real 404 if the resource doesn't already exist,
// unlike am-data/ue-policy-set's own upsert-capable PATCH).
//
// UPDATE (ADR-0117, gap-closure task #106): the `policy-data` group's PLMN UE Policy Set resource
// (ReadPlmnUePolicySet, real GET-only, no create/update operation exists for this resource at
// all) is now implemented -- reuses the real UePolicySet schema (same type as the per-UE
// ue-policy-set resource) but keyed by plmn_id, a genuinely distinct resource, not a UE-scoped
// alias.
//
// UPDATE (ADR-0118, gap-closure task #106): the `policy-data` group's Slice-specific Policy
// Control Data resource (ReadSlicePolicyControlData/UpdateSlicePolicyControlData, real
// GET+PATCH-only, no PUT/POST create operation exists at all) is now implemented -- merge_patch
// is upsert-capable, same disclosed precedent as AmPolicyDataStore/SmPolicyDataStore. Real,
// disclosed: the YAML types the {snssai} path parameter as the Snssai object schema with no
// documented bare-path-segment string encoding; this project reuses its own already-disclosed
// "sst + '-' + sd" convention (ADR-0072/PCF's snssai_map_key) rather than inventing a new one.
//
// UPDATE (ADR-0119, gap-closure task #106): the `policy-data` group's group-specific Policy
// Control Data resource (ReadGroupPolCtrlData/ModifyGroupPolCtrlData, real GET+PATCH-only, no
// PUT/POST create operation exists at all) is now implemented -- merge_patch is upsert-capable,
// same precedent as slice-control-data above. Keyed by the real GroupId schema (plain string, no
// encoding ambiguity, unlike slice-control-data's own snssai key).
//
// UPDATE (ADR-0120, gap-closure task #106): the real GetRoutingIDs resource (/routing-ids) is now
// implemented -- real, disclosed: this is a genuinely DIFFERENT real Nudr API
// (TS29504_Nudr_GroupIDmap.yaml's Nudr_GroupIDmap service, server base path
// `/nudr-group-id-map/v1`, OAuth2 scope `nudr-group-id-map`), not Nudr_DataRepository
// (`/nudr-dr/v2`) like every other resource in this file -- does NOT count toward the "N of
// free5GC's ~42+ Nudr_DataRepository resources" metric tracked in this file's own UPDATE entries
// above. Real GET-only, composite-keyed by the two real required query parameters (nf-type,
// nf-group-id), no PUT/POST/PATCH exists at all.
//
// UPDATE (ADR-0121, gap-closure task #106): the NIDD Authorization Info context-data resource
// (CreateNIDDAuthorizationInfo/GetNiddAuthorizationInfo/ModifyNiddAuthorizationInfo/
// RemoveNiddAuthorizationInfo, real PUT+GET+PATCH+DELETE) is now implemented. Real, disclosed
// correction: earlier UPDATE entries above lumped `nidd-authorizations` in with
// `ee-subscriptions`/`sdm-subscriptions` as a deferred deeply-nested sub-subscription shape
// without individually checking the real YAML -- it is genuinely a flat per-UE document, same
// shape as amf-3gpp-access's own real distinct-201-vs-204 PUT + RFC 6902 PATCH, plus a real
// DELETE (which amf-3gpp-access's own resource lacks).
//
// UPDATE (ADR-0122, gap-closure task #106): the real Query/Modify Identity Data by SUPI or GPSI
// resource (GetIdentityData/ModifyIdentityData, real GET+PATCH, no PUT/POST create operation
// exists at all) is now implemented -- apply_patch is upsert-capable, same precedent as
// pp-data/operator-specific-data. Real RFC 6902 JSON Patch, not merge-patch. Confirmed while
// surveying: `ee-subscriptions`/`sdm-subscriptions` ARE genuinely deeply-nested
// subscription-lifecycle resources (collection -> individual -> amf-/smf-/hss-subscriptions
// sub-collections, plus a parallel group-data-scoped tree, server-generated subsId via POST) --
// the original deferral for those two was correct, unlike nidd-authorizations above.
// `subs-to-notify` also confirmed genuinely deferred: real POST-based collection with a
// server-generated Location header and real webhook callback registration
// (`{$request.body#/notificationUri}`), no existing project precedent for either.
//
// UPDATE (ADR-0123, gap-closure task #106): the real Query ODB Data by SUPI or GPSI resource
// (GetOdbData -- real GET-only, no create/update operation exists at all) is now implemented,
// seeded at startup, same shape as coverage-restriction-data.
//
// UPDATE (ADR-0125, gap-closure task #106): the real SMS Management Subscription Data resource
// (QuerySmsMngData -- real GET-only, no create/update operation exists at all) is now implemented
// as a new `sms_mng_data` column on `udr_provisioned_data`, same real sibling-column precedent
// ADR-0106 already established for lcs-bca-data (same provisioned-data group,
// same (ueId, servingPlmnId) key).
//
// UPDATE (ADR-0126, gap-closure task #106): the real SMS Subscription Data resource (QuerySmsData
// -- real GET-only, no create/update operation exists at all) is now implemented as a new
// `sms_data` column on `udr_provisioned_data`, same precedent, genuinely distinct from
// sms-mng-data above (real, separate schema, separate operationId).
//
// UPDATE (ADR-0127, gap-closure task #106): the real Trace Data resource (QueryTraceData -- real
// GET-only, no create/update operation exists at all) is now implemented as a new `trace_data`
// column on `udr_provisioned_data`, same precedent. Real response schema is a `oneOf` (full
// `TraceData` object or a bare `SharedDataId` string reference) -- handled as opaque JSON, no
// special-casing needed since this store never strongly types sub-resource bodies.
//
// UPDATE (ADR-0128, gap-closure task #106): the real V2X Subscription Data resource (QueryV2xData
// -- real GET-only, no create/update operation exists at all) is now implemented, seeded at
// startup, same shape as coverage-restriction-data. Keyed by ueId alone, a genuinely new key
// shape (not part of the provisioned-data group's own composite key), so a new
// store/table rather than a new column.
//
// UPDATE (ADR-0129, gap-closure task #106): the real ProSe Service Subscription Data resource
// (real spec operationId `QueryPorseData` -- a literal typo in the spec itself, cited as-is, not
// corrected -- real GET-only, no create/update operation exists at all) is now implemented,
// seeded at startup, same shape as v2x-data.
//
// UPDATE (ADR-0130, gap-closure task #106): the real User Consent Subscription Data resource
// (real spec operationId `QueryUserConsentData`, schema `UcSubscriptionData` --
// TS29503_Nudm_SDM.yaml -- a single optional userConsentPerPurposeList map, no required fields at
// all -- real GET-only, no create/update operation exists at all) is now implemented, seeded at
// startup, same shape as prose-data. Takes UDR's real Nudr_DataRepository resource-type coverage
// to 43 of free5GC's ~42+ real resources -- past the free5GC-comparison baseline; real remaining
// work is the not-yet-surveyed remainder of TS29505_Subscription_Data.yaml and the genuinely
// deferred subsystems below.
//
// UPDATE (ADR-0131, gap-closure task #106): the real Time Synchronization Subscription Data
// resource (real spec operationId `QueryTimeSyncSubscriptionData`, schema
// `TimeSyncSubscriptionData` -- TS29503_Nudm_SDM.yaml -- unlike the last several GET-only
// resources closed, this one has real required fields (`afReqAuthorizations`, `serviceIds`) --
// real GET-only, no create/update operation exists at all) is now implemented, seeded at startup
// with a minimal real-shaped body, same shape as uc-data.
//
// UPDATE (ADR-0133, gap-closure task #106): the real UE's Location Information (Document)
// resource (real spec operationId `QueryUeLocation`, schema `LocationInfo` --
// TS29503_Nudm_UECM.yaml -- requires a non-empty registrationLocationInfoList -- real GET-only,
// no create/update operation exists at all) is now implemented, seeded at startup, same shape as
// time-sync-data. Its real sibling `nidd-authorization-data` was surveyed in the same pass and is
// genuinely blocked (not attempted): its spec requires real complex-object query parameters
// (`single-nssai` passed as `content: application/json` in the query string) this project has no
// precedent for parsing, same class of gap already disclosed for `pdtq-data`.
//
// UPDATE (ADR-0134, gap-closure task #106): the real A2X Subscription Data resource (real spec
// operationId `QueryA2xData`, schema `A2xSubscriptionData` -- TS29503_Nudm_SDM.yaml -- every field
// optional, same shape as v2x-data/prose-data -- real GET-only, no create/update operation exists
// at all) is now implemented, seeded at startup.
//
// UPDATE (ADR-0135, gap-closure task #106): the real Ranging and Sidelink Positioning Privacy
// Subscription Data resource (real spec operationId `QueryRangingSlPrivacyData`, schema
// `RangingSlPrivacyData` -- TS29503_Nudm_SDM.yaml -- every top-level field optional -- real
// GET-only, no create/update operation exists at all) is now implemented, seeded at startup. Real,
// disclosed: the spec's own optional `fields` query parameter (RFC 6570 form-style array,
// explode=false) for field-selection filtering is not honored -- the full stored document is
// always returned.
//
// UPDATE (ADR-0136, gap-closure task #106): the real Ranging and Sidelink Positioning Service
// Subscription Data resource (real spec operationId `QueryRangingSlPosData`, schema
// `RangingSlPosSubscriptionData` -- TS29503_Nudm_SDM.yaml -- every top-level field optional --
// real GET-only, no create/update operation exists at all) is now implemented, seeded at startup.
//
// UPDATE (ADR-0137, gap-closure task #106): the real 5MBS Subscription Data (Document) resource
// (real spec operationId `Query5mbsData`, schema `MbsSubscriptionData` -- TS29503_Nudm_SDM.yaml
// -- every field optional -- real GET-only, no create/update operation exists at all) is now
// implemented, seeded at startup.
//
// UPDATE (ADR-0139, gap-closure task #106): the real Service Specific Authorization Info
// (Document) context-data resource (real PUT+GET+PATCH+DELETE, schema
// `ServiceSpecificAuthorizationInfo` -- TS29505_Subscription_Data.yaml -- required
// serviceSpecificAuthorizationList, a map of AuthorizationInfo keyed by authId, real distinct
// 201-vs-204 PUT response codes, real RFC 6902 JSON Patch, same shape as nidd-authorizations's
// own resource) is now implemented. Composite (ueId, serviceType) key, same precedent as
// pp-data-store. Its real sibling GET-only resource at
// /subscription-data/{ueId}/service-specific-authorization-data/{serviceType} was surveyed in
// the same pass and left deferred.
// CORRECTED (ADR-0256): the reason recorded here was "real required complex-object query
// parameters this project has no parsing precedent for". That reason is now STALE -- ADR-0165's
// GetNiddAuData established exactly that precedent (a `single-nssai` JSON-encoded query value,
// decomposed to sst/sd). The resource remains deferred on ADR-0160's OTHER, still-valid
// grounds, which are about schemas and not parsing: see the GetSSAuData paragraph below.
//
// UPDATE (ADR-0140, gap-closure task #106): the real Group Identifiers mapping resource (real
// spec operationId `GetGroupIdentifiers`, schema `GroupIdentifiers` -- every field optional --
// real GET-only, no path parameters, genuinely NOT per-UE) is now implemented, seeded at
// startup. Real, disclosed: `extGroupId`/`intGroupId` are alternate lookup keys for the same
// seeded record (at least one required, no unfiltered "list all" behavior implemented); the real
// `ue-id-ind` query parameter is not honored -- `ueIdList` is always included. This is the first
// real `group-data` sub-resource closed -- the rest of `group-data` (`5g-vn-groups`,
// `mbs-group-membership`, `ee-profile-data`'s own group-keyed sibling, and their own
// `/internal`/`/pp-profile-data` variants) remains genuinely deferred, not dropped.
//
// UPDATE (ADR-0141, gap-closure task #106): the real NSSAI update ack (Document) resource (real
// spec operations `CreateOrUpdateNssaiAck`/`QueryNssaiAck`, schema `NssaiAckData` -- required
// `provisioningTime`/`ueUpdateStatus` -- real PUT+GET, no PATCH/DELETE operation exists at all)
// is now implemented. Real, disclosed: unlike this project's other PUT resources, the spec
// documents only a single `204` response for this PUT (no `201`) -- no create-vs-update
// distinction exists for this resource. This is the first real
// `ue-update-confirmation-data` sub-resource closed -- its siblings (`sor-data`, `upu-data`,
// `subscribed-cag`) remain genuinely deferred, not dropped.
//
// UPDATE (ADR-0142, gap-closure task #106): the real CAG update ack (Document) resource (real
// spec operations `CreateCagUpdateAck`/`QueryCagAck`, schema `CagAckData` -- required
// `provisioningTime`/`ueUpdateStatus`, identical shape to `NssaiAckData` -- real PUT+GET, no
// PATCH/DELETE operation exists at all, same real 204-only-PUT shape) is now implemented. Its
// `sor-data`/`upu-data` siblings remain genuinely deferred, not dropped.
//
// UPDATE (ADR-0143, gap-closure task #106): the real Authentication SoR (Document) and
// Authentication UPU (Document) resources (real spec operations
// `CreateAuthenticationSoR`/`QueryAuthSoR`/`UpdateAuthenticationSoR` for `sor-data`, schema
// `SorData`; `CreateAuthenticationUPU`/`QueryAuthUPU` for `upu-data`, schema `UpuData` -- both
// required `provisioningTime`/`ueUpdateStatus`, real PUT+GET, same 204-only-PUT shape as
// `subscribed-snssais`/`subscribed-cag` above) are now implemented. Real, disclosed asymmetry:
// `sor-data` genuinely also has a real RFC 6902 `application/json-patch+json` PATCH (`apply_patch`
// NOT upsert-capable -- requires a prior PUT, same precedent as `nidd-authorizations`); `upu-data`
// has no PATCH/DELETE at all per the spec, a genuine difference despite both resources sharing the
// same `UeUpdateStatus`-based schema shape. This closes all four `ue-update-confirmation-data`
// sub-resources this project has surveyed.
//
// UPDATE (ADR-0144, gap-closure task #106): the real `group-data` individual 5G VN Group
// Configuration resource (`group-data/5g-vn-groups/{externalGroupId}`, real spec operations
// `Create5GVnGroup`/`Get5GVnGroupConfiguration`/`Modify5GVnGroup`/`Delete5GVnGroup`, schema
// `5GVnGroupConfiguration` generated as `sbi_gen::N5GVnGroupConfiguration` -- real
// GET+PUT+PATCH+DELETE) is now implemented. Real, disclosed: the PUT documents ONLY `201` (same
// precedent as `bdt-data`), PATCH is real RFC 6902 (NOT `bdt-data`'s own RFC 7396 merge-patch),
// NOT upsert-capable. Second real `group-data` sub-resource closed, after `group-identifiers`
// (ADR-0140).
//
// UPDATE (ADR-0145, gap-closure task #106): the real `group-data` individual 5G MBS Group
// Membership resource (`group-data/mbs-group-membership/{externalGroupId}`, real spec operations
// `Create5GmbsGroup`/`GetMulticastMbsGroupMemb`/`Modify5GmbsGroup`/`Delete5GmbsGroup`, schema
// `MulticastMbsGroupMemb` -- real GET+PUT+PATCH+DELETE) is now implemented. Structurally an exact
// twin of `5g-vn-groups/{externalGroupId}` above (same 201-only PUT, same real RFC 6902 PATCH,
// NOT upsert-capable). Its own bare collection GET sibling (`group-data/mbs-group-membership`,
// real spec `Query5GmbsGroup`) was surveyed and confirmed the same genuinely blocked
// `gpsis` `style: form, explode: false` array-query-param shape as `5g-vn-groups`'s own
// `Query5GVnGroup` -- left deferred, not silently skipped. Third real `group-data` sub-resource
// closed.
//
// UPDATE (ADR-0146, gap-closure task #106): the real `group-data` Event Exposure Data for a group
// resource (`group-data/{ueGroupId}/ee-profile-data`, real spec operation `QueryGroupEEData`,
// schema `EeGroupProfileData` -- every field optional) is now implemented. Real GET-only, no
// create/update operation exists at all -- genuinely NOT per-UE, keyed by `ueGroupId` (real
// schema `VarUeGroupId`, a plain string matching `anyUE` or `extgroupid-...@...`, no encoding
// ambiguity), a real, distinct sibling of the already-closed per-UE `ee-profile-data` resource.
// Seeded at startup for this project's own "anyUE" test case. Fourth real `group-data`
// sub-resource closed.
//
// UPDATE (ADR-0147, gap-closure task #106): the real aggregate UE Update Confirmation Data
// resource (bare `{ueId}/ue-update-confirmation-data`, real spec operation `QueryUeUpdConf`,
// schema `UeUpdConfData` -- every field optional: `sorData`/`upuData`/`nssaiAckData`/
// `cagAckData`) is now implemented. Real, disclosed design decision: composed live from the four
// already-closed individual sub-resource stores at request time (NOT a fifth, duplicate table) --
// always returns `200` (an empty object if all four sub-resources are absent), since this
// "document" is a real view over four independently-stored-or-absent sub-resources, not itself a
// stored entity with its own real create/update path to key a 404-vs-200 distinction off of.
//
// UPDATE (ADR-0148, gap-closure task #106): the real Event Exposure Subscriptions
// collection + individual document (`context-data/ee-subscriptions` /
// `context-data/ee-subscriptions/{subsId}`, real spec operations
// `Queryeesubscriptions`/`CreateEeSubscriptions`/`QueryeeSubscription`/`UpdateEesubscriptions`/
// `ModifyEesubscription`/`RemoveeeSubscriptions`, schema `EeSubscription`) is now implemented,
// correcting ADR-0122's own earlier characterization of this resource as blanket "genuinely
// deeply-nested" -- on direct read, the collection GET's own `event-types`/`nf-identifiers` array
// filters are genuinely OPTIONAL (not the required-array-param class that has blocked other
// resources), so this project simply doesn't honor them (same "optional filter not honored"
// precedent as `rangingsl-privacy-data`), rather than being structurally blocked. `subsId` is
// server-generated (real UUID v4); PUT is genuinely update-only, never create (real spec 404 for
// a nonexistent resource). Real, disclosed scope narrowing: only the collection + individual
// document are implemented -- the deeper `amf-subscriptions`/`smf-subscriptions`/
// `hss-subscriptions` nested sub-collections under each `subsId` remain genuinely deferred, and
// `sdm-subscriptions` (a real, separate resource) was not re-surveyed in this pass and also
// remains deferred.
//
// UPDATE (ADR-0149, gap-closure task #106): the real Subs To Notify collection
// (`subscription-data/subs-to-notify`, real spec operations
// `SubscriptionDataSubscriptions`/`QuerySubsToNotify`) and individual document
// (`subscription-data/subs-to-notify/{subsId}`, real spec operations
// `QuerySubscriptionDataSubscriptions`/`ModifysubscriptionDataSubscription`/
// `RemovesubscriptionDataSubscriptions` -- GET+PATCH+DELETE, genuinely no PUT) are now
// implemented. This resource's original ADR-0122-era deferral reason ("no existing project
// precedent for [a] server-generated Location header") is now resolved by `ee-subscriptions`'s
// own precedent (ADR-0148) -- `subsId` generation reuses the same `sbi_core::generate_uuid_v4()`
// approach. Real, disclosed scope narrowing kept from the original finding: the real webhook
// callback itself (`onDataChange`, `{$request.body#/notificationUri}`) is NOT implemented --
// this project stores subscriptions and answers CRUD on them, but does not yet send real
// `DataChangeNotify` callbacks to the caller-supplied URI when underlying data changes (no
// project precedent yet for outbound webhook delivery on data-change events).
//
// UPDATE (ADR-0151, gap-closure task #106): the real SDM Subscriptions collection
// (`context-data/sdm-subscriptions`, real spec operations
// `Querysdmsubscriptions`/`CreateSdmSubscriptions`) and individual document
// (`context-data/sdm-subscriptions/{subsId}`, real spec operations
// `QuerysdmSubscription`/`Updatesdmsubscriptions`/`ModifysdmSubscription`/
// `RemovesdmSubscriptions` -- GET+PUT+PATCH+DELETE) are now implemented, structurally identical
// to `ee-subscriptions` (server-generated `subsId`, PUT genuinely update-only). This corrects
// ADR-0122's own blanket "genuinely deeply-nested" characterization of
// `ee-subscriptions`/`sdm-subscriptions` together for the second of the two resources -- on
// direct, individual read (not re-trusting the old bundled deferral), only the deeper
// `hss-sdm-subscriptions` nested sub-collection under each `subsId` is genuinely deferred.
//
// UPDATE (ADR-0152, gap-closure task #106): the real AMF Subscription Info (Document) resource,
// nested under an individual ee-subscription (`context-data/ee-subscriptions/{subsId}/
// amf-subscriptions`, real spec operations "Create AMF Subscriptions"
// [PUT]/GetAmfSubscriptionInfo [GET]/ModifyAmfSubscriptionInfo
// [PATCH]/RemoveAmfSubscriptionsInfo [DELETE]) is now implemented -- the first of
// `ee-subscriptions`' own nested sub-collections to be surveyed directly rather than left
// blanket-deferred. Real, disclosed: the document body is a JSON ARRAY of `AmfSubscriptionInfo`
// (`minItems: 1`), not a single object; PUT documents a real distinct 201-vs-204 (same
// is-new-tracking precedent as `amf-3gpp-access`). No referential integrity is enforced against
// the parent `ee-subscriptions` resource (this project's own established precedent of not
// enforcing cross-resource existence checks elsewhere).
//
// UPDATE (ADR-0153, gap-closure task #106): the real SMF Event Subscription Info (Document)
// resource, nested under an individual ee-subscription (`context-data/ee-subscriptions/{subsId}/
// smf-subscriptions`, real spec operations "Create SMF Subscriptions"
// [PUT]/GetSmfSubscriptionInfo [GET]/ModifySmfSubscriptionInfo
// [PATCH]/RemoveSmfSubscriptionsInfo [DELETE]) is now implemented -- the second of
// `ee-subscriptions`' own nested sub-collections. Real, disclosed: unlike its sibling
// `amf-subscriptions`, the document body is a SINGLE `SmfSubscriptionInfo` object (required
// `smfSubscriptionList`, an array of `SmfSubscriptionItem`), not a bare array -- a genuine
// difference confirmed on direct read, not assumed from the sibling's shape. Same real, distinct
// 201-vs-204 PUT.
//
// UPDATE (ADR-0154, gap-closure task #106): the real HSS Subscription Info (Document) resource,
// nested under an individual ee-subscription (`context-data/ee-subscriptions/{subsId}/
// hss-subscriptions`, real spec operations "Create HSS Subscriptions" [PUT]/GetHssSubscriptionInfo
// [GET]/ModifyHssSubscriptionInfo [PATCH]/RemoveHssSubscriptionsInfo [DELETE]) is now
// implemented -- the third and final of `ee-subscriptions`' own nested sub-collections
// (siblings `amf-subscriptions`/`smf-subscriptions` closed in ADR-0152/ADR-0153). Real, disclosed
// spec inconsistency, asked and confirmed with the user: the real spec YAML's own
// GetHssSubscriptionInfo 200 response literally cites `SmfSubscriptionInfo`, not this resource's
// own `HssSubscriptionInfo` (used consistently by its own PUT/PATCH/DELETE) -- treated as a real
// spec typo (same precedent as ADR-0129's `QueryPorseData` typo), GET returns real
// `HssSubscriptionInfo`-shaped data. Same single-object-document shape and real, distinct
// 201-vs-204 PUT as `smf-subscriptions`. This closes all three of `ee-subscriptions`' own nested
// sub-collections.
//
// UPDATE (ADR-0155, gap-closure task #106): the real HSS SDM Subscription Info (Document)
// resource, nested under an individual sdm-subscription (`context-data/sdm-subscriptions/
// {subsId}/hss-sdm-subscriptions`, real spec operations "Create HSS SDM Subscriptions" [PUT]/
// GetHssSDMSubscriptionInfo [GET]/ModifyHssSDMSubscriptionInfo [PATCH]/
// RemoveHssSDMSubscriptionsInfo [DELETE]) is now implemented -- this is `sdm-subscriptions`' own
// final deferred nested sub-collection. Reuses the same `HssSubscriptionInfo` schema as
// `ee-subscriptions`' own `hss-subscriptions` sibling (ADR-0154). Two real, disclosed findings
// from direct read: (1) unlike `amf-`/`smf-`/`hss-subscriptions` under `ee-subscriptions`, this
// resource's real spec PUT documents ONLY a `204` response, never `201` -- matches the existing
// `sor-data`/`upu-data` (ADR-0143) 204-only upsert-PUT precedent, confirmed by direct read, not
// invented. (2) `GetHssSDMSubscriptionInfo`'s own `200` response again literally cites
// `SmfSubscriptionInfo`, not `HssSubscriptionInfo` -- the same typo class just resolved (and
// user-confirmed) in ADR-0154 for the sibling resource; the same resolution is applied here
// without re-asking, since it is the identical schema-citation error on a structurally parallel
// resource.
//
// UPDATE (ADR-0156, gap-closure task #106): the real Event Exposure Group Subscriptions
// collection (`group-data/{ueGroupId}/ee-subscriptions`, real spec operations
// `QueryEeGroupSubscriptions`/`CreateEeGroupSubscriptions`) and individual document
// (`group-data/{ueGroupId}/ee-subscriptions/{subsId}`, real spec operations
// `QueryEeGroupSubscription`/`UpdateEeGroupSubscriptions`/`ModifyEeGroupSubscription`/
// `RemoveEeGroupSubscriptions` -- GET+PUT+PATCH+DELETE) are now implemented -- the group-data-
// scoped sibling of `ee-subscriptions` (ADR-0148), structurally identical but keyed by
// `ueGroupId` instead of `ueId`: same `EeSubscription` schema, server-generated `subsId`, PUT
// genuinely update-only. Real, disclosed scope narrowing kept from ADR-0148's own precedent:
// only the collection + individual document are implemented -- the deeper group-data-scoped
// `amf-subscriptions`/`smf-subscriptions`/`hss-subscriptions` nested sub-collections under each
// `subsId` remain genuinely deferred, not yet surveyed.
//
// UPDATE (ADR-0157, gap-closure task #106): the real AMF Group Subscription Info (Document)
// resource, nested under an individual group-data-scoped ee-subscription
// (`group-data/{ueGroupId}/ee-subscriptions/{subsId}/amf-subscriptions`, real spec operations
// `CreateAmfGroupSubscriptions` [PUT]/`GetAmfGroupSubscriptions` [GET]/
// `ModifyAmfGroupSubscriptions` [PATCH]/`RemoveAmfGroupSubscriptions` [DELETE]) is now
// implemented -- the group-data-scoped sibling of `ee-subscriptions/{subsId}/amf-subscriptions`
// (ADR-0152), structurally identical but keyed by `ueGroupId` instead of `ueId`: same
// array-valued `AmfSubscriptionInfo[]` document body, real distinct 201-vs-204 PUT.
//
// Also, this ADR fixes a real, significant, disclosed bug found live-verifying this resource: a
// real RFC 9110 Location-header conformance defect, first found and fixed for 3 occurrences in
// ADR-0156, turned out to affect ~20 routes project-wide (most of UDR's PUT-with-201-Create
// routes, dating back to the earliest Tier 1a resources: `amf-3gpp-access`,
// `amf-non-3gpp-access`, `mwd`, `roaming-information`, `pei-info`, `smf-registrations`,
// `pp-data-store`, `ue-policy-set`, `bdt-data`, `nidd-authorizations`,
// `service-specific-authorizations`, `5g-vn-groups`, `mbs-group-membership`, and this same ADR's
// own `ee-subscriptions`' `amf-`/`smf-`/`hss-subscriptions` nested sub-collections). Presented to
// the user via `AskUserQuestion` before proceeding; user chose to fix all ~20 occurrences in this
// same turn rather than defer. Fixed with one new shared helper, `resolved_location()` (see its
// own comment above `check_bearer`), which substitutes every real `{name}` path-parameter value
// into a route pattern in one pass -- replacing every hand-built
// `path_pattern + "/" + value` / bare `path_pattern` Location construction project-wide, so this
// class of bug cannot recur route-by-route. Every occurrence was re-verified live (either by
// fresh `POST`/`PUT` against the resource, or by build+ctest for routes not otherwise exercised
// by this ADR's own live-verification pass).
//
// UPDATE (ADR-0158, gap-closure task #106): the real SMF Event Group Subscription Info
// (Document) resource, nested under an individual group-data-scoped ee-subscription
// (`group-data/{ueGroupId}/ee-subscriptions/{subsId}/smf-subscriptions`, real spec operations
// `CreateSmfGroupSubscriptions` [PUT]/`GetSmfGroupSubscriptions` [GET]/
// `ModifySmfGroupSubscriptions` [PATCH]/`RemoveSmfGroupSubscriptions` [DELETE]) is now
// implemented -- the group-data-scoped sibling of `ee-subscriptions/{subsId}/smf-subscriptions`
// (ADR-0153), structurally identical but keyed by `ueGroupId` instead of `ueId`: same
// single-object `SmfSubscriptionInfo` document body, real distinct 201-vs-204 PUT.
//
// UPDATE (ADR-0159, gap-closure task #106): the real HSS Event Group Subscription Info
// (Document) resource, nested under an individual group-data-scoped ee-subscription
// (`group-data/{ueGroupId}/ee-subscriptions/{subsId}/hss-subscriptions`, real spec operations
// `CreateHssGroupSubscriptions` [PUT]/`GetHssGroupSubscriptions` [GET]/
// `ModifyHssGroupSubscriptions` [PATCH]/`RemoveHssGroupSubscriptions` [DELETE]) is now
// implemented -- the group-data-scoped sibling of `ee-subscriptions/{subsId}/hss-subscriptions`
// (ADR-0154), structurally identical but keyed by `ueGroupId` instead of `ueId`: same
// single-object `HssSubscriptionInfo` document body, real distinct 201-vs-204 PUT.
// `GetHssGroupSubscriptions` correctly cites `HssSubscriptionInfo` -- no response-schema typo
// here, unlike its `ueId`-scoped and `sdm-subscriptions`-scoped siblings (ADR-0154/ADR-0155).
// Real, disclosed spec inconsistency found on direct read: the real spec's own
// DELETE/PATCH/GET operations on this path declare a parameter named `externalGroupId`, but the
// actual URL path template has no such placeholder -- only `{ueGroupId}` and `{subsId}` (matching
// PUT, and every sibling resource in this family). Not a genuine design ambiguity requiring a
// choice between two real behaviors: `externalGroupId` isn't bindable from the real path at all,
// so `ueGroupId` is used throughout. This completes all three of `group-data`'s own
// `ee-subscriptions/{subsId}/...` nested sub-collections.
//
// UPDATE (ADR-0160, gap-closure task #106, no code change): bare `/subscription-data/{ueId}`/
// `{ueId}/context-data` confirmed genuinely blocked on real array query params (same class as
// `pdtq-data`/`nf-group-ids`, at the time); `GetSSAuData` investigated in depth (real
// schema-citation mismatch resolved, then a deeper structural mismatch found against the
// already-implemented CRUD sibling's own storage shape) and deliberately left deferred per
// explicit user decision -- see the resource's own full disclosure in `docs/DECISIONS.md`.
//
// UPDATE (ADR-0161, gap-closure task #106): real `style: form, explode: false` array-query-param
// parsing infra added to `sbi_core` (`split_form_array()`, see its own header comment), the
// shared blocker confirmed in ADR-0160 across `pdtq-data`/`nidd-authorization-data`/
// `Nudr_GroupIDmap`'s `/nf-group-ids`/bare `/subscription-data/{ueId}`. First real consumer:
// `QueryContextData` (`{ueId}/context-data`, see its own comment above), a live-composed
// aggregate over 11 already-existing sub-resource stores.
//
// UPDATE (ADR-0162, gap-closure task #106): the real PDTQ Data collection (`policy-data/
// pdtq-data`, real spec `ReadPdtqData`) and individual document (`policy-data/
// pdtq-data/{pdtqReferenceId}`, real spec `ReadIndividualPdtqData`/`CreateIndividualPdtqData`/
// `UpdateIndividualPdtqData`/`DeleteIndividualPdtqData`) are now implemented -- the first real
// UDR resource genuinely unblocked (not merely a candidate) by ADR-0161's new parsing infra, per
// direct read. `pdtqReferenceId` is client-supplied (a real path parameter, not
// server-generated). Real, disclosed: `CreateIndividualPdtqData` documents ONLY `201` (no
// update-via-PUT status), same precedent as `bdt-data`. Real RFC 7396 JSON Merge Patch. The
// collection GET's own optional `pdtq-ref-ids` array filter is deliberately NOT honored, matching
// the established "optional filter not honored" precedent for consistency.
//
// UPDATE (ADR-0164, gap-closure task #106): the real `GetNfGroupIDs` resource (`/nf-group-ids`,
// `TS29504_Nudr_GroupIDmap.yaml`, the same `Nudr_GroupIDmap` service as `GetRoutingIDs` above, NOT
// `Nudr_DataRepository`) is now implemented -- the second real resource genuinely unblocked by
// ADR-0161's `split_form_array()` infra, closing the real `nf-type` array query param. GET-only,
// no create/update/delete exists anywhere in the service for the mapping data itself (confirmed by
// direct read), seeded at startup, same precedent as `routing_ids`/`group_identifiers`. Real
// `404` honored when the composed result is empty (unlike the aggregate live-view resources'
// deliberate always-`200`), since this resource's own response schema requires `minProperties: 1`
// and a real `404` is documented. The sibling `/nf-group-ids/subscriptions` change-notification
// family (real webhook callback `onGroupIdMapChange`) was surveyed but is a separate, genuinely
// more complex resource family, deliberately deferred to its own future turn.
//
// UPDATE (ADR-0165, gap-closure task #106): the real `GetNiddAuData` resource
// (`/subscription-data/{ueId}/nidd-authorization-data`) is now implemented -- confirmed by direct
// read to be genuinely distinct from the already-closed `context-data/nidd-authorizations`
// resource (ADR-0121). Its real REQUIRED `single-nssai` query param uses `content:
// application/json` (a JSON-encoded query value, decomposed to sst/sd) -- a genuinely different
// parsing shape than ADR-0161's `style: form, explode: false` array params, handled here with a
// direct `json::parse()` of the already-percent-decoded query value rather than new shared infra
// (narrow enough not to warrant it). Real REQUIRED `dnn`/`mtc-provider-information`; optional
// `af-id` deliberately not honored. GET-only, no create/update/delete exists anywhere in this
// project's in-scope APIs for this document (real provisioning lives in UDM's own Nudm_NIDDAU
// service, out of scope here) -- seeded at startup, same precedent as `routing_ids`/
// `nf_group_ids`. Real, disclosed: `AuthorizationData` (this resource's own real response schema)
// is the same schema ADR-0160 found cross-referenced by the deliberately-deferred `GetSSAuData` --
// reading this resource's own spec text confirms `AuthorizationData`'s real, unambiguous home is
// here, not there.
//
// STALE PARAGRAPH, corrected by ADR-0181 (docs/DECISIONS.md) -- kept, not deleted, per this
// project's own "supersede, don't rewrite history" comment practice: `5g-vn-groups`'s and
// `mbs-group-membership`'s own bare collection GETs (`Query5GVnGroup`/`Query5GmbsGroup`), their
// own `/internal`/`/pp-profile-data` variants, and the individual `{externalGroupId}` resources
// are now ALL closed (ADR-0167/ADR-0168/ADR-0169), and the `gpsis`/`ext-group-ids` filters this
// paragraph once called "genuinely blocked" are now honored too (ADR-0181) -- they were never
// actually blocked on missing infra by the time this was written, just not yet wired into these
// specific routes.
//
// Deliberately still deferred, not dropped:
// policy-data's
// own other resources (mbs-session-pol-data -- real, disclosed: its MbsSessPolDataId key is a
// deeply nested oneOf/anyOf object (mbsSessionId -> tmgi/ssm/nid, or afAppId) with no documented
// bare-path-segment string encoding at all, genuinely more ambiguous than snssai's own flat
// two-field shape, so left deferred rather than inventing a serialization; others); the
// `Nudr_GroupIDmap` `/nf-group-ids/subscriptions` and
// `/nf-group-ids/subscriptions/{subscriptionId}` change-notification family (real
// `onGroupIdMapChange` webhook callback -- `GetNfGroupIDs` itself closed, see ADR-0164 above);
// bare
// `/subscription-data/{ueId}` (`QueryUeSubscribedData`) -- real `style: form, explode: false`
// array query params (`dataset-names`/`adjacent-plmns`/`ext-group-ids`), same class ADR-0161's
// infra could now parse but not yet wired into this route (`{ueId}/context-data` was closed via
// exactly that infra, see `QueryContextData`'s own comment above and ADR-0161/ADR-0162).
//
// `GetSSAuData` (`/subscription-data/{ueId}/service-specific-authorization-data/{serviceType}`,
// distinct from the already-implemented `context-data/service-specific-authorizations/
// {serviceType}` CRUD sibling) was investigated in depth (ADR-0160, `docs/DECISIONS.md`) and
// deliberately left deferred: its real spec response schema `AuthorizationData` is explicitly
// described as "NIDD Authorization Information" and cross-references
// `TS29503_Nudm_NIDDAU.yaml`, a real mismatch against this resource's own `serviceType` enum
// (`AF_GUIDANCE_FOR_URSP`/`AF_REQUESTED_QOS`/`AF_PROVISION_N3GPP_DEV_ID_INFO`, from
// `TS29503_Nudm_SSAU.yaml`, which has its own better-matching `ServiceSpecificAuthorizationData`
// schema). User confirmed treating the citation as a real typo and returning
// `ServiceSpecificAuthorizationData` -- but that schema doesn't structurally match what the
// already-implemented PUT/PATCH/DELETE sibling actually stores (`ServiceSpecificAuthorizationInfo`,
// itself a NIDD-cross-referencing map of `authId` -> `AuthorizationInfo`), and `GetSSAuData` has
// no PUT counterpart of its own. Implementing it would require either fabricating an undocumented
// field mapping (violates this project's own never-invent-a-field rule) or building a second,
// permanently-empty store with no write path. User confirmed: leave deferred rather than do
// either.
//
// RFC 6902 JSON Patch, not RFC 7396 Merge Patch: AmfContext3gpp and UpdateSmfContext both use
// application/json-patch+json (confirmed by reading the YAML directly), unlike UDM's
// Update3GppRegistration/UpdateSmfRegistration which use application/merge-patch+json. Applied
// via nlohmann::json::patch() (matching nfs/nrf's own UpdateNFInstance), not .merge_patch(). Both
// patch responses always return 204 here (spec permits either 204-no-body or 200 with a
// PatchResult report body listing per-operation outcomes; 204 is simpler and doesn't require
// fabricating report items with no real per-op tracking behind them -- a disclosed, deliberate
// choice, not an oversight).

#include "sbi_core/http2_client.hpp"
#include "sbi_core/http2_server.hpp"
#include "sbi_core/io_context_pool.hpp"
#include "sbi_core/json_body.hpp"
#include "sbi_core/jwt.hpp"
#include "sbi_core/logging.hpp"
#include "sbi_core/metrics.hpp"
#include "sbi_core/oauth2_client.hpp"
#include "sbi_core/otel.hpp"
#include "sbi_core/rate_limit.hpp"
#include "sbi_core/sbi_headers.hpp"
#include "sbi_core/uuid.hpp"

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <optional>
#include <thread>
#include <unordered_map>

// docs/DECISIONS.md ADR-0077 -- no hardcoded DB URL/deployment literal in source, see
// nf_config.hpp's own comment.
#include "nf_config/nf_config.hpp"

// TS29505_Subscription_Data's own types now live in TS26510_CommonData_grp.hpp -- see
// nfs/chf/src/stores.hpp's own comment (ADR-0072).
#include "TS26510_CommonData_grp.hpp"
// Gap-closure (ADR-0193 audit, ADR-0198): real generated Nudr_GroupIDmap DTOs, replacing the
// hand-written raw-JSON validation this file used to have -- see this file's own comment at the
// GroupIDmap subscription-family route registrations for the full disclosure.
#include "TS29504_Nudr_GroupIDmap.hpp"
// AuthEvent (real Authentication Status resource schema, TS29503_Nudm_UEAU.yaml, reused verbatim
// per TS29505_Subscription_Data.yaml's own $ref -- ADR-0083, gap-closure task #106) lives in its
// own generated group file, not TS26510_CommonData_grp.hpp.
#include "TS29503_Nudm_UEAU_grp.hpp"
// ADR-0255: structured data for exposure. Its own generated header rather than the merged group
// file -- TS29519_Exposure_Data.yaml is newly added to the codegen pilot list this turn, and it
// $refs TS26510_CommonData_grp.hpp above for the common types it shares.
#include "TS29519_Exposure_Data.hpp"
// ADR-0256: AIoT data. The paths live in TS29506_Aiot_Data.yaml (TS 29.506 V19.2.0), which has
// ZERO schemas of its own -- every DTO it references comes from TS29369_Nadm_DM.yaml (TS 29.369
// V19.2.0). Only the schema file is in the codegen list; generating a schemaless paths-only file
// would produce an empty header.
#include "TS29369_Nadm_DM.hpp"
#include "stores.hpp"

namespace {

using nlohmann::json;

#ifndef CERTS_DIR
#error "CERTS_DIR must be defined by CMake (see nfs/udr/CMakeLists.txt)"
#endif
#ifndef CONFIG_DIR
#error "CONFIG_DIR must be defined by CMake (see nfs/udr/CMakeLists.txt)"
#endif

constexpr const char* kNfType = "UDR";
constexpr const char* kApiRoot = "/nudr-dr/v2";
// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0120). Real, distinct server base
// path for TS29504_Nudr_GroupIDmap.yaml's own Nudr_GroupIDmap service -- a genuinely different
// real Nudr API from Nudr_DataRepository above, not a typo/duplicate of kApiRoot.
constexpr const char* kGroupIdMapApiRoot = "/nudr-group-id-map/v1";

// Must match nfs/nrf/src/main.cpp's kNrfInstanceId exactly -- see docs/DECISIONS.md ADR-0018.
constexpr const char* kNrfInstanceId = "5ba9a927-1d31-4c8e-8a10-000000000001";

// Same pattern as every other NF's check_bearer -- see nfs/nrf/src/main.cpp's comment for why a
// missing Authorization header is not itself a 401 (bootstrap security alternative:
// `security: [{}, oAuth2ClientCredentials:[...]]` in the YAML).
std::optional<sbi_core::jwt::VerifyResult> check_bearer(const sbi_core::http2::Request& req,
                                                        sbi_core::jwt::Verifier& verifier) {
    auto it = req.headers.find("authorization");
    if (it == req.headers.end()) {
        return std::nullopt;
    }
    const std::string& value = it->second;
    constexpr std::string_view kPrefix = "Bearer ";
    if (value.size() <= kPrefix.size() || value.compare(0, kPrefix.size(), kPrefix) != 0) {
        sbi_core::jwt::VerifyResult r;
        r.valid = false;
        r.error = "Authorization header present but not a Bearer token";
        return r;
    }
    return verifier.verify(value.substr(kPrefix.size()));
}

// Bug fix (ADR-0157): a real, disclosed RFC 9110 Location-header conformance defect found across
// most of UDR's PUT-with-201-Create routes -- the header was built from the raw route
// REGISTRATION pattern (e.g. `.../subscription-data/{ueId}/context-data/amf-3gpp-access`), never
// substituting the real path-parameter values, so clients received the literal, unusable
// `{ueId}` placeholder text instead of the real UE ID. First found and fixed for 3 occurrences
// in ADR-0156; a full `grep` there found ~17 more pre-existing occurrences spanning back to the
// earliest Tier 1a routes. This single substitution helper replaces every hand-built
// `path_pattern + "/" + value` / bare `path_pattern` Location construction project-wide, so the
// same class of bug can't recur route-by-route. Resolves every `{name}` segment appearing in
// `pattern` from `path_params`, in one pass -- correct for routes with more than one path
// parameter (e.g. `{ueId}` and `{subsId}` on the same route).
std::string resolved_location(const std::string& pattern,
                              const std::map<std::string, std::string>& path_params) {
    std::string result = pattern;
    for (const auto& [key, value] : path_params) {
        const std::string placeholder = "{" + key + "}";
        auto pos = result.find(placeholder);
        if (pos != std::string::npos) {
            result.replace(pos, placeholder.size(), value);
        }
    }
    return result;
}

// Real Nudr_DataRepository `onDataChange` webhook delivery (docs/DECISIONS.md ADR-0171,
// gap-closure task #106). `subs-to-notify`'s own real `SubscriptionDataSubscriptions` schema
// (`TS29505_Subscription_Data.yaml`) lets a consumer watch an open-ended list of
// `monitoredResourceUris` and receive a real `DataChangeNotify` at `callbackReference` when any
// of them changes -- this helper is the one, shared call site every real write route in this file
// invokes after a successful mutation. Real, disclosed simplifications:
// - This project's UDR has no configured external base URL (scheme+host) to construct a real,
//   fully-qualified resource URI matching what an external subscriber would have supplied in
//   `monitoredResourceUris` (which the real `Uri` schema documents as absolute). Matching is done
//   on the real, well-known API *path* only (`resolved_path`, built the same way this file's own
//   `resolved_location()` already builds Location headers) via substring containment against each
//   subscription's own monitored URIs -- a real, disclosed compromise, not a fabricated exact-URI
//   match this project cannot actually perform without inventing a base URL.
// - Delivery is synchronous, best-effort, and never surfaces its own failure to the caller of the
//   write operation that triggered it (a failed or slow webhook must not break or delay the real
//   response to the actual write request beyond the delivery attempt itself) -- consistent with
//   this project's own already-disclosed synchronous-HTTP-client debt (ADR-0009), not a new one.
// - `sbi_core::http2::Client` is internally mutex-guarded (`libs/sbi-core/include/sbi_core/
//   http2_client.hpp`), confirmed safe to share one instance across this server's concurrent
//   request-handling calls.
// Shared delivery core for both `notify_subscribers` (per-UE) and `notify_subscribers_ue_less`
// (ADR-0176, non-per-UE resources) -- `ue_id` is only present in the outgoing `DataChangeNotify`
// body for the per-UE case, since the real schema's own `ueId` field is genuinely OPTIONAL
// (confirmed by direct read of the generated `DataChangeNotify` struct).
void deliver_onDataChange(sbi_core::http2::Client& notify_client,
                          const std::vector<json>& subs,
                          const std::optional<std::string>& ue_id,
                          const std::string& resolved_path,
                          const json& changes) {
    for (const auto& sub : subs) {
        if (!sub.contains("monitoredResourceUris") || !sub.contains("callbackReference")) {
            continue;
        }
        bool matched = false;
        for (const auto& uri : sub.at("monitoredResourceUris")) {
            if (uri.is_string() &&
                uri.get<std::string>().find(resolved_path) != std::string::npos) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            continue;
        }
        json notify_body;
        if (ue_id.has_value()) {
            notify_body["ueId"] = *ue_id;
        }
        notify_body["notifyItems"] =
            json::array({json{{"resourceId", resolved_path}, {"changes", changes}}});

        sbi_core::http2::ClientRequest req;
        req.method = "POST";
        req.url = sub.at("callbackReference").get<std::string>();
        req.headers.emplace("content-type", "application/json");
        req.body = notify_body.dump();
        if (auto resp = notify_client.send(req); !resp.has_value() || resp->status != 204) {
            spdlog::warn("udr: onDataChange delivery to {} failed or non-204 for ueId={} path={}",
                         req.url,
                         ue_id.value_or("<none>"),
                         resolved_path);
        }
    }
}

void notify_subscribers(udr::SubsToNotifyStore& subs_to_notify,
                        sbi_core::http2::Client& notify_client,
                        const std::string& ue_id,
                        const std::string& resolved_path,
                        const json& changes) {
    deliver_onDataChange(
        notify_client, subs_to_notify.list_by_ue_id(ue_id), ue_id, resolved_path, changes);
}

// Non-per-UE resources (bdt-data, pdtq-data, slice-control-data, group-control-data, and the
// group-data family) have no `ueId` to match `list_by_ue_id` against -- this backs their real
// `onDataChange` delivery via `list_ue_less()` (ADR-0176), matching subscriptions with no `ueId`
// in their own `SubscriptionDataSubscriptions` body against `monitoredResourceUris` the same way.
void notify_subscribers_ue_less(udr::SubsToNotifyStore& subs_to_notify,
                                sbi_core::http2::Client& notify_client,
                                const std::string& resolved_path,
                                const json& changes) {
    deliver_onDataChange(
        notify_client, subs_to_notify.list_ue_less(), std::nullopt, resolved_path, changes);
}

// Real `ChangeItem` (`TS29571_CommonData.yaml`) constructors for the three real write shapes
// every route in this file uses -- PUT (create-or-replace), RFC 6902/7396 PATCH, DELETE. Real,
// disclosed: PUT/DELETE always describe a single, whole-resource `REPLACE`/`REMOVE` at the real
// RFC 6901 root pointer `"/"` (no partial-field diffing attempted for a full replace or removal,
// which is spec-accurate -- the whole resource genuinely did change). RFC 6902 PATCH ops are
// forwarded near-directly (their own `op`/`path`/`value` map onto `ChangeItem`'s own
// `op`/`path`/`newValue`); RFC 7396 merge-patch is real, disclosed, and reported as a single
// whole-resource `REPLACE` with the patched result, since merge-patch's own semantics don't carry
// a per-field op list to forward (same simplification precedent as PUT above).
json change_replace(const json& new_value) {
    return json::array({json{{"op", "REPLACE"}, {"path", "/"}, {"newValue", new_value}}});
}

json change_remove() {
    return json::array({json{{"op", "REMOVE"}, {"path", "/"}}});
}

json change_from_json_patch(const json& patch_ops) {
    json changes = json::array();
    for (const auto& op : patch_ops) {
        json item;
        std::string op_name = op.value("op", "replace");
        for (auto& c : op_name) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        item["op"] = op_name;
        item["path"] = op.value("path", "/");
        if (op.contains("value")) {
            item["newValue"] = op.at("value");
        }
        changes.push_back(item);
    }
    return changes;
}

// Real Nudr_GroupIDmap `onGroupIdMapChange` delivery (docs/DECISIONS.md ADR-0180, gap-closure
// task #106). Genuinely a different real API from `Nudr_DataRepository`'s own `subs-to-notify`/
// `onDataChange` infra above -- this project's own `nf-group-id-subscriptions` collection
// (ADR-0170) is what backs it, not `SubsToNotifyStore`. Real, disclosed: `TS29504_Nudr_
// GroupIDmap.yaml`'s own `/nf-group-ids` resource has genuinely NO write operation at all (GET
// -only, confirmed by direct read) -- there is no live SBI call this callback could fire from.
// The only real mutation this mapping data ever undergoes in this project is the one-time startup
// seed() in main() below, so that is the one real, honest trigger point used here -- fired once
// per seeded (subscriberId, nfType, nfGroupId) entry, not from any request handler. A real
// subscription matches by (nfType, nfGroupId) per `SubscriptionData`'s own real required fields,
// not by subscriberId (the real schema's own subscription key is coarser than the mapping key --
// confirmed by direct read, not an invented simplification).
void notify_group_id_map_change(udr::NfGroupIdSubscriptionStore& nf_group_id_subscriptions,
                                sbi_core::http2::Client& notify_client,
                                const std::string& subscriber_id,
                                const std::string& nf_type,
                                const std::string& nf_group_id) {
    for (const auto& sub : nf_group_id_subscriptions.list_all()) {
        if (!sub.contains("notificationUri") || !sub.contains("nfType") ||
            !sub.contains("nfGroupId")) {
            continue;
        }
        if (sub.at("nfType").get<std::string>() != nf_type ||
            sub.at("nfGroupId").get<std::string>() != nf_group_id) {
            continue;
        }
        json notify_body;
        notify_body["subscriberId"] = subscriber_id;
        notify_body["nfType"] = nf_type;
        notify_body["nfGroupId"] = nf_group_id;

        sbi_core::http2::ClientRequest req;
        req.method = "POST";
        req.url = sub.at("notificationUri").get<std::string>();
        req.headers.emplace("content-type", "application/json");
        req.body = notify_body.dump();
        if (auto resp = notify_client.send(req); !resp.has_value() || resp->status != 204) {
            spdlog::warn("udr: onGroupIdMapChange delivery to {} failed or non-204 for "
                         "subscriberId={} nfType={} nfGroupId={}",
                         req.url,
                         subscriber_id,
                         nf_type,
                         nf_group_id);
        }
    }
}

// Runs on a dedicated thread, never on the server's io_context -- same reasoning as
// nfs/amf/src/main.cpp's run_nrf_lifecycle (docs/DECISIONS.md ADR-0006/ADR-0019).
void run_nrf_lifecycle(const std::string& udr_instance_id, const std::string& nrf_base) {
    sbi_core::http2::TlsConfig client_tls{
        .cert_path = CERTS_DIR "/udr/cert.pem",
        .key_path = CERTS_DIR "/udr/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client http_client(std::move(client_tls));

    for (int attempt = 0; attempt < 300; ++attempt) {
        sbi_core::http2::ClientRequest probe;
        probe.method = "GET";
        probe.url = nrf_base + "/nnrf-nfm/v1/nf-instances/00000000-0000-4000-8000-000000000000";
        if (http_client.send(probe).has_value()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    sbi_core::OAuth2Client oauth(
        http_client, nrf_base + "/oauth2/token", udr_instance_id, "nnrf-nfm", "NRF");

    constexpr int kHeartbeatSeconds = 30;
    json profile{
        {"nfInstanceId", udr_instance_id},
        {"nfType", kNfType},
        {"nfStatus", "REGISTERED"},
        {"ipv4Addresses", json::array({"127.0.0.1"})},
        {"heartBeatTimer", kHeartbeatSeconds},
    };

    while (true) {
        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("udr: OAuth2 token fetch failed: {}", token.error());
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        sbi_core::http2::ClientRequest put_req;
        put_req.method = "PUT";
        put_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + udr_instance_id;
        put_req.headers.emplace("content-type", "application/json");
        put_req.headers.emplace("authorization", "Bearer " + *token);
        put_req.headers.emplace(
            sbi_core::headers::kSenderTimestamp,
            sbi_core::headers::format_sender_timestamp(std::chrono::system_clock::now()));
        put_req.body = profile.dump();

        auto put_resp = http_client.send(put_req);
        if (put_resp.has_value() && (put_resp->status == 200 || put_resp->status == 201)) {
            spdlog::info("udr: registered with NRF (HTTP {})", put_resp->status);
            break;
        }
        spdlog::warn("udr: NRF registration attempt failed, retrying in 5s");
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(kHeartbeatSeconds / 2));

        auto token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("udr: OAuth2 token fetch failed for heartbeat: {}", token.error());
            continue;
        }

        sbi_core::http2::ClientRequest patch_req;
        patch_req.method = "PATCH";
        patch_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + udr_instance_id;
        patch_req.headers.emplace("content-type", "application/json-patch+json");
        patch_req.headers.emplace("authorization", "Bearer " + *token);
        patch_req.body =
            json::array({json{{"op", "replace"}, {"path", "/nfStatus"}, {"value", "REGISTERED"}}})
                .dump();

        auto patch_resp = http_client.send(patch_req);
        if (!patch_resp.has_value() || patch_resp->status != 200) {
            spdlog::warn("udr: heartbeat failed");
        }
    }
}

} // namespace

int main() {
    const auto config = nf_config::load("udr", CONFIG_DIR);
    const auto port = nf_config::require<unsigned short>(config, "port");
    const auto metrics_bind_address =
        nf_config::require<std::string>(config, "metrics_bind_address");
    const auto nrf_base_url =
        nf_config::require<std::string>(config, "nrf_base_url", "UDR_NRF_BASE_URL");
    const auto conninfo =
        nf_config::require<std::string>(config, "database_url", "UDR_DATABASE_URL");

    sbi_core::init_logging("udr");
    sbi_core::init_tracing("udr");
    sbi_core::init_metrics(metrics_bind_address);

    const std::string udr_instance_id = sbi_core::generate_uuid_v4();
    spdlog::info("udr: starting, nfInstanceId={}", udr_instance_id);

    sbi_core::http2::TlsConfig server_tls{
        .cert_path = CERTS_DIR "/udr/cert.pem",
        .key_path = CERTS_DIR "/udr/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };

    sbi_core::jwt::Verifier verifier(CERTS_DIR "/nrf-jwt/public.pem", kNrfInstanceId);

    udr::AmfContextStore amf_contexts(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0093).
    udr::AmfNon3GppContextStore amf_non3gpp_contexts(conninfo);
    udr::SmfRegistrationStore smf_registrations(conninfo);
    udr::ProvisionedDataStore provisioned_data(conninfo);
    udr::SmPolicyDataStore sm_policy_data(conninfo);
    udr::AuthenticationSubscriptionDataStore auth_subscription_data(conninfo);
    udr::AuthenticationStatusStore auth_status(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0212).
    udr::IndividualAuthenticationStatusStore individual_auth_status(conninfo);
    udr::AmPolicyDataStore am_policy_data(conninfo);
    // ADR-0253 (from ADR-0252's full-coverage audit): real application-data traffic-influence
    // family -- the one unrouted resource a real NEF/AF path depends on.
    udr::TrafficInfluenceDataStore traffic_influence_data(conninfo);
    udr::TrafficInfluenceSubStore traffic_influence_subs(conninfo);
    // ADR-0254: the remaining real application-data resources. Five distinct tables, one shared
    // store class -- see stores.hpp for why that is not the rejected discriminator shape.
    udr::KeyedJsonDocStore pfd_data(conninfo, "udr_pfd_data", "app_id");
    udr::KeyedJsonDocStore bdt_policy_data(conninfo, "udr_bdt_policy_data", "bdt_policy_id");
    udr::KeyedJsonDocStore iptv_config_data(conninfo, "udr_iptv_config_data", "configuration_id");
    udr::KeyedJsonDocStore service_param_data(
        conninfo, "udr_service_param_data", "service_param_id");
    udr::KeyedJsonDocStore application_data_subs(conninfo, "udr_application_data_subs", "subs_id");
    // ADR-0255: structured data for exposure. access-and-mobility-data and subs-to-notify are
    // single-key documents and reuse the same store class; session-management-data is keyed by
    // (ueId, pduSessionId) and therefore has its own.
    udr::KeyedJsonDocStore exposure_am_data(conninfo, "udr_exposure_am_data", "ue_id");
    udr::KeyedJsonDocStore exposure_data_subs(conninfo, "udr_exposure_data_subs", "sub_id");
    udr::ExposureSessionManagementDataStore exposure_sm_data(conninfo);
    // ADR-0256: the last of ADR-0252's UDR gap. AIoT data (both resources are seed-only for
    // writes -- the spec defines no create/replace/delete), data-restoration subscriptions, and
    // the service-specific authorization data document.
    udr::AiotDeviceProfileDataStore aiot_device_profile_data(conninfo);
    udr::AiotAfAuthorizationDataStore aiot_af_authorization_data(conninfo);
    udr::DataRestorationSubscriptionStore data_restoration_subscriptions(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0097).
    udr::SmsfContext3gppStore smsf_3gpp_context(conninfo);
    udr::SmsfNon3GppContextStore smsf_non3gpp_context(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0098).
    udr::IpSmGwContextStore ip_sm_gw_context(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0099).
    udr::MessageWaitingDataStore mwd(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0100).
    udr::RoamingInformationStore roaming_information(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0101).
    udr::PeiInfoStore pei_info(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0102).
    udr::CoverageRestrictionDataStore coverage_restriction_data(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0103).
    udr::LcsPrivacyDataStore lcs_privacy_data(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0104).
    udr::LcsSubscriptionDataStore lcs_subscription_data(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0105).
    udr::LcsMoDataStore lcs_mo_data(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0107).
    udr::PpDataStore pp_data(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0108).
    udr::PpProfileDataStore pp_profile_data(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0109).
    udr::PpDataEntryStore pp_data_entry(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0110).
    udr::SharedDataStore shared_data(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0111).
    udr::OperatorSpecificDataStore operator_specific_data(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0112).
    udr::EeProfileDataStore ee_profile_data(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0113).
    udr::UePolicySetStore ue_policy_set(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0114).
    udr::PolicyOperatorSpecificDataStore policy_operator_specific_data(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0115).
    udr::SponsorConnectivityDataStore sponsor_connectivity_data(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0116).
    udr::BdtDataStore bdt_data(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0213).
    udr::UsageMonDataStore usage_mon_data(conninfo);
    udr::PolicyDataSubsToNotifyStore policy_data_subs_to_notify(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0117).
    udr::PlmnUePolicySetStore plmn_ue_policy_set(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0118).
    udr::SlicePolicyDataStore slice_control_data(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0119).
    udr::GroupPolicyDataStore group_control_data(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0120).
    udr::RoutingIdStore routing_ids(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0164).
    udr::NfGroupIdStore nf_group_ids(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0165).
    udr::NiddAuthorizationDataStore nidd_authorization_data(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0121).
    udr::NiddAuthorizationInfoStore nidd_authorization_info(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0122).
    udr::IdentityDataStore identity_data(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0123).
    udr::OdbDataStore odb_data(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0128).
    udr::V2xDataStore v2x_data(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0129).
    udr::ProseDataStore prose_data(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0130).
    udr::UcDataStore uc_data(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0131).
    udr::TimeSyncDataStore time_sync_data(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0133).
    udr::LocationDataStore location_data(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0134).
    udr::A2xDataStore a2x_data(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0135).
    udr::RangingSlPrivacyDataStore rangingsl_privacy_data(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0136).
    udr::RangingSlPosDataStore ranging_slpos_data(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0137).
    udr::MbsDataStore mbs_data(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0139).
    udr::ServiceSpecificAuthorizationInfoStore service_specific_authorization_info(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0140).
    udr::GroupIdentifiersStore group_identifiers(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0141).
    udr::NssaiAckDataStore nssai_ack_data(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0142).
    udr::CagAckDataStore cag_ack_data(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0143).
    udr::SorDataStore sor_data(conninfo);
    udr::UpuDataStore upu_data(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0144).
    udr::FiveGVnGroupStore five_g_vn_groups(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0145).
    udr::MbsGroupMembershipStore mbs_group_membership(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0169).
    udr::FiveGVnGroupPpProfileDataStore five_g_vn_group_pp_profile_data(conninfo);
    udr::MbsGroupPpProfileDataStore mbs_group_pp_profile_data(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0170).
    udr::NfGroupIdSubscriptionStore nf_group_id_subscriptions(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0182).
    udr::MbsSessionPolicyDataStore mbs_session_pol_data(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0146).
    udr::GroupEeProfileDataStore group_ee_profile_data(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0148).
    udr::EeSubscriptionsStore ee_subscriptions(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0149).
    udr::SubsToNotifyStore subs_to_notify(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0151).
    udr::SdmSubscriptionsStore sdm_subscriptions(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0152).
    udr::EeAmfSubscriptionInfoStore ee_amf_subscription_info(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0153).
    udr::EeSmfSubscriptionInfoStore ee_smf_subscription_info(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0154).
    udr::EeHssSubscriptionInfoStore ee_hss_subscription_info(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0155).
    udr::SdmHssSubscriptionInfoStore sdm_hss_subscription_info(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0156).
    udr::GroupEeSubscriptionsStore group_ee_subscriptions(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0157).
    udr::GroupAmfSubscriptionInfoStore group_amf_subscription_info(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0158).
    udr::GroupSmfSubscriptionInfoStore group_smf_subscription_info(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0159).
    udr::GroupHssSubscriptionInfoStore group_hss_subscription_info(conninfo);
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0162).
    udr::PdtqDataStore pdtq_data(conninfo);

    // Real seed data (ADR-0069, gap-closure Tier 1b) -- the real provisioned-data group is
    // GET-only per spec (no create/update operation exists at all, see schema.postgres.sql's own
    // header), so there is no live provisioning path yet; seeded here for the same two real test
    // SUPIs nfs/udm/src/main.cpp's own AuthenticationSubscriptionStore already seeds
    // ("imsi-999700000000001"/"...002", UERANSIM's own real test values), so a real end-to-end
    // AUSF->UDM->UDR chain has real, non-empty data to return for at least these subscribers.
    // servingPlmnId "99970" = this project's own real lab PLMN, mcc=999/mnc=70 (ADR-0016),
    // VarPlmnId's real format per TS29505_Subscription_Data.yaml (mcc+mnc concatenated).
    // sst=1/sd="000001" matches that same ADR-0016 lab S-NSSAI. dnn="internet" is the real,
    // standard default DNN/APN name used industry-wide (free5gc/open5gs both default to it too),
    // not invented for this project. SmfSelectionSubscriptionData.subscribedSnssaiInfos and
    // SessionManagementSubscriptionData.dnnConfigurations are real, cited, opaque-JSON fields this
    // codegen couldn't strongly type (OPAQUE FALLBACK) -- left unpopulated here, a real, disclosed
    // gap rather than a guessed nested shape. lcs_bca_data.locationAssistanceType is a real `Bytes`
    // field (TS29571_CommonData.yaml, base64) -- "dGVzdA==" (base64 of "test") is this project's
    // own arbitrary representative test payload, not real 3GPP assistance-data content (ADR-0106,
    // gap-closure task #106). sms_mng_data.mtSmsSubscribed is a real optional boolean field
    // (TS29503_Nudm_SDM.yaml's SmsManagementSubscriptionData) -- true is this project's own
    // representative test choice (ADR-0125, gap-closure task #106). sms_data.smsSubscribed is a
    // real optional boolean field (TS29503_Nudm_SDM.yaml's SmsSubscriptionData, genuinely
    // distinct from SmsManagementSubscriptionData's own mtSmsSubscribed above) -- true is this
    // project's own representative test choice (ADR-0126, gap-closure task #106).
    // trace_data.traceRef/traceDepth are real fields (TS29571_CommonData.yaml's TraceData) --
    // "99970-A1B2C3" matches the real cited traceRef pattern (MCC+MNC + "-" + 3-octet hex Trace
    // ID per TS 32.422) for this project's own real lab PLMN, and "MEDIUM" is a real TraceDepth
    // enum value, this project's own representative test choice (ADR-0127, gap-closure task
    // #106).
    // gpsis (ADR-0236, gap-closure task #169): real field on AccessAndMobilitySubscriptionData
    // (TS29503_Nudm_SDM.yaml) -- the real, spec-correct home for a subscriber's GPSI, not a new
    // invented location. "msisdn-9997000001"/"...002" are this project's own representative test
    // MSISDNs (real Gpsi pattern `msisdn-[0-9]{5,15}`), one per seeded SUPI so GetSupiOrGpsi/
    // GetMultipleIdentifiers have real, non-degenerate SUPI->GPSI data to return.
    const std::unordered_map<std::string, std::string> test_gpsi_by_supi = {
        {"imsi-999700000000001", "msisdn-9997000001"},
        {"imsi-999700000000002", "msisdn-9997000002"},
    };
    for (const std::string& supi :
         {std::string("imsi-999700000000001"), std::string("imsi-999700000000002")}) {
        json am_data;
        am_data["nssai"]["defaultSingleNssais"] = json::array({json{{"sst", 1}, {"sd", "000001"}}});
        am_data["gpsis"] = json::array({test_gpsi_by_supi.at(supi)});
        json sm_data;
        sm_data["singleNssai"] = json{{"sst", 1}, {"sd", "000001"}};
        json lcs_bca_data;
        lcs_bca_data["locationAssistanceType"] = "dGVzdA==";
        json sms_mng_data;
        sms_mng_data["mtSmsSubscribed"] = true;
        json sms_data;
        sms_data["smsSubscribed"] = true;
        json trace_data;
        trace_data["traceRef"] = "99970-A1B2C3";
        trace_data["traceDepth"] = "MEDIUM";
        provisioned_data.seed(supi,
                              "99970",
                              std::make_optional(am_data),
                              std::make_optional(json::object()),
                              std::make_optional(sm_data),
                              std::make_optional(lcs_bca_data),
                              std::make_optional(sms_mng_data),
                              std::make_optional(sms_data),
                              std::make_optional(trace_data));
    }

    // Real seed data (ADR-0102, gap-closure task #106) -- the real Enhanced Coverage Restriction
    // Data resource is genuinely GET-only per spec, same "no live provisioning path yet" reasoning
    // as provisioned-data above. Seeded for the same two real test SUPIs, real lab PLMN
    // (mcc=999/mnc=70, ADR-0016); ecRestrictionDataNb explicitly false is this project's own
    // representative test value, not a spec default beyond the schema's own documented
    // `default: false`.
    for (const std::string& supi :
         {std::string("imsi-999700000000001"), std::string("imsi-999700000000002")}) {
        json coverage_data;
        coverage_data["plmnEcInfoList"] = json::array({json{
            {"plmnId", json{{"mcc", "999"}, {"mnc", "70"}}}, {"ecRestrictionDataNb", false}}});
        coverage_restriction_data.seed(supi, coverage_data);
    }

    // Real seed data (ADR-0103, gap-closure task #106) -- the real LCS Privacy Subscription Data
    // resource is genuinely GET-only per spec, same "no live provisioning path yet" reasoning as
    // above. `locationPrivacyInd` is a real enum value from LocationPrivacyInd
    // (TS29503_Nudm_SDM.yaml) -- LOCATION_ALLOWED is this project's own representative test
    // choice, not a spec default.
    for (const std::string& supi :
         {std::string("imsi-999700000000001"), std::string("imsi-999700000000002")}) {
        json lcs_privacy;
        lcs_privacy["lpi"]["locationPrivacyInd"] = "LOCATION_ALLOWED";
        lcs_privacy_data.seed(supi, lcs_privacy);
    }

    // Real seed data (ADR-0104, gap-closure task #106) -- the real LCS Subscription Data resource
    // is genuinely GET-only per spec, same "no live provisioning path yet" reasoning as above.
    // `pruInd: "NON_PRU"` is a real enum value from PruInd (TS29503_Nudm_SDM.yaml), this project's
    // own representative test choice; `userPlanePosIndLmf: false` matches the schema's own
    // documented `default: false`.
    for (const std::string& supi :
         {std::string("imsi-999700000000001"), std::string("imsi-999700000000002")}) {
        json lcs_subscription;
        lcs_subscription["pruInd"] = "NON_PRU";
        lcs_subscription["userPlanePosIndLmf"] = false;
        lcs_subscription_data.seed(supi, lcs_subscription);
    }

    // Real seed data (ADR-0105, gap-closure task #106) -- the real LCS Mobile Originated
    // Subscription Data resource is genuinely GET-only per spec, same "no live provisioning path
    // yet" reasoning as above. `BASIC_SELF_LOCATION` is a real enum value from LcsMoServiceClass
    // (TS29503_Nudm_SDM.yaml), this project's own representative test choice for the mandatory
    // `allowedServiceClasses` field.
    for (const std::string& supi :
         {std::string("imsi-999700000000001"), std::string("imsi-999700000000002")}) {
        json lcs_mo;
        lcs_mo["allowedServiceClasses"] = json::array({"BASIC_SELF_LOCATION"});
        lcs_mo_data.seed(supi, lcs_mo);
    }

    // Real seed data (ADR-0108, gap-closure task #106) -- the real Parameter Provision profile
    // Data resource is genuinely GET-only per spec, same "no live provisioning path yet" reasoning
    // as above. `"ALL"` is a real, documented special key for `allowedMtcProviders` (per this
    // schema's own description text, not fabricated); `afId: "af1"` is this project's own
    // arbitrary representative test value.
    for (const std::string& supi :
         {std::string("imsi-999700000000001"), std::string("imsi-999700000000002")}) {
        json pp_profile;
        pp_profile["allowedMtcProviders"]["ALL"] = json::array({json{{"afId", "af1"}}});
        pp_profile_data.seed(supi, pp_profile);
    }

    // Real seed data (ADR-0110, gap-closure task #106) -- the real individual Shared Data
    // resource is genuinely GET-only per spec, same "no live provisioning path yet" reasoning as
    // above. Genuinely NOT per-UE, so seeded once (not looped over the two test SUPIs).
    // "10000-default" matches SharedDataId's own real pattern (^[0-9]{5,6}-.+$), this project's
    // own representative test identifier, not fabricated spec content.
    {
        json shared;
        shared["sharedDataId"] = "10000-default";
        shared_data.seed("10000-default", shared);
    }

    // Real seed data (ADR-0112, gap-closure task #106) -- the real Event Exposure Data resource
    // is genuinely GET-only per spec, same "no live provisioning path yet" reasoning as above.
    // `LOSS_OF_CONNECTIVITY` is a real enum value from EventType (TS29503_Nudm_EE.yaml), this
    // project's own representative test choice for the optional `restrictedEventTypes` field.
    for (const std::string& supi :
         {std::string("imsi-999700000000001"), std::string("imsi-999700000000002")}) {
        json ee_profile;
        ee_profile["restrictedEventTypes"] = json::array({"LOSS_OF_CONNECTIVITY"});
        ee_profile_data.seed(supi, ee_profile);
    }

    // Real seed data (ADR-0115, gap-closure task #106) -- the real Sponsor Connectivity Data
    // resource is genuinely GET-only per spec, same "no live provisioning path yet" reasoning as
    // above. Genuinely NOT per-UE, so seeded once. "sponsor1" is this project's own arbitrary
    // representative test sponsorId; `aspIds` is the real mandatory field.
    {
        json sponsor;
        sponsor["aspIds"] = json::array({"asp1"});
        sponsor_connectivity_data.seed("sponsor1", sponsor);
    }

    // Real seed data (ADR-0182, gap-closure task #106) -- the real MBS Session Policy Control Data
    // resource is genuinely GET-only per spec, same "no live provisioning path yet" reasoning as
    // above. Real, disclosed: `polSessionId`'s own real schema is a `oneOf` of
    // `{mbsSessionId}`/`{afAppId}`, where `mbsSessionId` is itself an `anyOf` of `{tmgi}`/`{ssm}`.
    // ADR-0119's own original deferral already explicitly declined inventing a serialization for
    // that nested `mbsSessionId` branch (calling it "fabrication, not a disclosed convention
    // choice," unlike `slice-control-data`'s own disclosed `sst + '-' + sd` convention for the
    // genuinely flat `Snssai` object) -- that reasoning stands unreversed here. What this ADR
    // actually implements: the `oneOf`'s own `afAppId` branch is, on its own, just `type: string`
    // -- already unambiguous, no encoding decision needed. Seeded with one representative key on
    // that branch only; the `mbsSessionId`/`tmgi`/`ssm` branch remains untouched, not addressed.
    {
        json mbs_pol;
        mbs_pol["5qis"] = json::array({9});
        mbs_session_pol_data.seed("af-app-1", mbs_pol);
    }

    // Real seed data (ADR-0146, gap-closure task #106) -- the real Event Exposure Data for a
    // group resource is genuinely GET-only per spec, same "no live provisioning path yet"
    // reasoning as above. Genuinely NOT per-UE (keyed by VarUeGroupId, "anyUE" or
    // "extgroupid-...@..."), so seeded once for this project's own real "anyUE" test case -- every
    // real field on EeGroupProfileData is optional, so an empty object is a genuine, real,
    // schema-valid response, not a fabricated placeholder.
    { group_ee_profile_data.seed("anyUE", json::object()); }

    // Real seed data (ADR-0117, gap-closure task #106) -- the real PLMN UE Policy Set resource is
    // genuinely GET-only per spec, same "no live provisioning path yet" reasoning as above.
    // Genuinely NOT per-UE, so seeded once for this project's own real lab PLMN ("99970",
    // mcc=999/mnc=70, ADR-0016). `subscCats` is a real optional field (array of strings, no
    // further-typed enum in the spec); "cat1" is this project's own arbitrary representative test
    // value, not fabricated spec content.
    {
        json plmn_ue_policy;
        plmn_ue_policy["subscCats"] = json::array({"cat1"});
        plmn_ue_policy_set.seed("99970", plmn_ue_policy);
    }

    // Real seed data (ADR-0120, gap-closure task #106) -- the real GetRoutingIDs resource
    // (Nudr_GroupIDmap, genuinely distinct API from Nudr_DataRepository above) is genuinely
    // GET-only per spec, same "no live provisioning path yet" reasoning as above. Composite-keyed
    // by (nf_type, nf_group_id); "UDM" matches this project's own real, already-built NF type
    // (TS29510_Nnrf_NFManagement.yaml's own NFType enum). "udm-group-1" is this project's own
    // arbitrary representative test NfGroupId, not fabricated spec content. "0001" is a real
    // RoutingIndicator per its own documented pattern (^[0-9]{1,4}$).
    {
        json routing_id;
        routing_id["routingIndicators"] = json::array({"0001"});
        routing_ids.seed("UDM", "udm-group-1", routing_id);
    }

    // Real seed data (ADR-0164, gap-closure task #106) -- the real GetNfGroupIDs resource
    // (Nudr_GroupIDmap) is genuinely GET-only, same "no live provisioning path yet" reasoning as
    // above (its only other write operation, /nf-group-ids/subscriptions, creates a change
    // notification subscription, not a way to set this mapping). Composite-keyed by
    // (subscriber_id, nf_type); this project's own two real, already-seeded test SUPIs
    // (imsi-999700000000001/002) each mapped to a real "AMF"/"SMF" NFType (TS29510's own real
    // NFType enum), real, arbitrary representative group IDs (NfGroupId is a plain string per its
    // own schema, no documented format to match).
    {
        nf_group_ids.seed("imsi-999700000000001", "AMF", "amf-group-01");
        nf_group_ids.seed("imsi-999700000000001", "SMF", "smf-group-01");
        nf_group_ids.seed("imsi-999700000000002", "AMF", "amf-group-02");
    }

    // Real seed data (ADR-0165, gap-closure task #106) -- the real GetNiddAuData resource is
    // genuinely GET-only from this project's own in-scope APIs (real provisioning lives in UDM's
    // Nudm_NIDDAU service, out of scope here), same "no live provisioning path yet" reasoning as
    // above. Composite-keyed by (ue_id, sst, sd, dnn, mtc_provider_information); sst=1/sd="000001"
    // is this project's own already-established lab S-NSSAI (ADR-0016), dnn="internet" is this
    // project's own already-established lab DNN, mtc-provider-information is a real, arbitrary
    // representative string (MtcProviderInformation has no documented format to match). The
    // response body is a real, minimal-but-valid AuthorizationData document: one UserIdentifier
    // (required `supi`) in the required `authorizationData` array.
    {
        json nidd_auth_data;
        nidd_auth_data["authorizationData"] = json::array({json{{"supi", "imsi-999700000000001"}}});
        nidd_authorization_data.seed(
            "imsi-999700000000001", 1, "000001", "internet", "mtc-provider-1", nidd_auth_data);
    }

    // Real seed data (ADR-0169, gap-closure task #106) -- the real Query5GVnGroupPPData/
    // Query5GmbsGroupPPData resources are genuinely keyless singletons with no
    // create/update/delete path anywhere in the spec, same "no live provisioning path yet"
    // reasoning as above. Real, disclosed: seeded with an empty `allowedMtcProviders`/
    // `allowedMbsInfos` map absent entirely -- every field on both `Pp5gVnGroupProfileData`/
    // `Pp5gMbsGroupProfileData` is optional, so an empty top-level object is a real, valid
    // document, not a fabricated placeholder.
    {
        five_g_vn_group_pp_profile_data.seed(json::object());
        mbs_group_pp_profile_data.seed(json::object());
    }

    // Real seed data (ADR-0256) -- both AIoT resources are genuinely write-less in UDR's own
    // spec: TS29506_Aiot_Data.yaml gives aiot-device-profile-data GET+PATCH and
    // af-authorization-data GET only, with no create/replace/delete anywhere, so seeding is the
    // only way a row can first exist. Same "no live provisioning path yet" reasoning as the
    // provisioned-data group above.
    //
    // Every field below is a real required field from TS29369_Nadm_DM.yaml, not a placeholder:
    // AiotDevProfileData requires `aiotDevPermId` and `lastKnownAiotfInfo`;
    // IndividualAfAuthorizationData is keyed in `afAuthData` by the AF id, per that schema's own
    // "the key of the map is the AF ID" description. The identifier values themselves are this
    // project's own lab choices, in the same style as the existing lab SUPIs.
    {
        json profile;
        // AiotDevPermId is `Bytes` (`format: byte`) in TS29571_CommonData.yaml, i.e. base64 --
        // NOT a free-form label. The codegen emits it as an opaque nlohmann::json fallback, so
        // nothing would have rejected a plain string; this value is base64 ("aiot-dev-1") because
        // the YAML says so, not because a compiler complained.
        profile["aiotDevPermId"] = "YWlvdC1kZXYtMQ==";
        // `lastKnownAiotfInfoInd` is LastKnownAiotfInfo's ONLY required field (a boolean) --
        // checked against TS29369_Nadm_DM.yaml, not guessed. An earlier draft of this seed
        // invented an `aiotfInstanceId` field here; the generated from_json rejected it, which is
        // exactly what generated DTOs are for.
        profile["lastKnownAiotfInfo"] = json{{"lastKnownAiotfInfoInd", true}};
        aiot_device_profile_data.seed("YWlvdC1kZXYtMQ==", profile);

        json af_auth;
        // `afId` is IndividualAfAuthorizationData's ONLY required field. An empty object here
        // would be a stored document that violates its own schema -- and the GET route returns
        // this map opaquely, so no round-trip would have caught it.
        af_auth["afAuthData"] = json{{"af-app-1", json{{"afId", "af-app-1"}}}};
        aiot_af_authorization_data.seed(af_auth);
    }

    // Real seed data (ADR-0123, gap-closure task #106) -- the real ODB Data resource is genuinely
    // GET-only per spec, same "no live provisioning path yet" reasoning as above.
    // `roamingOdb: "OUTSIDE_HOME_PLMN"` is a real enum value from RoamingOdb
    // (TS29571_CommonData.yaml), this project's own representative test choice.
    for (const std::string& supi :
         {std::string("imsi-999700000000001"), std::string("imsi-999700000000002")}) {
        json odb;
        odb["roamingOdb"] = "OUTSIDE_HOME_PLMN";
        odb_data.seed(supi, odb);
    }

    // Real seed data (ADR-0128, gap-closure task #106) -- the real V2X Subscription Data resource
    // is genuinely GET-only per spec, same "no live provisioning path yet" reasoning as above.
    // `nrV2xServicesAuth.vehicleUeAuth: "AUTHORIZED"` is a real enum value from UeAuth
    // (TS29571_CommonData.yaml), this project's own representative test choice.
    for (const std::string& supi :
         {std::string("imsi-999700000000001"), std::string("imsi-999700000000002")}) {
        json v2x;
        v2x["nrV2xServicesAuth"]["vehicleUeAuth"] = "AUTHORIZED";
        v2x_data.seed(supi, v2x);
    }

    // Real seed data (ADR-0129, gap-closure task #106) -- the real ProSe Service Subscription
    // Data resource is genuinely GET-only per spec, same "no live provisioning path yet"
    // reasoning as above. `proseServiceAuth.proseDirectDiscoveryAuth: "AUTHORIZED"` is a real
    // enum value from UeAuth (TS29571_CommonData.yaml), this project's own representative test
    // choice.
    for (const std::string& supi :
         {std::string("imsi-999700000000001"), std::string("imsi-999700000000002")}) {
        json prose;
        prose["proseServiceAuth"]["proseDirectDiscoveryAuth"] = "AUTHORIZED";
        prose_data.seed(supi, prose);
    }

    // Real seed data (ADR-0130, gap-closure task #106) -- the real User Consent Subscription Data
    // resource is genuinely GET-only per spec, same "no live provisioning path yet" reasoning as
    // above. `ANALYTICS`/`CONSENT_GIVEN` are real enum values from UcPurpose/UserConsent
    // (TS29503_Nudm_SDM.yaml), this project's own representative test choice.
    for (const std::string& supi :
         {std::string("imsi-999700000000001"), std::string("imsi-999700000000002")}) {
        json uc;
        uc["userConsentPerPurposeList"]["ANALYTICS"] = "CONSENT_GIVEN";
        uc_data.seed(supi, uc);
    }

    // Real seed data (ADR-0131, gap-closure task #106) -- the real Time Synchronization
    // Subscription Data resource is genuinely GET-only per spec, same "no live provisioning path
    // yet" reasoning as above. Real schema TimeSyncSubscriptionData (TS29503_Nudm_SDM.yaml)
    // requires both afReqAuthorizations (oneOf gptpAllowedInfoList/astiAllowedInfo) and
    // serviceIds; the minimal real-shaped seed below uses gptpAllowedInfoList (an array of
    // GptpAllowedInfo, every field inside optional) and a single TimeSyncServiceId (its own
    // `reference` field is the only required one), this project's own representative test choice.
    for (const std::string& supi :
         {std::string("imsi-999700000000001"), std::string("imsi-999700000000002")}) {
        json time_sync;
        time_sync["afReqAuthorizations"]["gptpAllowedInfoList"] =
            json::array({json{{"dnn", "internet"}, {"gptpAllowed", true}}});
        time_sync["serviceIds"] = json::array({json{{"reference", "ts-service-1"}}});
        time_sync_data.seed(supi, time_sync);
    }

    // Real seed data (ADR-0133, gap-closure task #106) -- the real UE's Location Information
    // resource is genuinely GET-only per spec, same "no live provisioning path yet" reasoning as
    // above. Real schema LocationInfo (TS29503_Nudm_UECM.yaml) requires a non-empty
    // registrationLocationInfoList; each RegistrationLocationInfo requires amfInstanceId (a real
    // UUID-format NfInstanceId) and accessTypeList (real AccessType enum,
    // TS29571_CommonData.yaml). Synthetic test AMF instance ID and `3GPP_ACCESS`, this project's
    // own representative test choice.
    for (const std::string& supi :
         {std::string("imsi-999700000000001"), std::string("imsi-999700000000002")}) {
        json location;
        location["registrationLocationInfoList"] =
            json::array({json{{"amfInstanceId", "00000000-0000-4000-8000-00000000a001"},
                              {"accessTypeList", json::array({"3GPP_ACCESS"})}}});
        location_data.seed(supi, location);
    }

    // Real seed data (ADR-0134, gap-closure task #106) -- the real A2X Subscription Data
    // resource is genuinely GET-only per spec, same "no live provisioning path yet" reasoning as
    // above. `nrA2xServicesAuth.uavUeAuth: "AUTHORIZED"` is a real enum value from UeAuth
    // (TS29571_CommonData.yaml), this project's own representative test choice.
    for (const std::string& supi :
         {std::string("imsi-999700000000001"), std::string("imsi-999700000000002")}) {
        json a2x;
        a2x["nrA2xServicesAuth"]["uavUeAuth"] = "AUTHORIZED";
        a2x_data.seed(supi, a2x);
    }

    // Real seed data (ADR-0135, gap-closure task #106) -- the real Ranging and Sidelink
    // Positioning Privacy Subscription Data resource is genuinely GET-only per spec, same "no
    // live provisioning path yet" reasoning as above. `rslppi.rangingSlPrivacyInd:
    // "RANGINGSL_ALLOWED"` is a real enum value from RangingSlPrivacyInd
    // (TS29503_Nudm_SDM.yaml), this project's own representative test choice.
    for (const std::string& supi :
         {std::string("imsi-999700000000001"), std::string("imsi-999700000000002")}) {
        json rangingsl_privacy;
        rangingsl_privacy["rslppi"]["rangingSlPrivacyInd"] = "RANGINGSL_ALLOWED";
        rangingsl_privacy_data.seed(supi, rangingsl_privacy);
    }

    // Real seed data (ADR-0136, gap-closure task #106) -- the real Ranging and Sidelink
    // Positioning Service Subscription Data resource is genuinely GET-only per spec, same "no
    // live provisioning path yet" reasoning as above. `rangingSlPosAuth.rgSlPosPc5Auth:
    // "AUTHORIZED"` is a real enum value from UeAuth (TS29571_CommonData.yaml), this project's
    // own representative test choice.
    for (const std::string& supi :
         {std::string("imsi-999700000000001"), std::string("imsi-999700000000002")}) {
        json ranging_slpos;
        ranging_slpos["rangingSlPosAuth"]["rgSlPosPc5Auth"] = "AUTHORIZED";
        ranging_slpos_data.seed(supi, ranging_slpos);
    }

    // Real seed data (ADR-0137, gap-closure task #106) -- the real 5MBS Subscription Data
    // (Document) resource is genuinely GET-only per spec, same "no live provisioning path yet"
    // reasoning as above. `mbsAllowed: true` is a real, simple boolean field from
    // MbsSubscriptionData (TS29503_Nudm_SDM.yaml), this project's own representative test choice.
    for (const std::string& supi :
         {std::string("imsi-999700000000001"), std::string("imsi-999700000000002")}) {
        json mbs;
        mbs["mbsAllowed"] = true;
        mbs_data.seed(supi, mbs);
    }

    // Real seed data (ADR-0140, gap-closure task #106) -- the real Group Identifiers mapping
    // resource is genuinely GET-only per spec, same "no live provisioning path yet" reasoning as
    // above. Real, disclosed test values matching each field's own real pattern:
    // "extgroupid-group1@example.com" (ExtGroupId, ^extgroupid-[^@]+@[^@]+$),
    // "A1B2C3D4-001-01-AB" (GroupId/intGroupId,
    // ^[A-Fa-f0-9]{8}-[0-9]{3}-[0-9]{2,3}-([A-Fa-f0-9][A-Fa-f0-9]){1,10}$). ueIdList uses the same
    // two real test SUPIs every other seeded resource uses.
    {
        json group;
        group["extGroupId"] = "extgroupid-group1@example.com";
        group["intGroupId"] = "A1B2C3D4-001-01-AB";
        group["ueIdList"] = json::array(
            {json{{"supi", "imsi-999700000000001"}}, json{{"supi", "imsi-999700000000002"}}});
        group_identifiers.seed("extgroupid-group1@example.com", "A1B2C3D4-001-01-AB", group);
    }

    auto meter = sbi_core::get_meter("udr");
    auto amf_ctx_write_counter = meter->CreateUInt64Counter(
        "udr_amf_context_write_total", "Total CreateAmfContext3gpp/AmfContext3gpp calls");
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0093).
    auto amf_non3gpp_ctx_write_counter = meter->CreateUInt64Counter(
        "udr_amf_non3gpp_context_write_total", "Total CreateAmfContextNon3gpp calls");
    auto smf_reg_write_counter =
        meter->CreateUInt64Counter("udr_smf_registration_write_total",
                                   "Total CreateOrUpdateSmfRegistration/UpdateSmfContext calls");
    auto smf_reg_delete_counter = meter->CreateUInt64Counter("udr_smf_registration_delete_total",
                                                             "Total DeleteSmfRegistration calls");
    auto provisioned_data_get_counter = meter->CreateUInt64Counter(
        "udr_provisioned_data_get_total",
        "Total provisioned-data am-data/smf-selection-subscription-data/sm-data/lcs-bca-data GET "
        "calls");
    auto sm_policy_data_get_counter = meter->CreateUInt64Counter(
        "udr_sm_policy_data_get_total", "Total ReadSessionManagementPolicyData calls");
    auto sm_policy_data_patch_counter = meter->CreateUInt64Counter(
        "udr_sm_policy_data_patch_total", "Total UpdateSessionManagementPolicyData calls");
    auto auth_subscription_get_counter = meter->CreateUInt64Counter(
        "udr_auth_subscription_get_total", "Total QueryAuthSubsData calls");
    auto auth_subscription_patch_counter = meter->CreateUInt64Counter(
        "udr_auth_subscription_patch_total", "Total ModifyAuthenticationSubscription calls");
    auto auth_status_put_counter = meter->CreateUInt64Counter(
        "udr_auth_status_put_total", "Total CreateAuthenticationStatus calls");
    auto auth_status_get_counter = meter->CreateUInt64Counter(
        "udr_auth_status_get_total", "Total QueryAuthenticationStatus calls");
    auto auth_status_delete_counter = meter->CreateUInt64Counter(
        "udr_auth_status_delete_total", "Total DeleteAuthenticationStatus calls");
    auto individual_auth_status_put_counter = meter->CreateUInt64Counter(
        "udr_individual_auth_status_put_total", "Total CreateIndividualAuthenticationStatus calls");
    auto individual_auth_status_get_counter = meter->CreateUInt64Counter(
        "udr_individual_auth_status_get_total", "Total QueryIndividualAuthenticationStatus calls");
    auto individual_auth_status_delete_counter =
        meter->CreateUInt64Counter("udr_individual_auth_status_delete_total",
                                   "Total DeleteIndividualAuthenticationStatus calls");
    auto provisioned_data_sets_get_counter = meter->CreateUInt64Counter(
        "udr_provisioned_data_sets_get_total", "Total QueryProvisionedData calls");
    auto am_policy_data_get_counter = meter->CreateUInt64Counter(
        "udr_am_policy_data_get_total", "Total ReadAccessAndMobilityPolicyData calls");
    auto am_policy_data_patch_counter = meter->CreateUInt64Counter(
        "udr_am_policy_data_patch_total", "Total UpdateAccessAndMobilityPolicyData calls");
    auto smsf_3gpp_write_counter = meter->CreateUInt64Counter("udr_smsf_3gpp_context_write_total",
                                                              "Total CreateSmsfContext3gpp calls");
    auto smsf_3gpp_delete_counter = meter->CreateUInt64Counter("udr_smsf_3gpp_context_delete_total",
                                                               "Total DeleteSmsfContext3gpp calls");
    auto smsf_non3gpp_write_counter = meter->CreateUInt64Counter(
        "udr_smsf_non3gpp_context_write_total", "Total CreateSmsfContextNon3gpp calls");
    auto smsf_non3gpp_delete_counter = meter->CreateUInt64Counter(
        "udr_smsf_non3gpp_context_delete_total", "Total DeleteSmsfContextNon3gpp calls");
    auto ip_sm_gw_write_counter = meter->CreateUInt64Counter(
        "udr_ip_sm_gw_context_write_total", "Total CreateIpSmGwContext/ModifyIpSmGwContext calls");
    auto ip_sm_gw_delete_counter = meter->CreateUInt64Counter("udr_ip_sm_gw_context_delete_total",
                                                              "Total DeleteIpSmGwContext calls");
    auto mwd_write_counter = meter->CreateUInt64Counter(
        "udr_mwd_write_total", "Total CreateMessageWaitingData/ModifyMessageWaitingData calls");
    auto mwd_delete_counter =
        meter->CreateUInt64Counter("udr_mwd_delete_total", "Total DeleteMessageWaitingData calls");
    auto roaming_information_write_counter = meter->CreateUInt64Counter(
        "udr_roaming_information_write_total", "Total UpdateRoamingInformation calls");
    auto pei_info_write_counter = meter->CreateUInt64Counter(
        "udr_pei_info_write_total", "Total CreateOrUpdatePeiInformation calls");
    auto coverage_restriction_data_get_counter = meter->CreateUInt64Counter(
        "udr_coverage_restriction_data_get_total", "Total QueryCoverageRestrictionData calls");
    auto lcs_privacy_data_get_counter = meter->CreateUInt64Counter(
        "udr_lcs_privacy_data_get_total", "Total QueryLcsPrivacyData calls");
    auto lcs_subscription_data_get_counter = meter->CreateUInt64Counter(
        "udr_lcs_subscription_data_get_total", "Total QueryLcsSubscriptionData calls");
    auto lcs_mo_data_get_counter =
        meter->CreateUInt64Counter("udr_lcs_mo_data_get_total", "Total QueryLcsMoData calls");
    auto pp_data_get_counter =
        meter->CreateUInt64Counter("udr_pp_data_get_total", "Total GetppData calls");
    auto pp_data_patch_counter =
        meter->CreateUInt64Counter("udr_pp_data_patch_total", "Total ModifyPpData calls");
    auto pp_profile_data_get_counter =
        meter->CreateUInt64Counter("udr_pp_profile_data_get_total", "Total QueryPPData calls");
    auto pp_data_entry_write_counter = meter->CreateUInt64Counter(
        "udr_pp_data_entry_write_total", "Total Create PP Data Entry calls");
    auto pp_data_entry_delete_counter = meter->CreateUInt64Counter(
        "udr_pp_data_entry_delete_total", "Total Delete PP Data Entry calls");
    auto shared_data_get_counter = meter->CreateUInt64Counter(
        "udr_shared_data_get_total", "Total GetIndividualSharedData calls");
    auto operator_specific_data_get_counter = meter->CreateUInt64Counter(
        "udr_operator_specific_data_get_total", "Total QueryOperSpecData calls");
    auto operator_specific_data_patch_counter = meter->CreateUInt64Counter(
        "udr_operator_specific_data_patch_total", "Total ModifyOperSpecData calls");
    // ADR-0197: correcting ADR-0111's own real documentation error.
    auto operator_specific_data_put_counter = meter->CreateUInt64Counter(
        "udr_operator_specific_data_put_total", "Total CreateOperSpecData calls");
    auto operator_specific_data_delete_counter = meter->CreateUInt64Counter(
        "udr_operator_specific_data_delete_total", "Total DeleteOperSpecData calls");
    auto ee_profile_data_get_counter =
        meter->CreateUInt64Counter("udr_ee_profile_data_get_total", "Total QueryEEData calls");
    auto ue_policy_set_write_counter = meter->CreateUInt64Counter(
        "udr_ue_policy_set_write_total", "Total CreateOrReplaceUEPolicySet calls");
    auto ue_policy_set_get_counter =
        meter->CreateUInt64Counter("udr_ue_policy_set_get_total", "Total ReadUEPolicySet calls");
    auto ue_policy_set_patch_counter = meter->CreateUInt64Counter("udr_ue_policy_set_patch_total",
                                                                  "Total UpdateUEPolicySet calls");
    auto policy_operator_specific_data_get_counter = meter->CreateUInt64Counter(
        "udr_policy_operator_specific_data_get_total", "Total ReadOperatorSpecificData calls");
    auto policy_operator_specific_data_patch_counter = meter->CreateUInt64Counter(
        "udr_policy_operator_specific_data_patch_total", "Total UpdateOperatorSpecificData calls");
    // ADR-0197: correcting ADR-0114's own real documentation error.
    auto policy_operator_specific_data_put_counter = meter->CreateUInt64Counter(
        "udr_policy_operator_specific_data_put_total", "Total ReplaceOperatorSpecificData calls");
    auto policy_operator_specific_data_delete_counter = meter->CreateUInt64Counter(
        "udr_policy_operator_specific_data_delete_total", "Total DeleteOperatorSpecificData calls");
    auto sponsor_connectivity_data_get_counter = meter->CreateUInt64Counter(
        "udr_sponsor_connectivity_data_get_total", "Total ReadSponsorConnectivityData calls");
    auto bdt_data_write_counter = meter->CreateUInt64Counter("udr_bdt_data_write_total",
                                                             "Total CreateIndividualBdtData calls");
    auto bdt_data_get_counter =
        meter->CreateUInt64Counter("udr_bdt_data_get_total", "Total ReadIndividualBdtData calls");
    auto bdt_data_patch_counter = meter->CreateUInt64Counter("udr_bdt_data_patch_total",
                                                             "Total UpdateIndividualBdtData calls");
    auto bdt_data_delete_counter = meter->CreateUInt64Counter(
        "udr_bdt_data_delete_total", "Total DeleteIndividualBdtData calls");
    auto bdt_data_list_counter =
        meter->CreateUInt64Counter("udr_bdt_data_list_total", "Total ReadBdtData calls");
    auto usage_mon_data_put_counter = meter->CreateUInt64Counter(
        "udr_usage_mon_data_put_total", "Total CreateUsageMonitoringResource calls");
    auto usage_mon_data_get_counter = meter->CreateUInt64Counter(
        "udr_usage_mon_data_get_total", "Total ReadUsageMonitoringInformation calls");
    auto usage_mon_data_delete_counter = meter->CreateUInt64Counter(
        "udr_usage_mon_data_delete_total", "Total DeleteUsageMonitoringInformation calls");
    auto policy_data_read_counter =
        meter->CreateUInt64Counter("udr_policy_data_read_total", "Total ReadPolicyData calls");
    auto policy_data_subs_to_notify_list_counter = meter->CreateUInt64Counter(
        "udr_policy_data_subs_to_notify_list_total", "Total ReadPolicyDataSubscriptions calls");
    auto policy_data_subs_to_notify_create_counter =
        meter->CreateUInt64Counter("udr_policy_data_subs_to_notify_create_total",
                                   "Total CreateIndividualPolicyDataSubscription calls");
    auto policy_data_subs_to_notify_get_counter =
        meter->CreateUInt64Counter("udr_policy_data_subs_to_notify_get_total",
                                   "Total ReadIndividualPolicySubscriptionData calls");
    auto policy_data_subs_to_notify_replace_counter =
        meter->CreateUInt64Counter("udr_policy_data_subs_to_notify_replace_total",
                                   "Total ReplaceIndividualPolicyDataSubscription calls");
    auto policy_data_subs_to_notify_delete_counter =
        meter->CreateUInt64Counter("udr_policy_data_subs_to_notify_delete_total",
                                   "Total DeleteIndividualPolicyDataSubscription calls");
    auto plmn_ue_policy_set_get_counter = meter->CreateUInt64Counter(
        "udr_plmn_ue_policy_set_get_total", "Total ReadPlmnUePolicySet calls");
    auto slice_control_data_get_counter = meter->CreateUInt64Counter(
        "udr_slice_control_data_get_total", "Total ReadSlicePolicyControlData calls");
    auto slice_control_data_patch_counter = meter->CreateUInt64Counter(
        "udr_slice_control_data_patch_total", "Total UpdateSlicePolicyControlData calls");
    auto group_control_data_get_counter = meter->CreateUInt64Counter(
        "udr_group_control_data_get_total", "Total ReadGroupPolCtrlData calls");
    auto group_control_data_patch_counter = meter->CreateUInt64Counter(
        "udr_group_control_data_patch_total", "Total ModifyGroupPolCtrlData calls");
    auto routing_ids_get_counter =
        meter->CreateUInt64Counter("udr_routing_ids_get_total", "Total GetRoutingIDs calls");
    auto nf_group_ids_get_counter =
        meter->CreateUInt64Counter("udr_nf_group_ids_get_total", "Total GetNfGroupIDs calls");
    auto nidd_authorization_data_get_counter = meter->CreateUInt64Counter(
        "udr_nidd_authorization_data_get_total", "Total GetNiddAuData calls");
    auto nidd_authorization_write_counter = meter->CreateUInt64Counter(
        "udr_nidd_authorization_write_total",
        "Total CreateNIDDAuthorizationInfo/ModifyNiddAuthorizationInfo calls");
    auto nidd_authorization_delete_counter = meter->CreateUInt64Counter(
        "udr_nidd_authorization_delete_total", "Total RemoveNiddAuthorizationInfo calls");
    auto identity_data_get_counter =
        meter->CreateUInt64Counter("udr_identity_data_get_total", "Total GetIdentityData calls");
    auto identity_data_patch_counter = meter->CreateUInt64Counter("udr_identity_data_patch_total",
                                                                  "Total ModifyIdentityData calls");
    auto odb_data_get_counter =
        meter->CreateUInt64Counter("udr_odb_data_get_total", "Total GetOdbData calls");
    auto v2x_data_get_counter =
        meter->CreateUInt64Counter("udr_v2x_data_get_total", "Total QueryV2xData calls");
    auto prose_data_get_counter =
        meter->CreateUInt64Counter("udr_prose_data_get_total", "Total QueryPorseData calls");
    auto uc_data_get_counter =
        meter->CreateUInt64Counter("udr_uc_data_get_total", "Total QueryUserConsentData calls");
    auto time_sync_data_get_counter = meter->CreateUInt64Counter(
        "udr_time_sync_data_get_total", "Total QueryTimeSyncSubscriptionData calls");
    auto location_data_get_counter =
        meter->CreateUInt64Counter("udr_location_data_get_total", "Total QueryUeLocation calls");
    auto a2x_data_get_counter =
        meter->CreateUInt64Counter("udr_a2x_data_get_total", "Total QueryA2xData calls");
    auto rangingsl_privacy_data_get_counter = meter->CreateUInt64Counter(
        "udr_rangingsl_privacy_data_get_total", "Total QueryRangingSlPrivacyData calls");
    auto ranging_slpos_data_get_counter = meter->CreateUInt64Counter(
        "udr_ranging_slpos_data_get_total", "Total QueryRangingSlPosData calls");
    auto mbs_data_get_counter =
        meter->CreateUInt64Counter("udr_5mbs_data_get_total", "Total Query5mbsData calls");
    auto service_specific_auth_write_counter = meter->CreateUInt64Counter(
        "udr_service_specific_auth_write_total",
        "Total CreateServiceSpecificAuthorizationInfo/ModifyServiceSpecificAuthorizationInfo "
        "calls");
    auto service_specific_auth_delete_counter =
        meter->CreateUInt64Counter("udr_service_specific_auth_delete_total",
                                   "Total RemoveServiceSpecificAuthorizationInfo calls");
    auto group_identifiers_get_counter = meter->CreateUInt64Counter(
        "udr_group_identifiers_get_total", "Total GetGroupIdentifiers calls");
    auto nssai_ack_data_write_counter = meter->CreateUInt64Counter(
        "udr_nssai_ack_data_write_total", "Total CreateOrUpdateNssaiAck calls");
    auto nssai_ack_data_get_counter =
        meter->CreateUInt64Counter("udr_nssai_ack_data_get_total", "Total QueryNssaiAck calls");
    auto cag_ack_data_write_counter = meter->CreateUInt64Counter("udr_cag_ack_data_write_total",
                                                                 "Total CreateCagUpdateAck calls");
    auto cag_ack_data_get_counter =
        meter->CreateUInt64Counter("udr_cag_ack_data_get_total", "Total QueryCagAck calls");
    auto sor_data_write_counter = meter->CreateUInt64Counter(
        "udr_sor_data_write_total", "Total CreateAuthenticationSoR/UpdateAuthenticationSoR calls");
    auto sor_data_get_counter =
        meter->CreateUInt64Counter("udr_sor_data_get_total", "Total QueryAuthSoR calls");
    auto upu_data_write_counter = meter->CreateUInt64Counter("udr_upu_data_write_total",
                                                             "Total CreateAuthenticationUPU calls");
    auto upu_data_get_counter =
        meter->CreateUInt64Counter("udr_upu_data_get_total", "Total QueryAuthUPU calls");
    auto ue_upd_conf_data_get_counter =
        meter->CreateUInt64Counter("udr_ue_upd_conf_data_get_total", "Total QueryUeUpdConf calls");
    auto context_data_get_counter =
        meter->CreateUInt64Counter("udr_context_data_get_total", "Total QueryContextData calls");
    auto ue_subscribed_data_get_counter = meter->CreateUInt64Counter(
        "udr_ue_subscribed_data_get_total", "Total QueryUeSubscribedData calls");
    auto ee_subscriptions_create_counter = meter->CreateUInt64Counter(
        "udr_ee_subscriptions_create_total", "Total CreateEeSubscriptions calls");
    auto ee_subscriptions_list_counter = meter->CreateUInt64Counter(
        "udr_ee_subscriptions_list_total", "Total Queryeesubscriptions calls");
    auto ee_subscriptions_get_counter = meter->CreateUInt64Counter(
        "udr_ee_subscriptions_get_total", "Total QueryeeSubscription calls");
    auto ee_subscriptions_write_counter =
        meter->CreateUInt64Counter("udr_ee_subscriptions_write_total",
                                   "Total UpdateEesubscriptions/ModifyEesubscription calls");
    auto ee_subscriptions_delete_counter = meter->CreateUInt64Counter(
        "udr_ee_subscriptions_delete_total", "Total RemoveeeSubscriptions calls");
    auto subs_to_notify_create_counter = meter->CreateUInt64Counter(
        "udr_subs_to_notify_create_total", "Total SubscriptionDataSubscriptions calls");
    auto subs_to_notify_list_counter = meter->CreateUInt64Counter("udr_subs_to_notify_list_total",
                                                                  "Total QuerySubsToNotify calls");
    auto subs_to_notify_get_counter = meter->CreateUInt64Counter(
        "udr_subs_to_notify_get_total", "Total QuerySubscriptionDataSubscriptions calls");
    auto subs_to_notify_write_counter = meter->CreateUInt64Counter(
        "udr_subs_to_notify_write_total", "Total ModifysubscriptionDataSubscription calls");
    auto subs_to_notify_delete_counter = meter->CreateUInt64Counter(
        "udr_subs_to_notify_delete_total", "Total RemovesubscriptionDataSubscriptions calls");
    auto sdm_subscriptions_create_counter = meter->CreateUInt64Counter(
        "udr_sdm_subscriptions_create_total", "Total CreateSdmSubscriptions calls");
    auto sdm_subscriptions_list_counter = meter->CreateUInt64Counter(
        "udr_sdm_subscriptions_list_total", "Total Querysdmsubscriptions calls");
    auto sdm_subscriptions_get_counter = meter->CreateUInt64Counter(
        "udr_sdm_subscriptions_get_total", "Total QuerysdmSubscription calls");
    auto sdm_subscriptions_write_counter =
        meter->CreateUInt64Counter("udr_sdm_subscriptions_write_total",
                                   "Total Updatesdmsubscriptions/ModifysdmSubscription calls");
    auto sdm_subscriptions_delete_counter = meter->CreateUInt64Counter(
        "udr_sdm_subscriptions_delete_total", "Total RemovesdmSubscriptions calls");
    auto ee_amf_subscription_info_write_counter = meter->CreateUInt64Counter(
        "udr_ee_amf_subscription_info_write_total",
        "Total Create AMF Subscriptions/ModifyAmfSubscriptionInfo calls");
    auto ee_amf_subscription_info_get_counter = meter->CreateUInt64Counter(
        "udr_ee_amf_subscription_info_get_total", "Total GetAmfSubscriptionInfo calls");
    auto ee_amf_subscription_info_delete_counter = meter->CreateUInt64Counter(
        "udr_ee_amf_subscription_info_delete_total", "Total RemoveAmfSubscriptionsInfo calls");
    auto ee_smf_subscription_info_write_counter = meter->CreateUInt64Counter(
        "udr_ee_smf_subscription_info_write_total",
        "Total Create SMF Subscriptions/ModifySmfSubscriptionInfo calls");
    auto ee_smf_subscription_info_get_counter = meter->CreateUInt64Counter(
        "udr_ee_smf_subscription_info_get_total", "Total GetSmfSubscriptionInfo calls");
    auto ee_smf_subscription_info_delete_counter = meter->CreateUInt64Counter(
        "udr_ee_smf_subscription_info_delete_total", "Total RemoveSmfSubscriptionsInfo calls");
    auto ee_hss_subscription_info_write_counter = meter->CreateUInt64Counter(
        "udr_ee_hss_subscription_info_write_total",
        "Total Create HSS Subscriptions/ModifyHssSubscriptionInfo calls");
    auto ee_hss_subscription_info_get_counter = meter->CreateUInt64Counter(
        "udr_ee_hss_subscription_info_get_total", "Total GetHssSubscriptionInfo calls");
    auto ee_hss_subscription_info_delete_counter = meter->CreateUInt64Counter(
        "udr_ee_hss_subscription_info_delete_total", "Total RemoveHssSubscriptionsInfo calls");
    auto sdm_hss_subscription_info_write_counter = meter->CreateUInt64Counter(
        "udr_sdm_hss_subscription_info_write_total",
        "Total Create HSS SDM Subscriptions/ModifyHssSDMSubscriptionInfo calls");
    auto sdm_hss_subscription_info_get_counter = meter->CreateUInt64Counter(
        "udr_sdm_hss_subscription_info_get_total", "Total GetHssSDMSubscriptionInfo calls");
    auto sdm_hss_subscription_info_delete_counter = meter->CreateUInt64Counter(
        "udr_sdm_hss_subscription_info_delete_total", "Total RemoveHssSDMSubscriptionsInfo calls");
    auto group_ee_subscriptions_create_counter = meter->CreateUInt64Counter(
        "udr_group_ee_subscriptions_create_total", "Total CreateEeGroupSubscriptions calls");
    auto group_ee_subscriptions_list_counter = meter->CreateUInt64Counter(
        "udr_group_ee_subscriptions_list_total", "Total QueryEeGroupSubscriptions calls");
    auto group_ee_subscriptions_get_counter = meter->CreateUInt64Counter(
        "udr_group_ee_subscriptions_get_total", "Total QueryEeGroupSubscription calls");
    auto group_ee_subscriptions_write_counter = meter->CreateUInt64Counter(
        "udr_group_ee_subscriptions_write_total",
        "Total UpdateEeGroupSubscriptions/ModifyEeGroupSubscription calls");
    auto group_ee_subscriptions_delete_counter = meter->CreateUInt64Counter(
        "udr_group_ee_subscriptions_delete_total", "Total RemoveEeGroupSubscriptions calls");
    auto group_amf_subscription_info_write_counter = meter->CreateUInt64Counter(
        "udr_group_amf_subscription_info_write_total",
        "Total CreateAmfGroupSubscriptions/ModifyAmfGroupSubscriptions calls");
    auto group_amf_subscription_info_get_counter = meter->CreateUInt64Counter(
        "udr_group_amf_subscription_info_get_total", "Total GetAmfGroupSubscriptions calls");
    auto group_amf_subscription_info_delete_counter = meter->CreateUInt64Counter(
        "udr_group_amf_subscription_info_delete_total", "Total RemoveAmfGroupSubscriptions calls");
    auto group_smf_subscription_info_write_counter = meter->CreateUInt64Counter(
        "udr_group_smf_subscription_info_write_total",
        "Total CreateSmfGroupSubscriptions/ModifySmfGroupSubscriptions calls");
    auto group_smf_subscription_info_get_counter = meter->CreateUInt64Counter(
        "udr_group_smf_subscription_info_get_total", "Total GetSmfGroupSubscriptions calls");
    auto group_smf_subscription_info_delete_counter = meter->CreateUInt64Counter(
        "udr_group_smf_subscription_info_delete_total", "Total RemoveSmfGroupSubscriptions calls");
    auto group_hss_subscription_info_write_counter = meter->CreateUInt64Counter(
        "udr_group_hss_subscription_info_write_total",
        "Total CreateHssGroupSubscriptions/ModifyHssGroupSubscriptions calls");
    auto group_hss_subscription_info_get_counter = meter->CreateUInt64Counter(
        "udr_group_hss_subscription_info_get_total", "Total GetHssGroupSubscriptions calls");
    auto group_hss_subscription_info_delete_counter = meter->CreateUInt64Counter(
        "udr_group_hss_subscription_info_delete_total", "Total RemoveHssGroupSubscriptions calls");
    auto pdtq_data_list_counter =
        meter->CreateUInt64Counter("udr_pdtq_data_list_total", "Total ReadPdtqData calls");
    auto pdtq_data_get_counter =
        meter->CreateUInt64Counter("udr_pdtq_data_get_total", "Total ReadIndividualPdtqData calls");
    auto pdtq_data_write_counter =
        meter->CreateUInt64Counter("udr_pdtq_data_write_total",
                                   "Total CreateIndividualPdtqData/UpdateIndividualPdtqData calls");
    auto pdtq_data_delete_counter = meter->CreateUInt64Counter(
        "udr_pdtq_data_delete_total", "Total DeleteIndividualPdtqData calls");
    auto five_g_vn_groups_write_counter = meter->CreateUInt64Counter(
        "udr_5g_vn_groups_write_total", "Total Create5GVnGroup/Modify5GVnGroup calls");
    auto five_g_vn_groups_get_counter = meter->CreateUInt64Counter(
        "udr_5g_vn_groups_get_total", "Total Get5GVnGroupConfiguration calls");
    auto five_g_vn_groups_delete_counter =
        meter->CreateUInt64Counter("udr_5g_vn_groups_delete_total", "Total Delete5GVnGroup calls");
    auto five_g_vn_groups_list_counter =
        meter->CreateUInt64Counter("udr_5g_vn_groups_list_total", "Total Query5GVnGroup calls");
    auto five_g_vn_groups_internal_get_counter = meter->CreateUInt64Counter(
        "udr_5g_vn_groups_internal_get_total", "Total Query5GVnGroupInternal calls");
    auto five_g_vn_group_pp_profile_data_get_counter = meter->CreateUInt64Counter(
        "udr_5g_vn_group_pp_profile_data_get_total", "Total Query5GVnGroupPPData calls");
    auto mbs_group_membership_write_counter = meter->CreateUInt64Counter(
        "udr_mbs_group_membership_write_total", "Total Create5GmbsGroup/Modify5GmbsGroup calls");
    auto mbs_group_membership_get_counter = meter->CreateUInt64Counter(
        "udr_mbs_group_membership_get_total", "Total GetMulticastMbsGroupMemb calls");
    auto mbs_group_membership_delete_counter = meter->CreateUInt64Counter(
        "udr_mbs_group_membership_delete_total", "Total Delete5GmbsGroup calls");
    auto mbs_group_membership_list_counter = meter->CreateUInt64Counter(
        "udr_mbs_group_membership_list_total", "Total Query5GmbsGroup calls");
    auto mbs_group_membership_internal_get_counter = meter->CreateUInt64Counter(
        "udr_mbs_group_membership_internal_get_total", "Total Query5GMbsGroupInternal calls");
    auto mbs_group_pp_profile_data_get_counter = meter->CreateUInt64Counter(
        "udr_mbs_group_pp_profile_data_get_total", "Total Query5GMbsGroupPPData calls");
    auto nf_group_id_subscriptions_create_counter = meter->CreateUInt64Counter(
        "udr_nf_group_id_subscriptions_create_total", "Total CreateGroupIdSubscription calls");
    auto nf_group_id_subscriptions_get_counter = meter->CreateUInt64Counter(
        "udr_nf_group_id_subscriptions_get_total", "Total QueryGroupIdSubscription calls");
    auto nf_group_id_subscriptions_write_counter = meter->CreateUInt64Counter(
        "udr_nf_group_id_subscriptions_write_total", "Total ModifyGroupIdSubscription calls");
    auto nf_group_id_subscriptions_delete_counter = meter->CreateUInt64Counter(
        "udr_nf_group_id_subscriptions_delete_total", "Total RemoveGroupIdSubscription calls");
    auto group_ee_profile_data_get_counter = meter->CreateUInt64Counter(
        "udr_group_ee_profile_data_get_total", "Total QueryGroupEEData calls");
    auto mbs_session_pol_data_get_counter = meter->CreateUInt64Counter(
        "udr_mbs_session_pol_data_get_total", "Total GetMBSSessPolCtrlData calls");

    boost::asio::io_context ioc;
    // 0.0.0.0: same Docker-reachability reasoning as NRF's bind -- see docs/DECISIONS.md ADR-0014.
    sbi_core::http2::Server server(ioc, "0.0.0.0", port, server_tls);

    // P15 / P4.12 (ADR-0280): optional TPS ceiling from this NF's own config (`max_tps`,
    // `tps_burst`), overridable per deployment via SBI_MAX_TPS. Absent means unlimited, so this
    // changes nothing until an operator opts in.
    if (const auto tps_limit = sbi_core::read_tps_limit(config); tps_limit.enabled()) {
        server.set_tps_limit(tps_limit.sustained_tps, tps_limit.burst);
        spdlog::info("TPS ceiling active: {} req/s sustained, burst {}",
                     tps_limit.sustained_tps,
                     tps_limit.burst > 0.0 ? tps_limit.burst : tps_limit.sustained_tps);
    }

    // Real onDataChange webhook delivery (ADR-0171, gap-closure task #106) -- shared client for
    // notify_subscribers(), constructed once (internally mutex-guarded, safe to call from every
    // concurrent write-route handler below).
    sbi_core::http2::TlsConfig notify_client_tls{
        .cert_path = CERTS_DIR "/udr/cert.pem",
        .key_path = CERTS_DIR "/udr/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client notify_client(std::move(notify_client_tls));

    // Real onGroupIdMapChange delivery (ADR-0180, gap-closure task #106) for the seed data above
    // (nf_group_ids.seed(...), the only real mutation this project's own NfGroupIdStore ever
    // undergoes -- see notify_group_id_map_change()'s own comment for the full disclosure of why
    // this is fired here rather than from a live write route). No subscription can genuinely exist
    // yet this early in a fresh process's own lifetime, so in practice this fires zero real
    // deliveries on every real run -- disclosed, not hidden; still the one honest trigger point.
    notify_group_id_map_change(
        nf_group_id_subscriptions, notify_client, "imsi-999700000000001", "AMF", "amf-group-01");
    notify_group_id_map_change(
        nf_group_id_subscriptions, notify_client, "imsi-999700000000001", "SMF", "smf-group-01");
    notify_group_id_map_change(
        nf_group_id_subscriptions, notify_client, "imsi-999700000000002", "AMF", "amf-group-02");

    const std::string amf_ctx_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/context-data/amf-3gpp-access";

    server.add_route(
        "GET",
        amf_ctx_path_pattern,
        [&verifier, &amf_contexts](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto context = amf_contexts.get(ue_id);
            if (!context.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No AMF context for ueId " + ue_id);
            }
            return sbi_core::http2::Response::json(200, context->dump());
        });

    server.add_route(
        "PUT",
        amf_ctx_path_pattern,
        [&verifier,
         &amf_contexts,
         &amf_ctx_write_counter,
         &subs_to_notify,
         &notify_client,
         amf_ctx_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::Amf3GppAccessRegistration>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            json j = *body;
            const bool is_new = amf_contexts.put(ue_id, j);
            amf_ctx_write_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(amf_ctx_path_pattern, req.path_params),
                               change_replace(j));

            if (!is_new) {
                sbi_core::http2::Response resp;
                resp.status = 204;
                return resp;
            }
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 resolved_location(amf_ctx_path_pattern, req.path_params));
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "PATCH",
        amf_ctx_path_pattern,
        [&verifier,
         &amf_contexts,
         &amf_ctx_write_counter,
         &subs_to_notify,
         &notify_client,
         amf_ctx_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json patch_ops;
            try {
                patch_ops = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto ue_id = req.path_params.at("ueId");
            std::optional<json> patched;
            try {
                patched = amf_contexts.apply_patch(ue_id, patch_ops);
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(400, "Invalid JSON Patch", e.what());
            }
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No AMF context for ueId " + ue_id);
            }
            amf_ctx_write_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(amf_ctx_path_pattern, req.path_params),
                               change_from_json_patch(patch_ops));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #106, ADR-0093): real
    // QueryAmfContextNon3gpp/CreateAmfContextNon3gpp -- GET+PUT, mirrors the 3GPP-access group
    // above exactly (same real spec shape: no PATCH/DELETE for this resource either), backed by
    // its own distinct table/store (a real, separate resource, not a rename of the 3GPP one).
    const std::string amf_non3gpp_ctx_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/context-data/amf-non-3gpp-access";

    server.add_route(
        "GET",
        amf_non3gpp_ctx_path_pattern,
        [&verifier, &amf_non3gpp_contexts](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto context = amf_non3gpp_contexts.get(ue_id);
            if (!context.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No AMF non-3GPP-access context for ueId " + ue_id);
            }
            return sbi_core::http2::Response::json(200, context->dump());
        });

    server.add_route(
        "PUT",
        amf_non3gpp_ctx_path_pattern,
        [&verifier,
         &amf_non3gpp_contexts,
         &amf_non3gpp_ctx_write_counter,
         &subs_to_notify,
         &notify_client,
         amf_non3gpp_ctx_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::AmfNon3GppAccessRegistration>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            json j = *body;
            const bool is_new = amf_non3gpp_contexts.put(ue_id, j);
            amf_non3gpp_ctx_write_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(amf_non3gpp_ctx_path_pattern, req.path_params),
                               change_replace(j));

            if (!is_new) {
                sbi_core::http2::Response resp;
                resp.status = 204;
                return resp;
            }
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 resolved_location(amf_non3gpp_ctx_path_pattern, req.path_params));
            resp.body = j.dump();
            return resp;
        });

    const std::string smf_reg_list_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/context-data/smf-registrations";
    const std::string smf_reg_path_pattern = smf_reg_list_path_pattern + "/{pduSessionId}";

    server.add_route(
        "GET",
        smf_reg_list_path_pattern,
        [&verifier, &smf_registrations](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            sbi_gen::SmfRegList list;
            for (const auto& registration : smf_registrations.list_for_ue(ue_id)) {
                list.push_back(registration.get<sbi_gen::SmfRegistration>());
            }
            json j = list;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "GET",
        smf_reg_path_pattern,
        [&verifier, &smf_registrations](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto pdu_session_id = req.path_params.at("pduSessionId");
            auto registration = smf_registrations.get(ue_id, pdu_session_id);
            if (!registration.has_value()) {
                return sbi_core::http2::problem_response(
                    404,
                    "Not Found",
                    "No SMF registration for ueId/pduSessionId " + ue_id + "/" + pdu_session_id);
            }
            return sbi_core::http2::Response::json(200, registration->dump());
        });

    server.add_route(
        "PUT",
        smf_reg_path_pattern,
        [&verifier,
         &smf_registrations,
         &smf_reg_write_counter,
         &subs_to_notify,
         &notify_client,
         smf_reg_list_path_pattern,
         smf_reg_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::SmfRegistration>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto pdu_session_id = req.path_params.at("pduSessionId");
            json j = *body;
            const bool is_new = smf_registrations.put(ue_id, pdu_session_id, j);
            smf_reg_write_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(smf_reg_path_pattern, req.path_params),
                               change_replace(j));

            if (!is_new) {
                sbi_core::http2::Response resp;
                resp.status = 204;
                return resp;
            }
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 resolved_location(smf_reg_list_path_pattern, req.path_params) +
                                     "/" + pdu_session_id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "PATCH",
        smf_reg_path_pattern,
        [&verifier,
         &smf_registrations,
         &smf_reg_write_counter,
         &subs_to_notify,
         &notify_client,
         smf_reg_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json patch_ops;
            try {
                patch_ops = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto pdu_session_id = req.path_params.at("pduSessionId");
            std::optional<json> patched;
            try {
                patched = smf_registrations.apply_patch(ue_id, pdu_session_id, patch_ops);
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(400, "Invalid JSON Patch", e.what());
            }
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404,
                    "Not Found",
                    "No SMF registration for ueId/pduSessionId " + ue_id + "/" + pdu_session_id);
            }
            smf_reg_write_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(smf_reg_path_pattern, req.path_params),
                               change_from_json_patch(patch_ops));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "DELETE",
        smf_reg_path_pattern,
        [&verifier,
         &smf_registrations,
         &smf_reg_delete_counter,
         &subs_to_notify,
         &notify_client,
         smf_reg_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto pdu_session_id = req.path_params.at("pduSessionId");
            if (!smf_registrations.get(ue_id, pdu_session_id).has_value()) {
                return sbi_core::http2::problem_response(
                    404,
                    "Not Found",
                    "No SMF registration for ueId/pduSessionId " + ue_id + "/" + pdu_session_id);
            }
            smf_registrations.remove(ue_id, pdu_session_id);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(smf_reg_path_pattern, req.path_params),
                               change_remove());
            smf_reg_delete_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nudr_DataRepository: provisioned-data group (ADR-0069, gap-closure Tier 1b) -- real,
    // GET-only per spec (see this file's own header), keyed by (ueId, servingPlmnId) per the real
    // path shape TS29505_Subscription_Data.yaml defines. ---

    const std::string provisioned_data_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/{servingPlmnId}/provisioned-data";

    server.add_route(
        "GET",
        provisioned_data_path_pattern + "/am-data",
        [&verifier, &provisioned_data, &provisioned_data_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto serving_plmn_id = req.path_params.at("servingPlmnId");
            auto data = provisioned_data.get_am_data(ue_id, serving_plmn_id);
            provisioned_data_get_counter->Add(1);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No provisioned am-data for ueId " + ue_id);
            }
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "GET",
        provisioned_data_path_pattern + "/smf-selection-subscription-data",
        [&verifier, &provisioned_data, &provisioned_data_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto serving_plmn_id = req.path_params.at("servingPlmnId");
            auto data = provisioned_data.get_smf_sel_data(ue_id, serving_plmn_id);
            provisioned_data_get_counter->Add(1);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404,
                    "Not Found",
                    "No provisioned smf-selection-subscription-data for ueId " + ue_id);
            }
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "GET",
        provisioned_data_path_pattern + "/sm-data",
        [&verifier, &provisioned_data, &provisioned_data_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto serving_plmn_id = req.path_params.at("servingPlmnId");
            auto data = provisioned_data.get_sm_data(ue_id, serving_plmn_id);
            provisioned_data_get_counter->Add(1);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No provisioned sm-data for ueId " + ue_id);
            }
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // ADR-0106, gap-closure task #106: real LCS Broadcast Assistance Subscription Data
    // (QueryLcsBcaData), same real GET-only (ueId, servingPlmnId) shape as the three routes above.
    server.add_route(
        "GET",
        provisioned_data_path_pattern + "/lcs-bca-data",
        [&verifier, &provisioned_data, &provisioned_data_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto serving_plmn_id = req.path_params.at("servingPlmnId");
            auto data = provisioned_data.get_lcs_bca_data(ue_id, serving_plmn_id);
            provisioned_data_get_counter->Add(1);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No provisioned lcs-bca-data for ueId " + ue_id);
            }
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // ADR-0125, gap-closure task #106: real SMS Management Subscription Data
    // (QuerySmsMngData), same real GET-only (ueId, servingPlmnId) shape as the routes above.
    server.add_route(
        "GET",
        provisioned_data_path_pattern + "/sms-mng-data",
        [&verifier, &provisioned_data, &provisioned_data_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto serving_plmn_id = req.path_params.at("servingPlmnId");
            auto data = provisioned_data.get_sms_mng_data(ue_id, serving_plmn_id);
            provisioned_data_get_counter->Add(1);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No provisioned sms-mng-data for ueId " + ue_id);
            }
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // ADR-0126, gap-closure task #106: real SMS Subscription Data (QuerySmsData), same real
    // GET-only (ueId, servingPlmnId) shape as the routes above -- genuinely distinct from
    // sms-mng-data above, not a duplicate.
    server.add_route(
        "GET",
        provisioned_data_path_pattern + "/sms-data",
        [&verifier, &provisioned_data, &provisioned_data_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto serving_plmn_id = req.path_params.at("servingPlmnId");
            auto data = provisioned_data.get_sms_data(ue_id, serving_plmn_id);
            provisioned_data_get_counter->Add(1);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No provisioned sms-data for ueId " + ue_id);
            }
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // ADR-0127, gap-closure task #106: real Trace Data (QueryTraceData), same real GET-only
    // (ueId, servingPlmnId) shape as the routes above. Real response schema is a `oneOf` (full
    // `TraceData` object or a bare `SharedDataId` string) -- returned as opaque JSON.
    server.add_route(
        "GET",
        provisioned_data_path_pattern + "/trace-data",
        [&verifier, &provisioned_data, &provisioned_data_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto serving_plmn_id = req.path_params.at("servingPlmnId");
            auto data = provisioned_data.get_trace_data(ue_id, serving_plmn_id);
            provisioned_data_get_counter->Add(1);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No provisioned trace-data for ueId " + ue_id);
            }
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // --- Nudr_DataRepository: Provisioned Data (Document), aggregate resource (ADR-0212,
    // gap-closure task #106) -- real GET-only per TS29505_Subscription_Data.yaml,
    // `QueryProvisionedData`. Real, disclosed: the response is exactly the real
    // `ProvisionedDataSets` schema's own 21 fields, backed by the SAME already-independently-stored
    // sub-resources `QueryUeSubscribedData` (ADR-0166) already composes for its own
    // `ProvisionedDataSets`-typed subset -- confirmed field-for-field against that schema's own
    // real definition, no 22nd field exists. Genuine improvement over that bare aggregate: here
    // `servingPlmnId` is a REQUIRED path parameter (not an optional query param), so all 7
    // PLMN-keyed fields
    // (`amData`/`smfSelData`/`smsSubsData`/`smData`/`traceData`/`smsMngData`/`lcsBcaData`) are
    // ALWAYS composed, never skipped. `niddAuthData` (`AuthorizationData`) is still never composed
    // here, same real disclosed gap as ADR-0166: its own composite key requires
    // `mtc-provider-information`, a query param this resource does not expose. The real, optional
    // `adjacent-plmns`/`single-nssai`/`dnn`/`ext-group-ids`/`uc-purpose` query params are accepted
    // but not honored, matching the established precedent -- no existing store supports filtering
    // by them. The real, optional `3gpp-Sbi-Etags` response header (a comma-separated
    // ProvisionedDatasetName=Etag list) is NOT populated: this store has no per-field
    // ETag/versioning layer -- a genuine, disclosed gap, not a fabricated header value. Same real
    // `200`-always design as the other aggregates: a live view, so an empty result is `200 {}`, not
    // `404`. ---

    const std::string provisioned_data_sets_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/{servingPlmnId}/provisioned-data";

    server.add_route(
        "GET",
        provisioned_data_sets_path_pattern,
        [&verifier,
         &provisioned_data,
         &lcs_privacy_data,
         &lcs_mo_data,
         &lcs_subscription_data,
         &v2x_data,
         &prose_data,
         &odb_data,
         &ee_profile_data,
         &pp_profile_data,
         &uc_data,
         &mbs_data,
         &pp_data,
         &a2x_data,
         &rangingsl_privacy_data,
         &provisioned_data_sets_get_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto serving_plmn_id = req.path_params.at("servingPlmnId");
            std::vector<std::string> names;
            if (const auto names_it = req.query_params.find("dataset-names");
                names_it != req.query_params.end()) {
                names = sbi_core::http2::split_form_array(names_it->second);
            }
            const auto wanted = [&names](const char* name) {
                return names.empty() || std::find(names.begin(), names.end(), name) != names.end();
            };

            json result = json::object();

            if (wanted("AM")) {
                if (auto v = provisioned_data.get_am_data(ue_id, serving_plmn_id); v.has_value()) {
                    result["amData"] = *v;
                }
            }
            if (wanted("SMF_SEL")) {
                if (auto v = provisioned_data.get_smf_sel_data(ue_id, serving_plmn_id);
                    v.has_value()) {
                    result["smfSelData"] = *v;
                }
            }
            if (wanted("SMS_SUB")) {
                if (auto v = provisioned_data.get_sms_data(ue_id, serving_plmn_id); v.has_value()) {
                    result["smsSubsData"] = *v;
                }
            }
            if (wanted("SM")) {
                if (auto v = provisioned_data.get_sm_data(ue_id, serving_plmn_id); v.has_value()) {
                    result["smData"] = *v;
                }
            }
            if (wanted("TRACE")) {
                if (auto v = provisioned_data.get_trace_data(ue_id, serving_plmn_id);
                    v.has_value()) {
                    result["traceData"] = *v;
                }
            }
            if (wanted("SMS_MNG")) {
                if (auto v = provisioned_data.get_sms_mng_data(ue_id, serving_plmn_id);
                    v.has_value()) {
                    result["smsMngData"] = *v;
                }
            }
            if (wanted("LCS_BCA")) {
                if (auto v = provisioned_data.get_lcs_bca_data(ue_id, serving_plmn_id);
                    v.has_value()) {
                    result["lcsBcaData"] = *v;
                }
            }
            if (wanted("LCS_PRIVACY")) {
                if (auto v = lcs_privacy_data.get(ue_id); v.has_value()) {
                    result["lcsPrivacyData"] = *v;
                }
            }
            if (wanted("LCS_MO")) {
                if (auto v = lcs_mo_data.get(ue_id); v.has_value()) {
                    result["lcsMoData"] = *v;
                }
            }
            if (wanted("LCS_SUB")) {
                if (auto v = lcs_subscription_data.get(ue_id); v.has_value()) {
                    result["lcsSubscriptionData"] = *v;
                }
            }
            if (wanted("V2X")) {
                if (auto v = v2x_data.get(ue_id); v.has_value()) {
                    result["v2xData"] = *v;
                }
            }
            if (wanted("PROSE")) {
                if (auto v = prose_data.get(ue_id); v.has_value()) {
                    result["proseData"] = *v;
                }
            }
            if (wanted("ODB")) {
                if (auto v = odb_data.get(ue_id); v.has_value()) {
                    result["odbData"] = *v;
                }
            }
            if (wanted("EE_PROF")) {
                if (auto v = ee_profile_data.get(ue_id); v.has_value()) {
                    result["eeProfileData"] = *v;
                }
            }
            if (wanted("PP_PROF")) {
                if (auto v = pp_profile_data.get(ue_id); v.has_value()) {
                    result["ppProfileData"] = *v;
                }
            }
            // NIDD_AUTH deliberately never composed here -- see this block's own header comment.
            if (wanted("USER_CONSENT")) {
                if (auto v = uc_data.get(ue_id); v.has_value()) {
                    result["ucData"] = *v;
                }
            }
            if (wanted("MBS")) {
                if (auto v = mbs_data.get(ue_id); v.has_value()) {
                    result["mbsSubscriptionData"] = *v;
                }
            }
            if (wanted("PP_DATA")) {
                if (auto v = pp_data.get(ue_id); v.has_value()) {
                    result["ppData"] = *v;
                }
            }
            if (wanted("A2X")) {
                if (auto v = a2x_data.get(ue_id); v.has_value()) {
                    result["a2xData"] = *v;
                }
            }
            if (wanted("RANGINGSL_PRIVACY")) {
                if (auto v = rangingsl_privacy_data.get(ue_id); v.has_value()) {
                    result["rangingSlPrivacyData"] = *v;
                }
            }

            provisioned_data_sets_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, result.dump());
        });

    // --- Nudr_DataRepository: policy-data group, SM policy resource (ADR-0072, gap-closure: real
    // N28 end-to-end) -- real GET+PATCH per TS29519_Policy_Data.yaml, keyed by ueId alone (no
    // servingPlmnId in the real path, unlike provisioned-data above -- genuinely different
    // resource, see schema.postgres.sql's own comment). ---

    const std::string sm_policy_data_path_pattern =
        std::string(kApiRoot) + "/policy-data/ues/{ueId}/sm-data";

    server.add_route(
        "GET",
        sm_policy_data_path_pattern,
        [&verifier, &sm_policy_data, &sm_policy_data_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = sm_policy_data.get(ue_id);
            sm_policy_data_get_counter->Add(1);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No SM policy data for ueId " + ue_id);
            }
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PATCH",
        sm_policy_data_path_pattern,
        [&verifier,
         &sm_policy_data,
         &sm_policy_data_patch_counter,
         &subs_to_notify,
         &notify_client,
         sm_policy_data_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json patch;
            try {
                patch = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto patched = sm_policy_data.merge_patch(ue_id, patch);
            sm_policy_data_patch_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(sm_policy_data_path_pattern, req.path_params),
                               change_replace(patched));
            // Real spec: 204 (no body) or 200 (with the updated SmPolicyData) are both valid --
            // this project returns 200 with the real updated document, same real information a
            // future GUI editing this resource would want back without a second GET round-trip.
            return sbi_core::http2::Response::json(200, patched.dump());
        });

    // --- Nudr_DataRepository: policy-data group, Usage Monitoring Information (Document)
    // resource (ADR-0213, gap-closure task #106) -- real PUT (create)+GET+DELETE per
    // TS29519_Policy_Data.yaml, keyed by (ueId, usageMonId). Real, disclosed: the spec's own GET
    // documents a real `204` ("found but no data") distinct from `404` -- this project's
    // single-row-per-key model has no exists-but-empty state, so an absent row is the real `404`,
    // not a fabricated `204` (see schema.postgres.sql's own comment). ---

    const std::string usage_mon_data_path_pattern =
        std::string(kApiRoot) + "/policy-data/ues/{ueId}/sm-data/{usageMonId}";

    server.add_route(
        "GET",
        usage_mon_data_path_pattern,
        [&verifier, &usage_mon_data, &usage_mon_data_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto usage_mon_id = req.path_params.at("usageMonId");
            auto data = usage_mon_data.get(ue_id, usage_mon_id);
            usage_mon_data_get_counter->Add(1);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No usage monitoring data for usageMonId " + usage_mon_id);
            }
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PUT",
        usage_mon_data_path_pattern,
        [&verifier, &usage_mon_data, &usage_mon_data_put_counter, usage_mon_data_path_pattern](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::UsageMonData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto usage_mon_id = req.path_params.at("usageMonId");
            json j = *body;
            usage_mon_data.put(ue_id, usage_mon_id, j);
            usage_mon_data_put_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 resolved_location(usage_mon_data_path_pattern, req.path_params));
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "DELETE",
        usage_mon_data_path_pattern,
        [&verifier, &usage_mon_data, &usage_mon_data_delete_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto usage_mon_id = req.path_params.at("usageMonId");
            if (!usage_mon_data.remove(ue_id, usage_mon_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No usage monitoring data for usageMonId " + usage_mon_id);
            }
            usage_mon_data_delete_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nudr_DataRepository: PolicyDataForIndividualUe (Document), bare aggregate resource
    // (ADR-0213, gap-closure task #106) -- real GET-only per TS29519_Policy_Data.yaml,
    // `ReadPolicyData`. Same real "live view over already-independently-stored sub-resources"
    // design as `QueryUeSubscribedData`/`QueryProvisionedData` (ADR-0166/ADR-0212): composes
    // `uePolicyDataSet` (`ue_policy_set`), `smPolicyDataSet` (`sm_policy_data`), `amPolicyDataSet`
    // (`am_policy_data`), `umData` (new `usage_mon_data`, keyed by `limitId` -- confirmed equal to
    // `usageMonId`), `operatorSpecificDataSet` (`policy_operator_specific_data`, already the real
    // map-shaped schema this field needs verbatim). Real, optional `data-subset-names` filter
    // honored (`PolicyDataSubset` enum: `AM_POLICY_DATA`/`SM_POLICY_DATA`/`UE_POLICY_DATA`/
    // `UM_DATA`/`OPERATOR_SPECIFIC_DATA`) via the same `wanted(name)` lambda pattern used
    // elsewhere; absent means every composable field is attempted. Real `200`-always design: an
    // empty result is `200 {}`, not `404`. ---

    const std::string policy_data_for_ue_path_pattern =
        std::string(kApiRoot) + "/policy-data/ues/{ueId}";

    server.add_route(
        "GET",
        policy_data_for_ue_path_pattern,
        [&verifier,
         &ue_policy_set,
         &sm_policy_data,
         &am_policy_data,
         &usage_mon_data,
         &policy_operator_specific_data,
         &policy_data_read_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            std::vector<std::string> names;
            if (const auto names_it = req.query_params.find("data-subset-names");
                names_it != req.query_params.end()) {
                names = sbi_core::http2::split_form_array(names_it->second);
            }
            const auto wanted = [&names](const char* name) {
                return names.empty() || std::find(names.begin(), names.end(), name) != names.end();
            };

            json result = json::object();
            if (wanted("UE_POLICY_DATA")) {
                if (auto v = ue_policy_set.get(ue_id); v.has_value()) {
                    result["uePolicyDataSet"] = *v;
                }
            }
            if (wanted("SM_POLICY_DATA")) {
                if (auto v = sm_policy_data.get(ue_id); v.has_value()) {
                    result["smPolicyDataSet"] = *v;
                }
            }
            if (wanted("AM_POLICY_DATA")) {
                if (auto v = am_policy_data.get(ue_id); v.has_value()) {
                    result["amPolicyDataSet"] = *v;
                }
            }
            if (wanted("UM_DATA")) {
                if (auto entries = usage_mon_data.list_by_ue_id(ue_id); !entries.empty()) {
                    json um = json::object();
                    for (auto& entry : entries) {
                        const auto limit_id = entry.at("limitId").get<std::string>();
                        um[limit_id] = std::move(entry);
                    }
                    result["umData"] = std::move(um);
                }
            }
            if (wanted("OPERATOR_SPECIFIC_DATA")) {
                if (auto v = policy_operator_specific_data.get(ue_id); v.has_value()) {
                    result["operatorSpecificDataSet"] = *v;
                }
            }

            policy_data_read_counter->Add(1);
            return sbi_core::http2::Response::json(200, result.dump());
        });

    // --- Nudr_DataRepository: Authentication Data group (ADR-0083, gap-closure task #106) ---

    const std::string auth_subscription_path_pattern =
        std::string(kApiRoot) +
        "/subscription-data/{ueId}/authentication-data/authentication-subscription";

    server.add_route(
        "GET",
        auth_subscription_path_pattern,
        [&verifier, &auth_subscription_data, &auth_subscription_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = auth_subscription_data.get(ue_id);
            auth_subscription_get_counter->Add(1);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No authentication subscription data for ueId " + ue_id);
            }
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PATCH",
        auth_subscription_path_pattern,
        [&verifier,
         &auth_subscription_data,
         &auth_subscription_patch_counter,
         &subs_to_notify,
         &notify_client,
         auth_subscription_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            // Real spec: application/json-patch+json (RFC 6902) -- same standard AmfContext3gpp
            // above uses, confirmed by reading the YAML directly.
            json patch_ops;
            try {
                patch_ops = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto ue_id = req.path_params.at("ueId");
            json patched;
            try {
                patched = auth_subscription_data.apply_patch(ue_id, patch_ops);
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(400, "Invalid JSON Patch", e.what());
            }
            auth_subscription_patch_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(auth_subscription_path_pattern, req.path_params),
                               change_from_json_patch(patch_ops));
            return sbi_core::http2::Response::json(200, patched.dump());
        });

    const std::string auth_status_path_pattern =
        std::string(kApiRoot) +
        "/subscription-data/{ueId}/authentication-data/authentication-status";

    server.add_route(
        "PUT",
        auth_status_path_pattern,
        [&verifier,
         &auth_status,
         &auth_status_put_counter,
         &subs_to_notify,
         &notify_client,
         auth_status_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::AuthEvent>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            json j = *body;
            auth_status.put(ue_id, j);
            auth_status_put_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(auth_status_path_pattern, req.path_params),
                               change_replace(j));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "GET",
        auth_status_path_pattern,
        [&verifier, &auth_status, &auth_status_get_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = auth_status.get(ue_id);
            auth_status_get_counter->Add(1);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No authentication status for ueId " + ue_id);
            }
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "DELETE",
        auth_status_path_pattern,
        [&verifier,
         &auth_status,
         &auth_status_delete_counter,
         &subs_to_notify,
         &notify_client,
         auth_status_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            if (!auth_status.remove(ue_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No authentication status for ueId " + ue_id);
            }
            auth_status_delete_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(auth_status_path_pattern, req.path_params),
                               change_remove());
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nudr_DataRepository: Individual Authentication Status (Document) (ADR-0212,
    // gap-closure task #106) -- real PUT (replace) + GET + DELETE per
    // TS29505_Subscription_Data.yaml (CreateIndividualAuthenticationStatus/
    // QueryIndividualAuthenticationStatus/DeleteIndividualAuthenticationStatus), genuinely
    // distinct from the bare `authentication-status` resource above: keyed by (ueId,
    // servingNetworkName), not ueId alone. Same real `AuthEvent` schema. Real, disclosed: the
    // GET operation's own optional `fields`/`supported-features` query params are accepted but
    // not honored (matching the established "optional filter not honored" precedent elsewhere in
    // this file -- no field-projection or supported-features layer exists in this store). ---

    const std::string individual_auth_status_path_pattern =
        std::string(kApiRoot) +
        "/subscription-data/{ueId}/authentication-data/authentication-status/"
        "{servingNetworkName}";

    server.add_route(
        "PUT",
        individual_auth_status_path_pattern,
        [&verifier,
         &individual_auth_status,
         &individual_auth_status_put_counter,
         &subs_to_notify,
         &notify_client,
         individual_auth_status_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::AuthEvent>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto serving_network_name = req.path_params.at("servingNetworkName");
            json j = *body;
            individual_auth_status.put(ue_id, serving_network_name, j);
            individual_auth_status_put_counter->Add(1);
            notify_subscribers(
                subs_to_notify,
                notify_client,
                ue_id,
                resolved_location(individual_auth_status_path_pattern, req.path_params),
                change_replace(j));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "GET",
        individual_auth_status_path_pattern,
        [&verifier, &individual_auth_status, &individual_auth_status_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto serving_network_name = req.path_params.at("servingNetworkName");
            auto data = individual_auth_status.get(ue_id, serving_network_name);
            individual_auth_status_get_counter->Add(1);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404,
                    "Not Found",
                    "No individual authentication status for ueId " + ue_id +
                        ", servingNetworkName " + serving_network_name);
            }
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "DELETE",
        individual_auth_status_path_pattern,
        [&verifier,
         &individual_auth_status,
         &individual_auth_status_delete_counter,
         &subs_to_notify,
         &notify_client,
         individual_auth_status_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto serving_network_name = req.path_params.at("servingNetworkName");
            if (!individual_auth_status.remove(ue_id, serving_network_name)) {
                return sbi_core::http2::problem_response(
                    404,
                    "Not Found",
                    "No individual authentication status for ueId " + ue_id +
                        ", servingNetworkName " + serving_network_name);
            }
            individual_auth_status_delete_counter->Add(1);
            notify_subscribers(
                subs_to_notify,
                notify_client,
                ue_id,
                resolved_location(individual_auth_status_path_pattern, req.path_params),
                change_remove());
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nudr_DataRepository: policy-data group, AM policy resource (ADR-0083, gap-closure task
    // #106) -- real GET+PATCH per TS29519_Policy_Data.yaml, the real UDR-side backing for PCF's
    // own Npcf_AMPolicyControl. Genuinely distinct from provisioned-data's own `am_data` column
    // (AccessAndMobilitySubscriptionData) -- see schema.postgres.sql's own comment. ---

    const std::string am_policy_data_path_pattern =
        std::string(kApiRoot) + "/policy-data/ues/{ueId}/am-data";

    server.add_route(
        "GET",
        am_policy_data_path_pattern,
        [&verifier, &am_policy_data, &am_policy_data_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = am_policy_data.get(ue_id);
            am_policy_data_get_counter->Add(1);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No AM policy data for ueId " + ue_id);
            }
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PATCH",
        am_policy_data_path_pattern,
        [&verifier,
         &am_policy_data,
         &am_policy_data_patch_counter,
         &subs_to_notify,
         &notify_client,
         am_policy_data_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json patch;
            try {
                patch = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto patched = am_policy_data.merge_patch(ue_id, patch);
            am_policy_data_patch_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(am_policy_data_path_pattern, req.path_params),
                               change_replace(patched));
            return sbi_core::http2::Response::json(200, patched.dump());
        });

    // --- Nudr_DataRepository: SMSF Registration context-data group (ADR-0097, gap-closure task
    // #106) -- real GET+PUT+DELETE per TS29505_Subscription_Data.yaml, two distinct real
    // resources (3GPP-access / non-3GPP-access) sharing the identical real `SmsfRegistration`
    // schema -- see schema.postgres.sql's own comment for why these stay two separate
    // tables/stores rather than merged. ---

    const std::string smsf_3gpp_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/context-data/smsf-3gpp-access";

    server.add_route(
        "PUT",
        smsf_3gpp_path_pattern,
        [&verifier,
         &smsf_3gpp_context,
         &smsf_3gpp_write_counter,
         &subs_to_notify,
         &notify_client,
         smsf_3gpp_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::SmsfRegistration>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            json j = *body;
            smsf_3gpp_context.put(ue_id, j);
            smsf_3gpp_write_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(smsf_3gpp_path_pattern, req.path_params),
                               change_replace(j));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "GET",
        smsf_3gpp_path_pattern,
        [&verifier, &smsf_3gpp_context](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = smsf_3gpp_context.get(ue_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No SMSF 3GPP-access context for ueId " + ue_id);
            }
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "DELETE",
        smsf_3gpp_path_pattern,
        [&verifier,
         &smsf_3gpp_context,
         &smsf_3gpp_delete_counter,
         &subs_to_notify,
         &notify_client,
         smsf_3gpp_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            if (!smsf_3gpp_context.remove(ue_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No SMSF 3GPP-access context for ueId " + ue_id);
            }
            smsf_3gpp_delete_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(smsf_3gpp_path_pattern, req.path_params),
                               change_remove());
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    const std::string smsf_non3gpp_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/context-data/smsf-non-3gpp-access";

    server.add_route(
        "PUT",
        smsf_non3gpp_path_pattern,
        [&verifier,
         &smsf_non3gpp_context,
         &smsf_non3gpp_write_counter,
         &subs_to_notify,
         &notify_client,
         smsf_non3gpp_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::SmsfRegistration>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            json j = *body;
            smsf_non3gpp_context.put(ue_id, j);
            smsf_non3gpp_write_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(smsf_non3gpp_path_pattern, req.path_params),
                               change_replace(j));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "GET",
        smsf_non3gpp_path_pattern,
        [&verifier, &smsf_non3gpp_context](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = smsf_non3gpp_context.get(ue_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No SMSF non-3GPP-access context for ueId " + ue_id);
            }
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "DELETE",
        smsf_non3gpp_path_pattern,
        [&verifier,
         &smsf_non3gpp_context,
         &smsf_non3gpp_delete_counter,
         &subs_to_notify,
         &notify_client,
         smsf_non3gpp_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            if (!smsf_non3gpp_context.remove(ue_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No SMSF non-3GPP-access context for ueId " + ue_id);
            }
            smsf_non3gpp_delete_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(smsf_non3gpp_path_pattern, req.path_params),
                               change_remove());
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nudr_DataRepository: IP-SM-GW Registration context-data resource (ADR-0098, gap-closure
    // task #106) -- real PUT+GET+PATCH(RFC 6902)+DELETE per TS29505_Subscription_Data.yaml, the
    // richest operation set of any context-data resource this project has closed so far. ---

    const std::string ip_sm_gw_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/context-data/ip-sm-gw";

    server.add_route(
        "PUT",
        ip_sm_gw_path_pattern,
        [&verifier,
         &ip_sm_gw_context,
         &ip_sm_gw_write_counter,
         &subs_to_notify,
         &notify_client,
         ip_sm_gw_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::IpSmGwRegistration>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            json j = *body;
            ip_sm_gw_context.put(ue_id, j);
            ip_sm_gw_write_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(ip_sm_gw_path_pattern, req.path_params),
                               change_replace(j));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "GET",
        ip_sm_gw_path_pattern,
        [&verifier, &ip_sm_gw_context](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = ip_sm_gw_context.get(ue_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No IP-SM-GW context for ueId " + ue_id);
            }
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PATCH",
        ip_sm_gw_path_pattern,
        [&verifier,
         &ip_sm_gw_context,
         &ip_sm_gw_write_counter,
         &subs_to_notify,
         &notify_client,
         ip_sm_gw_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json patch_ops;
            try {
                patch_ops = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto ue_id = req.path_params.at("ueId");
            std::optional<json> patched;
            try {
                patched = ip_sm_gw_context.apply_patch(ue_id, patch_ops);
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(400, "Invalid JSON Patch", e.what());
            }
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No IP-SM-GW context for ueId " + ue_id);
            }
            ip_sm_gw_write_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(ip_sm_gw_path_pattern, req.path_params),
                               change_from_json_patch(patch_ops));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "DELETE",
        ip_sm_gw_path_pattern,
        [&verifier,
         &ip_sm_gw_context,
         &ip_sm_gw_delete_counter,
         &subs_to_notify,
         &notify_client,
         ip_sm_gw_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            if (!ip_sm_gw_context.remove(ue_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No IP-SM-GW context for ueId " + ue_id);
            }
            ip_sm_gw_delete_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(ip_sm_gw_path_pattern, req.path_params),
                               change_remove());
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nudr_DataRepository: Message Waiting Data (Document) resource (ADR-0099, gap-closure
    // task #106) -- real PUT+GET+PATCH(RFC 6902)+DELETE per TS29505_Subscription_Data.yaml. ---

    const std::string mwd_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/context-data/mwd";

    server.add_route(
        "PUT",
        mwd_path_pattern,
        [&verifier, &mwd, &mwd_write_counter, &subs_to_notify, &notify_client, mwd_path_pattern](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::MessageWaitingData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            json j = *body;
            const bool is_new = mwd.put(ue_id, j);
            mwd_write_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(mwd_path_pattern, req.path_params),
                               change_replace(j));

            if (!is_new) {
                sbi_core::http2::Response resp;
                resp.status = 204;
                return resp;
            }
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location", resolved_location(mwd_path_pattern, req.path_params));
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET", mwd_path_pattern, [&verifier, &mwd](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = mwd.get(ue_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No Message Waiting Data for ueId " + ue_id);
            }
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PATCH",
        mwd_path_pattern,
        [&verifier, &mwd, &mwd_write_counter, &subs_to_notify, &notify_client, mwd_path_pattern](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json patch_ops;
            try {
                patch_ops = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto ue_id = req.path_params.at("ueId");
            std::optional<json> patched;
            try {
                patched = mwd.apply_patch(ue_id, patch_ops);
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(400, "Invalid JSON Patch", e.what());
            }
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No Message Waiting Data for ueId " + ue_id);
            }
            mwd_write_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(mwd_path_pattern, req.path_params),
                               change_from_json_patch(patch_ops));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "DELETE",
        mwd_path_pattern,
        [&verifier, &mwd, &mwd_delete_counter, &subs_to_notify, &notify_client, mwd_path_pattern](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            if (!mwd.remove(ue_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No Message Waiting Data for ueId " + ue_id);
            }
            mwd_delete_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(mwd_path_pattern, req.path_params),
                               change_remove());
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nudr_DataRepository: Roaming Information (Document) resource (ADR-0100, gap-closure
    // task #106) -- real PUT+GET per TS29505_Subscription_Data.yaml, no PATCH/DELETE. ---

    const std::string roaming_information_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/context-data/roaming-information";

    server.add_route(
        "PUT",
        roaming_information_path_pattern,
        [&verifier,
         &roaming_information,
         &roaming_information_write_counter,
         &subs_to_notify,
         &notify_client,
         roaming_information_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::RoamingInfoUpdate>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            json j = *body;
            const bool is_new = roaming_information.put(ue_id, j);
            roaming_information_write_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(roaming_information_path_pattern, req.path_params),
                               change_replace(j));

            if (!is_new) {
                sbi_core::http2::Response resp;
                resp.status = 204;
                return resp;
            }
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace(
                "location", resolved_location(roaming_information_path_pattern, req.path_params));
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        roaming_information_path_pattern,
        [&verifier, &roaming_information](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = roaming_information.get(ue_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No Roaming Information for ueId " + ue_id);
            }
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // --- Nudr_DataRepository: PEI Information (Document) resource (ADR-0101, gap-closure
    // task #106) -- real PUT+GET per TS29505_Subscription_Data.yaml, no PATCH/DELETE. ---

    const std::string pei_info_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/context-data/pei-info";

    server.add_route(
        "PUT",
        pei_info_path_pattern,
        [&verifier,
         &pei_info,
         &pei_info_write_counter,
         &subs_to_notify,
         &notify_client,
         pei_info_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::PeiUpdateInfo_Subscription_Data>(
                req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            json j = *body;
            const bool is_new = pei_info.put(ue_id, j);
            pei_info_write_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(pei_info_path_pattern, req.path_params),
                               change_replace(j));

            if (!is_new) {
                sbi_core::http2::Response resp;
                resp.status = 204;
                return resp;
            }
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 resolved_location(pei_info_path_pattern, req.path_params));
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET", pei_info_path_pattern, [&verifier, &pei_info](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = pei_info.get(ue_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No PEI Information for ueId " + ue_id);
            }
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // --- Nudr_DataRepository: Enhanced Coverage Restriction Data resource (ADR-0102, gap-closure
    // task #106) -- real GET-only per TS29505_Subscription_Data.yaml, seeded at startup (no
    // create/update operation exists in the spec, same shape as provisioned-data). ---

    const std::string coverage_restriction_data_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/coverage-restriction-data";

    server.add_route(
        "GET",
        coverage_restriction_data_path_pattern,
        [&verifier, &coverage_restriction_data, &coverage_restriction_data_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = coverage_restriction_data.get(ue_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No Coverage Restriction Data for ueId " + ue_id);
            }
            coverage_restriction_data_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // --- Nudr_DataRepository: LCS Privacy Subscription Data resource (ADR-0103, gap-closure
    // task #106) -- real GET-only per TS29505_Subscription_Data.yaml, seeded at startup. ---

    const std::string lcs_privacy_data_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/lcs-privacy-data";

    server.add_route(
        "GET",
        lcs_privacy_data_path_pattern,
        [&verifier, &lcs_privacy_data, &lcs_privacy_data_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = lcs_privacy_data.get(ue_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No LCS Privacy Data for ueId " + ue_id);
            }
            lcs_privacy_data_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // --- Nudr_DataRepository: LCS Subscription Data resource (ADR-0104, gap-closure task #106)
    // -- real GET-only per TS29505_Subscription_Data.yaml, seeded at startup. ---

    const std::string lcs_subscription_data_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/lcs-subscription-data";

    server.add_route(
        "GET",
        lcs_subscription_data_path_pattern,
        [&verifier, &lcs_subscription_data, &lcs_subscription_data_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = lcs_subscription_data.get(ue_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No LCS Subscription Data for ueId " + ue_id);
            }
            lcs_subscription_data_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // --- Nudr_DataRepository: LCS Mobile Originated Subscription Data resource (ADR-0105,
    // gap-closure task #106) -- real GET-only per TS29505_Subscription_Data.yaml, seeded at
    // startup. ---

    const std::string lcs_mo_data_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/lcs-mo-data";

    server.add_route(
        "GET",
        lcs_mo_data_path_pattern,
        [&verifier, &lcs_mo_data, &lcs_mo_data_get_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = lcs_mo_data.get(ue_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No LCS Mobile Originated Data for ueId " + ue_id);
            }
            lcs_mo_data_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // --- Nudr_DataRepository: Parameter Provision (Document) resource (ADR-0107, gap-closure
    // task #106) -- real GET+PATCH(RFC 6902) per TS29505_Subscription_Data.yaml, no PUT/DELETE. ---

    const std::string pp_data_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/pp-data";

    server.add_route(
        "GET",
        pp_data_path_pattern,
        [&verifier, &pp_data, &pp_data_get_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = pp_data.get(ue_id);
            pp_data_get_counter->Add(1);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No pp-data for ueId " + ue_id);
            }
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PATCH",
        pp_data_path_pattern,
        [&verifier,
         &pp_data,
         &pp_data_patch_counter,
         &subs_to_notify,
         &notify_client,
         pp_data_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json patch_ops;
            try {
                patch_ops = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto ue_id = req.path_params.at("ueId");
            json patched;
            try {
                patched = pp_data.apply_patch(ue_id, patch_ops);
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(400, "Invalid JSON Patch", e.what());
            }
            pp_data_patch_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(pp_data_path_pattern, req.path_params),
                               change_from_json_patch(patch_ops));
            return sbi_core::http2::Response::json(200, patched.dump());
        });

    // --- Nudr_DataRepository: Parameter Provision profile Data (Document) resource (ADR-0108,
    // gap-closure task #106) -- real GET-only per TS29505_Subscription_Data.yaml, seeded at
    // startup. ---

    const std::string pp_profile_data_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/pp-profile-data";

    server.add_route(
        "GET",
        pp_profile_data_path_pattern,
        [&verifier, &pp_profile_data, &pp_profile_data_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = pp_profile_data.get(ue_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No pp-profile-data for ueId " + ue_id);
            }
            pp_profile_data_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // --- Nudr_DataRepository: Provisioned Parameter Data Entry resource (ADR-0109, gap-closure
    // task #106) -- real PUT+GET+DELETE per TS29505_Subscription_Data.yaml, plus a real sibling
    // collection GET, composite (ueId, afInstanceId) key. ---

    const std::string pp_data_store_list_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/pp-data-store";
    const std::string pp_data_store_path_pattern =
        pp_data_store_list_path_pattern + "/{afInstanceId}";

    server.add_route(
        "GET",
        pp_data_store_list_path_pattern,
        [&verifier, &pp_data_entry](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            sbi_gen::PpDataEntryList list;
            list.ppDataEntryList = std::vector<sbi_gen::PpDataEntry>{};
            for (const auto& entry : pp_data_entry.list_for_ue(ue_id)) {
                list.ppDataEntryList->push_back(entry.get<sbi_gen::PpDataEntry>());
            }
            json j = list;
            return sbi_core::http2::Response::json(200, j.dump());
        });

    server.add_route(
        "GET",
        pp_data_store_path_pattern,
        [&verifier, &pp_data_entry](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto af_instance_id = req.path_params.at("afInstanceId");
            auto data = pp_data_entry.get(ue_id, af_instance_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(404,
                                                         "Not Found",
                                                         "No PP data entry for ueId/afInstanceId " +
                                                             ue_id + "/" + af_instance_id);
            }
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PUT",
        pp_data_store_path_pattern,
        [&verifier,
         &pp_data_entry,
         &pp_data_entry_write_counter,
         &subs_to_notify,
         &notify_client,
         pp_data_store_list_path_pattern,
         pp_data_store_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::PpDataEntry>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto af_instance_id = req.path_params.at("afInstanceId");
            json j = *body;
            const bool is_new = pp_data_entry.put(ue_id, af_instance_id, j);
            pp_data_entry_write_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(pp_data_store_path_pattern, req.path_params),
                               change_replace(j));

            if (!is_new) {
                sbi_core::http2::Response resp;
                resp.status = 204;
                return resp;
            }
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace(
                "location",
                resolved_location(pp_data_store_list_path_pattern, req.path_params) + "/" +
                    af_instance_id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "DELETE",
        pp_data_store_path_pattern,
        [&verifier,
         &pp_data_entry,
         &pp_data_entry_delete_counter,
         &subs_to_notify,
         &notify_client,
         pp_data_store_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto af_instance_id = req.path_params.at("afInstanceId");
            if (!pp_data_entry.remove(ue_id, af_instance_id)) {
                return sbi_core::http2::problem_response(404,
                                                         "Not Found",
                                                         "No PP data entry for ueId/afInstanceId " +
                                                             ue_id + "/" + af_instance_id);
            }
            pp_data_entry_delete_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(pp_data_store_path_pattern, req.path_params),
                               change_remove());
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nudr_DataRepository: individual Shared Data resource (ADR-0110, gap-closure task #106)
    // -- real GET-only per TS29505_Subscription_Data.yaml, seeded at startup. Genuinely NOT
    // per-UE -- keyed by sharedDataId alone. ---

    const std::string shared_data_path_pattern =
        std::string(kApiRoot) + "/subscription-data/shared-data/{sharedDataId}";

    server.add_route(
        "GET",
        shared_data_path_pattern,
        [&verifier, &shared_data, &shared_data_get_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto shared_data_id = req.path_params.at("sharedDataId");
            auto data = shared_data.get(shared_data_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No shared data for sharedDataId " + shared_data_id);
            }
            shared_data_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // --- Nudr_DataRepository: Operator-Specific Data Container (Document) resource (ADR-0111,
    // gap-closure task #106) -- real GET+PATCH(RFC 6902)+PUT+DELETE per
    // TS29505_Subscription_Data.yaml (PUT/DELETE added ADR-0197, correcting ADR-0111's own "no
    // PUT/DELETE exists" error). ---

    const std::string operator_specific_data_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/operator-specific-data";

    server.add_route(
        "GET",
        operator_specific_data_path_pattern,
        [&verifier, &operator_specific_data, &operator_specific_data_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = operator_specific_data.get(ue_id);
            operator_specific_data_get_counter->Add(1);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No operator-specific-data for ueId " + ue_id);
            }
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PATCH",
        operator_specific_data_path_pattern,
        [&verifier,
         &operator_specific_data,
         &operator_specific_data_patch_counter,
         &subs_to_notify,
         &notify_client,
         operator_specific_data_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json patch_ops;
            try {
                patch_ops = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto ue_id = req.path_params.at("ueId");
            json patched;
            try {
                patched = operator_specific_data.apply_patch(ue_id, patch_ops);
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(400, "Invalid JSON Patch", e.what());
            }
            operator_specific_data_patch_counter->Add(1);
            notify_subscribers(
                subs_to_notify,
                notify_client,
                ue_id,
                resolved_location(operator_specific_data_path_pattern, req.path_params),
                change_from_json_patch(patch_ops));
            return sbi_core::http2::Response::json(200, patched.dump());
        });

    // ADR-0197: real PUT/DELETE, correcting ADR-0111's own documentation error (a direct re-read
    // of TS29505_Subscription_Data.yaml found CreateOperSpecData/DeleteOperSpecData both real and
    // declared, contrary to that ADR's own "no PUT/DELETE exists" claim).
    server.add_route(
        "PUT",
        operator_specific_data_path_pattern,
        [&verifier,
         &operator_specific_data,
         &operator_specific_data_put_counter,
         &subs_to_notify,
         &notify_client,
         operator_specific_data_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json body;
            try {
                body = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto ue_id = req.path_params.at("ueId");
            const bool is_new = operator_specific_data.put(ue_id, body);
            operator_specific_data_put_counter->Add(1);
            notify_subscribers(
                subs_to_notify,
                notify_client,
                ue_id,
                resolved_location(operator_specific_data_path_pattern, req.path_params),
                change_replace(body));

            if (!is_new) {
                sbi_core::http2::Response resp;
                resp.status = 204;
                return resp;
            }
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace(
                "location",
                resolved_location(operator_specific_data_path_pattern, req.path_params));
            resp.body = body.dump();
            return resp;
        });

    server.add_route(
        "DELETE",
        operator_specific_data_path_pattern,
        [&verifier,
         &operator_specific_data,
         &operator_specific_data_delete_counter,
         &subs_to_notify,
         &notify_client,
         operator_specific_data_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            if (!operator_specific_data.remove(ue_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No operator-specific-data for ueId " + ue_id);
            }
            operator_specific_data_delete_counter->Add(1);
            notify_subscribers(
                subs_to_notify,
                notify_client,
                ue_id,
                resolved_location(operator_specific_data_path_pattern, req.path_params),
                change_remove());
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nudr_DataRepository: Event Exposure Data (Document) resource (ADR-0112, gap-closure
    // task #106) -- real GET-only per TS29505_Subscription_Data.yaml, seeded at startup. ---

    const std::string ee_profile_data_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/ee-profile-data";

    server.add_route(
        "GET",
        ee_profile_data_path_pattern,
        [&verifier, &ee_profile_data, &ee_profile_data_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = ee_profile_data.get(ue_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No ee-profile-data for ueId " + ue_id);
            }
            ee_profile_data_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // --- Nudr_DataRepository: policy-data group, UE Policy Set resource (ADR-0113, gap-closure
    // task #106) -- real GET+PUT+PATCH(RFC 7396) per TS29519_Policy_Data.yaml, no DELETE. ---

    const std::string ue_policy_set_path_pattern =
        std::string(kApiRoot) + "/policy-data/ues/{ueId}/ue-policy-set";

    server.add_route(
        "GET",
        ue_policy_set_path_pattern,
        [&verifier, &ue_policy_set, &ue_policy_set_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = ue_policy_set.get(ue_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No UE policy set for ueId " + ue_id);
            }
            ue_policy_set_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PUT",
        ue_policy_set_path_pattern,
        [&verifier,
         &ue_policy_set,
         &ue_policy_set_write_counter,
         &subs_to_notify,
         &notify_client,
         ue_policy_set_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json body;
            try {
                body = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto ue_id = req.path_params.at("ueId");
            const bool is_new = ue_policy_set.put(ue_id, body);
            ue_policy_set_write_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(ue_policy_set_path_pattern, req.path_params),
                               change_replace(body));

            if (!is_new) {
                sbi_core::http2::Response resp;
                resp.status = 204;
                return resp;
            }
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 resolved_location(ue_policy_set_path_pattern, req.path_params));
            resp.body = body.dump();
            return resp;
        });

    server.add_route(
        "PATCH",
        ue_policy_set_path_pattern,
        [&verifier,
         &ue_policy_set,
         &ue_policy_set_patch_counter,
         &subs_to_notify,
         &notify_client,
         ue_policy_set_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json patch;
            try {
                patch = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto patched = ue_policy_set.merge_patch(ue_id, patch);
            ue_policy_set_patch_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(ue_policy_set_path_pattern, req.path_params),
                               change_replace(patched));
            // Real spec: only 204 (no-body) is documented for this resource's real PATCH, unlike
            // am-data's own 200-with-body option -- confirmed by direct YAML read.
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nudr_DataRepository: policy-data group, Operator-Specific Data resource (ADR-0114,
    // gap-closure task #106) -- real GET+PATCH(RFC 6902)+PUT+DELETE per TS29519_Policy_Data.yaml
    // (PUT/DELETE added ADR-0197, correcting ADR-0114's own "no PUT/DELETE exists" error). ---

    const std::string policy_operator_specific_data_path_pattern =
        std::string(kApiRoot) + "/policy-data/ues/{ueId}/operator-specific-data";

    server.add_route(
        "GET",
        policy_operator_specific_data_path_pattern,
        [&verifier, &policy_operator_specific_data, &policy_operator_specific_data_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = policy_operator_specific_data.get(ue_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No policy-data operator-specific-data for ueId " + ue_id);
            }
            policy_operator_specific_data_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PATCH",
        policy_operator_specific_data_path_pattern,
        [&verifier,
         &policy_operator_specific_data,
         &policy_operator_specific_data_patch_counter,
         &subs_to_notify,
         &notify_client,
         policy_operator_specific_data_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json patch_ops;
            try {
                patch_ops = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto ue_id = req.path_params.at("ueId");
            json patched;
            try {
                patched = policy_operator_specific_data.apply_patch(ue_id, patch_ops);
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(400, "Invalid JSON Patch", e.what());
            }
            policy_operator_specific_data_patch_counter->Add(1);
            notify_subscribers(
                subs_to_notify,
                notify_client,
                ue_id,
                resolved_location(policy_operator_specific_data_path_pattern, req.path_params),
                change_from_json_patch(patch_ops));
            return sbi_core::http2::Response::json(200, patched.dump());
        });

    server.add_route(
        "PUT",
        policy_operator_specific_data_path_pattern,
        [&verifier,
         &policy_operator_specific_data,
         &policy_operator_specific_data_put_counter,
         &subs_to_notify,
         &notify_client,
         policy_operator_specific_data_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json body;
            try {
                body = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto ue_id = req.path_params.at("ueId");
            const bool is_new = policy_operator_specific_data.put(ue_id, body);
            policy_operator_specific_data_put_counter->Add(1);
            notify_subscribers(
                subs_to_notify,
                notify_client,
                ue_id,
                resolved_location(policy_operator_specific_data_path_pattern, req.path_params),
                change_replace(body));

            if (!is_new) {
                sbi_core::http2::Response resp;
                resp.status = 204;
                return resp;
            }
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace(
                "location",
                resolved_location(policy_operator_specific_data_path_pattern, req.path_params));
            resp.body = body.dump();
            return resp;
        });

    server.add_route(
        "DELETE",
        policy_operator_specific_data_path_pattern,
        [&verifier,
         &policy_operator_specific_data,
         &policy_operator_specific_data_delete_counter,
         &subs_to_notify,
         &notify_client,
         policy_operator_specific_data_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            if (!policy_operator_specific_data.remove(ue_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No policy-data operator-specific-data for ueId " + ue_id);
            }
            policy_operator_specific_data_delete_counter->Add(1);
            notify_subscribers(
                subs_to_notify,
                notify_client,
                ue_id,
                resolved_location(policy_operator_specific_data_path_pattern, req.path_params),
                change_remove());
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nudr_DataRepository: policy-data group, Sponsor Connectivity Data resource (ADR-0115,
    // gap-closure task #106) -- real GET-only per TS29519_Policy_Data.yaml, keyed by sponsorId
    // (not ueId), seeded at startup. ---

    const std::string sponsor_connectivity_data_path_pattern =
        std::string(kApiRoot) + "/policy-data/sponsor-connectivity-data/{sponsorId}";

    server.add_route(
        "GET",
        sponsor_connectivity_data_path_pattern,
        [&verifier, &sponsor_connectivity_data, &sponsor_connectivity_data_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto sponsor_id = req.path_params.at("sponsorId");
            auto data = sponsor_connectivity_data.get(sponsor_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No sponsor connectivity data for sponsorId " + sponsor_id);
            }
            sponsor_connectivity_data_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // --- Nudr_DataRepository: policy-data group, MBS Session Policy Control Data resource
    // (ADR-0182, gap-closure task #106) -- real GET-only per TS29519_Policy_Data.yaml
    // (`GetMBSSessPolCtrlData`), keyed by `polSessionId`, seeded at startup. Real, disclosed
    // partial scope: `polSessionId`'s own real schema (`MbsSessPolDataId`) is a `oneOf` of
    // `{mbsSessionId}`/`{afAppId}`, where `mbsSessionId` is itself an `anyOf` of `{tmgi}`/`{ssm}`.
    // ADR-0119's own original deferral already explicitly declined inventing a serialization for
    // that nested branch -- unreversed here. This route only ever needed to handle the `afAppId`
    // branch, which is just `type: string` on its own -- already unambiguous, no encoding
    // decision needed, so the literal path segment is simply the store key. The `mbsSessionId`
    // branch remains untouched. See the full disclosure on `udr_mbs_session_pol_data` in
    // `schema.postgres.sql`. ---

    const std::string mbs_session_pol_data_path_pattern =
        std::string(kApiRoot) + "/policy-data/mbs-session-pol-data/{polSessionId}";

    server.add_route(
        "GET",
        mbs_session_pol_data_path_pattern,
        [&verifier, &mbs_session_pol_data, &mbs_session_pol_data_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto pol_session_id = req.path_params.at("polSessionId");
            auto data = mbs_session_pol_data.get(pol_session_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404,
                    "Not Found",
                    "No MBS Session Policy Control Data for polSessionId " + pol_session_id);
            }
            mbs_session_pol_data_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // --- Nudr_DataRepository: policy-data group, bare BDT Data collection GET (ADR-0213,
    // gap-closure task #106) -- real GET-only per TS29519_Policy_Data.yaml, `ReadBdtData`. Real,
    // optional `bdt-ref-ids` array filter honored via the same "store returns all, route
    // filters" pattern as `pdtq-data`'s own collection GET; `supp-feat` accepted but not honored
    // (no supported-features layer exists in this store). ---

    const std::string bdt_data_collection_path_pattern =
        std::string(kApiRoot) + "/policy-data/bdt-data";

    server.add_route(
        "GET",
        bdt_data_collection_path_pattern,
        [&verifier, &bdt_data, &bdt_data_list_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            std::vector<std::string> ref_ids;
            if (const auto ids_it = req.query_params.find("bdt-ref-ids");
                ids_it != req.query_params.end()) {
                ref_ids = sbi_core::http2::split_form_array(ids_it->second);
            }
            auto all = bdt_data.list_all();
            json arr = json::array();
            for (auto& item : all) {
                if (ref_ids.empty()) {
                    arr.push_back(std::move(item));
                    continue;
                }
                const auto ref_id_it = item.find("bdtRefId");
                if (ref_id_it != item.end() &&
                    std::find(ref_ids.begin(), ref_ids.end(), ref_id_it->get<std::string>()) !=
                        ref_ids.end()) {
                    arr.push_back(std::move(item));
                }
            }
            bdt_data_list_counter->Add(1);
            return sbi_core::http2::Response::json(200, arr.dump());
        });

    // --- Nudr_DataRepository: policy-data group, individual BDT Data resource (ADR-0116,
    // gap-closure task #106) -- real GET+PUT+PATCH(RFC 7396)+DELETE per
    // TS29519_Policy_Data.yaml. ---

    const std::string bdt_data_path_pattern =
        std::string(kApiRoot) + "/policy-data/bdt-data/{bdtReferenceId}";

    server.add_route(
        "GET",
        bdt_data_path_pattern,
        [&verifier, &bdt_data, &bdt_data_get_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto bdt_ref_id = req.path_params.at("bdtReferenceId");
            auto data = bdt_data.get(bdt_ref_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No BDT data for bdtReferenceId " + bdt_ref_id);
            }
            bdt_data_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PUT",
        bdt_data_path_pattern,
        [&verifier,
         &bdt_data,
         &bdt_data_write_counter,
         &subs_to_notify,
         &notify_client,
         bdt_data_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json body;
            try {
                body = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto bdt_ref_id = req.path_params.at("bdtReferenceId");
            bdt_data.put(bdt_ref_id, body);
            bdt_data_write_counter->Add(1);
            notify_subscribers_ue_less(subs_to_notify,
                                       notify_client,
                                       resolved_location(bdt_data_path_pattern, req.path_params),
                                       change_replace(body));
            // Real spec: CreateIndividualBdtData documents ONLY 201 as a success response (no
            // update-via-PUT status) -- confirmed by direct read, this route always responds 201,
            // matching the real spec literally rather than inventing an undocumented 204.
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 resolved_location(bdt_data_path_pattern, req.path_params));
            resp.body = body.dump();
            return resp;
        });

    server.add_route(
        "PATCH",
        bdt_data_path_pattern,
        [&verifier,
         &bdt_data,
         &bdt_data_patch_counter,
         &subs_to_notify,
         &notify_client,
         bdt_data_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json patch;
            try {
                patch = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto bdt_ref_id = req.path_params.at("bdtReferenceId");
            const auto patched = bdt_data.merge_patch(bdt_ref_id, patch);
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No BDT data for bdtReferenceId " + bdt_ref_id);
            }
            bdt_data_patch_counter->Add(1);
            notify_subscribers_ue_less(subs_to_notify,
                                       notify_client,
                                       resolved_location(bdt_data_path_pattern, req.path_params),
                                       change_replace(*patched));
            return sbi_core::http2::Response::json(200, patched->dump());
        });

    server.add_route(
        "DELETE",
        bdt_data_path_pattern,
        [&verifier,
         &bdt_data,
         &bdt_data_delete_counter,
         &subs_to_notify,
         &notify_client,
         bdt_data_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto bdt_ref_id = req.path_params.at("bdtReferenceId");
            if (!bdt_data.remove(bdt_ref_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No BDT data for bdtReferenceId " + bdt_ref_id);
            }
            bdt_data_delete_counter->Add(1);
            notify_subscribers_ue_less(subs_to_notify,
                                       notify_client,
                                       resolved_location(bdt_data_path_pattern, req.path_params),
                                       change_remove());
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nudr_DataRepository: policy-data group, PLMN UE Policy Set resource (ADR-0117,
    // gap-closure task #106) -- real GET-only per TS29519_Policy_Data.yaml, seeded at startup (no
    // create/update operation exists in the spec, same shape as coverage-restriction-data). ---

    const std::string plmn_ue_policy_set_path_pattern =
        std::string(kApiRoot) + "/policy-data/plmns/{plmnId}/ue-policy-set";

    server.add_route(
        "GET",
        plmn_ue_policy_set_path_pattern,
        [&verifier, &plmn_ue_policy_set, &plmn_ue_policy_set_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto plmn_id = req.path_params.at("plmnId");
            auto data = plmn_ue_policy_set.get(plmn_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No PLMN UE Policy Set for plmnId " + plmn_id);
            }
            plmn_ue_policy_set_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // --- Nudr_DataRepository: policy-data group, Slice-specific Policy Control Data resource
    // (ADR-0118, gap-closure task #106) -- real GET+PATCH-only per TS29519_Policy_Data.yaml, no
    // PUT/POST create operation exists, so merge_patch is upsert-capable (same precedent as
    // am-data). ---

    const std::string slice_control_data_path_pattern =
        std::string(kApiRoot) + "/policy-data/slice-control-data/{snssai}";

    server.add_route(
        "GET",
        slice_control_data_path_pattern,
        [&verifier, &slice_control_data, &slice_control_data_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto snssai = req.path_params.at("snssai");
            auto data = slice_control_data.get(snssai);
            slice_control_data_get_counter->Add(1);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No Slice Policy Control Data for snssai " + snssai);
            }
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PATCH",
        slice_control_data_path_pattern,
        [&verifier,
         &slice_control_data,
         &slice_control_data_patch_counter,
         &subs_to_notify,
         &notify_client,
         slice_control_data_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json patch;
            try {
                patch = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto snssai = req.path_params.at("snssai");
            const auto patched = slice_control_data.merge_patch(snssai, patch);
            slice_control_data_patch_counter->Add(1);
            notify_subscribers_ue_less(
                subs_to_notify,
                notify_client,
                resolved_location(slice_control_data_path_pattern, req.path_params),
                change_replace(patched));
            return sbi_core::http2::Response::json(200, patched.dump());
        });

    // --- Nudr_DataRepository: policy-data group, group-specific Policy Control Data resource
    // (ADR-0119, gap-closure task #106) -- real GET+PATCH-only per TS29519_Policy_Data.yaml, no
    // PUT/POST create operation exists, so merge_patch is upsert-capable (same precedent as
    // am-data/slice-control-data). ---

    const std::string group_control_data_path_pattern =
        std::string(kApiRoot) + "/policy-data/group-control-data/{intGroupId}";

    server.add_route(
        "GET",
        group_control_data_path_pattern,
        [&verifier, &group_control_data, &group_control_data_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto int_group_id = req.path_params.at("intGroupId");
            auto data = group_control_data.get(int_group_id);
            group_control_data_get_counter->Add(1);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404,
                    "Not Found",
                    "No Group Policy Control Data for intGroupId " + int_group_id);
            }
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PATCH",
        group_control_data_path_pattern,
        [&verifier,
         &group_control_data,
         &group_control_data_patch_counter,
         &subs_to_notify,
         &notify_client,
         group_control_data_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json patch;
            try {
                patch = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto int_group_id = req.path_params.at("intGroupId");
            const auto patched = group_control_data.merge_patch(int_group_id, patch);
            group_control_data_patch_counter->Add(1);
            notify_subscribers_ue_less(
                subs_to_notify,
                notify_client,
                resolved_location(group_control_data_path_pattern, req.path_params),
                change_replace(patched));
            return sbi_core::http2::Response::json(200, patched.dump());
        });

    // --- Nudr_GroupIDmap: GetRoutingIDs (ADR-0120, gap-closure task #106) -- real GET-only per
    // TS29504_Nudr_GroupIDmap.yaml, a genuinely different real Nudr API from Nudr_DataRepository
    // above (see this file's own header comment for the full disclosure). Two real required
    // query parameters, nf-type and nf-group-id; no path parameters. ---

    const std::string routing_ids_path_pattern = std::string(kGroupIdMapApiRoot) + "/routing-ids";

    server.add_route(
        "GET",
        routing_ids_path_pattern,
        [&verifier, &routing_ids, &routing_ids_get_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto nf_type_it = req.query_params.find("nf-type");
            const auto nf_group_id_it = req.query_params.find("nf-group-id");
            if (nf_type_it == req.query_params.end() || nf_group_id_it == req.query_params.end()) {
                return sbi_core::http2::problem_response(
                    400,
                    "Bad Request",
                    "nf-type and nf-group-id are both required query parameters");
            }
            auto data = routing_ids.get(nf_type_it->second, nf_group_id_it->second);
            routing_ids_get_counter->Add(1);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(404,
                                                         "Not Found",
                                                         "No Routing IDs for nf-type " +
                                                             nf_type_it->second + " nf-group-id " +
                                                             nf_group_id_it->second);
            }
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // --- Nudr_GroupIDmap: GetNfGroupIDs (ADR-0164, gap-closure task #106) -- real GET-only per
    // TS29504_Nudr_GroupIDmap.yaml lines 28-84, same service as GetRoutingIDs above, NOT
    // Nudr_DataRepository. Real REQUIRED array query param nf-type (style: form, explode: false,
    // minItems: 1 -- the second real consumer of sbi_core::http2::split_form_array(), after
    // QueryContextData/ADR-0161) and real REQUIRED subscriberId (plain string). Response is a
    // real map {NFType: NfGroupId}; unlike the aggregate live-view resources
    // (ue-update-confirmation-data/context-data, ADR-0147/ADR-0161) which always return 200 since
    // those have no independent existence, this resource's own response schema literally requires
    // minProperties: 1 and a real 404 is documented, so an empty result honors the spec's real
    // 404 rather than deviating to always-200. No create/update/delete operation exists anywhere
    // in this service for the mapping data itself -- confirmed by direct read, seeded at startup,
    // same "provisioned out-of-band" precedent as routing_ids/group_identifiers above. The
    // sibling /nf-group-ids/subscriptions change-notification family (POST/GET/PATCH/DELETE +
    // onGroupIdMapChange webhook callback) was surveyed but is a separate, genuinely more complex
    // resource family, deliberately deferred to its own future turn (same "no real webhook
    // delivery" gap class as subs-to-notify). ---

    const std::string nf_group_ids_path_pattern = std::string(kGroupIdMapApiRoot) + "/nf-group-ids";

    server.add_route(
        "GET",
        nf_group_ids_path_pattern,
        [&verifier, &nf_group_ids, &nf_group_ids_get_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto nf_type_it = req.query_params.find("nf-type");
            const auto subscriber_id_it = req.query_params.find("subscriberId");
            if (nf_type_it == req.query_params.end() ||
                subscriber_id_it == req.query_params.end()) {
                return sbi_core::http2::problem_response(
                    400,
                    "Bad Request",
                    "nf-type and subscriberId are both required query parameters");
            }
            const auto nf_types = sbi_core::http2::split_form_array(nf_type_it->second);
            json result = json::object();
            for (const auto& nf_type : nf_types) {
                if (auto group_id = nf_group_ids.get(subscriber_id_it->second, nf_type);
                    group_id.has_value()) {
                    result[nf_type] = *group_id;
                }
            }
            nf_group_ids_get_counter->Add(1);
            if (result.empty()) {
                return sbi_core::http2::problem_response(404,
                                                         "Not Found",
                                                         "No NF Group IDs for subscriberId " +
                                                             subscriber_id_it->second);
            }
            return sbi_core::http2::Response::json(200, result.dump());
        });

    // --- Nudr_GroupIDmap: NF Group ID subscription-management family (ADR-0170, gap-closure
    // task #106; real generated DTO wired in ADR-0198, correcting this project's own standing
    // "never hand-write a DTO the YAML can generate" rule, which this handler had been violating)
    // -- real POST+GET+PATCH+DELETE per TS29504_Nudr_GroupIDmap.yaml,
    // CreateGroupIdSubscription/QueryGroupIdSubscription/ModifyGroupIdSubscription/
    // RemoveGroupIdSubscription. Real REQUIRED `SubscriptionData_Nudr_GroupIDmap` fields
    // (notificationUri/nfType/nfGroupId) now validated via the real generated DTO
    // (`sbi_core::http2::parse_json_body`), same discipline as every other UDR write route.
    // Real, server-generated `subscriptionId` (UUID v4, same `sbi_core::generate_uuid_v4()`
    // helper this project already uses for every other subscription resource). Real, disclosed:
    // this resource's own real PATCH documents both `200` (with body) and `204` (empty) for a
    // successful modify with no further distinguishing rule -- resolved the same way this
    // project already resolved the identical real ambiguity on `group_control_data`'s own
    // real RFC 7396 PATCH (ADR-0118/ADR-0119): always respond `200` with the patched body, the
    // richer of the two documented options. Real, disclosed gap: the spec's own
    // `onGroupIdMapChange` webhook callback is NOT implemented (no real outbound HTTP delivery to
    // the caller's `notificationUri`) -- same disclosed gap class as `subs-to-notify`'s own lack
    // of real webhook delivery (ADR-0149). ---

    const std::string nf_group_id_subscriptions_collection_path_pattern =
        std::string(kGroupIdMapApiRoot) + "/nf-group-ids/subscriptions";
    const std::string nf_group_id_subscriptions_individual_path_pattern =
        std::string(kGroupIdMapApiRoot) + "/nf-group-ids/subscriptions/{subscriptionId}";

    server.add_route(
        "POST",
        nf_group_id_subscriptions_collection_path_pattern,
        [&verifier,
         &nf_group_id_subscriptions,
         &nf_group_id_subscriptions_create_counter,
         nf_group_id_subscriptions_collection_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto parsed =
                sbi_core::http2::parse_json_body<sbi_gen::SubscriptionData_Nudr_GroupIDmap>(req,
                                                                                            err);
            if (!parsed.has_value()) {
                return err;
            }
            const auto subscription_id = sbi_core::generate_uuid_v4();
            parsed->subscriptionId = subscription_id;
            json body = *parsed;
            nf_group_id_subscriptions.create(subscription_id, body);
            nf_group_id_subscriptions_create_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace(
                "location",
                resolved_location(nf_group_id_subscriptions_collection_path_pattern,
                                  req.path_params) +
                    "/" + subscription_id);
            resp.body = body.dump();
            return resp;
        });

    server.add_route(
        "GET",
        nf_group_id_subscriptions_individual_path_pattern,
        [&verifier, &nf_group_id_subscriptions, &nf_group_id_subscriptions_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto subscription_id = req.path_params.at("subscriptionId");
            auto data = nf_group_id_subscriptions.get(subscription_id);
            nf_group_id_subscriptions_get_counter->Add(1);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No subscription for subscriptionId " + subscription_id);
            }
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PATCH",
        nf_group_id_subscriptions_individual_path_pattern,
        [&verifier, &nf_group_id_subscriptions, &nf_group_id_subscriptions_write_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json patch_ops;
            try {
                patch_ops = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto subscription_id = req.path_params.at("subscriptionId");
            std::optional<json> patched;
            try {
                patched = nf_group_id_subscriptions.apply_patch(subscription_id, patch_ops);
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(400, "Invalid JSON Patch", e.what());
            }
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No subscription for subscriptionId " + subscription_id);
            }
            nf_group_id_subscriptions_write_counter->Add(1);
            return sbi_core::http2::Response::json(200, patched->dump());
        });

    server.add_route(
        "DELETE",
        nf_group_id_subscriptions_individual_path_pattern,
        [&verifier, &nf_group_id_subscriptions, &nf_group_id_subscriptions_delete_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto subscription_id = req.path_params.at("subscriptionId");
            if (!nf_group_id_subscriptions.remove(subscription_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No subscription for subscriptionId " + subscription_id);
            }
            nf_group_id_subscriptions_delete_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nudr_DataRepository: GetNiddAuData (ADR-0165, gap-closure task #106) -- real GET-only
    // per TS29505_Subscription_Data.yaml, `/subscription-data/{ueId}/nidd-authorization-data`.
    // Real REQUIRED `single-nssai` query param uses `content: application/json` (a JSON-encoded
    // query value, decomposed to sst/sd here) -- genuinely different from ADR-0161's array-style
    // params. Real REQUIRED `dnn`/`mtc-provider-information` (plain strings); optional `af-id`
    // deliberately not honored, matching the established "optional filter not honored" precedent.
    // Distinct from the already-implemented `context-data/nidd-authorizations` CRUD resource
    // below (ADR-0121) -- confirmed by direct read to be a genuinely different resource, matching
    // this project's own deferred-list note ("query-parameter-keyed, not ueId-alone"). ---

    const std::string nidd_authorization_data_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/nidd-authorization-data";

    server.add_route(
        "GET",
        nidd_authorization_data_path_pattern,
        [&verifier, &nidd_authorization_data, &nidd_authorization_data_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto single_nssai_it = req.query_params.find("single-nssai");
            const auto dnn_it = req.query_params.find("dnn");
            const auto mtc_provider_it = req.query_params.find("mtc-provider-information");
            if (single_nssai_it == req.query_params.end() || dnn_it == req.query_params.end() ||
                mtc_provider_it == req.query_params.end()) {
                return sbi_core::http2::problem_response(400,
                                                         "Bad Request",
                                                         "single-nssai, dnn and "
                                                         "mtc-provider-information are all "
                                                         "required query parameters");
            }
            json snssai;
            try {
                snssai = json::parse(single_nssai_it->second);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", std::string("malformed single-nssai: ") + e.what());
            }
            if (!snssai.contains("sst") || !snssai.at("sst").is_number_integer()) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", "single-nssai is missing required field sst");
            }
            const int sst = snssai.at("sst").get<int>();
            const std::string sd = snssai.value("sd", "");
            const auto ue_id = req.path_params.at("ueId");
            auto data = nidd_authorization_data.get(
                ue_id, sst, sd, dnn_it->second, mtc_provider_it->second);
            nidd_authorization_data_get_counter->Add(1);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No NIDD Authorization Data for ueId " + ue_id);
            }
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // --- Nudr_DataRepository: NIDD Authorization Info context-data resource (ADR-0121,
    // gap-closure task #106) -- real PUT+GET+PATCH+DELETE per TS29505_Subscription_Data.yaml,
    // real distinct 201-vs-204 PUT response codes (same shape as amf-3gpp-access's own resource),
    // real RFC 6902 application/json-patch+json PATCH. ---

    const std::string nidd_authorization_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/context-data/nidd-authorizations";

    server.add_route(
        "PUT",
        nidd_authorization_path_pattern,
        [&verifier,
         &nidd_authorization_info,
         &nidd_authorization_write_counter,
         &subs_to_notify,
         &notify_client,
         nidd_authorization_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::NiddAuthorizationInfo>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            json j = *body;
            const bool is_new = nidd_authorization_info.put(ue_id, j);
            nidd_authorization_write_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(nidd_authorization_path_pattern, req.path_params),
                               change_replace(j));

            if (!is_new) {
                sbi_core::http2::Response resp;
                resp.status = 204;
                return resp;
            }
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace(
                "location", resolved_location(nidd_authorization_path_pattern, req.path_params));
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        nidd_authorization_path_pattern,
        [&verifier, &nidd_authorization_info](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = nidd_authorization_info.get(ue_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No NIDD Authorization Info for ueId " + ue_id);
            }
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PATCH",
        nidd_authorization_path_pattern,
        [&verifier,
         &nidd_authorization_info,
         &nidd_authorization_write_counter,
         &subs_to_notify,
         &notify_client,
         nidd_authorization_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json patch_ops;
            try {
                patch_ops = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto ue_id = req.path_params.at("ueId");
            std::optional<json> patched;
            try {
                patched = nidd_authorization_info.apply_patch(ue_id, patch_ops);
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(400, "Invalid JSON Patch", e.what());
            }
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No NIDD Authorization Info for ueId " + ue_id);
            }
            nidd_authorization_write_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(nidd_authorization_path_pattern, req.path_params),
                               change_from_json_patch(patch_ops));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "DELETE",
        nidd_authorization_path_pattern,
        [&verifier,
         &nidd_authorization_info,
         &nidd_authorization_delete_counter,
         &subs_to_notify,
         &notify_client,
         nidd_authorization_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            if (!nidd_authorization_info.remove(ue_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No NIDD Authorization Info for ueId " + ue_id);
            }
            nidd_authorization_delete_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(nidd_authorization_path_pattern, req.path_params),
                               change_remove());
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nudr_DataRepository: Query/Modify Identity Data by SUPI or GPSI resource (ADR-0122,
    // gap-closure task #106) -- real GET+PATCH per TS29505_Subscription_Data.yaml, no PUT/POST
    // create operation exists, so apply_patch is upsert-capable (same precedent as pp-data). ---

    const std::string identity_data_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/identity-data";

    server.add_route(
        "GET",
        identity_data_path_pattern,
        [&verifier, &identity_data, &identity_data_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = identity_data.get(ue_id);
            identity_data_get_counter->Add(1);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No Identity Data for ueId " + ue_id);
            }
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PATCH",
        identity_data_path_pattern,
        [&verifier,
         &identity_data,
         &identity_data_patch_counter,
         &subs_to_notify,
         &notify_client,
         identity_data_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json patch_ops;
            try {
                patch_ops = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto ue_id = req.path_params.at("ueId");
            json patched;
            try {
                patched = identity_data.apply_patch(ue_id, patch_ops);
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(400, "Invalid JSON Patch", e.what());
            }
            identity_data_patch_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(identity_data_path_pattern, req.path_params),
                               change_from_json_patch(patch_ops));
            return sbi_core::http2::Response::json(200, patched.dump());
        });

    // --- Nudr_DataRepository: Query ODB Data by SUPI or GPSI resource (ADR-0123, gap-closure
    // task #106) -- real GET-only per TS29505_Subscription_Data.yaml, seeded at startup (no
    // create/update operation exists in the spec, same shape as coverage-restriction-data). ---

    const std::string odb_data_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/operator-determined-barring-data";

    server.add_route(
        "GET",
        odb_data_path_pattern,
        [&verifier, &odb_data, &odb_data_get_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = odb_data.get(ue_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No ODB Data for ueId " + ue_id);
            }
            odb_data_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // --- Nudr_DataRepository: V2X Subscription Data resource (ADR-0128, gap-closure task #106)
    // -- real GET-only per TS29505_Subscription_Data.yaml, seeded at startup (no create/update
    // operation exists in the spec, same shape as coverage-restriction-data). Keyed by ueId
    // alone, genuinely NOT part of the provisioned-data group's own composite key shape. ---

    const std::string v2x_data_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/v2x-data";

    server.add_route(
        "GET",
        v2x_data_path_pattern,
        [&verifier, &v2x_data, &v2x_data_get_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = v2x_data.get(ue_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No V2X Subscription Data for ueId " + ue_id);
            }
            v2x_data_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // --- Nudr_DataRepository: ProSe Service Subscription Data resource (ADR-0129, gap-closure
    // task #106) -- real GET-only per TS29505_Subscription_Data.yaml (real spec operationId
    // `QueryPorseData`, a literal spec typo, cited as-is), seeded at startup. Keyed by ueId
    // alone. ---

    const std::string prose_data_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/prose-data";

    server.add_route(
        "GET",
        prose_data_path_pattern,
        [&verifier, &prose_data, &prose_data_get_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = prose_data.get(ue_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No ProSe Service Subscription Data for ueId " + ue_id);
            }
            prose_data_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // --- Nudr_DataRepository: User Consent Subscription Data resource (ADR-0130, gap-closure
    // task #106) -- real GET-only per TS29505_Subscription_Data.yaml (real spec operationId
    // `QueryUserConsentData`), schema `UcSubscriptionData` (TS29503_Nudm_SDM.yaml), seeded at
    // startup. Keyed by ueId alone. ---

    const std::string uc_data_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/uc-data";

    server.add_route(
        "GET",
        uc_data_path_pattern,
        [&verifier, &uc_data, &uc_data_get_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = uc_data.get(ue_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No User Consent Subscription Data for ueId " + ue_id);
            }
            uc_data_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // --- Nudr_DataRepository: Time Synchronization Subscription Data resource (ADR-0131,
    // gap-closure task #106) -- real GET-only per TS29505_Subscription_Data.yaml (real spec
    // operationId `QueryTimeSyncSubscriptionData`), schema `TimeSyncSubscriptionData`
    // (TS29503_Nudm_SDM.yaml), seeded at startup. Keyed by ueId alone. ---

    const std::string time_sync_data_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/time-sync-data";

    server.add_route(
        "GET",
        time_sync_data_path_pattern,
        [&verifier, &time_sync_data, &time_sync_data_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = time_sync_data.get(ue_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404,
                    "Not Found",
                    "No Time Synchronization Subscription Data for ueId " + ue_id);
            }
            time_sync_data_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // --- Nudr_DataRepository: UE's Location Information (Document) resource (ADR-0133,
    // gap-closure task #106) -- real GET-only per TS29505_Subscription_Data.yaml (real spec
    // operationId `QueryUeLocation`), schema `LocationInfo` (TS29503_Nudm_UECM.yaml), seeded at
    // startup. Keyed by ueId alone. ---

    const std::string location_data_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/context-data/location";

    server.add_route(
        "GET",
        location_data_path_pattern,
        [&verifier, &location_data, &location_data_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = location_data.get(ue_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No Location Information for ueId " + ue_id);
            }
            location_data_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // --- Nudr_DataRepository: A2X Subscription Data resource (ADR-0134, gap-closure task #106)
    // -- real GET-only per TS29505_Subscription_Data.yaml (real spec operationId
    // `QueryA2xData`), seeded at startup. Keyed by ueId alone. ---

    const std::string a2x_data_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/a2x-data";

    server.add_route(
        "GET",
        a2x_data_path_pattern,
        [&verifier, &a2x_data, &a2x_data_get_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = a2x_data.get(ue_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No A2X Subscription Data for ueId " + ue_id);
            }
            a2x_data_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // --- Nudr_DataRepository: Ranging and Sidelink Positioning Privacy Subscription Data
    // resource (ADR-0135, gap-closure task #106) -- real GET-only per
    // TS29505_Subscription_Data.yaml (real spec operationId `QueryRangingSlPrivacyData`), seeded
    // at startup. Keyed by ueId alone. Real, disclosed: the spec's own optional `fields` query
    // parameter for field-selection filtering is not honored -- the full stored document is
    // always returned. ---

    const std::string rangingsl_privacy_data_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/rangingsl-privacy-data";

    server.add_route(
        "GET",
        rangingsl_privacy_data_path_pattern,
        [&verifier, &rangingsl_privacy_data, &rangingsl_privacy_data_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = rangingsl_privacy_data.get(ue_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404,
                    "Not Found",
                    "No Ranging and Sidelink Positioning Privacy Subscription Data for ueId " +
                        ue_id);
            }
            rangingsl_privacy_data_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // --- Nudr_DataRepository: Ranging and Sidelink Positioning Service Subscription Data
    // resource (ADR-0136, gap-closure task #106) -- real GET-only per
    // TS29505_Subscription_Data.yaml (real spec operationId `QueryRangingSlPosData`), seeded at
    // startup. Keyed by ueId alone. ---

    const std::string ranging_slpos_data_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/ranging-slpos-data";

    server.add_route(
        "GET",
        ranging_slpos_data_path_pattern,
        [&verifier, &ranging_slpos_data, &ranging_slpos_data_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = ranging_slpos_data.get(ue_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404,
                    "Not Found",
                    "No Ranging and Sidelink Positioning Service Subscription Data for ueId " +
                        ue_id);
            }
            ranging_slpos_data_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // --- Nudr_DataRepository: 5MBS Subscription Data (Document) resource (ADR-0137, gap-closure
    // task #106) -- real GET-only per TS29505_Subscription_Data.yaml (real spec operationId
    // `Query5mbsData`), seeded at startup. Keyed by ueId alone. ---

    const std::string mbs_data_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/5mbs-data";

    server.add_route(
        "GET",
        mbs_data_path_pattern,
        [&verifier, &mbs_data, &mbs_data_get_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = mbs_data.get(ue_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No 5MBS Subscription Data for ueId " + ue_id);
            }
            mbs_data_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // --- Nudr_DataRepository: Service Specific Authorization Info (Document) context-data
    // resource (ADR-0139, gap-closure task #106) -- real PUT+GET+PATCH+DELETE per
    // TS29505_Subscription_Data.yaml, real distinct 201-vs-204 PUT response codes (same shape as
    // nidd-authorizations's own resource), real RFC 6902 application/json-patch+json PATCH.
    // Composite (ueId, serviceType) key. Real, disclosed: the sibling GET-only resource at
    // /subscription-data/{ueId}/service-specific-authorization-data/{serviceType} remains
    // deliberately deferred per ADR-0160, which the user confirmed twice.
    // CORRECTED (ADR-0256): the query-parameter reason recorded here is stale -- ADR-0165's
    // GetNiddAuData established that parsing precedent. The live reason is ADR-0160's: the
    // resource has no PUT counterpart, and the schema it would have to return does not
    // structurally match what this CRUD sibling actually stores, so implementing it means either
    // fabricating a field mapping or shipping a permanently-empty store. See this file's header.
    // ---

    const std::string service_specific_auth_path_pattern =
        std::string(kApiRoot) +
        "/subscription-data/{ueId}/context-data/service-specific-authorizations/{serviceType}";

    server.add_route(
        "PUT",
        service_specific_auth_path_pattern,
        [&verifier,
         &service_specific_authorization_info,
         &service_specific_auth_write_counter,
         &subs_to_notify,
         &notify_client,
         service_specific_auth_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            // Real name collision (ADR-0202): TS29503_Nudm_SSAU.yaml declares its own, distinct
            // ServiceSpecificAuthorizationInfo (a client-facing request schema) -- disambiguated
            // by the codegen to ServiceSpecificAuthorizationInfo_Subscription_Data for this UDR
            // resource's own real TS 29.505 schema.
            auto body = sbi_core::http2::parse_json_body<
                sbi_gen::ServiceSpecificAuthorizationInfo_Subscription_Data>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto service_type = req.path_params.at("serviceType");
            json j = *body;
            const bool is_new = service_specific_authorization_info.put(ue_id, service_type, j);
            service_specific_auth_write_counter->Add(1);
            notify_subscribers(
                subs_to_notify,
                notify_client,
                ue_id,
                resolved_location(service_specific_auth_path_pattern, req.path_params),
                change_replace(j));

            if (!is_new) {
                sbi_core::http2::Response resp;
                resp.status = 204;
                return resp;
            }
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace(
                "location", resolved_location(service_specific_auth_path_pattern, req.path_params));
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        service_specific_auth_path_pattern,
        [&verifier, &service_specific_authorization_info](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto service_type = req.path_params.at("serviceType");
            auto data = service_specific_authorization_info.get(ue_id, service_type);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404,
                    "Not Found",
                    "No Service Specific Authorization Info for ueId/serviceType " + ue_id + "/" +
                        service_type);
            }
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PATCH",
        service_specific_auth_path_pattern,
        [&verifier,
         &service_specific_authorization_info,
         &service_specific_auth_write_counter,
         &subs_to_notify,
         &notify_client,
         service_specific_auth_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json patch_ops;
            try {
                patch_ops = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto service_type = req.path_params.at("serviceType");
            std::optional<json> patched;
            try {
                patched =
                    service_specific_authorization_info.apply_patch(ue_id, service_type, patch_ops);
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(400, "Invalid JSON Patch", e.what());
            }
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404,
                    "Not Found",
                    "No Service Specific Authorization Info for ueId/serviceType " + ue_id + "/" +
                        service_type);
            }
            service_specific_auth_write_counter->Add(1);
            notify_subscribers(
                subs_to_notify,
                notify_client,
                ue_id,
                resolved_location(service_specific_auth_path_pattern, req.path_params),
                change_from_json_patch(patch_ops));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "DELETE",
        service_specific_auth_path_pattern,
        [&verifier,
         &service_specific_authorization_info,
         &service_specific_auth_delete_counter,
         &subs_to_notify,
         &notify_client,
         service_specific_auth_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto service_type = req.path_params.at("serviceType");
            if (!service_specific_authorization_info.remove(ue_id, service_type)) {
                return sbi_core::http2::problem_response(
                    404,
                    "Not Found",
                    "No Service Specific Authorization Info for ueId/serviceType " + ue_id + "/" +
                        service_type);
            }
            service_specific_auth_delete_counter->Add(1);
            notify_subscribers(
                subs_to_notify,
                notify_client,
                ue_id,
                resolved_location(service_specific_auth_path_pattern, req.path_params),
                change_remove());
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nudr_DataRepository: Group Identifiers mapping resource (ADR-0140, gap-closure task
    // #106) -- real GET-only per TS29505_Subscription_Data.yaml (real spec operationId
    // `GetGroupIdentifiers`), seeded at startup. Genuinely NOT per-UE, no path parameters --
    // real, optional query parameters `ext-group-id`/`int-group-id` select the lookup key. Real,
    // disclosed: at least one of the two is required by this implementation (400 otherwise,
    // since the spec defines no "list all groups" behavior this project has any precedent for
    // returning); the real `ue-id-ind` query parameter is not honored -- `ueIdList` is always
    // included in the response regardless. ---

    const std::string group_identifiers_path_pattern =
        std::string(kApiRoot) + "/subscription-data/group-data/group-identifiers";

    server.add_route(
        "GET",
        group_identifiers_path_pattern,
        [&verifier, &group_identifiers, &group_identifiers_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ext_group_id_it = req.query_params.find("ext-group-id");
            const auto int_group_id_it = req.query_params.find("int-group-id");
            std::optional<json> data;
            std::string lookup_desc;
            if (ext_group_id_it != req.query_params.end()) {
                data = group_identifiers.get_by_ext_group_id(ext_group_id_it->second);
                lookup_desc = "ext-group-id " + ext_group_id_it->second;
            } else if (int_group_id_it != req.query_params.end()) {
                data = group_identifiers.get_by_int_group_id(int_group_id_it->second);
                lookup_desc = "int-group-id " + int_group_id_it->second;
            } else {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", "At least one of ext-group-id or int-group-id is required");
            }
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No Group Identifiers for " + lookup_desc);
            }
            group_identifiers_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // --- Nudr_DataRepository: NSSAI update ack (Document) resource (ADR-0141, gap-closure task
    // #106) -- real PUT+GET per TS29505_Subscription_Data.yaml, no PATCH/DELETE operation exists
    // at all. Real, disclosed: unlike this project's other PUT resources, the spec documents only
    // a single `204` response for this PUT (no `201`) -- no create-vs-update distinction. Keyed
    // by ueId. ---

    const std::string nssai_ack_data_path_pattern =
        std::string(kApiRoot) +
        "/subscription-data/{ueId}/ue-update-confirmation-data/subscribed-snssais";

    server.add_route(
        "PUT",
        nssai_ack_data_path_pattern,
        [&verifier,
         &nssai_ack_data,
         &nssai_ack_data_write_counter,
         &subs_to_notify,
         &notify_client,
         nssai_ack_data_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::NssaiAckData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            json j = *body;
            nssai_ack_data.put(ue_id, j);
            nssai_ack_data_write_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(nssai_ack_data_path_pattern, req.path_params),
                               change_replace(j));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "GET",
        nssai_ack_data_path_pattern,
        [&verifier, &nssai_ack_data, &nssai_ack_data_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = nssai_ack_data.get(ue_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No NSSAI Ack Data for ueId " + ue_id);
            }
            nssai_ack_data_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // --- Nudr_DataRepository: CAG update ack (Document) resource (ADR-0142, gap-closure task
    // #106) -- real PUT+GET per TS29505_Subscription_Data.yaml, no PATCH/DELETE operation exists
    // at all, identical shape to subscribed-snssais's own resource above. Real, disclosed: the
    // spec documents only a single `204` response for this PUT (no `201`) -- no create-vs-update
    // distinction. Keyed by ueId. ---

    const std::string cag_ack_data_path_pattern =
        std::string(kApiRoot) +
        "/subscription-data/{ueId}/ue-update-confirmation-data/subscribed-cag";

    server.add_route(
        "PUT",
        cag_ack_data_path_pattern,
        [&verifier,
         &cag_ack_data,
         &cag_ack_data_write_counter,
         &subs_to_notify,
         &notify_client,
         cag_ack_data_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::CagAckData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            json j = *body;
            cag_ack_data.put(ue_id, j);
            cag_ack_data_write_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(cag_ack_data_path_pattern, req.path_params),
                               change_replace(j));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "GET",
        cag_ack_data_path_pattern,
        [&verifier, &cag_ack_data, &cag_ack_data_get_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = cag_ack_data.get(ue_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No CAG Ack Data for ueId " + ue_id);
            }
            cag_ack_data_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // --- Nudr_DataRepository: Authentication SoR (Document) resource (ADR-0143, gap-closure task
    // #106) -- real PUT+GET+PATCH per TS29505_Subscription_Data.yaml. Real, disclosed: same as
    // subscribed-snssais/subscribed-cag above, the spec documents only a single `204` response for
    // this PUT (no `201`) -- no create-vs-update distinction. Genuinely richer than either ack
    // resource: a real RFC 6902 application/json-patch+json PATCH also exists (apply_patch is NOT
    // upsert-capable -- requires a prior PUT, same precedent as nidd-authorizations). Keyed by
    // ueId. ---

    const std::string sor_data_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/ue-update-confirmation-data/sor-data";

    server.add_route(
        "PUT",
        sor_data_path_pattern,
        [&verifier,
         &sor_data,
         &sor_data_write_counter,
         &subs_to_notify,
         &notify_client,
         sor_data_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::SorData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            json j = *body;
            sor_data.put(ue_id, j);
            sor_data_write_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(sor_data_path_pattern, req.path_params),
                               change_replace(j));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "GET",
        sor_data_path_pattern,
        [&verifier, &sor_data, &sor_data_get_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = sor_data.get(ue_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No SoR Data for ueId " + ue_id);
            }
            sor_data_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PATCH",
        sor_data_path_pattern,
        [&verifier,
         &sor_data,
         &sor_data_write_counter,
         &subs_to_notify,
         &notify_client,
         sor_data_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json patch_ops;
            try {
                patch_ops = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto ue_id = req.path_params.at("ueId");
            std::optional<json> patched;
            try {
                patched = sor_data.apply_patch(ue_id, patch_ops);
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(400, "Invalid JSON Patch", e.what());
            }
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No SoR Data for ueId " + ue_id);
            }
            sor_data_write_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(sor_data_path_pattern, req.path_params),
                               change_from_json_patch(patch_ops));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nudr_DataRepository: Authentication UPU (Document) resource (ADR-0143, gap-closure task
    // #106) -- real PUT+GET only per TS29505_Subscription_Data.yaml, no PATCH/DELETE operation
    // exists at all -- genuinely narrower than sor-data's own resource above despite sharing the
    // same UeUpdateStatus-based schema shape. Real, disclosed: same 204-only PUT, no
    // create-vs-update distinction. Keyed by ueId. ---

    const std::string upu_data_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/ue-update-confirmation-data/upu-data";

    server.add_route(
        "PUT",
        upu_data_path_pattern,
        [&verifier,
         &upu_data,
         &upu_data_write_counter,
         &subs_to_notify,
         &notify_client,
         upu_data_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::UpuData_Subscription_Data>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            json j = *body;
            upu_data.put(ue_id, j);
            upu_data_write_counter->Add(1);
            notify_subscribers(subs_to_notify,
                               notify_client,
                               ue_id,
                               resolved_location(upu_data_path_pattern, req.path_params),
                               change_replace(j));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "GET",
        upu_data_path_pattern,
        [&verifier, &upu_data, &upu_data_get_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto data = upu_data.get(ue_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No UPU Data for ueId " + ue_id);
            }
            upu_data_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // --- Nudr_DataRepository: aggregate UE Update Confirmation Data resource (ADR-0147,
    // gap-closure task #106) -- real GET-only per TS29505_Subscription_Data.yaml, no
    // create/update operation exists in the spec at all. Real, disclosed design decision: this
    // resource is a real aggregate VIEW over the four already-closed individual sub-resources
    // (subscribed-snssais/subscribed-cag/sor-data/upu-data, ADR-0141/ADR-0142/ADR-0143) -- composed
    // live from their own existing stores at request time, not a fifth, duplicate table. The spec
    // documents a `404` response code but every field on `UeUpdConfData` is optional and this
    // "document" isn't itself a stored entity, only a live composition of four independently
    // stored/absent sub-resources -- so this route always returns `200`, omitting whichever
    // sub-resources are absent (an empty object if all four are absent), rather than inventing a
    // rule for when the aggregate itself should 404. ---

    const std::string ue_upd_conf_data_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/ue-update-confirmation-data";

    server.add_route(
        "GET",
        ue_upd_conf_data_path_pattern,
        [&verifier,
         &sor_data,
         &upu_data,
         &nssai_ack_data,
         &cag_ack_data,
         &ue_upd_conf_data_get_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            json result = json::object();
            if (auto v = sor_data.get(ue_id); v.has_value()) {
                result["sorData"] = *v;
            }
            if (auto v = upu_data.get(ue_id); v.has_value()) {
                result["upuData"] = *v;
            }
            if (auto v = nssai_ack_data.get(ue_id); v.has_value()) {
                result["nssaiAckData"] = *v;
            }
            if (auto v = cag_ack_data.get(ue_id); v.has_value()) {
                result["cagAckData"] = *v;
            }
            ue_upd_conf_data_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, result.dump());
        });

    // --- Nudr_DataRepository: Context Data (Document), aggregate resource (ADR-0161, gap-closure
    // task #106) -- real GET-only per TS29505_Subscription_Data.yaml, real REQUIRED
    // `context-dataset-names` array query param (`style: form, explode: false`), the first
    // resource in this project unblocked by the new `sbi_core::http2::split_form_array()` helper
    // (docs/DECISIONS.md ADR-0161). Same real, disclosed live-composition design as
    // `ue-update-confirmation-data` (ADR-0147): this "document" is a live VIEW over 11 already
    // independently-stored sub-resources, composed at request time from their own existing
    // stores -- not a twelfth, duplicate table. Real, disclosed: `context-dataset-names`' own
    // schema (`ContextDataSetName`) documents a real forward-compatible "any other string" case
    // alongside its 11 known enum values -- an unrecognized name is silently skipped (nothing to
    // populate for it), not rejected, matching the spec's own forward-compatibility intent.
    // Missing the required query param is a real `400`. ---

    const std::string context_data_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/context-data";

    server.add_route(
        "GET",
        context_data_path_pattern,
        [&verifier,
         &amf_contexts,
         &amf_non3gpp_contexts,
         &sdm_subscriptions,
         &ee_subscriptions,
         &smsf_3gpp_context,
         &smsf_non3gpp_context,
         &subs_to_notify,
         &smf_registrations,
         &ip_sm_gw_context,
         &roaming_information,
         &pei_info,
         &context_data_get_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto names_it = req.query_params.find("context-dataset-names");
            if (names_it == req.query_params.end()) {
                return sbi_core::http2::problem_response(
                    400,
                    "Bad Request",
                    "Required query parameter context-dataset-names is missing");
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto names = sbi_core::http2::split_form_array(names_it->second);
            json result = json::object();
            for (const auto& name : names) {
                if (name == "AMF_3GPP") {
                    if (auto v = amf_contexts.get(ue_id); v.has_value()) {
                        result["amf3Gpp"] = *v;
                    }
                } else if (name == "AMF_NON_3GPP") {
                    if (auto v = amf_non3gpp_contexts.get(ue_id); v.has_value()) {
                        result["amfNon3Gpp"] = *v;
                    }
                } else if (name == "SDM_SUBSCRIPTIONS") {
                    if (auto list = sdm_subscriptions.list(ue_id); !list.empty()) {
                        result["sdmSubscriptions"] = json(list);
                    }
                } else if (name == "EE_SUBSCRIPTIONS") {
                    if (auto list = ee_subscriptions.list(ue_id); !list.empty()) {
                        result["eeSubscriptions"] = json(list);
                    }
                } else if (name == "SMSF_3GPP") {
                    if (auto v = smsf_3gpp_context.get(ue_id); v.has_value()) {
                        result["smsf3GppAccess"] = *v;
                    }
                } else if (name == "SMSF_NON_3GPP") {
                    if (auto v = smsf_non3gpp_context.get(ue_id); v.has_value()) {
                        result["smsfNon3GppAccess"] = *v;
                    }
                } else if (name == "SUBS_TO_NOTIFY") {
                    if (auto list = subs_to_notify.list_by_ue_id(ue_id); !list.empty()) {
                        result["subscriptionDataSubscriptions"] = json(list);
                    }
                } else if (name == "SMF_REG") {
                    if (auto list = smf_registrations.list_for_ue(ue_id); !list.empty()) {
                        result["smfRegistrations"] = json(list);
                    }
                } else if (name == "IP_SM_GW") {
                    if (auto v = ip_sm_gw_context.get(ue_id); v.has_value()) {
                        result["ipSmGw"] = *v;
                    }
                } else if (name == "ROAMING_INFO") {
                    if (auto v = roaming_information.get(ue_id); v.has_value()) {
                        result["roamingInfo"] = *v;
                    }
                } else if (name == "PEI_INFO") {
                    if (auto v = pei_info.get(ue_id); v.has_value()) {
                        result["peiInfo"] = *v;
                    }
                }
                // Real, disclosed: an unrecognized name (the spec's own documented
                // forward-compatible "any other string" case) is silently skipped.
            }
            context_data_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, result.dump());
        });

    // --- Nudr_DataRepository: bare UE Subscribed Data (Document), aggregate resource (ADR-0166,
    // gap-closure task #106) -- real GET-only per TS29505_Subscription_Data.yaml,
    // `QueryUeSubscribedData`. Real, disclosed: `UeSubscribedDataSets = ProvisionedDataSets &
    // ContextDataSets & UeUpdConfData` (an `allOf` of three object schemas) -- a live VIEW over
    // 32 already-independently-stored sub-resources (21 `ProvisionedDataSets` fields, the same 11
    // `ContextDataSets` fields `QueryContextData` (ADR-0161) already composes, plus the 4
    // `UeUpdConfData` fields `ue-update-confirmation-data` (ADR-0147) already composes), not a
    // 33rd, duplicate table. All of this resource's own query parameters are OPTIONAL (unlike
    // `QueryContextData`'s REQUIRED `context-dataset-names`): the real, optional `dataset-names`
    // array filter (`style: form, explode: false`) is honored when present (values match the real
    // `UeSubscribedDataSetName` enum -- the union of `ContextDataSetName`,
    // `ProvisionedDataSetName`, and the literal `UE_UPD_CONF`, which expands to all four
    // `UeUpdConfData` sub-fields at once); when absent, every composable field is attempted (no
    // filter). Real, disclosed partial- composability gap: the real, optional `serving-plmn` query
    // param is required by the underlying `ProvisionedDataStore`'s own `(ueId, servingPlmnId)`
    // composite key (confirmed by direct read of the already-implemented individual
    // `provisioned-data` routes) -- when `serving-plmn` is absent, the 7 `ProvisionedDataSets`
    // fields it backs (`amData`/`smfSelData`/
    // `smsSubsData`/`smData`/`traceData`/`smsMngData`/`lcsBcaData`) are skipped, not fabricated
    // with a guessed PLMN. `niddAuthData` (`AuthorizationData`, ADR-0165) is never composed here:
    // its own real composite key requires `mtc-provider-information`, a query param this resource
    // does not expose at all -- a genuine, disclosed gap, not an oversight. The real, optional
    // `adjacent-plmns`/`single-nssai`/`dnn`/`ext-group-ids`/`uc-purpose` query params are accepted
    // but not honored (matching the established "optional filter not honored" precedent) -- no
    // existing store supports filtering by them. Same real, disclosed `200`-always design as
    // `QueryContextData`/`ue-update-confirmation-data`: this is a live view with no independent
    // existence, so an empty result (all fields absent) is a real `200 {}`, not a `404`. ---

    const std::string ue_subscribed_data_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}";

    server.add_route(
        "GET",
        ue_subscribed_data_path_pattern,
        [&verifier,
         &provisioned_data,
         &lcs_privacy_data,
         &lcs_mo_data,
         &lcs_subscription_data,
         &v2x_data,
         &prose_data,
         &odb_data,
         &ee_profile_data,
         &pp_profile_data,
         &uc_data,
         &mbs_data,
         &pp_data,
         &a2x_data,
         &rangingsl_privacy_data,
         &amf_contexts,
         &amf_non3gpp_contexts,
         &sdm_subscriptions,
         &ee_subscriptions,
         &smsf_3gpp_context,
         &smsf_non3gpp_context,
         &subs_to_notify,
         &smf_registrations,
         &ip_sm_gw_context,
         &roaming_information,
         &pei_info,
         &sor_data,
         &upu_data,
         &nssai_ack_data,
         &cag_ack_data,
         &ue_subscribed_data_get_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            std::vector<std::string> names;
            if (const auto names_it = req.query_params.find("dataset-names");
                names_it != req.query_params.end()) {
                names = sbi_core::http2::split_form_array(names_it->second);
            }
            const auto wanted = [&names](const char* name) {
                return names.empty() || std::find(names.begin(), names.end(), name) != names.end();
            };
            const auto serving_plmn_it = req.query_params.find("serving-plmn");
            const bool have_serving_plmn = serving_plmn_it != req.query_params.end();

            json result = json::object();

            if (have_serving_plmn) {
                const auto& serving_plmn_id = serving_plmn_it->second;
                if (wanted("AM")) {
                    if (auto v = provisioned_data.get_am_data(ue_id, serving_plmn_id);
                        v.has_value()) {
                        result["amData"] = *v;
                    }
                }
                if (wanted("SMF_SEL")) {
                    if (auto v = provisioned_data.get_smf_sel_data(ue_id, serving_plmn_id);
                        v.has_value()) {
                        result["smfSelData"] = *v;
                    }
                }
                if (wanted("SMS_SUB")) {
                    if (auto v = provisioned_data.get_sms_data(ue_id, serving_plmn_id);
                        v.has_value()) {
                        result["smsSubsData"] = *v;
                    }
                }
                if (wanted("SM")) {
                    if (auto v = provisioned_data.get_sm_data(ue_id, serving_plmn_id);
                        v.has_value()) {
                        result["smData"] = *v;
                    }
                }
                if (wanted("TRACE")) {
                    if (auto v = provisioned_data.get_trace_data(ue_id, serving_plmn_id);
                        v.has_value()) {
                        result["traceData"] = *v;
                    }
                }
                if (wanted("SMS_MNG")) {
                    if (auto v = provisioned_data.get_sms_mng_data(ue_id, serving_plmn_id);
                        v.has_value()) {
                        result["smsMngData"] = *v;
                    }
                }
                if (wanted("LCS_BCA")) {
                    if (auto v = provisioned_data.get_lcs_bca_data(ue_id, serving_plmn_id);
                        v.has_value()) {
                        result["lcsBcaData"] = *v;
                    }
                }
            }
            if (wanted("LCS_PRIVACY")) {
                if (auto v = lcs_privacy_data.get(ue_id); v.has_value()) {
                    result["lcsPrivacyData"] = *v;
                }
            }
            if (wanted("LCS_MO")) {
                if (auto v = lcs_mo_data.get(ue_id); v.has_value()) {
                    result["lcsMoData"] = *v;
                }
            }
            if (wanted("LCS_SUB")) {
                if (auto v = lcs_subscription_data.get(ue_id); v.has_value()) {
                    result["lcsSubscriptionData"] = *v;
                }
            }
            if (wanted("V2X")) {
                if (auto v = v2x_data.get(ue_id); v.has_value()) {
                    result["v2xData"] = *v;
                }
            }
            if (wanted("PROSE")) {
                if (auto v = prose_data.get(ue_id); v.has_value()) {
                    result["proseData"] = *v;
                }
            }
            if (wanted("ODB")) {
                if (auto v = odb_data.get(ue_id); v.has_value()) {
                    result["odbData"] = *v;
                }
            }
            if (wanted("EE_PROF")) {
                if (auto v = ee_profile_data.get(ue_id); v.has_value()) {
                    result["eeProfileData"] = *v;
                }
            }
            if (wanted("PP_PROF")) {
                if (auto v = pp_profile_data.get(ue_id); v.has_value()) {
                    result["ppProfileData"] = *v;
                }
            }
            // NIDD_AUTH deliberately never composed here -- see this block's own header comment.
            if (wanted("USER_CONSENT")) {
                if (auto v = uc_data.get(ue_id); v.has_value()) {
                    result["ucData"] = *v;
                }
            }
            if (wanted("MBS")) {
                if (auto v = mbs_data.get(ue_id); v.has_value()) {
                    result["mbsSubscriptionData"] = *v;
                }
            }
            if (wanted("PP_DATA")) {
                if (auto v = pp_data.get(ue_id); v.has_value()) {
                    result["ppData"] = *v;
                }
            }
            if (wanted("A2X")) {
                if (auto v = a2x_data.get(ue_id); v.has_value()) {
                    result["a2xData"] = *v;
                }
            }
            if (wanted("RANGINGSL_PRIVACY")) {
                if (auto v = rangingsl_privacy_data.get(ue_id); v.has_value()) {
                    result["rangingSlPrivacyData"] = *v;
                }
            }
            if (wanted("AMF_3GPP")) {
                if (auto v = amf_contexts.get(ue_id); v.has_value()) {
                    result["amf3Gpp"] = *v;
                }
            }
            if (wanted("AMF_NON_3GPP")) {
                if (auto v = amf_non3gpp_contexts.get(ue_id); v.has_value()) {
                    result["amfNon3Gpp"] = *v;
                }
            }
            if (wanted("SDM_SUBSCRIPTIONS")) {
                if (auto list = sdm_subscriptions.list(ue_id); !list.empty()) {
                    result["sdmSubscriptions"] = json(list);
                }
            }
            if (wanted("EE_SUBSCRIPTIONS")) {
                if (auto list = ee_subscriptions.list(ue_id); !list.empty()) {
                    result["eeSubscriptions"] = json(list);
                }
            }
            if (wanted("SMSF_3GPP")) {
                if (auto v = smsf_3gpp_context.get(ue_id); v.has_value()) {
                    result["smsf3GppAccess"] = *v;
                }
            }
            if (wanted("SMSF_NON_3GPP")) {
                if (auto v = smsf_non3gpp_context.get(ue_id); v.has_value()) {
                    result["smsfNon3GppAccess"] = *v;
                }
            }
            if (wanted("SUBS_TO_NOTIFY")) {
                if (auto list = subs_to_notify.list_by_ue_id(ue_id); !list.empty()) {
                    result["subscriptionDataSubscriptions"] = json(list);
                }
            }
            if (wanted("SMF_REG")) {
                if (auto list = smf_registrations.list_for_ue(ue_id); !list.empty()) {
                    result["smfRegistrations"] = json(list);
                }
            }
            if (wanted("IP_SM_GW")) {
                if (auto v = ip_sm_gw_context.get(ue_id); v.has_value()) {
                    result["ipSmGw"] = *v;
                }
            }
            if (wanted("ROAMING_INFO")) {
                if (auto v = roaming_information.get(ue_id); v.has_value()) {
                    result["roamingInfo"] = *v;
                }
            }
            if (wanted("PEI_INFO")) {
                if (auto v = pei_info.get(ue_id); v.has_value()) {
                    result["peiInfo"] = *v;
                }
            }
            if (wanted("UE_UPD_CONF")) {
                if (auto v = sor_data.get(ue_id); v.has_value()) {
                    result["sorData"] = *v;
                }
                if (auto v = upu_data.get(ue_id); v.has_value()) {
                    result["upuData"] = *v;
                }
                if (auto v = nssai_ack_data.get(ue_id); v.has_value()) {
                    result["nssaiAckData"] = *v;
                }
                if (auto v = cag_ack_data.get(ue_id); v.has_value()) {
                    result["cagAckData"] = *v;
                }
            }
            // Real, disclosed: an unrecognized name (the spec's own documented forward-compatible
            // "any other string" case, see `ProvisionedDataSetName`'s own schema) contributes
            // nothing and is silently skipped, same precedent as `QueryContextData`.

            ue_subscribed_data_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, result.dump());
        });

    // --- Nudr_DataRepository: Event Exposure Subscriptions collection + individual document
    // (ADR-0148, gap-closure task #106) -- real GET+POST on the collection, GET+PUT+PATCH+DELETE
    // on the individual document, per TS29505_Subscription_Data.yaml. Real, disclosed: `subsId`
    // is server-generated (real UUID v4), the collection GET does not honor its own real, genuinely
    // optional `event-types`/`nf-identifiers` array query-param filters (always returns the full
    // list), and PUT is genuinely update-only -- never create (real spec 404 for a nonexistent
    // resource). Scope, disclosed: only the collection + individual document, NOT the deeper
    // amf-subscriptions/smf-subscriptions/hss-subscriptions nested sub-collections under each
    // subsId (remain genuinely deferred). ---

    const std::string ee_subscriptions_collection_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/context-data/ee-subscriptions";
    const std::string ee_subscriptions_individual_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/context-data/ee-subscriptions/{subsId}";

    server.add_route(
        "GET",
        ee_subscriptions_collection_path_pattern,
        [&verifier, &ee_subscriptions, &ee_subscriptions_list_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto subs = ee_subscriptions.list(ue_id);
            json arr = json::array();
            for (auto& s : subs) {
                arr.push_back(std::move(s));
            }
            ee_subscriptions_list_counter->Add(1);
            return sbi_core::http2::Response::json(200, arr.dump());
        });

    server.add_route(
        "POST",
        ee_subscriptions_collection_path_pattern,
        [&verifier,
         &ee_subscriptions,
         &ee_subscriptions_create_counter,
         ee_subscriptions_collection_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::EeSubscription>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto subs_id = sbi_core::generate_uuid_v4();
            json j = *body;
            ee_subscriptions.create(ue_id, subs_id, j);
            ee_subscriptions_create_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            // Bug fix (ADR-0156, refactored onto the shared helper in ADR-0157): real ueId, not
            // the unsubstituted {ueId} route-pattern placeholder.
            resp.headers.emplace(
                "location",
                resolved_location(ee_subscriptions_collection_path_pattern, req.path_params) + "/" +
                    subs_id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        ee_subscriptions_individual_path_pattern,
        [&verifier, &ee_subscriptions, &ee_subscriptions_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto subs_id = req.path_params.at("subsId");
            auto data = ee_subscriptions.get(ue_id, subs_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No EE Subscription for subsId " + subs_id);
            }
            ee_subscriptions_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PUT",
        ee_subscriptions_individual_path_pattern,
        [&verifier,
         &ee_subscriptions,
         &ee_subscriptions_write_counter,
         &subs_to_notify,
         &notify_client,
         ee_subscriptions_individual_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::EeSubscription>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto subs_id = req.path_params.at("subsId");
            json j = *body;
            // Real spec: UpdateEesubscriptions is genuinely update-only -- "update of
            // non-existing resource is rejected" (real 404), no create-via-PUT path exists.
            if (!ee_subscriptions.update(ue_id, subs_id, j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No EE Subscription for subsId " + subs_id);
            }
            ee_subscriptions_write_counter->Add(1);
            notify_subscribers(
                subs_to_notify,
                notify_client,
                ue_id,
                resolved_location(ee_subscriptions_individual_path_pattern, req.path_params),
                change_replace(j));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "PATCH",
        ee_subscriptions_individual_path_pattern,
        [&verifier,
         &ee_subscriptions,
         &ee_subscriptions_write_counter,
         &subs_to_notify,
         &notify_client,
         ee_subscriptions_individual_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json patch_ops;
            try {
                patch_ops = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto subs_id = req.path_params.at("subsId");
            std::optional<json> patched;
            try {
                patched = ee_subscriptions.apply_patch(ue_id, subs_id, patch_ops);
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(400, "Invalid JSON Patch", e.what());
            }
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No EE Subscription for subsId " + subs_id);
            }
            ee_subscriptions_write_counter->Add(1);
            notify_subscribers(
                subs_to_notify,
                notify_client,
                ue_id,
                resolved_location(ee_subscriptions_individual_path_pattern, req.path_params),
                change_from_json_patch(patch_ops));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "DELETE",
        ee_subscriptions_individual_path_pattern,
        [&verifier,
         &ee_subscriptions,
         &ee_subscriptions_delete_counter,
         &subs_to_notify,
         &notify_client,
         ee_subscriptions_individual_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto subs_id = req.path_params.at("subsId");
            if (!ee_subscriptions.remove(ue_id, subs_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No EE Subscription for subsId " + subs_id);
            }
            ee_subscriptions_delete_counter->Add(1);
            notify_subscribers(
                subs_to_notify,
                notify_client,
                ue_id,
                resolved_location(ee_subscriptions_individual_path_pattern, req.path_params),
                change_remove());
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nudr_DataRepository: SDM Subscriptions collection + individual document (ADR-0151,
    // gap-closure task #106) -- real GET+POST on the collection, GET+PUT+PATCH+DELETE on the
    // individual document, per TS29505_Subscription_Data.yaml. Structurally identical to
    // ee-subscriptions above: server-generated subsId (real UUID v4), PUT genuinely update-only
    // (real spec 404 for a nonexistent resource). Corrects ADR-0122's own blanket
    // "genuinely deeply-nested" characterization of ee-subscriptions/sdm-subscriptions together --
    // on direct read, only the deeper hss-sdm-subscriptions nested sub-collection remains
    // genuinely deferred. ---

    const std::string sdm_subscriptions_collection_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/context-data/sdm-subscriptions";
    const std::string sdm_subscriptions_individual_path_pattern =
        std::string(kApiRoot) + "/subscription-data/{ueId}/context-data/sdm-subscriptions/{subsId}";

    server.add_route(
        "GET",
        sdm_subscriptions_collection_path_pattern,
        [&verifier, &sdm_subscriptions, &sdm_subscriptions_list_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto subs = sdm_subscriptions.list(ue_id);
            json arr = json::array();
            for (auto& s : subs) {
                arr.push_back(std::move(s));
            }
            sdm_subscriptions_list_counter->Add(1);
            return sbi_core::http2::Response::json(200, arr.dump());
        });

    server.add_route(
        "POST",
        sdm_subscriptions_collection_path_pattern,
        [&verifier,
         &sdm_subscriptions,
         &sdm_subscriptions_create_counter,
         sdm_subscriptions_collection_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::SdmSubscription>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto subs_id = sbi_core::generate_uuid_v4();
            json j = *body;
            sdm_subscriptions.create(ue_id, subs_id, j);
            sdm_subscriptions_create_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            // Bug fix (ADR-0156, refactored onto the shared helper in ADR-0157): real ueId, not
            // the unsubstituted {ueId} route-pattern placeholder.
            resp.headers.emplace(
                "location",
                resolved_location(sdm_subscriptions_collection_path_pattern, req.path_params) +
                    "/" + subs_id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        sdm_subscriptions_individual_path_pattern,
        [&verifier, &sdm_subscriptions, &sdm_subscriptions_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto subs_id = req.path_params.at("subsId");
            auto data = sdm_subscriptions.get(ue_id, subs_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No SDM Subscription for subsId " + subs_id);
            }
            sdm_subscriptions_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PUT",
        sdm_subscriptions_individual_path_pattern,
        [&verifier,
         &sdm_subscriptions,
         &sdm_subscriptions_write_counter,
         &subs_to_notify,
         &notify_client,
         sdm_subscriptions_individual_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::SdmSubscription>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto subs_id = req.path_params.at("subsId");
            json j = *body;
            // Real spec: Updatesdmsubscriptions is genuinely update-only -- "update of
            // non-existing resource is rejected" (real 404), no create-via-PUT path exists.
            if (!sdm_subscriptions.update(ue_id, subs_id, j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No SDM Subscription for subsId " + subs_id);
            }
            sdm_subscriptions_write_counter->Add(1);
            notify_subscribers(
                subs_to_notify,
                notify_client,
                ue_id,
                resolved_location(sdm_subscriptions_individual_path_pattern, req.path_params),
                change_replace(j));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "PATCH",
        sdm_subscriptions_individual_path_pattern,
        [&verifier,
         &sdm_subscriptions,
         &sdm_subscriptions_write_counter,
         &subs_to_notify,
         &notify_client,
         sdm_subscriptions_individual_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json patch_ops;
            try {
                patch_ops = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto subs_id = req.path_params.at("subsId");
            std::optional<json> patched;
            try {
                patched = sdm_subscriptions.apply_patch(ue_id, subs_id, patch_ops);
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(400, "Invalid JSON Patch", e.what());
            }
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No SDM Subscription for subsId " + subs_id);
            }
            sdm_subscriptions_write_counter->Add(1);
            notify_subscribers(
                subs_to_notify,
                notify_client,
                ue_id,
                resolved_location(sdm_subscriptions_individual_path_pattern, req.path_params),
                change_from_json_patch(patch_ops));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "DELETE",
        sdm_subscriptions_individual_path_pattern,
        [&verifier,
         &sdm_subscriptions,
         &sdm_subscriptions_delete_counter,
         &subs_to_notify,
         &notify_client,
         sdm_subscriptions_individual_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto subs_id = req.path_params.at("subsId");
            if (!sdm_subscriptions.remove(ue_id, subs_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No SDM Subscription for subsId " + subs_id);
            }
            sdm_subscriptions_delete_counter->Add(1);
            notify_subscribers(
                subs_to_notify,
                notify_client,
                ue_id,
                resolved_location(sdm_subscriptions_individual_path_pattern, req.path_params),
                change_remove());
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nudr_DataRepository: AMF Subscription Info (Document), nested under an individual
    // ee-subscription (ADR-0152, gap-closure task #106) -- real GET+PUT+PATCH+DELETE per
    // TS29505_Subscription_Data.yaml. Real, disclosed: the document body is a JSON ARRAY of
    // AmfSubscriptionInfo (minItems 1), not a single object. Real, distinct 201-vs-204 PUT (same
    // is-new-tracking precedent as amf-3gpp-access). First of ee-subscriptions' own nested
    // sub-collections closed. ---

    const std::string ee_amf_subscription_info_path_pattern =
        std::string(kApiRoot) +
        "/subscription-data/{ueId}/context-data/ee-subscriptions/{subsId}/amf-subscriptions";

    server.add_route(
        "GET",
        ee_amf_subscription_info_path_pattern,
        [&verifier, &ee_amf_subscription_info, &ee_amf_subscription_info_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto subs_id = req.path_params.at("subsId");
            auto data = ee_amf_subscription_info.get(ue_id, subs_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No AMF Subscription Info for subsId " + subs_id);
            }
            ee_amf_subscription_info_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PUT",
        ee_amf_subscription_info_path_pattern,
        [&verifier,
         &ee_amf_subscription_info,
         &ee_amf_subscription_info_write_counter,
         &subs_to_notify,
         &notify_client,
         ee_amf_subscription_info_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<std::vector<sbi_gen::AmfSubscriptionInfo>>(
                req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto subs_id = req.path_params.at("subsId");
            json j = *body;
            const bool is_new = ee_amf_subscription_info.put(ue_id, subs_id, j);
            ee_amf_subscription_info_write_counter->Add(1);
            notify_subscribers(
                subs_to_notify,
                notify_client,
                ue_id,
                resolved_location(ee_amf_subscription_info_path_pattern, req.path_params),
                change_replace(j));

            if (!is_new) {
                sbi_core::http2::Response resp;
                resp.status = 204;
                return resp;
            }
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace(
                "location",
                resolved_location(ee_amf_subscription_info_path_pattern, req.path_params));
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "PATCH",
        ee_amf_subscription_info_path_pattern,
        [&verifier,
         &ee_amf_subscription_info,
         &ee_amf_subscription_info_write_counter,
         &subs_to_notify,
         &notify_client,
         ee_amf_subscription_info_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json patch_ops;
            try {
                patch_ops = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto subs_id = req.path_params.at("subsId");
            std::optional<json> patched;
            try {
                patched = ee_amf_subscription_info.apply_patch(ue_id, subs_id, patch_ops);
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(400, "Invalid JSON Patch", e.what());
            }
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No AMF Subscription Info for subsId " + subs_id);
            }
            ee_amf_subscription_info_write_counter->Add(1);
            notify_subscribers(
                subs_to_notify,
                notify_client,
                ue_id,
                resolved_location(ee_amf_subscription_info_path_pattern, req.path_params),
                change_from_json_patch(patch_ops));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "DELETE",
        ee_amf_subscription_info_path_pattern,
        [&verifier,
         &ee_amf_subscription_info,
         &ee_amf_subscription_info_delete_counter,
         &subs_to_notify,
         &notify_client,
         ee_amf_subscription_info_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto subs_id = req.path_params.at("subsId");
            if (!ee_amf_subscription_info.remove(ue_id, subs_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No AMF Subscription Info for subsId " + subs_id);
            }
            ee_amf_subscription_info_delete_counter->Add(1);
            notify_subscribers(
                subs_to_notify,
                notify_client,
                ue_id,
                resolved_location(ee_amf_subscription_info_path_pattern, req.path_params),
                change_remove());
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nudr_DataRepository: SMF Event Subscription Info (Document), nested under an
    // individual ee-subscription (ADR-0153, gap-closure task #106) -- real GET+PUT+PATCH+DELETE
    // per TS29505_Subscription_Data.yaml. Real, disclosed: unlike its sibling amf-subscriptions,
    // the document body is a SINGLE SmfSubscriptionInfo object, not an array. Real, distinct
    // 201-vs-204 PUT. Second of ee-subscriptions' own nested sub-collections closed. ---

    const std::string ee_smf_subscription_info_path_pattern =
        std::string(kApiRoot) +
        "/subscription-data/{ueId}/context-data/ee-subscriptions/{subsId}/smf-subscriptions";

    server.add_route(
        "GET",
        ee_smf_subscription_info_path_pattern,
        [&verifier, &ee_smf_subscription_info, &ee_smf_subscription_info_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto subs_id = req.path_params.at("subsId");
            auto data = ee_smf_subscription_info.get(ue_id, subs_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No SMF Subscription Info for subsId " + subs_id);
            }
            ee_smf_subscription_info_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PUT",
        ee_smf_subscription_info_path_pattern,
        [&verifier,
         &ee_smf_subscription_info,
         &ee_smf_subscription_info_write_counter,
         &subs_to_notify,
         &notify_client,
         ee_smf_subscription_info_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::SmfSubscriptionInfo>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto subs_id = req.path_params.at("subsId");
            json j = *body;
            const bool is_new = ee_smf_subscription_info.put(ue_id, subs_id, j);
            ee_smf_subscription_info_write_counter->Add(1);
            notify_subscribers(
                subs_to_notify,
                notify_client,
                ue_id,
                resolved_location(ee_smf_subscription_info_path_pattern, req.path_params),
                change_replace(j));

            if (!is_new) {
                sbi_core::http2::Response resp;
                resp.status = 204;
                return resp;
            }
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace(
                "location",
                resolved_location(ee_smf_subscription_info_path_pattern, req.path_params));
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "PATCH",
        ee_smf_subscription_info_path_pattern,
        [&verifier,
         &ee_smf_subscription_info,
         &ee_smf_subscription_info_write_counter,
         &subs_to_notify,
         &notify_client,
         ee_smf_subscription_info_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json patch_ops;
            try {
                patch_ops = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto subs_id = req.path_params.at("subsId");
            std::optional<json> patched;
            try {
                patched = ee_smf_subscription_info.apply_patch(ue_id, subs_id, patch_ops);
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(400, "Invalid JSON Patch", e.what());
            }
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No SMF Subscription Info for subsId " + subs_id);
            }
            ee_smf_subscription_info_write_counter->Add(1);
            notify_subscribers(
                subs_to_notify,
                notify_client,
                ue_id,
                resolved_location(ee_smf_subscription_info_path_pattern, req.path_params),
                change_from_json_patch(patch_ops));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "DELETE",
        ee_smf_subscription_info_path_pattern,
        [&verifier,
         &ee_smf_subscription_info,
         &ee_smf_subscription_info_delete_counter,
         &subs_to_notify,
         &notify_client,
         ee_smf_subscription_info_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto subs_id = req.path_params.at("subsId");
            if (!ee_smf_subscription_info.remove(ue_id, subs_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No SMF Subscription Info for subsId " + subs_id);
            }
            ee_smf_subscription_info_delete_counter->Add(1);
            notify_subscribers(
                subs_to_notify,
                notify_client,
                ue_id,
                resolved_location(ee_smf_subscription_info_path_pattern, req.path_params),
                change_remove());
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nudr_DataRepository: HSS Event Subscription Info (Document), nested under an
    // individual ee-subscription (ADR-0154, gap-closure task #106) -- real
    // GET+PUT+PATCH+DELETE per TS29505_Subscription_Data.yaml. Real, disclosed spec
    // inconsistency, asked and confirmed: the real spec's own GetHssSubscriptionInfo response
    // literally cites SmfSubscriptionInfo, not HssSubscriptionInfo -- treated as a real typo,
    // this route returns real HssSubscriptionInfo-shaped data, matching PUT/PATCH/DELETE on this
    // same resource and every sibling's own internally-consistent pattern. Real, distinct
    // 201-vs-204 PUT. Third and final of ee-subscriptions' own nested sub-collections closed. ---

    const std::string ee_hss_subscription_info_path_pattern =
        std::string(kApiRoot) +
        "/subscription-data/{ueId}/context-data/ee-subscriptions/{subsId}/hss-subscriptions";

    server.add_route(
        "GET",
        ee_hss_subscription_info_path_pattern,
        [&verifier, &ee_hss_subscription_info, &ee_hss_subscription_info_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto subs_id = req.path_params.at("subsId");
            auto data = ee_hss_subscription_info.get(ue_id, subs_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No HSS Subscription Info for subsId " + subs_id);
            }
            ee_hss_subscription_info_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PUT",
        ee_hss_subscription_info_path_pattern,
        [&verifier,
         &ee_hss_subscription_info,
         &ee_hss_subscription_info_write_counter,
         &subs_to_notify,
         &notify_client,
         ee_hss_subscription_info_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::HssSubscriptionInfo>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto subs_id = req.path_params.at("subsId");
            json j = *body;
            const bool is_new = ee_hss_subscription_info.put(ue_id, subs_id, j);
            ee_hss_subscription_info_write_counter->Add(1);
            notify_subscribers(
                subs_to_notify,
                notify_client,
                ue_id,
                resolved_location(ee_hss_subscription_info_path_pattern, req.path_params),
                change_replace(j));

            if (!is_new) {
                sbi_core::http2::Response resp;
                resp.status = 204;
                return resp;
            }
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace(
                "location",
                resolved_location(ee_hss_subscription_info_path_pattern, req.path_params));
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "PATCH",
        ee_hss_subscription_info_path_pattern,
        [&verifier,
         &ee_hss_subscription_info,
         &ee_hss_subscription_info_write_counter,
         &subs_to_notify,
         &notify_client,
         ee_hss_subscription_info_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json patch_ops;
            try {
                patch_ops = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto subs_id = req.path_params.at("subsId");
            std::optional<json> patched;
            try {
                patched = ee_hss_subscription_info.apply_patch(ue_id, subs_id, patch_ops);
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(400, "Invalid JSON Patch", e.what());
            }
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No HSS Subscription Info for subsId " + subs_id);
            }
            ee_hss_subscription_info_write_counter->Add(1);
            notify_subscribers(
                subs_to_notify,
                notify_client,
                ue_id,
                resolved_location(ee_hss_subscription_info_path_pattern, req.path_params),
                change_from_json_patch(patch_ops));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "DELETE",
        ee_hss_subscription_info_path_pattern,
        [&verifier,
         &ee_hss_subscription_info,
         &ee_hss_subscription_info_delete_counter,
         &subs_to_notify,
         &notify_client,
         ee_hss_subscription_info_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto subs_id = req.path_params.at("subsId");
            if (!ee_hss_subscription_info.remove(ue_id, subs_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No HSS Subscription Info for subsId " + subs_id);
            }
            ee_hss_subscription_info_delete_counter->Add(1);
            notify_subscribers(
                subs_to_notify,
                notify_client,
                ue_id,
                resolved_location(ee_hss_subscription_info_path_pattern, req.path_params),
                change_remove());
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nudr_DataRepository: HSS SDM Subscription Info (Document), nested under an individual
    // sdm-subscription (ADR-0155, gap-closure task #106) -- real GET+PUT+PATCH+DELETE per
    // TS29505_Subscription_Data.yaml. sdm-subscriptions' own final deferred nested sub-collection.
    // Reuses the same HssSubscriptionInfo schema as ee-subscriptions' own hss-subscriptions
    // sibling (ADR-0154). Two real, disclosed findings from direct read: (1) the real spec's own
    // PUT response list documents ONLY 204, no 201 anywhere -- genuinely unlike amf-/smf-/
    // hss-subscriptions under ee-subscriptions; matches the existing sor-data/upu-data (ADR-0143)
    // 204-only upsert-PUT precedent, confirmed by direct read, not invented. (2)
    // GetHssSDMSubscriptionInfo's own 200 response again literally cites SmfSubscriptionInfo, not
    // HssSubscriptionInfo -- the same typo class just resolved (and user-confirmed) in ADR-0154
    // for the sibling resource; the same resolution (return HssSubscriptionInfo) is applied here
    // without re-asking, since it is the identical schema-citation error on a structurally
    // parallel resource. ---

    const std::string sdm_hss_subscription_info_path_pattern =
        std::string(kApiRoot) +
        "/subscription-data/{ueId}/context-data/sdm-subscriptions/{subsId}/hss-sdm-subscriptions";

    server.add_route(
        "GET",
        sdm_hss_subscription_info_path_pattern,
        [&verifier, &sdm_hss_subscription_info, &sdm_hss_subscription_info_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto subs_id = req.path_params.at("subsId");
            auto data = sdm_hss_subscription_info.get(ue_id, subs_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No HSS SDM Subscription Info for subsId " + subs_id);
            }
            sdm_hss_subscription_info_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PUT",
        sdm_hss_subscription_info_path_pattern,
        [&verifier,
         &sdm_hss_subscription_info,
         &sdm_hss_subscription_info_write_counter,
         &subs_to_notify,
         &notify_client,
         sdm_hss_subscription_info_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::HssSubscriptionInfo>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto subs_id = req.path_params.at("subsId");
            json j = *body;
            sdm_hss_subscription_info.put(ue_id, subs_id, j);
            sdm_hss_subscription_info_write_counter->Add(1);
            notify_subscribers(
                subs_to_notify,
                notify_client,
                ue_id,
                resolved_location(sdm_hss_subscription_info_path_pattern, req.path_params),
                change_replace(j));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "PATCH",
        sdm_hss_subscription_info_path_pattern,
        [&verifier,
         &sdm_hss_subscription_info,
         &sdm_hss_subscription_info_write_counter,
         &subs_to_notify,
         &notify_client,
         sdm_hss_subscription_info_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json patch_ops;
            try {
                patch_ops = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto subs_id = req.path_params.at("subsId");
            std::optional<json> patched;
            try {
                patched = sdm_hss_subscription_info.apply_patch(ue_id, subs_id, patch_ops);
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(400, "Invalid JSON Patch", e.what());
            }
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No HSS SDM Subscription Info for subsId " + subs_id);
            }
            sdm_hss_subscription_info_write_counter->Add(1);
            notify_subscribers(
                subs_to_notify,
                notify_client,
                ue_id,
                resolved_location(sdm_hss_subscription_info_path_pattern, req.path_params),
                change_from_json_patch(patch_ops));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "DELETE",
        sdm_hss_subscription_info_path_pattern,
        [&verifier,
         &sdm_hss_subscription_info,
         &sdm_hss_subscription_info_delete_counter,
         &subs_to_notify,
         &notify_client,
         sdm_hss_subscription_info_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto subs_id = req.path_params.at("subsId");
            if (!sdm_hss_subscription_info.remove(ue_id, subs_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No HSS SDM Subscription Info for subsId " + subs_id);
            }
            sdm_hss_subscription_info_delete_counter->Add(1);
            notify_subscribers(
                subs_to_notify,
                notify_client,
                ue_id,
                resolved_location(sdm_hss_subscription_info_path_pattern, req.path_params),
                change_remove());
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nudr_DataRepository: Event Exposure Group Subscriptions collection + individual
    // document (ADR-0156, gap-closure task #106) -- real GET+POST on the collection,
    // GET+PUT+PATCH+DELETE on the individual document, per TS29505_Subscription_Data.yaml. The
    // group-data-scoped sibling of ee-subscriptions (ADR-0148), structurally identical but keyed
    // by ueGroupId instead of ueId: same EeSubscription schema, server-generated subsId (real
    // UUID v4), PUT genuinely update-only (real spec 404 "update of non-existing resource is
    // rejected"). The individual GET response schema has the same real `items:` (no
    // `type: array`) authoring artifact already found and resolved in ADR-0148's own
    // QueryeeSubscription -- treated identically here, returns a single EeSubscription object.
    // Real, disclosed scope narrowing kept from ADR-0148's own precedent: only the collection +
    // individual document are implemented -- the deeper group-data-scoped
    // amf-subscriptions/smf-subscriptions/hss-subscriptions nested sub-collections under each
    // subsId remain genuinely deferred. ---

    const std::string group_ee_subscriptions_collection_path_pattern =
        std::string(kApiRoot) + "/subscription-data/group-data/{ueGroupId}/ee-subscriptions";
    const std::string group_ee_subscriptions_individual_path_pattern =
        std::string(kApiRoot) +
        "/subscription-data/group-data/{ueGroupId}/ee-subscriptions/{subsId}";

    server.add_route(
        "GET",
        group_ee_subscriptions_collection_path_pattern,
        [&verifier, &group_ee_subscriptions, &group_ee_subscriptions_list_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_group_id = req.path_params.at("ueGroupId");
            auto subs = group_ee_subscriptions.list(ue_group_id);
            json arr = json::array();
            for (auto& s : subs) {
                arr.push_back(std::move(s));
            }
            group_ee_subscriptions_list_counter->Add(1);
            return sbi_core::http2::Response::json(200, arr.dump());
        });

    server.add_route(
        "POST",
        group_ee_subscriptions_collection_path_pattern,
        [&verifier,
         &group_ee_subscriptions,
         &group_ee_subscriptions_create_counter,
         group_ee_subscriptions_collection_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::EeSubscription>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_group_id = req.path_params.at("ueGroupId");
            const auto subs_id = sbi_core::generate_uuid_v4();
            json j = *body;
            group_ee_subscriptions.create(ue_group_id, subs_id, j);
            group_ee_subscriptions_create_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            // Bug fix (ADR-0156, refactored onto the shared helper in ADR-0157): real ueGroupId,
            // not the unsubstituted {ueGroupId} route-pattern placeholder.
            resp.headers.emplace(
                "location",
                resolved_location(group_ee_subscriptions_collection_path_pattern, req.path_params) +
                    "/" + subs_id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        group_ee_subscriptions_individual_path_pattern,
        [&verifier, &group_ee_subscriptions, &group_ee_subscriptions_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_group_id = req.path_params.at("ueGroupId");
            const auto subs_id = req.path_params.at("subsId");
            auto data = group_ee_subscriptions.get(ue_group_id, subs_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No EE Group Subscription for subsId " + subs_id);
            }
            group_ee_subscriptions_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PUT",
        group_ee_subscriptions_individual_path_pattern,
        [&verifier,
         &group_ee_subscriptions,
         &group_ee_subscriptions_write_counter,
         &subs_to_notify,
         &notify_client,
         group_ee_subscriptions_individual_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::EeSubscription>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_group_id = req.path_params.at("ueGroupId");
            const auto subs_id = req.path_params.at("subsId");
            json j = *body;
            // Real spec: UpdateEeGroupSubscriptions is genuinely update-only -- "update of
            // non-existing resource is rejected" (real 404), no create-via-PUT path exists.
            if (!group_ee_subscriptions.update(ue_group_id, subs_id, j)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No EE Group Subscription for subsId " + subs_id);
            }
            group_ee_subscriptions_write_counter->Add(1);
            notify_subscribers_ue_less(
                subs_to_notify,
                notify_client,
                resolved_location(group_ee_subscriptions_individual_path_pattern, req.path_params),
                change_replace(j));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "PATCH",
        group_ee_subscriptions_individual_path_pattern,
        [&verifier,
         &group_ee_subscriptions,
         &group_ee_subscriptions_write_counter,
         &subs_to_notify,
         &notify_client,
         group_ee_subscriptions_individual_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json patch_ops;
            try {
                patch_ops = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto ue_group_id = req.path_params.at("ueGroupId");
            const auto subs_id = req.path_params.at("subsId");
            std::optional<json> patched;
            try {
                patched = group_ee_subscriptions.apply_patch(ue_group_id, subs_id, patch_ops);
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(400, "Invalid JSON Patch", e.what());
            }
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No EE Group Subscription for subsId " + subs_id);
            }
            group_ee_subscriptions_write_counter->Add(1);
            notify_subscribers_ue_less(
                subs_to_notify,
                notify_client,
                resolved_location(group_ee_subscriptions_individual_path_pattern, req.path_params),
                change_from_json_patch(patch_ops));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "DELETE",
        group_ee_subscriptions_individual_path_pattern,
        [&verifier,
         &group_ee_subscriptions,
         &group_ee_subscriptions_delete_counter,
         &subs_to_notify,
         &notify_client,
         group_ee_subscriptions_individual_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_group_id = req.path_params.at("ueGroupId");
            const auto subs_id = req.path_params.at("subsId");
            if (!group_ee_subscriptions.remove(ue_group_id, subs_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No EE Group Subscription for subsId " + subs_id);
            }
            group_ee_subscriptions_delete_counter->Add(1);
            notify_subscribers_ue_less(
                subs_to_notify,
                notify_client,
                resolved_location(group_ee_subscriptions_individual_path_pattern, req.path_params),
                change_remove());
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nudr_DataRepository: AMF Group Subscription Info (Document), nested under an
    // individual group-data-scoped ee-subscription (ADR-0157, gap-closure task #106) -- real
    // GET+PUT+PATCH+DELETE per TS29505_Subscription_Data.yaml. The group-data-scoped sibling of
    // ee-subscriptions/{subsId}/amf-subscriptions (ADR-0152), structurally identical but keyed
    // by ueGroupId instead of ueId: same array-valued AmfSubscriptionInfo[] document body, real
    // distinct 201-vs-204 PUT. ---

    const std::string group_amf_subscription_info_path_pattern =
        std::string(kApiRoot) + "/subscription-data/group-data/{ueGroupId}/ee-subscriptions/"
                                "{subsId}/amf-subscriptions";

    server.add_route(
        "GET",
        group_amf_subscription_info_path_pattern,
        [&verifier, &group_amf_subscription_info, &group_amf_subscription_info_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_group_id = req.path_params.at("ueGroupId");
            const auto subs_id = req.path_params.at("subsId");
            auto data = group_amf_subscription_info.get(ue_group_id, subs_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No AMF Group Subscription Info for subsId " + subs_id);
            }
            group_amf_subscription_info_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PUT",
        group_amf_subscription_info_path_pattern,
        [&verifier,
         &group_amf_subscription_info,
         &group_amf_subscription_info_write_counter,
         &subs_to_notify,
         &notify_client,
         group_amf_subscription_info_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<std::vector<sbi_gen::AmfSubscriptionInfo>>(
                req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_group_id = req.path_params.at("ueGroupId");
            const auto subs_id = req.path_params.at("subsId");
            json j = *body;
            const bool is_new = group_amf_subscription_info.put(ue_group_id, subs_id, j);
            group_amf_subscription_info_write_counter->Add(1);
            notify_subscribers_ue_less(
                subs_to_notify,
                notify_client,
                resolved_location(group_amf_subscription_info_path_pattern, req.path_params),
                change_replace(j));

            if (!is_new) {
                sbi_core::http2::Response resp;
                resp.status = 204;
                return resp;
            }
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace(
                "location",
                resolved_location(group_amf_subscription_info_path_pattern, req.path_params));
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "PATCH",
        group_amf_subscription_info_path_pattern,
        [&verifier,
         &group_amf_subscription_info,
         &group_amf_subscription_info_write_counter,
         &subs_to_notify,
         &notify_client,
         group_amf_subscription_info_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json patch_ops;
            try {
                patch_ops = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto ue_group_id = req.path_params.at("ueGroupId");
            const auto subs_id = req.path_params.at("subsId");
            std::optional<json> patched;
            try {
                patched = group_amf_subscription_info.apply_patch(ue_group_id, subs_id, patch_ops);
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(400, "Invalid JSON Patch", e.what());
            }
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No AMF Group Subscription Info for subsId " + subs_id);
            }
            group_amf_subscription_info_write_counter->Add(1);
            notify_subscribers_ue_less(
                subs_to_notify,
                notify_client,
                resolved_location(group_amf_subscription_info_path_pattern, req.path_params),
                change_from_json_patch(patch_ops));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "DELETE",
        group_amf_subscription_info_path_pattern,
        [&verifier,
         &group_amf_subscription_info,
         &group_amf_subscription_info_delete_counter,
         &subs_to_notify,
         &notify_client,
         group_amf_subscription_info_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_group_id = req.path_params.at("ueGroupId");
            const auto subs_id = req.path_params.at("subsId");
            if (!group_amf_subscription_info.remove(ue_group_id, subs_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No AMF Group Subscription Info for subsId " + subs_id);
            }
            group_amf_subscription_info_delete_counter->Add(1);
            notify_subscribers_ue_less(
                subs_to_notify,
                notify_client,
                resolved_location(group_amf_subscription_info_path_pattern, req.path_params),
                change_remove());
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nudr_DataRepository: SMF Event Group Subscription Info (Document), nested under an
    // individual group-data-scoped ee-subscription (ADR-0158, gap-closure task #106) -- real
    // GET+PUT+PATCH+DELETE per TS29505_Subscription_Data.yaml. The group-data-scoped sibling of
    // ee-subscriptions/{subsId}/smf-subscriptions (ADR-0153), structurally identical but keyed
    // by ueGroupId instead of ueId: same single-object SmfSubscriptionInfo document body (unlike
    // its array-valued amf-subscriptions sibling), real distinct 201-vs-204 PUT. ---

    const std::string group_smf_subscription_info_path_pattern =
        std::string(kApiRoot) + "/subscription-data/group-data/{ueGroupId}/ee-subscriptions/"
                                "{subsId}/smf-subscriptions";

    server.add_route(
        "GET",
        group_smf_subscription_info_path_pattern,
        [&verifier, &group_smf_subscription_info, &group_smf_subscription_info_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_group_id = req.path_params.at("ueGroupId");
            const auto subs_id = req.path_params.at("subsId");
            auto data = group_smf_subscription_info.get(ue_group_id, subs_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No SMF Group Subscription Info for subsId " + subs_id);
            }
            group_smf_subscription_info_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PUT",
        group_smf_subscription_info_path_pattern,
        [&verifier,
         &group_smf_subscription_info,
         &group_smf_subscription_info_write_counter,
         &subs_to_notify,
         &notify_client,
         group_smf_subscription_info_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::SmfSubscriptionInfo>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_group_id = req.path_params.at("ueGroupId");
            const auto subs_id = req.path_params.at("subsId");
            json j = *body;
            const bool is_new = group_smf_subscription_info.put(ue_group_id, subs_id, j);
            group_smf_subscription_info_write_counter->Add(1);
            notify_subscribers_ue_less(
                subs_to_notify,
                notify_client,
                resolved_location(group_smf_subscription_info_path_pattern, req.path_params),
                change_replace(j));

            if (!is_new) {
                sbi_core::http2::Response resp;
                resp.status = 204;
                return resp;
            }
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace(
                "location",
                resolved_location(group_smf_subscription_info_path_pattern, req.path_params));
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "PATCH",
        group_smf_subscription_info_path_pattern,
        [&verifier,
         &group_smf_subscription_info,
         &group_smf_subscription_info_write_counter,
         &subs_to_notify,
         &notify_client,
         group_smf_subscription_info_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json patch_ops;
            try {
                patch_ops = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto ue_group_id = req.path_params.at("ueGroupId");
            const auto subs_id = req.path_params.at("subsId");
            std::optional<json> patched;
            try {
                patched = group_smf_subscription_info.apply_patch(ue_group_id, subs_id, patch_ops);
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(400, "Invalid JSON Patch", e.what());
            }
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No SMF Group Subscription Info for subsId " + subs_id);
            }
            group_smf_subscription_info_write_counter->Add(1);
            notify_subscribers_ue_less(
                subs_to_notify,
                notify_client,
                resolved_location(group_smf_subscription_info_path_pattern, req.path_params),
                change_from_json_patch(patch_ops));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "DELETE",
        group_smf_subscription_info_path_pattern,
        [&verifier,
         &group_smf_subscription_info,
         &group_smf_subscription_info_delete_counter,
         &subs_to_notify,
         &notify_client,
         group_smf_subscription_info_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_group_id = req.path_params.at("ueGroupId");
            const auto subs_id = req.path_params.at("subsId");
            if (!group_smf_subscription_info.remove(ue_group_id, subs_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No SMF Group Subscription Info for subsId " + subs_id);
            }
            group_smf_subscription_info_delete_counter->Add(1);
            notify_subscribers_ue_less(
                subs_to_notify,
                notify_client,
                resolved_location(group_smf_subscription_info_path_pattern, req.path_params),
                change_remove());
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nudr_DataRepository: HSS Event Group Subscription Info (Document), nested under an
    // individual group-data-scoped ee-subscription (ADR-0159, gap-closure task #106) -- real
    // GET+PUT+PATCH+DELETE per TS29505_Subscription_Data.yaml. The group-data-scoped sibling of
    // ee-subscriptions/{subsId}/hss-subscriptions (ADR-0154), structurally identical but keyed
    // by ueGroupId instead of ueId: same single-object HssSubscriptionInfo document body, real
    // distinct 201-vs-204 PUT. GetHssGroupSubscriptions correctly cites HssSubscriptionInfo (no
    // response-schema typo here, unlike its ueId-scoped and sdm-subscriptions-scoped siblings).
    // Real, disclosed spec inconsistency: the real spec's own DELETE/PATCH/GET operations on
    // this path declare a parameter named `externalGroupId`, but the actual URL path template
    // has no such placeholder -- only `{ueGroupId}` and `{subsId}` (matching PUT, and every
    // sibling resource in this family). Not a genuine design ambiguity: `externalGroupId` isn't
    // bindable from the real path at all, so `ueGroupId` is used throughout, consistent with
    // this resource's own PUT and its real URL. Completes all three of group-data's own
    // ee-subscriptions/{subsId}/... nested sub-collections. ---

    const std::string group_hss_subscription_info_path_pattern =
        std::string(kApiRoot) + "/subscription-data/group-data/{ueGroupId}/ee-subscriptions/"
                                "{subsId}/hss-subscriptions";

    server.add_route(
        "GET",
        group_hss_subscription_info_path_pattern,
        [&verifier, &group_hss_subscription_info, &group_hss_subscription_info_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_group_id = req.path_params.at("ueGroupId");
            const auto subs_id = req.path_params.at("subsId");
            auto data = group_hss_subscription_info.get(ue_group_id, subs_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No HSS Group Subscription Info for subsId " + subs_id);
            }
            group_hss_subscription_info_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PUT",
        group_hss_subscription_info_path_pattern,
        [&verifier,
         &group_hss_subscription_info,
         &group_hss_subscription_info_write_counter,
         &subs_to_notify,
         &notify_client,
         group_hss_subscription_info_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::HssSubscriptionInfo>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_group_id = req.path_params.at("ueGroupId");
            const auto subs_id = req.path_params.at("subsId");
            json j = *body;
            const bool is_new = group_hss_subscription_info.put(ue_group_id, subs_id, j);
            group_hss_subscription_info_write_counter->Add(1);
            notify_subscribers_ue_less(
                subs_to_notify,
                notify_client,
                resolved_location(group_hss_subscription_info_path_pattern, req.path_params),
                change_replace(j));

            if (!is_new) {
                sbi_core::http2::Response resp;
                resp.status = 204;
                return resp;
            }
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace(
                "location",
                resolved_location(group_hss_subscription_info_path_pattern, req.path_params));
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "PATCH",
        group_hss_subscription_info_path_pattern,
        [&verifier,
         &group_hss_subscription_info,
         &group_hss_subscription_info_write_counter,
         &subs_to_notify,
         &notify_client,
         group_hss_subscription_info_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json patch_ops;
            try {
                patch_ops = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto ue_group_id = req.path_params.at("ueGroupId");
            const auto subs_id = req.path_params.at("subsId");
            std::optional<json> patched;
            try {
                patched = group_hss_subscription_info.apply_patch(ue_group_id, subs_id, patch_ops);
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(400, "Invalid JSON Patch", e.what());
            }
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No HSS Group Subscription Info for subsId " + subs_id);
            }
            group_hss_subscription_info_write_counter->Add(1);
            notify_subscribers_ue_less(
                subs_to_notify,
                notify_client,
                resolved_location(group_hss_subscription_info_path_pattern, req.path_params),
                change_from_json_patch(patch_ops));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "DELETE",
        group_hss_subscription_info_path_pattern,
        [&verifier,
         &group_hss_subscription_info,
         &group_hss_subscription_info_delete_counter,
         &subs_to_notify,
         &notify_client,
         group_hss_subscription_info_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_group_id = req.path_params.at("ueGroupId");
            const auto subs_id = req.path_params.at("subsId");
            if (!group_hss_subscription_info.remove(ue_group_id, subs_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No HSS Group Subscription Info for subsId " + subs_id);
            }
            group_hss_subscription_info_delete_counter->Add(1);
            notify_subscribers_ue_less(
                subs_to_notify,
                notify_client,
                resolved_location(group_hss_subscription_info_path_pattern, req.path_params),
                change_remove());
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nudr_DataRepository: PDTQ Data collection + individual document (ADR-0162, gap-closure
    // task #106) -- real GET on the collection, GET+PUT+PATCH+DELETE on the individual document,
    // per TS29519_Policy_Data.yaml. The first real UDR resource closed using the array-query-param
    // parsing infra (ADR-0161) -- confirmed genuinely unblocked by it, not merely a candidate.
    // Real, disclosed: `pdtqReferenceId` is client-supplied (a real path parameter on the
    // individual document, not server-generated). Real, disclosed: `CreateIndividualPdtqData`
    // documents ONLY 201 (no update-via-PUT status) -- matches the existing `bdt-data` precedent
    // exactly, this route always responds 201. Real RFC 7396 JSON Merge Patch
    // (application/merge-patch+json, `PdtqDataPatch`), same idiom as `bdt-data`/UDM's own
    // merge-patch routes: validated against the real generated shape first, then applied via
    // nlohmann::json's `.merge_patch()` on the raw parsed body (not round-tripped through the
    // DTO) to preserve RFC 7396's own absent-vs-null field semantics. Real, disclosed scope
    // choice: the collection GET's own optional `pdtq-ref-ids` array query-param filter is NOT
    // honored, matching the established "optional filter not honored" precedent
    // (`ee-subscriptions`' `event-types`/`nf-identifiers`) for consistency, not because the new
    // parsing infra couldn't honor it. ---

    const std::string pdtq_data_collection_path_pattern =
        std::string(kApiRoot) + "/policy-data/pdtq-data";
    const std::string pdtq_data_individual_path_pattern =
        std::string(kApiRoot) + "/policy-data/pdtq-data/{pdtqReferenceId}";

    server.add_route(
        "GET",
        pdtq_data_collection_path_pattern,
        [&verifier, &pdtq_data, &pdtq_data_list_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto items = pdtq_data.list();
            json arr = json::array();
            for (auto& item : items) {
                arr.push_back(std::move(item));
            }
            pdtq_data_list_counter->Add(1);
            return sbi_core::http2::Response::json(200, arr.dump());
        });

    server.add_route(
        "GET",
        pdtq_data_individual_path_pattern,
        [&verifier, &pdtq_data, &pdtq_data_get_counter](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto pdtq_ref_id = req.path_params.at("pdtqReferenceId");
            auto data = pdtq_data.get(pdtq_ref_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No PDTQ data for pdtqReferenceId " + pdtq_ref_id);
            }
            pdtq_data_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PUT",
        pdtq_data_individual_path_pattern,
        [&verifier,
         &pdtq_data,
         &pdtq_data_write_counter,
         &subs_to_notify,
         &notify_client,
         pdtq_data_individual_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::PdtqData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto pdtq_ref_id = req.path_params.at("pdtqReferenceId");
            json j = *body;
            pdtq_data.put(pdtq_ref_id, j);
            pdtq_data_write_counter->Add(1);
            notify_subscribers_ue_less(
                subs_to_notify,
                notify_client,
                resolved_location(pdtq_data_individual_path_pattern, req.path_params),
                change_replace(j));
            // Real spec: CreateIndividualPdtqData documents ONLY 201 as a success response (no
            // update-via-PUT status) -- confirmed by direct read, this route always responds 201,
            // matching the real spec literally rather than inventing an undocumented 204.
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace(
                "location", resolved_location(pdtq_data_individual_path_pattern, req.path_params));
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "PATCH",
        pdtq_data_individual_path_pattern,
        [&verifier,
         &pdtq_data,
         &pdtq_data_write_counter,
         &subs_to_notify,
         &notify_client,
         pdtq_data_individual_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto patch_dto = sbi_core::http2::parse_json_body<sbi_gen::PdtqDataPatch>(req, err);
            if (!patch_dto.has_value()) {
                return err;
            }
            json patch;
            try {
                patch = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto pdtq_ref_id = req.path_params.at("pdtqReferenceId");
            const auto patched = pdtq_data.merge_patch(pdtq_ref_id, patch);
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No PDTQ data for pdtqReferenceId " + pdtq_ref_id);
            }
            pdtq_data_write_counter->Add(1);
            notify_subscribers_ue_less(
                subs_to_notify,
                notify_client,
                resolved_location(pdtq_data_individual_path_pattern, req.path_params),
                change_replace(*patched));
            return sbi_core::http2::Response::json(200, patched->dump());
        });

    server.add_route(
        "DELETE",
        pdtq_data_individual_path_pattern,
        [&verifier,
         &pdtq_data,
         &pdtq_data_delete_counter,
         &subs_to_notify,
         &notify_client,
         pdtq_data_individual_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto pdtq_ref_id = req.path_params.at("pdtqReferenceId");
            if (!pdtq_data.remove(pdtq_ref_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No PDTQ data for pdtqReferenceId " + pdtq_ref_id);
            }
            pdtq_data_delete_counter->Add(1);
            notify_subscribers_ue_less(
                subs_to_notify,
                notify_client,
                resolved_location(pdtq_data_individual_path_pattern, req.path_params),
                change_remove());
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nudr_DataRepository: Subs To Notify collection + individual document (ADR-0149,
    // gap-closure task #106) -- real GET+POST on the collection (GET requires a real, non-array
    // `ue-id` query filter), GET+PATCH+DELETE on the individual document (genuinely no PUT exists
    // for this resource), per TS29505_Subscription_Data.yaml. Real, disclosed: `subsId` is
    // server-generated (real UUID v4, same precedent as ee-subscriptions); this project stores the
    // POST body's own optional `ueId` field to back the real `ue-id` collection filter. ---

    const std::string subs_to_notify_collection_path_pattern =
        std::string(kApiRoot) + "/subscription-data/subs-to-notify";
    const std::string subs_to_notify_individual_path_pattern =
        std::string(kApiRoot) + "/subscription-data/subs-to-notify/{subsId}";

    server.add_route(
        "GET",
        subs_to_notify_collection_path_pattern,
        [&verifier, &subs_to_notify, &subs_to_notify_list_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id_it = req.query_params.find("ue-id");
            if (ue_id_it == req.query_params.end()) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", "Required query parameter ue-id is missing");
            }
            auto subs = subs_to_notify.list_by_ue_id(ue_id_it->second);
            json arr = json::array();
            for (auto& s : subs) {
                arr.push_back(std::move(s));
            }
            subs_to_notify_list_counter->Add(1);
            return sbi_core::http2::Response::json(200, arr.dump());
        });

    server.add_route(
        "POST",
        subs_to_notify_collection_path_pattern,
        [&verifier,
         &subs_to_notify,
         &subs_to_notify_create_counter,
         subs_to_notify_collection_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::SubscriptionDataSubscriptions>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto subs_id = sbi_core::generate_uuid_v4();
            json j = *body;
            subs_to_notify.create(subs_id, body->ueId, j);
            subs_to_notify_create_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 subs_to_notify_collection_path_pattern + "/" + subs_id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        subs_to_notify_individual_path_pattern,
        [&verifier, &subs_to_notify, &subs_to_notify_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto subs_id = req.path_params.at("subsId");
            auto data = subs_to_notify.get(subs_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No Subs To Notify for subsId " + subs_id);
            }
            subs_to_notify_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PATCH",
        subs_to_notify_individual_path_pattern,
        [&verifier, &subs_to_notify, &subs_to_notify_write_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json patch_ops;
            try {
                patch_ops = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto subs_id = req.path_params.at("subsId");
            std::optional<json> patched;
            try {
                patched = subs_to_notify.apply_patch(subs_id, patch_ops);
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(400, "Invalid JSON Patch", e.what());
            }
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No Subs To Notify for subsId " + subs_id);
            }
            subs_to_notify_write_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "DELETE",
        subs_to_notify_individual_path_pattern,
        [&verifier, &subs_to_notify, &subs_to_notify_delete_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto subs_id = req.path_params.at("subsId");
            if (!subs_to_notify.remove(subs_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No Subs To Notify for subsId " + subs_id);
            }
            subs_to_notify_delete_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nudr_DataRepository: policy-data group's own Policy Data Subscriptions (Collection) +
    // Individual Policy (Data) Subscription (Document) (ADR-0213, gap-closure task #106) -- real
    // GET+POST on the collection, GET+PUT+DELETE on the individual document, per
    // TS29519_Policy_Data.yaml. Genuinely distinct real resource from
    // `Subscription_Data.yaml`'s own `subs-to-notify` above: different real schema
    // (`PolicyDataSubscription`), different real individual-modify verb (PUT full replace here,
    // not RFC 6902 PATCH). Real, optional `mon-resources`/`ue-id` collection-GET filters honored
    // via the same "store returns all, route filters" pattern. Real, disclosed: the spec's own
    // `policyDataChangeNotification` webhook callback is not implemented -- same disclosed gap
    // class as `SubsToNotifyStore`'s own pre-ADR-0171 state. `subsId` server-generated (real
    // UUID v4, same precedent as `subs-to-notify` above). ---

    const std::string policy_subs_to_notify_collection_path_pattern =
        std::string(kApiRoot) + "/policy-data/subs-to-notify";
    const std::string policy_subs_to_notify_individual_path_pattern =
        std::string(kApiRoot) + "/policy-data/subs-to-notify/{subsId}";

    server.add_route(
        "GET",
        policy_subs_to_notify_collection_path_pattern,
        [&verifier, &policy_data_subs_to_notify, &policy_data_subs_to_notify_list_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            // Real, disclosed: the optional `mon-resources`/`ue-id` filters are accepted but not
            // honored -- `PolicyDataSubscription`'s own body has no `ueId` field to filter by (it
            // is genuinely not part of this real schema, unlike `SubscriptionDataSubscriptions`'
            // own), and `monResItems`' per-resource-path granularity isn't modeled by this store.
            auto all = policy_data_subs_to_notify.list_all();
            json arr = json::array();
            for (auto& item : all) {
                arr.push_back(std::move(item));
            }
            policy_data_subs_to_notify_list_counter->Add(1);
            return sbi_core::http2::Response::json(200, arr.dump());
        });

    server.add_route(
        "POST",
        policy_subs_to_notify_collection_path_pattern,
        [&verifier,
         &policy_data_subs_to_notify,
         &policy_data_subs_to_notify_create_counter,
         policy_subs_to_notify_collection_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::PolicyDataSubscription>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto subs_id = sbi_core::generate_uuid_v4();
            json j = *body;
            policy_data_subs_to_notify.create(subs_id, std::nullopt, j);
            policy_data_subs_to_notify_create_counter->Add(1);

            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 policy_subs_to_notify_collection_path_pattern + "/" + subs_id);
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        policy_subs_to_notify_individual_path_pattern,
        [&verifier, &policy_data_subs_to_notify, &policy_data_subs_to_notify_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto subs_id = req.path_params.at("subsId");
            auto data = policy_data_subs_to_notify.get(subs_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No Policy Data Subscription for subsId " + subs_id);
            }
            policy_data_subs_to_notify_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PUT",
        policy_subs_to_notify_individual_path_pattern,
        [&verifier, &policy_data_subs_to_notify, &policy_data_subs_to_notify_replace_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::PolicyDataSubscription>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto subs_id = req.path_params.at("subsId");
            json j = *body;
            auto replaced = policy_data_subs_to_notify.replace(subs_id, j);
            if (!replaced.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No Policy Data Subscription for subsId " + subs_id);
            }
            policy_data_subs_to_notify_replace_counter->Add(1);
            return sbi_core::http2::Response::json(200, replaced->dump());
        });

    server.add_route(
        "DELETE",
        policy_subs_to_notify_individual_path_pattern,
        [&verifier, &policy_data_subs_to_notify, &policy_data_subs_to_notify_delete_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto subs_id = req.path_params.at("subsId");
            if (!policy_data_subs_to_notify.remove(subs_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No Policy Data Subscription for subsId " + subs_id);
            }
            policy_data_subs_to_notify_delete_counter->Add(1);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nudr_DataRepository: group-data bare 5G VN Groups collection resource (ADR-0167,
    // gap-closure task #106) -- real GET-only per TS29505_Subscription_Data.yaml,
    // `Query5GVnGroup`. Real REQUIRED-schema response is a map `{ExtGroupId:
    // 5GVnGroupConfiguration}` over every persisted `FiveGVnGroupStore` row (the same store
    // `5g-vn-groups/{externalGroupId}`, ADR-0144, already writes to) -- no new table.
    //
    // UPDATE (ADR-0181, gap-closure task #106): the real, optional `gpsis` array query-param
    // filter (`style: form, explode: false`) is now honored -- confirmed by direct read of
    // `5GVnGroupConfiguration`'s own real schema (`TS29503_Nudm_PP.yaml`) that its optional
    // `members` field is exactly the array of `Gpsi` this filter needs to match against, making
    // this genuinely tractable (not the deeper "inspect membership data" blocker this file's own
    // ADR-0167 comment originally assumed before checking). A group is included when `gpsis` is
    // absent (no filter) or when at least one requested `Gpsi` appears in that group's own
    // `members` array; a group with no `members` field at all never matches a non-empty filter.
    // Real `200`-always (even an empty map) preserved regardless of filtering, matching this
    // project's own bare-collection-GET precedent (`pdtq-data`, ADR-0162). ---

    const std::string five_g_vn_groups_collection_path_pattern =
        std::string(kApiRoot) + "/subscription-data/group-data/5g-vn-groups";

    server.add_route(
        "GET",
        five_g_vn_groups_collection_path_pattern,
        [&verifier, &five_g_vn_groups, &five_g_vn_groups_list_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            std::optional<std::vector<std::string>> requested_gpsis;
            if (auto gpsis_it = req.query_params.find("gpsis");
                gpsis_it != req.query_params.end()) {
                requested_gpsis = sbi_core::http2::split_form_array(gpsis_it->second);
            }
            json result = json::object();
            for (const auto& [ext_group_id, data] : five_g_vn_groups.list_all()) {
                if (requested_gpsis.has_value()) {
                    if (!data.contains("members")) {
                        continue;
                    }
                    bool matched = false;
                    for (const auto& member : data.at("members")) {
                        if (member.is_string() &&
                            std::find(requested_gpsis->begin(),
                                      requested_gpsis->end(),
                                      member.get<std::string>()) != requested_gpsis->end()) {
                            matched = true;
                            break;
                        }
                    }
                    if (!matched) {
                        continue;
                    }
                }
                result[ext_group_id] = data;
            }
            five_g_vn_groups_list_counter->Add(1);
            return sbi_core::http2::Response::json(200, result.dump());
        });

    // --- Nudr_DataRepository: group-data 5G VN Groups Internal resource (ADR-0168, gap-closure
    // task #106) -- real GET-only per TS29505_Subscription_Data.yaml, `Query5GVnGroupInternal`.
    // Real REQUIRED `internal-group-ids` array query param (`style: form, explode: false`),
    // filtering by each stored group's own optional `internalGroupIdentifier` field (confirmed by
    // direct read of `5GVnGroupConfiguration`'s own schema) -- unlike the bare collection's own
    // `gpsis` filter (which needs member-list inspection, deferred), this filter targets a single
    // scalar field per group, real and tractable to implement here. Response is the same real map
    // `{ExtGroupId: 5GVnGroupConfiguration}` shape, composed from the same `FiveGVnGroupStore`
    // (`list_all()`, ADR-0167) -- no new table. Real `404` when no requested internal-group-id
    // matches any stored group (a genuine query-by-identifier, matching `GetNfGroupIDs`'s own
    // real-404 precedent, NOT the bare collection's own always-`200` literal-listing precedent).
    // CRITICAL ROUTE-ORDERING REQUIREMENT, confirmed by direct read of
    // `libs/sbi-core/src/http2_server.cpp`'s own `try_match`: this router has no literal-vs-
    // wildcard priority -- it matches routes in registration order, first match wins. This literal
    // `.../5g-vn-groups/internal` path has the SAME segment count as the individual
    // `.../5g-vn-groups/{externalGroupId}` GET route below, whose wildcard segment would otherwise
    // match the literal string "internal" as a (nonexistent) externalGroupId. This route MUST
    // stay registered before that one, or it would be permanently shadowed -- verified by live
    // curl below, not merely reasoned about. ---

    const std::string five_g_vn_groups_internal_path_pattern =
        std::string(kApiRoot) + "/subscription-data/group-data/5g-vn-groups/internal";

    server.add_route(
        "GET",
        five_g_vn_groups_internal_path_pattern,
        [&verifier, &five_g_vn_groups, &five_g_vn_groups_internal_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ids_it = req.query_params.find("internal-group-ids");
            if (ids_it == req.query_params.end()) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", "internal-group-ids is a required query parameter");
            }
            const auto requested_ids = sbi_core::http2::split_form_array(ids_it->second);
            json result = json::object();
            for (const auto& [ext_group_id, data] : five_g_vn_groups.list_all()) {
                if (data.contains("internalGroupIdentifier") &&
                    std::find(requested_ids.begin(),
                              requested_ids.end(),
                              data.at("internalGroupIdentifier").get<std::string>()) !=
                        requested_ids.end()) {
                    result[ext_group_id] = data;
                }
            }
            five_g_vn_groups_internal_get_counter->Add(1);
            if (result.empty()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No 5G VN Group matches the requested internal-group-ids");
            }
            return sbi_core::http2::Response::json(200, result.dump());
        });

    // --- Nudr_DataRepository: group-data 5G VN Group PP Data resource (ADR-0169, gap-closure
    // task #106) -- real GET-only per TS29505_Subscription_Data.yaml, `Query5GVnGroupPPData`.
    // Real, disclosed: unlike every other `group-data` sub-resource, `Pp5gVnGroupProfileData` is
    // a genuinely keyless singleton document (its own `allowedMtcProviders` field is itself a map
    // keyed by ExtGroupId, but the resource is not) -- backed by `FiveGVnGroupPpProfileDataStore`
    // (ADR-0169), a fixed single-row table. Real `200`-always (seeded at startup, always present).
    // SAME CRITICAL ROUTE-ORDERING REQUIREMENT as `5g-vn-groups/internal` above: this literal
    // 4-segment path must stay registered before the `{externalGroupId}` GET route below.
    //
    // UPDATE (ADR-0181, gap-closure task #106): the real, optional `ext-group-ids` array
    // query-param filter is now honored against `allowedMtcProviders`'s own real keys -- confirmed
    // by direct read of `TS29505_Subscription_Data.yaml`'s own `Pp5gVnGroupProfileData` schema
    // that the special key `"ALL"` ("a map entry which contains a list of AllowedMtcProviderInfo
    // that are allowed operating all the external group identifiers") always applies regardless of
    // which external group IDs were requested, so it is always kept in the filtered result, not
    // just the literally-requested keys. `supported-features` remains accepted-but-not-honored,
    // matching this project's own established, unrelated precedent for that capability-negotiation
    // param across every route that has it -- out of scope for this gpsis/ext-group-ids pass. ---

    const std::string five_g_vn_group_pp_profile_data_path_pattern =
        std::string(kApiRoot) + "/subscription-data/group-data/5g-vn-groups/pp-profile-data";

    server.add_route(
        "GET",
        five_g_vn_group_pp_profile_data_path_pattern,
        [&verifier, &five_g_vn_group_pp_profile_data, &five_g_vn_group_pp_profile_data_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto data = five_g_vn_group_pp_profile_data.get();
            five_g_vn_group_pp_profile_data_get_counter->Add(1);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No 5G VN Group PP Profile Data");
            }
            if (auto ids_it = req.query_params.find("ext-group-ids");
                ids_it != req.query_params.end() && data->contains("allowedMtcProviders")) {
                const auto requested_ids = sbi_core::http2::split_form_array(ids_it->second);
                json filtered = json::object();
                for (const auto& [key, value] : data->at("allowedMtcProviders").items()) {
                    if (key == "ALL" ||
                        std::find(requested_ids.begin(), requested_ids.end(), key) !=
                            requested_ids.end()) {
                        filtered[key] = value;
                    }
                }
                json filtered_doc = *data;
                filtered_doc["allowedMtcProviders"] = filtered;
                return sbi_core::http2::Response::json(200, filtered_doc.dump());
            }
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // --- Nudr_DataRepository: group-data individual 5G VN Group Configuration resource
    // (ADR-0144, gap-closure task #106) -- real GET+PUT+PATCH+DELETE per
    // TS29505_Subscription_Data.yaml. Real, disclosed: the real PUT documents ONLY `201` (no
    // update-via-PUT status, same precedent as `bdt-data`), so this route always responds 201.
    // PATCH is real RFC 6902 `application/json-patch+json` (NOT `bdt-data`'s own RFC 7396
    // merge-patch), NOT upsert-capable. Keyed by externalGroupId. First real `group-data`
    // sub-resource closed since `group-identifiers` (ADR-0140). ---

    const std::string five_g_vn_groups_path_pattern =
        std::string(kApiRoot) + "/subscription-data/group-data/5g-vn-groups/{externalGroupId}";

    server.add_route(
        "GET",
        five_g_vn_groups_path_pattern,
        [&verifier, &five_g_vn_groups, &five_g_vn_groups_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ext_group_id = req.path_params.at("externalGroupId");
            auto data = five_g_vn_groups.get(ext_group_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No 5G VN Group for externalGroupId " + ext_group_id);
            }
            five_g_vn_groups_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PUT",
        five_g_vn_groups_path_pattern,
        [&verifier,
         &five_g_vn_groups,
         &five_g_vn_groups_write_counter,
         &subs_to_notify,
         &notify_client,
         five_g_vn_groups_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::N5GVnGroupConfiguration>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ext_group_id = req.path_params.at("externalGroupId");
            json j = *body;
            five_g_vn_groups.put(ext_group_id, j);
            five_g_vn_groups_write_counter->Add(1);
            notify_subscribers_ue_less(
                subs_to_notify,
                notify_client,
                resolved_location(five_g_vn_groups_path_pattern, req.path_params),
                change_replace(j));
            // Real spec: Create5GVnGroup documents ONLY 201 as a success response (no
            // update-via-PUT status) -- confirmed by direct read, same precedent as bdt-data.
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location",
                                 resolved_location(five_g_vn_groups_path_pattern, req.path_params));
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "PATCH",
        five_g_vn_groups_path_pattern,
        [&verifier,
         &five_g_vn_groups,
         &five_g_vn_groups_write_counter,
         &subs_to_notify,
         &notify_client,
         five_g_vn_groups_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json patch_ops;
            try {
                patch_ops = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto ext_group_id = req.path_params.at("externalGroupId");
            std::optional<json> patched;
            try {
                patched = five_g_vn_groups.apply_patch(ext_group_id, patch_ops);
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(400, "Invalid JSON Patch", e.what());
            }
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No 5G VN Group for externalGroupId " + ext_group_id);
            }
            five_g_vn_groups_write_counter->Add(1);
            notify_subscribers_ue_less(
                subs_to_notify,
                notify_client,
                resolved_location(five_g_vn_groups_path_pattern, req.path_params),
                change_from_json_patch(patch_ops));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "DELETE",
        five_g_vn_groups_path_pattern,
        [&verifier,
         &five_g_vn_groups,
         &five_g_vn_groups_delete_counter,
         &subs_to_notify,
         &notify_client,
         five_g_vn_groups_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ext_group_id = req.path_params.at("externalGroupId");
            if (!five_g_vn_groups.remove(ext_group_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No 5G VN Group for externalGroupId " + ext_group_id);
            }
            five_g_vn_groups_delete_counter->Add(1);
            notify_subscribers_ue_less(
                subs_to_notify,
                notify_client,
                resolved_location(five_g_vn_groups_path_pattern, req.path_params),
                change_remove());
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nudr_DataRepository: group-data bare 5G MBS Group Membership collection resource
    // (ADR-0167, gap-closure task #106) -- real GET-only per TS29505_Subscription_Data.yaml,
    // `Query5GmbsGroup`, structurally an exact twin of the `5g-vn-groups` collection resource
    // above (map response over every persisted `MbsGroupMembershipStore` row, real `200`-always).
    //
    // UPDATE (ADR-0181, gap-closure task #106): the real, optional `gpsis` array query-param
    // filter is now honored the same way as `5g-vn-groups`'s own sibling filter above, but against
    // `MulticastMbsGroupMemb`'s own real field name -- `multicastGroupMemb`, confirmed by direct
    // read of `TS29503_Nudm_PP.yaml` to be genuinely REQUIRED (unlike `5GVnGroupConfiguration`'s
    // own optional `members`), so every stored row should have it; the `contains()` guard below is
    // still kept for defensive symmetry with the sibling route, not because absence is expected.
    // ---

    const std::string mbs_group_membership_collection_path_pattern =
        std::string(kApiRoot) + "/subscription-data/group-data/mbs-group-membership";

    server.add_route(
        "GET",
        mbs_group_membership_collection_path_pattern,
        [&verifier, &mbs_group_membership, &mbs_group_membership_list_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            std::optional<std::vector<std::string>> requested_gpsis;
            if (auto gpsis_it = req.query_params.find("gpsis");
                gpsis_it != req.query_params.end()) {
                requested_gpsis = sbi_core::http2::split_form_array(gpsis_it->second);
            }
            json result = json::object();
            for (const auto& [ext_group_id, data] : mbs_group_membership.list_all()) {
                if (requested_gpsis.has_value()) {
                    if (!data.contains("multicastGroupMemb")) {
                        continue;
                    }
                    bool matched = false;
                    for (const auto& member : data.at("multicastGroupMemb")) {
                        if (member.is_string() &&
                            std::find(requested_gpsis->begin(),
                                      requested_gpsis->end(),
                                      member.get<std::string>()) != requested_gpsis->end()) {
                            matched = true;
                            break;
                        }
                    }
                    if (!matched) {
                        continue;
                    }
                }
                result[ext_group_id] = data;
            }
            mbs_group_membership_list_counter->Add(1);
            return sbi_core::http2::Response::json(200, result.dump());
        });

    // --- Nudr_DataRepository: group-data 5G MBS Group Membership Internal resource (ADR-0168,
    // gap-closure task #106) -- real GET-only per TS29505_Subscription_Data.yaml,
    // `Query5GMbsGroupInternal`, structurally an exact twin of the `5g-vn-groups/internal`
    // resource above (`MulticastMbsGroupMemb`'s own schema has the same optional
    // `internalGroupIdentifier` field to filter by). Same CRITICAL ROUTE-ORDERING REQUIREMENT as
    // that resource's own comment -- this route MUST stay registered before the individual
    // `{externalGroupId}` GET route below, or the router's own first-match, no-literal-priority
    // semantics would permanently shadow it. ---

    const std::string mbs_group_membership_internal_path_pattern =
        std::string(kApiRoot) + "/subscription-data/group-data/mbs-group-membership/internal";

    server.add_route(
        "GET",
        mbs_group_membership_internal_path_pattern,
        [&verifier, &mbs_group_membership, &mbs_group_membership_internal_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ids_it = req.query_params.find("internal-group-ids");
            if (ids_it == req.query_params.end()) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", "internal-group-ids is a required query parameter");
            }
            const auto requested_ids = sbi_core::http2::split_form_array(ids_it->second);
            json result = json::object();
            for (const auto& [ext_group_id, data] : mbs_group_membership.list_all()) {
                if (data.contains("internalGroupIdentifier") &&
                    std::find(requested_ids.begin(),
                              requested_ids.end(),
                              data.at("internalGroupIdentifier").get<std::string>()) !=
                        requested_ids.end()) {
                    result[ext_group_id] = data;
                }
            }
            mbs_group_membership_internal_get_counter->Add(1);
            if (result.empty()) {
                return sbi_core::http2::problem_response(
                    404,
                    "Not Found",
                    "No 5G MBS Group Membership matches the requested internal-group-ids");
            }
            return sbi_core::http2::Response::json(200, result.dump());
        });

    // --- Nudr_DataRepository: group-data 5G MBS Group PP Data resource (ADR-0169, gap-closure
    // task #106) -- real GET-only per TS29505_Subscription_Data.yaml, `Query5GMbsGroupPPData`,
    // structurally an exact twin of `5g-vn-groups/pp-profile-data` above (`Pp5gMbsGroupProfileData`
    // has the same keyless-singleton shape, `allowedMbsInfos` instead of `allowedMtcProviders`).
    // SAME CRITICAL ROUTE-ORDERING REQUIREMENT: this route must stay registered before the
    // `{externalGroupId}` GET route below.
    //
    // UPDATE (ADR-0181, gap-closure task #106): the real, optional `ext-group-ids` array
    // query-param filter is now honored against `allowedMbsInfos`'s own real keys, same "ALL"
    // always-kept precedent as the `5g-vn-groups/pp-profile-data` sibling above (confirmed by
    // direct read of `Pp5gMbsGroupProfileData`'s own schema, which documents the identical special
    // key convention). ---

    const std::string mbs_group_pp_profile_data_path_pattern =
        std::string(kApiRoot) +
        "/subscription-data/group-data/mbs-group-membership/pp-profile-data";

    server.add_route(
        "GET",
        mbs_group_pp_profile_data_path_pattern,
        [&verifier, &mbs_group_pp_profile_data, &mbs_group_pp_profile_data_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto data = mbs_group_pp_profile_data.get();
            mbs_group_pp_profile_data_get_counter->Add(1);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No 5G MBS Group PP Profile Data");
            }
            if (auto ids_it = req.query_params.find("ext-group-ids");
                ids_it != req.query_params.end() && data->contains("allowedMbsInfos")) {
                const auto requested_ids = sbi_core::http2::split_form_array(ids_it->second);
                json filtered = json::object();
                for (const auto& [key, value] : data->at("allowedMbsInfos").items()) {
                    if (key == "ALL" ||
                        std::find(requested_ids.begin(), requested_ids.end(), key) !=
                            requested_ids.end()) {
                        filtered[key] = value;
                    }
                }
                json filtered_doc = *data;
                filtered_doc["allowedMbsInfos"] = filtered;
                return sbi_core::http2::Response::json(200, filtered_doc.dump());
            }
            return sbi_core::http2::Response::json(200, data->dump());
        });

    // --- Nudr_DataRepository: group-data individual 5G MBS Group Membership resource (ADR-0145,
    // gap-closure task #106) -- real GET+PUT+PATCH+DELETE per TS29505_Subscription_Data.yaml,
    // structurally an exact twin of the 5g-vn-groups resource above (PUT documents ONLY `201`,
    // PATCH real RFC 6902, NOT upsert-capable). Keyed by externalGroupId. Second real `group-data`
    // sub-resource closed after 5g-vn-groups/{externalGroupId} (ADR-0144). ---

    const std::string mbs_group_membership_path_pattern =
        std::string(kApiRoot) +
        "/subscription-data/group-data/mbs-group-membership/{externalGroupId}";

    server.add_route(
        "GET",
        mbs_group_membership_path_pattern,
        [&verifier, &mbs_group_membership, &mbs_group_membership_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ext_group_id = req.path_params.at("externalGroupId");
            auto data = mbs_group_membership.get(ext_group_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404,
                    "Not Found",
                    "No 5G MBS Group Membership for externalGroupId " + ext_group_id);
            }
            mbs_group_membership_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    server.add_route(
        "PUT",
        mbs_group_membership_path_pattern,
        [&verifier,
         &mbs_group_membership,
         &mbs_group_membership_write_counter,
         &subs_to_notify,
         &notify_client,
         mbs_group_membership_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::MulticastMbsGroupMemb>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ext_group_id = req.path_params.at("externalGroupId");
            json j = *body;
            mbs_group_membership.put(ext_group_id, j);
            mbs_group_membership_write_counter->Add(1);
            notify_subscribers_ue_less(
                subs_to_notify,
                notify_client,
                resolved_location(mbs_group_membership_path_pattern, req.path_params),
                change_replace(j));
            // Real spec: Create5GmbsGroup documents ONLY 201 as a success response (no
            // update-via-PUT status) -- confirmed by direct read, same precedent as 5g-vn-groups.
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace(
                "location", resolved_location(mbs_group_membership_path_pattern, req.path_params));
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "PATCH",
        mbs_group_membership_path_pattern,
        [&verifier,
         &mbs_group_membership,
         &mbs_group_membership_write_counter,
         &subs_to_notify,
         &notify_client,
         mbs_group_membership_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            json patch_ops;
            try {
                patch_ops = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return sbi_core::http2::problem_response(400, "Malformed JSON", e.what());
            }
            const auto ext_group_id = req.path_params.at("externalGroupId");
            std::optional<json> patched;
            try {
                patched = mbs_group_membership.apply_patch(ext_group_id, patch_ops);
            } catch (const json::exception& e) {
                return sbi_core::http2::problem_response(400, "Invalid JSON Patch", e.what());
            }
            if (!patched.has_value()) {
                return sbi_core::http2::problem_response(
                    404,
                    "Not Found",
                    "No 5G MBS Group Membership for externalGroupId " + ext_group_id);
            }
            mbs_group_membership_write_counter->Add(1);
            notify_subscribers_ue_less(
                subs_to_notify,
                notify_client,
                resolved_location(mbs_group_membership_path_pattern, req.path_params),
                change_from_json_patch(patch_ops));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "DELETE",
        mbs_group_membership_path_pattern,
        [&verifier,
         &mbs_group_membership,
         &mbs_group_membership_delete_counter,
         &subs_to_notify,
         &notify_client,
         mbs_group_membership_path_pattern](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ext_group_id = req.path_params.at("externalGroupId");
            if (!mbs_group_membership.remove(ext_group_id)) {
                return sbi_core::http2::problem_response(
                    404,
                    "Not Found",
                    "No 5G MBS Group Membership for externalGroupId " + ext_group_id);
            }
            mbs_group_membership_delete_counter->Add(1);
            notify_subscribers_ue_less(
                subs_to_notify,
                notify_client,
                resolved_location(mbs_group_membership_path_pattern, req.path_params),
                change_remove());
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- Nudr_DataRepository: group-data Event Exposure Data for a group resource (ADR-0146,
    // gap-closure task #106) -- real GET-only per TS29505_Subscription_Data.yaml, no
    // create/update operation exists at all, genuinely NOT per-UE (keyed by ueGroupId, real
    // schema VarUeGroupId). Seeded at startup for this project's own "anyUE" test case. ---

    const std::string group_ee_profile_data_path_pattern =
        std::string(kApiRoot) + "/subscription-data/group-data/{ueGroupId}/ee-profile-data";

    server.add_route(
        "GET",
        group_ee_profile_data_path_pattern,
        [&verifier, &group_ee_profile_data, &group_ee_profile_data_get_counter](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_group_id = req.path_params.at("ueGroupId");
            auto data = group_ee_profile_data.get(ue_group_id);
            if (!data.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No Group EE Profile Data for ueGroupId " + ue_group_id);
            }
            group_ee_profile_data_get_counter->Add(1);
            return sbi_core::http2::Response::json(200, data->dump());
        });

    std::thread(run_nrf_lifecycle, udr_instance_id, nrf_base_url).detach();

    // --- ADR-0253: Nudr_DataRepository `application-data` traffic-influence family ---
    // Paths taken from TS29504_Nudr_DR.yaml (which $refs TS29519_Application_Data.yaml, where the
    // real operations live). Methods and status codes read from that YAML directly, not inferred.
    const std::string influence_collection_path =
        std::string(kApiRoot) + "/application-data/influenceData";
    const std::string influence_item_path =
        std::string(kApiRoot) + "/application-data/influenceData/{influenceId}";
    const std::string influence_subs_collection_path =
        std::string(kApiRoot) + "/application-data/influenceData/subs-to-notify";
    const std::string influence_subs_item_path =
        std::string(kApiRoot) + "/application-data/influenceData/subs-to-notify/{subscriptionId}";

    // GET collection. Real, disclosed scope: the YAML defines 8 query parameters
    // (influence-Ids, dnns, snssais, internal-Group-Ids, internal-group-ids-Add,
    // subscriber-categories, supis, supp-feat). Only `influence-Ids` is honoured here -- it is the
    // one that filters on a field this store actually keys on. The others filter on fields inside
    // the stored TrafficInfluData document that this build does not index; applying them by
    // scanning would be possible but is NOT done, and an unhonoured filter returns the unfiltered
    // set rather than silently pretending to have filtered. Stated here rather than discovered.
    server.add_route(
        "GET",
        influence_collection_path,
        [&verifier, &traffic_influence_data](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto all = traffic_influence_data.list();
            const auto it = req.query_params.find("influence-Ids");
            if (it != req.query_params.end() && !it->second.empty()) {
                std::vector<std::string> wanted;
                std::string cur;
                for (const char c : it->second) {
                    if (c == ',') {
                        if (!cur.empty())
                            wanted.push_back(cur);
                        cur.clear();
                    } else {
                        cur.push_back(c);
                    }
                }
                if (!cur.empty())
                    wanted.push_back(cur);
                std::vector<nlohmann::json> filtered;
                for (const auto& doc : all) {
                    for (const auto& w : wanted) {
                        if (doc.contains("influenceId") && doc["influenceId"] == w) {
                            filtered.push_back(doc);
                            break;
                        }
                    }
                }
                all = filtered;
            }
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = nlohmann::json(all).dump();
            return resp;
        });

    server.add_route(
        "PUT",
        influence_item_path,
        [&verifier, &traffic_influence_data, influence_item_path](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::TrafficInfluData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto influence_id = req.path_params.at("influenceId");
            nlohmann::json j = *body;
            const bool is_new = traffic_influence_data.put(influence_id, j);
            sbi_core::http2::Response resp;
            resp.status = is_new ? 201 : 200; // real spec draws both, see the YAML
            resp.headers.emplace("content-type", "application/json");
            if (is_new) {
                resp.headers.emplace("location",
                                     resolved_location(influence_item_path, req.path_params));
            }
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "PATCH",
        influence_item_path,
        [&verifier, &traffic_influence_data](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            // Real RFC 7396 merge patch -- the YAML's request content type here is
            // application/merge-patch+json, confirmed directly rather than assumed from other
            // UDR resources (some of which are RFC 6902 instead).
            nlohmann::json patch;
            try {
                patch = nlohmann::json::parse(req.body);
            } catch (const std::exception& e) {
                return sbi_core::http2::problem_response(400, "Bad Request", e.what());
            }
            const auto influence_id = req.path_params.at("influenceId");
            auto updated = traffic_influence_data.merge_patch(influence_id, patch);
            if (!updated.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No influenceData with id " + influence_id);
            }
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = updated->dump();
            return resp;
        });

    server.add_route(
        "DELETE",
        influence_item_path,
        [&verifier, &traffic_influence_data](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto influence_id = req.path_params.at("influenceId");
            if (!traffic_influence_data.remove(influence_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No influenceData with id " + influence_id);
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "GET",
        influence_subs_collection_path,
        [&verifier, &traffic_influence_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = nlohmann::json(traffic_influence_subs.list()).dump();
            return resp;
        });

    // POST creates a subscription. Real, disclosed: the spec's own resource id is server-assigned
    // and this build derives it from a UUID, the same convention this file's other
    // server-assigned-id resources already use.
    server.add_route(
        "POST",
        influence_subs_collection_path,
        [&verifier, &traffic_influence_subs, influence_subs_item_path](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::TrafficInfluSub_Application_Data>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const std::string subscription_id = sbi_core::generate_uuid_v4();
            nlohmann::json j = *body;
            traffic_influence_subs.put(subscription_id, j);
            std::map<std::string, std::string> params{{"subscriptionId", subscription_id}};
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location", resolved_location(influence_subs_item_path, params));
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        influence_subs_item_path,
        [&verifier, &traffic_influence_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto subscription_id = req.path_params.at("subscriptionId");
            auto doc = traffic_influence_subs.get(subscription_id);
            if (!doc.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No subscription with id " + subscription_id);
            }
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = doc->dump();
            return resp;
        });

    server.add_route(
        "PUT",
        influence_subs_item_path,
        [&verifier, &traffic_influence_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::TrafficInfluSub_Application_Data>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto subscription_id = req.path_params.at("subscriptionId");
            nlohmann::json j = *body;
            traffic_influence_subs.put(subscription_id, j);
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "DELETE",
        influence_subs_item_path,
        [&verifier, &traffic_influence_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto subscription_id = req.path_params.at("subscriptionId");
            if (!traffic_influence_subs.remove(subscription_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No subscription with id " + subscription_id);
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- ADR-0254: remaining Nudr_DataRepository `application-data` resources ---
    // Methods, schemas and patch content types read per-path from TS29519_Application_Data.yaml
    // (which TS29504_Nudr_DR.yaml $refs). Note pfds has NO PATCH in the real spec while the other
    // three document resources do -- that asymmetry is the YAML's, not an oversight here.

    const std::string pfd_data_coll_path = std::string(kApiRoot) + "/application-data/pfds";
    const std::string pfd_data_item_path = std::string(kApiRoot) + "/application-data/pfds/{appId}";

    server.add_route(
        "GET", pfd_data_coll_path, [&verifier, &pfd_data](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = nlohmann::json(pfd_data.list()).dump();
            return resp;
        });

    server.add_route(
        "GET", pfd_data_item_path, [&verifier, &pfd_data](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("appId");
            auto doc = pfd_data.get(id);
            if (!doc.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such resource: " + id);
            }
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = doc->dump();
            return resp;
        });

    server.add_route(
        "PUT",
        pfd_data_item_path,
        [&verifier, &pfd_data, pfd_data_item_path](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::PfdDataForAppExt>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto id = req.path_params.at("appId");
            nlohmann::json j = *body;
            const bool is_new = pfd_data.put(id, j);
            sbi_core::http2::Response resp;
            resp.status = is_new ? 201 : 200;
            resp.headers.emplace("content-type", "application/json");
            if (is_new) {
                resp.headers.emplace("location",
                                     resolved_location(pfd_data_item_path, req.path_params));
            }
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "DELETE", pfd_data_item_path, [&verifier, &pfd_data](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("appId");
            if (!pfd_data.remove(id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such resource: " + id);
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    const std::string bdt_policy_data_coll_path =
        std::string(kApiRoot) + "/application-data/bdtPolicyData";
    const std::string bdt_policy_data_item_path =
        std::string(kApiRoot) + "/application-data/bdtPolicyData/{bdtPolicyId}";

    server.add_route(
        "GET",
        bdt_policy_data_coll_path,
        [&verifier, &bdt_policy_data](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = nlohmann::json(bdt_policy_data.list()).dump();
            return resp;
        });

    server.add_route(
        "GET",
        bdt_policy_data_item_path,
        [&verifier, &bdt_policy_data](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("bdtPolicyId");
            auto doc = bdt_policy_data.get(id);
            if (!doc.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such resource: " + id);
            }
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = doc->dump();
            return resp;
        });

    server.add_route(
        "PUT",
        bdt_policy_data_item_path,
        [&verifier, &bdt_policy_data, bdt_policy_data_item_path](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::BdtPolicyData_Application_Data>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto id = req.path_params.at("bdtPolicyId");
            nlohmann::json j = *body;
            const bool is_new = bdt_policy_data.put(id, j);
            sbi_core::http2::Response resp;
            resp.status = is_new ? 201 : 200;
            resp.headers.emplace("content-type", "application/json");
            if (is_new) {
                resp.headers.emplace("location",
                                     resolved_location(bdt_policy_data_item_path, req.path_params));
            }
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "DELETE",
        bdt_policy_data_item_path,
        [&verifier, &bdt_policy_data](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("bdtPolicyId");
            if (!bdt_policy_data.remove(id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such resource: " + id);
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // Real RFC 7396 merge patch (application/merge-patch+json per the YAML for this resource).
    server.add_route(
        "PATCH",
        bdt_policy_data_item_path,
        [&verifier, &bdt_policy_data](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            nlohmann::json patch;
            try {
                patch = nlohmann::json::parse(req.body);
            } catch (const std::exception& e) {
                return sbi_core::http2::problem_response(400, "Bad Request", e.what());
            }
            const auto id = req.path_params.at("bdtPolicyId");
            auto updated = bdt_policy_data.merge_patch(id, patch);
            if (!updated.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such resource: " + id);
            }
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = updated->dump();
            return resp;
        });

    const std::string iptv_config_data_coll_path =
        std::string(kApiRoot) + "/application-data/iptvConfigData";
    const std::string iptv_config_data_item_path =
        std::string(kApiRoot) + "/application-data/iptvConfigData/{configurationId}";

    server.add_route(
        "GET",
        iptv_config_data_coll_path,
        [&verifier, &iptv_config_data](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = nlohmann::json(iptv_config_data.list()).dump();
            return resp;
        });

    server.add_route(
        "GET",
        iptv_config_data_item_path,
        [&verifier, &iptv_config_data](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("configurationId");
            auto doc = iptv_config_data.get(id);
            if (!doc.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such resource: " + id);
            }
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = doc->dump();
            return resp;
        });

    server.add_route(
        "PUT",
        iptv_config_data_item_path,
        [&verifier, &iptv_config_data, iptv_config_data_item_path](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::IptvConfigData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto id = req.path_params.at("configurationId");
            nlohmann::json j = *body;
            const bool is_new = iptv_config_data.put(id, j);
            sbi_core::http2::Response resp;
            resp.status = is_new ? 201 : 200;
            resp.headers.emplace("content-type", "application/json");
            if (is_new) {
                resp.headers.emplace(
                    "location", resolved_location(iptv_config_data_item_path, req.path_params));
            }
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "DELETE",
        iptv_config_data_item_path,
        [&verifier, &iptv_config_data](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("configurationId");
            if (!iptv_config_data.remove(id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such resource: " + id);
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // Real RFC 7396 merge patch (application/merge-patch+json per the YAML for this resource).
    server.add_route(
        "PATCH",
        iptv_config_data_item_path,
        [&verifier, &iptv_config_data](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            nlohmann::json patch;
            try {
                patch = nlohmann::json::parse(req.body);
            } catch (const std::exception& e) {
                return sbi_core::http2::problem_response(400, "Bad Request", e.what());
            }
            const auto id = req.path_params.at("configurationId");
            auto updated = iptv_config_data.merge_patch(id, patch);
            if (!updated.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such resource: " + id);
            }
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = updated->dump();
            return resp;
        });

    const std::string service_param_data_coll_path =
        std::string(kApiRoot) + "/application-data/serviceParamData";
    const std::string service_param_data_item_path =
        std::string(kApiRoot) + "/application-data/serviceParamData/{serviceParamId}";

    server.add_route(
        "GET",
        service_param_data_coll_path,
        [&verifier, &service_param_data](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = nlohmann::json(service_param_data.list()).dump();
            return resp;
        });

    server.add_route(
        "GET",
        service_param_data_item_path,
        [&verifier, &service_param_data](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("serviceParamId");
            auto doc = service_param_data.get(id);
            if (!doc.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such resource: " + id);
            }
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = doc->dump();
            return resp;
        });

    server.add_route(
        "PUT",
        service_param_data_item_path,
        [&verifier, &service_param_data, service_param_data_item_path](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::ServiceParameterData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto id = req.path_params.at("serviceParamId");
            nlohmann::json j = *body;
            const bool is_new = service_param_data.put(id, j);
            sbi_core::http2::Response resp;
            resp.status = is_new ? 201 : 200;
            resp.headers.emplace("content-type", "application/json");
            if (is_new) {
                resp.headers.emplace(
                    "location", resolved_location(service_param_data_item_path, req.path_params));
            }
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "DELETE",
        service_param_data_item_path,
        [&verifier, &service_param_data](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("serviceParamId");
            if (!service_param_data.remove(id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such resource: " + id);
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // Real RFC 7396 merge patch (application/merge-patch+json per the YAML for this resource).
    server.add_route(
        "PATCH",
        service_param_data_item_path,
        [&verifier, &service_param_data](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            nlohmann::json patch;
            try {
                patch = nlohmann::json::parse(req.body);
            } catch (const std::exception& e) {
                return sbi_core::http2::problem_response(400, "Bad Request", e.what());
            }
            const auto id = req.path_params.at("serviceParamId");
            auto updated = service_param_data.merge_patch(id, patch);
            if (!updated.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No such resource: " + id);
            }
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = updated->dump();
            return resp;
        });

    const std::string app_subs_coll_path =
        std::string(kApiRoot) + "/application-data/subs-to-notify";
    const std::string app_subs_item_path =
        std::string(kApiRoot) + "/application-data/subs-to-notify/{subsId}";

    server.add_route(
        "GET",
        app_subs_coll_path,
        [&verifier, &application_data_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = nlohmann::json(application_data_subs.list()).dump();
            return resp;
        });

    server.add_route(
        "POST",
        app_subs_coll_path,
        [&verifier, &application_data_subs, app_subs_item_path](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::ApplicationDataSubs>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const std::string subs_id = sbi_core::generate_uuid_v4();
            nlohmann::json j = *body;
            application_data_subs.put(subs_id, j);
            std::map<std::string, std::string> params{{"subsId", subs_id}};
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location", resolved_location(app_subs_item_path, params));
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        app_subs_item_path,
        [&verifier, &application_data_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subsId");
            auto doc = application_data_subs.get(id);
            if (!doc.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No subscription: " + id);
            }
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = doc->dump();
            return resp;
        });

    server.add_route(
        "PUT",
        app_subs_item_path,
        [&verifier, &application_data_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::ApplicationDataSubs>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto id = req.path_params.at("subsId");
            nlohmann::json j = *body;
            application_data_subs.put(id, j);
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "DELETE",
        app_subs_item_path,
        [&verifier, &application_data_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("subsId");
            if (!application_data_subs.remove(id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No subscription: " + id);
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- ADR-0255: Nudr_DataRepository structured data for exposure (`/exposure-data/*`) ---
    // Paths from TS29504_Nudr_DR.yaml, which $refs TS29519_Exposure_Data.yaml where the real
    // operations and schemas live -- the same $ref indirection ADR-0253 documented.
    //
    // Method sets read per-path from the YAML, NOT generalised across the family. Three real
    // asymmetries this family has and which are reproduced here rather than smoothed over:
    //   * access-and-mobility-data has PUT/GET/DELETE/PATCH; session-management-data has NO PATCH.
    //   * subs-to-notify collection is POST-only -- there is no GET list operation, unlike
    //     application-data's own subs-to-notify collection (ADR-0254), which has both.
    //   * subs-to-notify item is PUT/DELETE only -- there is no GET on the individual
    //     subscription, again unlike application-data's.
    // PUT distinguishes 201 (created) from 200 (updated, body returned). The spec also documents
    // 204 as an alternative success for an update; this build consistently returns 200 with the
    // representation, the same choice ADR-0253/0254 made for their PUTs.

    const std::string exposure_am_data_path =
        std::string(kApiRoot) + "/exposure-data/{ueId}/access-and-mobility-data";

    server.add_route(
        "PUT",
        exposure_am_data_path,
        [&verifier, &exposure_am_data, exposure_am_data_path](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body = sbi_core::http2::parse_json_body<sbi_gen::AccessAndMobilityData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            nlohmann::json j = *body;
            const bool is_new = exposure_am_data.put(ue_id, j);
            sbi_core::http2::Response resp;
            resp.status = is_new ? 201 : 200;
            resp.headers.emplace("content-type", "application/json");
            if (is_new) {
                resp.headers.emplace("location",
                                     resolved_location(exposure_am_data_path, req.path_params));
            }
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        exposure_am_data_path,
        [&verifier, &exposure_am_data](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            auto doc = exposure_am_data.get(ue_id);
            if (!doc.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No access and mobility exposure data for ueId " + ue_id);
            }
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = doc->dump();
            return resp;
        });

    // PATCH here is RFC 7396 merge-patch, read from this path's own
    // `application/merge-patch+json` request content type. The real spec documents ONLY 204 for
    // success on this operation (no 200-with-body alternative), and documents 404, so this does
    // not upsert.
    server.add_route(
        "PATCH",
        exposure_am_data_path,
        [&verifier, &exposure_am_data](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            nlohmann::json patch;
            try {
                patch = nlohmann::json::parse(req.body);
            } catch (const nlohmann::json::parse_error& e) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", std::string("malformed merge patch: ") + e.what());
            }
            const auto ue_id = req.path_params.at("ueId");
            if (!exposure_am_data.merge_patch(ue_id, patch).has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No access and mobility exposure data for ueId " + ue_id);
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.add_route(
        "DELETE",
        exposure_am_data_path,
        [&verifier, &exposure_am_data](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            if (!exposure_am_data.remove(ue_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No access and mobility exposure data for ueId " + ue_id);
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    const std::string exposure_sm_data_path =
        std::string(kApiRoot) + "/exposure-data/{ueId}/session-management-data/{pduSessionId}";

    server.add_route(
        "PUT",
        exposure_sm_data_path,
        [&verifier, &exposure_sm_data, exposure_sm_data_path](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::PduSessionManagementData>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto pdu_session_id = req.path_params.at("pduSessionId");
            nlohmann::json j = *body;
            const bool is_new = exposure_sm_data.put(ue_id, pdu_session_id, j);
            sbi_core::http2::Response resp;
            resp.status = is_new ? 201 : 200;
            resp.headers.emplace("content-type", "application/json");
            if (is_new) {
                resp.headers.emplace("location",
                                     resolved_location(exposure_sm_data_path, req.path_params));
            }
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "GET",
        exposure_sm_data_path,
        [&verifier, &exposure_sm_data](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto pdu_session_id = req.path_params.at("pduSessionId");
            auto doc = exposure_sm_data.get(ue_id, pdu_session_id);
            if (!doc.has_value()) {
                return sbi_core::http2::problem_response(404,
                                                         "Not Found",
                                                         "No session management exposure data for "
                                                         "ueId " +
                                                             ue_id + " pduSessionId " +
                                                             pdu_session_id);
            }
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = doc->dump();
            return resp;
        });

    server.add_route(
        "DELETE",
        exposure_sm_data_path,
        [&verifier, &exposure_sm_data](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ue_id = req.path_params.at("ueId");
            const auto pdu_session_id = req.path_params.at("pduSessionId");
            if (!exposure_sm_data.remove(ue_id, pdu_session_id)) {
                return sbi_core::http2::problem_response(404,
                                                         "Not Found",
                                                         "No session management exposure data for "
                                                         "ueId " +
                                                             ue_id + " pduSessionId " +
                                                             pdu_session_id);
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    const std::string exposure_subs_coll_path =
        std::string(kApiRoot) + "/exposure-data/subs-to-notify";
    const std::string exposure_subs_item_path =
        std::string(kApiRoot) + "/exposure-data/subs-to-notify/{subId}";

    // POST only -- the real spec defines no GET list on this collection.
    server.add_route(
        "POST",
        exposure_subs_coll_path,
        [&verifier, &exposure_data_subs, exposure_subs_item_path](
            const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::ExposureDataSubscription>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const std::string sub_id = sbi_core::generate_uuid_v4();
            nlohmann::json j = *body;
            exposure_data_subs.put(sub_id, j);
            std::map<std::string, std::string> params{{"subId", sub_id}};
            sbi_core::http2::Response resp;
            resp.status = 201;
            resp.headers.emplace("content-type", "application/json");
            resp.headers.emplace("location", resolved_location(exposure_subs_item_path, params));
            resp.body = j.dump();
            return resp;
        });

    // PUT is modify-only: the real spec documents 200/204 and 404 for this operation but no 201,
    // so an unknown subId is a 404 rather than a create. That is deliberately different from
    // ADR-0254's application-data subs-to-notify PUT, which upserts -- the difference is read
    // from each path's own response set, not carried across.
    server.add_route(
        "PUT",
        exposure_subs_item_path,
        [&verifier, &exposure_data_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            sbi_core::http2::Response err;
            auto body =
                sbi_core::http2::parse_json_body<sbi_gen::ExposureDataSubscription>(req, err);
            if (!body.has_value()) {
                return err;
            }
            const auto sub_id = req.path_params.at("subId");
            if (!exposure_data_subs.get(sub_id).has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No exposure data subscription: " + sub_id);
            }
            nlohmann::json j = *body;
            exposure_data_subs.put(sub_id, j);
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/json");
            resp.body = j.dump();
            return resp;
        });

    server.add_route(
        "DELETE",
        exposure_subs_item_path,
        [&verifier, &exposure_data_subs](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto sub_id = req.path_params.at("subId");
            if (!exposure_data_subs.remove(sub_id)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No exposure data subscription: " + sub_id);
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // --- ADR-0256: Nudr_DataRepository AIoT data (`/aiot-data/*`) ---
    // Paths from TS29506_Aiot_Data.yaml (TS 29.506 V19.2.0); every schema from
    // TS29369_Nadm_DM.yaml (TS 29.369 V19.2.0), because the paths file defines none of its own.
    //
    // The spec gives this group GET and PATCH only -- no create, replace or delete operation
    // exists for either resource -- so rows are seeded at startup (see main() above), the same
    // shape as the provisioned-data group (ADR-0069). That is the spec's, not an omission here.

    const std::string aiot_profile_item_path =
        std::string(kApiRoot) + "/aiot-data/aiot-device-profile-data/{aiotDevPermId}";
    const std::string aiot_profile_coll_path =
        std::string(kApiRoot) + "/aiot-data/aiot-device-profile-data";
    const std::string aiot_af_auth_path =
        std::string(kApiRoot) + "/aiot-data/af-authorization-data";

    server.add_route(
        "GET",
        aiot_profile_item_path,
        [&verifier, &aiot_device_profile_data](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto id = req.path_params.at("aiotDevPermId");
            auto doc = aiot_device_profile_data.get(id);
            if (!doc.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No AIoT device profile data for aiotDevPermId " + id);
            }
            return sbi_core::http2::Response::json(200, doc->dump());
        });

    // RFC 6902 JSON Patch. Real, disclosed YAML defect: this operation's request body `$ref`s a
    // BARE `PatchItem` under `application/json-patch+json`, but RFC 6902 bodies are arrays and
    // every other RFC-6902 route in this file takes an array. The array reading is implemented --
    // one contract, not a superset that accepts both.
    //
    // 204 rather than a 200 + `PatchResult` execution report: same deliberate choice this file
    // already makes for AmfContext3gpp and UpdateSmfContext (see this file's header comment) --
    // the spec permits either, and fabricating report items with no real per-op tracking behind
    // them would be inventing content.
    server.add_route(
        "PATCH",
        aiot_profile_item_path,
        [&verifier, &aiot_device_profile_data](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            nlohmann::json patch_ops;
            try {
                patch_ops = nlohmann::json::parse(req.body);
            } catch (const nlohmann::json::parse_error& e) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", std::string("malformed JSON Patch: ") + e.what());
            }
            if (!patch_ops.is_array()) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", "an RFC 6902 JSON Patch body must be an array");
            }
            const auto id = req.path_params.at("aiotDevPermId");
            try {
                if (!aiot_device_profile_data.patch(id, patch_ops).has_value()) {
                    return sbi_core::http2::problem_response(
                        404, "Not Found", "No AIoT device profile data for aiotDevPermId " + id);
                }
            } catch (const nlohmann::json::exception& e) {
                return sbi_core::http2::problem_response(
                    400,
                    "Bad Request",
                    std::string("JSON Patch could not be applied: ") + e.what());
            }
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    // `requester-aiot-devices-id` is a REQUIRED query parameter on this operation, so there is
    // deliberately no unfiltered "list every profile" behaviour: its absence is a 400, not an
    // empty or full list. Split with split_form_array (OpenAPI `style: form, explode: false`,
    // ADR-0161) rather than an ad-hoc comma split.
    server.add_route(
        "GET",
        aiot_profile_coll_path,
        [&verifier, &aiot_device_profile_data](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            const auto ids_it = req.query_params.find("requester-aiot-devices-id");
            if (ids_it == req.query_params.end()) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", "requester-aiot-devices-id is a required query parameter");
            }
            const auto ids = sbi_core::http2::split_form_array(ids_it->second);
            if (ids.empty()) {
                return sbi_core::http2::problem_response(
                    400,
                    "Bad Request",
                    "requester-aiot-devices-id must carry at least one AIoT device id");
            }
            sbi_gen::AiotDevProfileDataList list;
            try {
                for (const auto& doc : aiot_device_profile_data.get_many(ids)) {
                    list.aiotDevProfileDataList.push_back(doc.get<sbi_gen::AiotDevProfileData>());
                }
            } catch (const nlohmann::json::exception& e) {
                // A stored row that does not satisfy AiotDevProfileData is a real server-side
                // problem, and it must be ANSWERED. Without this catch the exception escapes the
                // handler and the connection is dropped with no response at all -- which is what
                // an earlier draft of this route did, found by the integration test rather than
                // by inspection.
                return sbi_core::http2::problem_response(
                    500,
                    "Internal Server Error",
                    std::string("stored AIoT device profile data is not a valid "
                                "AiotDevProfileData: ") +
                        e.what());
            }
            if (list.aiotDevProfileDataList.empty()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No AIoT device profile data for any requested device id");
            }
            return sbi_core::http2::Response::json(200, nlohmann::json(list).dump());
        });

    // Registered, and deliberately NOT implemented -- with the reason returned to the caller
    // rather than buried in a comment.
    //
    // `UpdateBundledAiotDeviceProfileData` is a real operation in TS29506_Aiot_Data.yaml, but the
    // YAML does not define what it operates on. Its sibling GET on this same path makes
    // `requester-aiot-devices-id` REQUIRED to select which devices form the bundle; this PATCH
    // has no device selector at all -- its only parameter is `supported-features`. Two independent
    // consequences, both fatal to guessing a scope:
    //   * the set of devices being modified is unbounded and undefined, and
    //   * RFC 6902 paths into an `AiotDevProfileDataList` document are INDEX-based
    //     (`/aiotDevProfileDataList/0/...`), so index 0 would name a different device depending on
    //     row ordering.
    // Applying the patch to every stored profile would be a mass-mutation endpoint invented here,
    // not read from the spec, so it is not shipped. The body is still parsed and validated so a
    // malformed request is answered as a malformed request. See ADR-0256; this is the one
    // operation in UDR's whole API surface left deliberately unimplemented on spec-ambiguity
    // grounds, and it is an open question for the user rather than a closed decision.
    server.add_route(
        "PATCH", aiot_profile_coll_path, [&verifier](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            nlohmann::json patch_ops;
            try {
                patch_ops = nlohmann::json::parse(req.body);
            } catch (const nlohmann::json::parse_error& e) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", std::string("malformed JSON Patch: ") + e.what());
            }
            if (!patch_ops.is_array()) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", "an RFC 6902 JSON Patch body must be an array");
            }
            if (req.query_params.find("supported-features") == req.query_params.end()) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", "supported-features is a required query parameter");
            }
            return sbi_core::http2::problem_response(
                501,
                "Not Implemented",
                "UpdateBundledAiotDeviceProfileData defines no device selector -- unlike the GET "
                "on this resource, which requires requester-aiot-devices-id -- so the set of "
                "devices the patch applies to is not determined by the spec. Deliberately not "
                "implemented rather than guessing a scope. Use the per-device PATCH at "
                "/aiot-data/aiot-device-profile-data/{aiotDevPermId}.");
        });

    // GET-only. `af-id` is an OPTIONAL filter and IS honoured here: it selects one entry from the
    // `afAuthData` map, which is exactly the field this document is keyed on, and the response
    // keeps the AfAuthorizationData envelope rather than returning a bare entry.
    server.add_route(
        "GET",
        aiot_af_auth_path,
        [&verifier, &aiot_af_authorization_data](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            auto doc = aiot_af_authorization_data.get();
            if (!doc.has_value()) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No AF authorization data");
            }
            // Same lesson as the bundled GET one route above: a stored row that does not match
            // its schema must be ANSWERED, not thrown out of the handler. `afAuthData` generates
            // as an opaque map, so its entries are validated explicitly here -- a round-trip
            // through AfAuthorizationData alone would not check them.
            if (!doc->contains("afAuthData") || !doc->at("afAuthData").is_object()) {
                return sbi_core::http2::problem_response(
                    500, "Internal Server Error", "stored AF authorization data has no afAuthData");
            }
            try {
                for (const auto& [af_id, entry] : doc->at("afAuthData").items()) {
                    (void)af_id;
                    (void)entry.get<sbi_gen::IndividualAfAuthorizationData>();
                }
            } catch (const nlohmann::json::exception& e) {
                return sbi_core::http2::problem_response(
                    500,
                    "Internal Server Error",
                    std::string("stored AF authorization data is not valid "
                                "IndividualAfAuthorizationData: ") +
                        e.what());
            }
            const auto af_id_it = req.query_params.find("af-id");
            if (af_id_it == req.query_params.end()) {
                return sbi_core::http2::Response::json(200, doc->dump());
            }
            const auto& all = doc->at("afAuthData");
            if (!all.contains(af_id_it->second)) {
                return sbi_core::http2::problem_response(
                    404, "Not Found", "No AF authorization data for af-id " + af_id_it->second);
            }
            nlohmann::json filtered;
            filtered["afAuthData"] = nlohmann::json{{af_id_it->second, all.at(af_id_it->second)}};
            return sbi_core::http2::Response::json(200, filtered.dump());
        });

    // --- ADR-0256: `/data-restoration-events` (TS29504_Nudr_DR.yaml) ---
    // Real, disclosed: this is the thinnest operation in UDR's whole API surface. The YAML gives
    // it a request body whose schema is literally `{}` -- no fields at all -- and defines NO 2xx
    // response, only `default` (ProblemDetails). So:
    //   * the body is persisted opaquely under a server-assigned id, because there is no shape to
    //     validate against and inventing one would violate this project's own first rule;
    //   * the success code is 204, not 201. 201 is the HTTP/TS 29.500 convention for a creating
    //     POST, but it obliges a `Location`, and the spec defines NO individual-subscription
    //     resource path -- any Location emitted here would resolve to nothing. 204 claims less and
    //     is the more honest of the two readings. Flagged for the user, not silently settled.
    //   * there is consequently no unsubscribe: nothing in the API can address a stored row.
    //   * the `restorationNotification` callback (`DataRestorationNotification`) is NOT delivered
    //     -- same disclosed gap class as every other subscription resource in this file.
    // Note the spec's own operationId typo, reproduced verbatim rather than tidied:
    // `CreateIndividualSubcription`.

    const std::string data_restoration_events_path =
        std::string(kApiRoot) + "/data-restoration-events";

    server.add_route(
        "POST",
        data_restoration_events_path,
        [&verifier, &data_restoration_subscriptions](const sbi_core::http2::Request& req) {
            if (auto auth = check_bearer(req, verifier); auth.has_value() && !auth->valid) {
                return sbi_core::http2::problem_response(401, "Unauthorized", auth->error);
            }
            nlohmann::json body;
            try {
                body = nlohmann::json::parse(req.body);
            } catch (const nlohmann::json::parse_error& e) {
                return sbi_core::http2::problem_response(
                    400, "Bad Request", std::string("malformed request body: ") + e.what());
            }
            data_restoration_subscriptions.add(sbi_core::generate_uuid_v4(), body);
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });

    server.start();
    spdlog::info("udr: listening on https://0.0.0.0:{} (TLS 1.3 + mTLS)", port);
    spdlog::info("udr: Prometheus metrics at http://{}/metrics", metrics_bind_address);
    sbi_core::run_multi_threaded(ioc);
    return 0;
}
