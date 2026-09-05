# Wires tools/sbi-codegen into the build: generates C++ DTOs + JSON
# serializers from the R19 3GPP OpenAPI YAML and compiles them into the
# sbi_generated library. See docs/DECISIONS.md ADR-0010 for why this exists
# (a custom generator) instead of openapi-generator.
#
# KNOWN LIMITATION: relies on `python3` on PATH having jinja2 and pyyaml
# installed (not vcpkg-managed -- there is no vcpkg C++ substitute for a
# Python code generator). CI installs these explicitly; see
# .github/workflows/ci.yml. Adding or removing a *pilot* YAML file, or a
# schema that changes which output file-group appears, needs a CMake
# re-configure for the file(GLOB) below to notice new/removed generated
# files -- a known limitation of globbing dynamically-generated sources,
# not silently hidden.

set(SBI_CODEGEN_SCRIPT ${CMAKE_SOURCE_DIR}/tools/sbi-codegen/generate.py)
set(SBI_CODEGEN_SPECS_DIR ${CMAKE_SOURCE_DIR}/specs/5G_APIs-REL-19)
set(SBI_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated/sbi_gen)

set(SBI_CODEGEN_PILOT_FILES
    TS29510_Nnrf_NFManagement.yaml
    TS29510_Nnrf_NFDiscovery.yaml
    TS29510_Nnrf_AccessToken.yaml
    TS29510_Nnrf_Bootstrapping.yaml
    TS29518_Namf_Communication.yaml
    TS29502_Nsmf_PDUSession.yaml
    TS29503_Nudm_UECM.yaml
    TS29503_Nudm_SDM.yaml
    TS29503_Nudm_UEAU.yaml
    TS29503_Nudm_EE.yaml
    TS29503_Nudm_PP.yaml
    TS29505_Subscription_Data.yaml
    TS29509_Nausf_UEAuthentication.yaml
    TS29507_Npcf_AMPolicyControl.yaml
    TS29512_Npcf_SMPolicyControl.yaml
    TS29514_Npcf_PolicyAuthorization.yaml
    TS29509_Nausf_SoRProtection.yaml
    TS29509_Nausf_UPUProtection.yaml
    TS29544_Nspaf_SecuredPacket.yaml
    TS32291_Nchf_ConvergedCharging.yaml
    TS32291_Nchf_OfflineOnlyCharging.yaml
    TS29594_Nchf_SpendingLimitControl.yaml
    TS29519_Policy_Data.yaml
    TS29571_CommonData.yaml
    TS29531_Nnssf_NSSelection.yaml
    TS29531_Nnssf_NSSAIAvailability.yaml
    TS29521_Nbsf_Management.yaml
    TS29551_Nnef_PFDmanagement.yaml
    TS29541_Nnef_SMService.yaml
    TS29591_Nnef_UEId.yaml
    TS29591_Nnef_DNAIMapping.yaml
    TS29522_DNAIMapping.yaml
    TS29522_TrafficInfluence.yaml
    TS29591_Nnef_EASDeployment.yaml
    TS29570_Nscp_EventExposure.yaml
    TS29511_N5g-eir_EquipmentIdentityCheck.yaml
    TS29540_Nsmsf_SMService.yaml
    TS29577_Nipsmgw_SMService.yaml
    TS29515_Ngmlc_Location.yaml
    TS29572_Nlmf_Location.yaml
    TS29572_Nlmf_Broadcast.yaml
    TS29572_Nlmf_DataExposure.yaml
    TS29504_Nudr_GroupIDmap.yaml
    TS29518_Namf_Location.yaml
    TS29518_Namf_EventExposure.yaml
    TS29518_Namf_AIoT.yaml
    TS29518_Namf_MBSBroadcast.yaml
    TS29518_Namf_MBSCommunication.yaml
    TS29518_Namf_MT.yaml
    TS29508_Nsmf_EventExposure.yaml
    TS29542_Nsmf_NIDD.yaml
    TS29503_Nudm_MT.yaml
    TS29503_Nudm_NIDDAU.yaml
    TS29503_Nudm_RSDS.yaml
    TS29503_Nudm_SSAU.yaml
    TS29503_Nudm_UEID.yaml
    TS29564_Nupf_EventExposure.yaml
    TS29564_Nupf_GetUEPrivateIPaddrAndIdentifiers.yaml
    TS29523_Npcf_EventExposure.yaml
    TS29525_Npcf_UEPolicyControl.yaml
    TS29534_Npcf_AMPolicyAuthorization.yaml
    TS29537_Npcf_MBSPolicyAuthorization.yaml
    TS29537_Npcf_MBSPolicyControl.yaml
    TS29543_Npcf_PDTQPolicyControl.yaml
    TS29554_Npcf_BDTPolicyControl.yaml
    TS29541_Nnef_SMContext.yaml
    TS29256_Nnef_Authentication.yaml
    TS29591_Nnef_ECSAddress.yaml
    TS29522_ECSAddress.yaml
    TS29591_Nnef_EventExposure.yaml
    TS29517_Naf_EventExposure.yaml
    TS26512_EventExposure.yaml
    TS29591_Nnef_TrafficInfluenceData.yaml
    TS29519_Application_Data.yaml
    TS29519_Exposure_Data.yaml
    TS29369_Nadm_DM.yaml
    TS29591_Nnef_Inference.yaml
    TS29530_Naf_Inference.yaml
    TS29520_Nnwdaf_EventsSubscription.yaml
    TS29591_Nnef_Training.yaml
    TS29530_Naf_Training.yaml
    TS29591_Nnef_VFLInference.yaml
    TS29520_Nnwdaf_VFLInference.yaml
    TS29591_Nnef_VFLTraining.yaml
    TS29520_Nnwdaf_VFLTraining.yaml
    # ADR-0276: NSACF (TS 29.536) -- both of its real R19 API files, so the NF is generated
    # from its complete YAML surface rather than half of it.
    TS29536_Nnsacf_NSAC.yaml
    TS29536_Nnsacf_SliceEventExposure.yaml
)
set(SBI_CODEGEN_PILOT_PATHS
    ${SBI_CODEGEN_SPECS_DIR}/TS29510_Nnrf_NFManagement.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29510_Nnrf_NFDiscovery.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29510_Nnrf_AccessToken.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29510_Nnrf_Bootstrapping.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29502_Nsmf_PDUSession.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29518_Namf_Communication.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29503_Nudm_UECM.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29503_Nudm_SDM.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29503_Nudm_UEAU.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29503_Nudm_EE.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29503_Nudm_PP.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29505_Subscription_Data.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29509_Nausf_UEAuthentication.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29507_Npcf_AMPolicyControl.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29512_Npcf_SMPolicyControl.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29514_Npcf_PolicyAuthorization.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29509_Nausf_SoRProtection.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29509_Nausf_UPUProtection.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29544_Nspaf_SecuredPacket.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS32291_Nchf_ConvergedCharging.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS32291_Nchf_OfflineOnlyCharging.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29594_Nchf_SpendingLimitControl.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29519_Policy_Data.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29571_CommonData.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29531_Nnssf_NSSelection.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29531_Nnssf_NSSAIAvailability.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29521_Nbsf_Management.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29551_Nnef_PFDmanagement.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29541_Nnef_SMService.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29591_Nnef_UEId.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29591_Nnef_DNAIMapping.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29522_DNAIMapping.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29522_TrafficInfluence.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29591_Nnef_EASDeployment.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29570_Nscp_EventExposure.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29511_N5g-eir_EquipmentIdentityCheck.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29540_Nsmsf_SMService.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29577_Nipsmgw_SMService.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29515_Ngmlc_Location.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29572_Nlmf_Location.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29572_Nlmf_Broadcast.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29572_Nlmf_DataExposure.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29504_Nudr_GroupIDmap.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29518_Namf_Location.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29518_Namf_EventExposure.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29518_Namf_AIoT.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29518_Namf_MBSBroadcast.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29518_Namf_MBSCommunication.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29518_Namf_MT.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29508_Nsmf_EventExposure.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29542_Nsmf_NIDD.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29503_Nudm_MT.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29503_Nudm_NIDDAU.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29503_Nudm_RSDS.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29503_Nudm_SSAU.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29503_Nudm_UEID.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29564_Nupf_EventExposure.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29564_Nupf_GetUEPrivateIPaddrAndIdentifiers.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29523_Npcf_EventExposure.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29525_Npcf_UEPolicyControl.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29534_Npcf_AMPolicyAuthorization.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29537_Npcf_MBSPolicyAuthorization.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29537_Npcf_MBSPolicyControl.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29543_Npcf_PDTQPolicyControl.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29554_Npcf_BDTPolicyControl.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29541_Nnef_SMContext.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29256_Nnef_Authentication.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29591_Nnef_ECSAddress.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29522_ECSAddress.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29591_Nnef_EventExposure.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29517_Naf_EventExposure.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS26512_EventExposure.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29591_Nnef_TrafficInfluenceData.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29519_Application_Data.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29519_Exposure_Data.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29369_Nadm_DM.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29591_Nnef_Inference.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29530_Naf_Inference.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29520_Nnwdaf_EventsSubscription.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29591_Nnef_Training.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29530_Naf_Training.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29591_Nnef_VFLInference.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29520_Nnwdaf_VFLInference.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29591_Nnef_VFLTraining.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29520_Nnwdaf_VFLTraining.yaml
    # ADR-0276: NSACF (TS 29.536), this project's fifth Tier 2 NF -- Network Slice Admission
    # Control. Both of its real R19 API files, added together: the NSAC service itself and
    # its own event-exposure service, so the NF is generated from the complete YAML surface
    # rather than half of it.
    ${SBI_CODEGEN_SPECS_DIR}/TS29536_Nnsacf_NSAC.yaml
    ${SBI_CODEGEN_SPECS_DIR}/TS29536_Nnsacf_SliceEventExposure.yaml
)

