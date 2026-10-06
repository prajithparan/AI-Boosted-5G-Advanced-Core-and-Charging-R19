## ADR-0326: fifteen AF services, generated from the spec rather than transcribed

**Date:** 2026-09-09. **Status:** accepted. Services 21-35 of the 57. 82 operations.

ChargeableParty (TS 29.122); 5GLANParameterProvision, AMInfluence, ApplyingBdtPolicy, DNAIMapping,
EcsAddressProvision, ImsParamProvision, ImsSessionManagement, MBSGroupMsgDelivery,
MSEventExposure, MemberUESelectionAssistance, PDTQPolicyNegotiation, RSLPPIParametersProvisioning,
VFLInference, VFLTraining (TS 29.522).

### Generated, because ADR-0325 was a transcription failure

ADR-0325's ten invented roots came from hand-writing what the YAML already states. So this batch
is emitted by a script that reads each spec and takes the root from `servers[0].url`, the path
templates and parameter names from `paths`, the method set from each path's operations, and the
DTO from each operation's request body. Nothing about a service is typed by hand, so the
filename-derivation that produced ADR-0325 is not expressible here. `api_root_conformance` passes
over all 105 root constants.

### Four things the verification caught that a hand pass would have shipped

- `5GLanParametersProvision` is generated as **`N5GLanParametersProvision`** -- the codegen prefixes
  a leading digit, because C++ identifiers cannot start with one.
- `VflInferSub` and `VflTrainingSubs` are collision-suffixed **`_VFLInference`** / **`_VFLTraining`**;
  NF-facing (`Nnef_`, `Nnwdaf_`) variants of both names exist and the AF-facing one is not the
  unsuffixed spelling.
- Several DTOs are not in the header named after their spec at all -- `AmInfluSub` and
  `AfEventExposureSubsc` live in `TS26510_CommonData_grp.hpp` (ADR-0010's SCC grouping), and
  `TS29522_AMInfluence.hpp` does not exist as a file. The include list is now computed from where
  each struct is actually defined. An earlier attempt in this same increment assumed header name
  == spec name and failed to compile: the mapping had been computed correctly and then not used,
  which is ADR-0325's exact shape caught in seconds instead of in a commit.
- **Two services declare `application/json-patch+json`, not `application/merge-patch+json`.**
  TS29522_ImsParamProvision and TS29522_ImsSessionManagement take RFC 6902 -- an ordered array of
  `PatchItem` operations -- where the other thirteen take RFC 7396. The generator assumed
  merge-patch uniformly, and merge-patching an RFC 6902 array would have stored the operations
  array *as the document*: no error, wrong data, silently. They now validate each element against
  the generated `PatchItem` and apply `nlohmann::json::patch` via a new
  `AfDocumentStore::json_patch`, which runs under the store's existing mutex so the read-modify-write
  is atomic against concurrent writers -- a get/apply/put in the handler would not have been. Same
  idiom the AMF already uses for its own RFC 6902 handling.

The uniform shape held for thirteen of fifteen. That is the ratio that makes a pattern feel safe
enough to stop checking, and it is the second time in two increments that the exceptions were only
visible by reading each spec individually.

### MBSUserService is deferred, not implemented

TS29522_MBSUserService's POST body is
`TS29580_Nmbsf_MBSUserService.yaml#/components/schemas/MBSUserService`, and **TS29580 is not in the
codegen input set** (`libs/sbi-generated/CMakeLists.txt`), so that type does not exist. Adding the
spec pulls in an MBSF NF's worth of types plus a full regeneration, with its own collision surface
-- a separate increment, not a rider on this one. It is excluded here rather than approximated with
a nearby type.

### Downstream: none of the fifteen

Unchanged from ADR-0324 and worth repeating rather than burying: these are accepted, validated
against their real generated DTOs, and stored at NEF. VFLInference and VFLTraining belong to NWDAF,
MBSGroupMsgDelivery to the 5MBS functions, AMInfluence and ApplyingBdtPolicy to PCF -- none wired.

Running total: **35 of 57 services, 5 with a real downstream**, 30 accepted-and-stored. Remaining:
21 multi-path services (NIDD, MBSSession, TimeSyncExposure and similar have genuine sub-resources
that the collection+item generator does not cover) plus MBSUserService.

