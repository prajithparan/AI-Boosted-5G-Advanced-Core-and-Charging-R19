find_package(GTest CONFIG REQUIRED)
# P4.8/ADR-0074: real ONNX Runtime in-process inference, needed to compile
# nfs/chf/src/ai_inference.cpp directly into this test binary (same precedent as
# nfs/amf/src/nas_codec.cpp / nfs/smf/src/nas_5gsm_codec.cpp below). ONNX::onnx needs
# whole-archive linking -- see nfs/chf/CMakeLists.txt's own comment for the real static-linking
# bug this works around (found via this exact test binary).
find_package(onnxruntime CONFIG REQUIRED)
find_package(ONNX CONFIG REQUIRED)
# libmariadb (ADR-0192): test_cdr_asn1.cpp compiles nfs/chf/src/cdr_asn1.cpp directly, which
# includes cdr_asn1.hpp -> cdr.hpp -> <mysql.h> even though this test binary never constructs a
# real CdrWriter -- same real dependency chf itself needs (see nfs/chf/CMakeLists.txt's own
# comment for why libmariadb, not mariadb-connector-cpp).
find_package(unofficial-libmariadb CONFIG REQUIRED)

add_executable(conformance_tests test_round_trip.cpp test_multipart.cpp test_milenage.cpp test_eap_aka_prime.cpp test_sor_mac.cpp test_nas_codec.cpp test_nas_security.cpp test_nas_5gsm_codec.cpp test_pfcp_core.cpp test_bss_sid.cpp test_bss_sid_product.cpp test_diameter_core.cpp test_ss7_core.cpp test_tcap_core.cpp test_ss7_tcap_stack.cpp test_cap_core.cpp test_map_core.cpp test_tbcd_core.cpp test_tap3_core.cpp test_roaming_interconnect_tap3.cpp test_suci.cpp test_ai_inference.cpp test_cdr_asn1.cpp test_query_params.cpp test_nssf_dtos.cpp test_bsf_dtos.cpp test_nef_dtos.cpp test_scp_dtos.cpp test_eir_dtos.cpp test_smsf_dtos.cpp test_gmlc_dtos.cpp test_lmf_dtos.cpp test_nrf_bootstrapping_dtos.cpp test_ausf_upu_dtos.cpp test_lmf_broadcast_dataexposure_dtos.cpp test_rfc3339_datetime.cpp test_smf_ambr.cpp ${CMAKE_SOURCE_DIR}/nfs/smf/src/ambr.cpp
    ${CMAKE_SOURCE_DIR}/nfs/amf/src/nas_codec.cpp
    ${CMAKE_SOURCE_DIR}/nfs/smf/src/nas_5gsm_codec.cpp
    ${CMAKE_SOURCE_DIR}/nfs/chf/src/ai_inference.cpp
    ${CMAKE_SOURCE_DIR}/nfs/chf/src/cdr_asn1.cpp)
target_include_directories(conformance_tests PRIVATE ${CMAKE_SOURCE_DIR}/bss/roaming-interconnect/src)
target_link_libraries(conformance_tests PRIVATE GTest::gtest GTest::gtest_main sbi_generated sbi_core aka_crypto pfcp_core bss_sid diameter_core ss7_core tcap_core cap_core map_core tbcd_core tap3_core roaming_interconnect_store onnxruntime::onnxruntime "$<LINK_LIBRARY:WHOLE_ARCHIVE,ONNX::onnx>" unofficial::libmariadb)
target_include_directories(conformance_tests PRIVATE ${CMAKE_SOURCE_DIR}/nfs/amf/src ${CMAKE_SOURCE_DIR}/nfs/smf/src ${CMAKE_SOURCE_DIR}/nfs/chf/src)
target_compile_definitions(conformance_tests PRIVATE QUOTA_MODEL_FIXTURE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/test_quota_model.onnx")
target_compile_features(conformance_tests PRIVATE cxx_std_23)

set(SBI_CONFORMANCE_SAMPLES_DIR ${CMAKE_CURRENT_BINARY_DIR}/samples)

include(GoogleTest)
gtest_discover_tests(conformance_tests
    PROPERTIES ENVIRONMENT "SBI_CONFORMANCE_SAMPLES_DIR=${SBI_CONFORMANCE_SAMPLES_DIR}"
)

# Structural schema-conformance check against the real OpenAPI YAML, run
# after the C++ round-trip test has written its sample JSON. See
# validate_structural_conformance.py for what this does and does not check.
add_test(
    NAME structural_conformance
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/validate_structural_conformance.py ${SBI_CONFORMANCE_SAMPLES_DIR}
)
set_tests_properties(structural_conformance PROPERTIES
    DEPENDS "RoundTrip.GuamiNestedObjectRoundTrips;RoundTrip.NFTypeUnknownValueRoundTrips"
)
