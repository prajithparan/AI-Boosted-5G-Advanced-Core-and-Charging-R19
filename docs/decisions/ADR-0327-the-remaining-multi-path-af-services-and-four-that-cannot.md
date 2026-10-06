## ADR-0327: the remaining multi-path AF services, and four that cannot be built

**Date:** 2026-09-10. **Status:** accepted. Services 36-53 of the 57. 119 operations.

CpProvisioning, ECRControl, GMDviaMBMSbyMB2, GMDviaMBMSbyxMB, NIDD, PfdManagement (TS 29.122);
AIoT, AKMA, AMPolicyAuthorization, EASDeployment, ECSAddress, MBSSession, MBSTMGI, MoLcsNotify,
TimeSyncExposure, UAVFlightAssistance, UEAddress, VFLNFDiscovery (TS 29.522).

These are the services the ADR-0326 generator could not express: they have genuine sub-resources
(NIDD's downlink deliveries under a configuration, PfdManagement's applications under a
transaction, TimeSyncExposure's configurations under a subscription) and non-CRUD operations.

### Three kinds of operation, classified from each path's real method set

- **103 CRUD** on collections and items, at any nesting depth. A nested resource is scoped by
  *every* parent parameter rather than the first: a NIDD downlink delivery is addressed by
  `scsAsId` AND `configurationId`, and scoping by `scsAsId` alone would let two AF configurations
  collide in one namespace.
- **2 singleton sub-resources** -- `AMPolicyAuthorization`'s `events-subscription`, a
  fixed-name child with PUT and DELETE.
- **14 custom actions** -- `/allocate`, `/deallocate`, `/retrieve`, `/query`, `/configure`,
  `/discover-nwdaf`, `/release-nwdaf`, `/remove-edis`, `/remove-ecsaddr`, `/request-inv`,
  `/request-cmd`. These validate their real request DTO and return **501**. They are queries and
  commands against downstreams that do not exist here (NWDAF, MBSF, GMLC, the AIoT function), and
  answering one with a success code would tell the AF that a TMGI was allocated or an EDI removed
  when nothing happened. A 501 is a true statement; a 200 with an empty body would not be.

The classification was wrong on the first pass: `events-subscription` was filed as a custom action
because its last segment is a literal. It has PUT and DELETE, so it is an addressable resource, and
as an "action" it would have been unreadable and undeletable. The rule is now the method set, not
the shape of the last path segment.

### Two defects caught before the compiler saw them

- `Location` was being built from `req.path`, which still carries the query string -- the router
  strips it only for matching (`libs/sbi-core/src/http2_server.cpp`). A POST with `?foo=bar` would
  have returned a Location containing the query.
- Several of these APIs define PUT as an upsert: the AF names the resource and PUT creates it if
  absent. `AfDocumentStore::put` only replaces, so `create_with_id` was added rather than letting a
  legitimate create fall through to a 404.

`MBSSession` also declares RFC 6902 on `/mbs-sessions/{mbsSessionRef}` while its sibling
`/mbs-pp/{mbsPpId}` declares merge-patch -- two patch semantics inside one specification, handled
per-operation from the declared content type rather than per-service.

### Four services cannot be built, and are not approximated

| Service | DTO it needs | Why not |
|---|---|---|
| TS29522_MBSUserService | `TS29580_Nmbsf_MBSUserService#MBSUserService` | TS29580 not in codegen set |
| TS29522_MBSUserDataIngestSession | `TS29580_Nmbsf_MBSUserDataIngestSession#MBSUserDataIngSession` | TS29580 not in codegen set |
| TS29522_DataReporting | `TS26532_Ndcaf_DataReporting#DataReportingSession` | TS26532 not in codegen set |
| TS29522_DataReportingProvisioning | `TS26532_Ndcaf_DataReportingProvisioning#...` | TS26532 not in codegen set |

Each references a schema in a specification that is not in `libs/sbi-generated/CMakeLists.txt`, so
the C++ type does not exist. Adding TS29580 and TS26532 pulls in two NFs' worth of types with their
own collision surface and a full regeneration -- a separate increment. Substituting a
similarly-named existing type would be exactly the fabrication ADR-0325 was written about.

### Where the AF surface actually stands

**53 of 57 services implemented, 5 with a real downstream.** The other 48 are accepted, validated
against their real generated DTOs, and stored at NEF -- or, for the 14 custom actions, refused with
a 501. Nothing in the core changes behaviour because an AF called one of them.

That ratio is the honest headline and it has not improved across four increments: the northbound
API is nearly complete and the southbound wiring behind it is not. Completing the API was the
stated goal and it is nearly met; it should not be read as exposure that works end to end.