file(GLOB_RECURSE SBI_CODEGEN_TOOL_SOURCES
    ${CMAKE_SOURCE_DIR}/tools/sbi-codegen/*.py
    ${CMAKE_SOURCE_DIR}/tools/sbi-codegen/templates/*.j2
)

# Real bug found and fixed (2026-08-16, while adding TS29519_Policy_Data.yaml as a pilot file):
# generate.py only ever WRITES files, never deletes ones from a previous run -- so a pilot-file-
# list change that moves a type from its own standalone header into a merged SCC group (or vice
# versa, see render.py's own module docstring on SCC grouping) leaves the OLD, now-stale header
# behind on disk. file(GLOB) below then picks up BOTH the stale standalone header and the new
# merged one, and the same types get compiled twice -- a real, reproducible "redefinition of
# struct" build failure, not a hypothetical one. Removing the whole output directory first, both
# here and in the build-time add_custom_command below, makes every regeneration a clean one --
# this was already flagged as a known limitation in this file's own header comment before being
# actually hit; fixed properly (clean-slate regeneration) rather than left as a recurring trap.
file(REMOVE_RECURSE ${SBI_GENERATED_DIR})

# Run once at configure time so the file(GLOB) below has output to find.
execute_process(
    COMMAND python3 ${SBI_CODEGEN_SCRIPT}
            ${SBI_CODEGEN_PILOT_FILES}
            --specs-dir ${SBI_CODEGEN_SPECS_DIR}
            --out ${SBI_GENERATED_DIR}
    RESULT_VARIABLE SBI_CODEGEN_RESULT
)
if(NOT SBI_CODEGEN_RESULT EQUAL 0)
    message(FATAL_ERROR "tools/sbi-codegen/generate.py failed at configure time (see output above)")
endif()

file(GLOB SBI_GENERATED_SOURCES ${SBI_GENERATED_DIR}/*.cpp)
file(GLOB SBI_GENERATED_HEADERS ${SBI_GENERATED_DIR}/*.hpp)

# Re-run automatically on rebuild whenever the generator, its templates, or
# the pilot YAML change.
add_custom_command(
    OUTPUT ${SBI_GENERATED_DIR}/.stamp
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${SBI_GENERATED_DIR}
    COMMAND python3 ${SBI_CODEGEN_SCRIPT}
            ${SBI_CODEGEN_PILOT_FILES}
            --specs-dir ${SBI_CODEGEN_SPECS_DIR}
            --out ${SBI_GENERATED_DIR}
    COMMAND ${CMAKE_COMMAND} -E touch ${SBI_GENERATED_DIR}/.stamp
    DEPENDS ${SBI_CODEGEN_TOOL_SOURCES} ${SBI_CODEGEN_PILOT_PATHS}
    COMMENT "Regenerating SBI DTOs from 3GPP R19 OpenAPI YAML (tools/sbi-codegen)"
)
add_custom_target(sbi_codegen_run DEPENDS ${SBI_GENERATED_DIR}/.stamp)

add_library(sbi_generated STATIC ${SBI_GENERATED_SOURCES})
add_dependencies(sbi_generated sbi_codegen_run)
target_include_directories(sbi_generated PUBLIC ${SBI_GENERATED_DIR})
target_link_libraries(sbi_generated PUBLIC sbi_core)
target_compile_features(sbi_generated PUBLIC cxx_std_23)
# Generated code doesn't get the hand-written-code warning treatment.
target_compile_options(sbi_generated PRIVATE -w)
