find_package(RdKafka CONFIG REQUIRED) # ADR-0355
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

add_executable(conformance_tests test_round_trip.cpp test_multipart.cpp test_milenage.cpp test_eap_aka_prime.cpp test_sor_mac.cpp test_nas_codec.cpp test_nas_security.cpp test_nas_5gsm_codec.cpp test_pfcp_core.cpp test_bss_sid.cpp test_bss_sid_product.cpp test_diameter_core.cpp test_ss7_core.cpp test_tcap_core.cpp test_ss7_tcap_stack.cpp test_cap_core.cpp test_map_core.cpp test_tbcd_core.cpp test_tap3_core.cpp test_roaming_interconnect_tap3.cpp test_suci.cpp test_ai_inference.cpp test_cdr_asn1.cpp test_query_params.cpp test_nssf_dtos.cpp test_bsf_dtos.cpp test_nef_dtos.cpp test_scp_dtos.cpp test_eir_dtos.cpp test_smsf_dtos.cpp test_gmlc_dtos.cpp test_lmf_dtos.cpp test_nrf_bootstrapping_dtos.cpp test_ausf_upu_dtos.cpp test_lmf_broadcast_dataexposure_dtos.cpp test_rfc3339_datetime.cpp test_smf_ambr.cpp test_bill_run.cpp test_sbi_request_info.cpp test_cdr_event.cpp test_nwdaf_analytics.cpp test_li_core.cpp test_li_x1.cpp test_li_hi2.cpp test_smf_qos_mon.cpp test_nwdaf_qos_mon_collect.cpp test_nwdaf_vfl.cpp test_nwdaf_energy_efficiency.cpp ${CMAKE_SOURCE_DIR}/nfs/smf/src/ambr.cpp ${CMAKE_SOURCE_DIR}/nfs/smf/src/qos_mon_producer.cpp ${CMAKE_SOURCE_DIR}/nfs/chf/src/cdr_event_producer.cpp ${CMAKE_SOURCE_DIR}/nfs/nwdaf/src/analytics.cpp ${CMAKE_SOURCE_DIR}/nfs/nwdaf/src/qos_mon_collect.cpp ${CMAKE_SOURCE_DIR}/nfs/nwdaf/src/vfl_subscription_store.cpp ${CMAKE_SOURCE_DIR}/nfs/nwdaf/src/model_runtime.cpp
    ${CMAKE_SOURCE_DIR}/nfs/amf/src/nas_codec.cpp
    ${CMAKE_SOURCE_DIR}/nfs/smf/src/nas_5gsm_codec.cpp
    ${CMAKE_SOURCE_DIR}/nfs/chf/src/ai_inference.cpp
    ${CMAKE_SOURCE_DIR}/nfs/chf/src/cdr_asn1.cpp)
target_include_directories(conformance_tests PRIVATE ${CMAKE_SOURCE_DIR}/bss/roaming-interconnect/src)
target_link_libraries(conformance_tests PRIVATE balance_management_store GTest::gtest GTest::gtest_main sbi_generated sbi_core aka_crypto pfcp_core bss_sid diameter_core ss7_core tcap_core cap_core map_core tbcd_core tap3_core roaming_interconnect_store li_core onnxruntime::onnxruntime "$<LINK_LIBRARY:WHOLE_ARCHIVE,ONNX::onnx>" unofficial::libmariadb RdKafka::rdkafka++)
target_include_directories(conformance_tests PRIVATE ${CMAKE_SOURCE_DIR}/nfs/amf/src ${CMAKE_SOURCE_DIR}/nfs/smf/src ${CMAKE_SOURCE_DIR}/nfs/chf/src ${CMAKE_SOURCE_DIR}/nfs/nwdaf/src)
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

# ADR-0325: every 3GPP API root a server registers must equal its YAML's servers[0].url.
# Ten NEF roots were derived from spec filenames instead and no test could see it, because a
# test that builds its URL from the same constant the server registers is self-consistent by
# construction. This one reads the YAML.
add_test(
    NAME api_root_conformance
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/validate_api_roots.py ${CMAKE_SOURCE_DIR}
)

# ADR-0364: proves the TS 33.128 codec (hidden inside libli_core.so) and the NGAP codec (static,
# 83 colliding type names) coexist in one executable -- the shape the AMF/SMF will have once they
# host an IRI-POI. Its own binary because the link line is the test.
add_executable(test_li_ngap_coexistence test_li_ngap_coexistence.cpp)
target_link_libraries(test_li_ngap_coexistence PRIVATE li_core ngap_generated GTest::gtest GTest::gtest_main)
target_compile_features(test_li_ngap_coexistence PRIVATE cxx_std_23)
gtest_discover_tests(test_li_ngap_coexistence)

# LI increment 4 prerequisite 1: the AMF's NGAP UserLocationInformation -> xiri::Location parser
# (nfs/amf/src/li_location.cpp). Its own binary, same li_core + ngap_generated coexistence link.
add_executable(test_li_amf_location test_li_amf_location.cpp
    ${CMAKE_SOURCE_DIR}/nfs/amf/src/li_location.cpp)
target_include_directories(test_li_amf_location PRIVATE ${CMAKE_SOURCE_DIR}/nfs/amf/src)
target_link_libraries(test_li_amf_location PRIVATE li_core ngap_core ngap_generated GTest::gtest GTest::gtest_main)
target_compile_features(test_li_amf_location PRIVATE cxx_std_23)
gtest_discover_tests(test_li_amf_location)

# ADR-0440: the LI_X1 IdentifierAssociationExtensions gating parse (TS 33.128 table 6.2.2.1.1-1)
# against the combined ETSI + 3GPP X1 schema set. Its own binary: li_core + gtest only, so it
# builds without conformance_tests' heavy link line.
add_executable(test_li_x1_gating test_li_x1_gating.cpp)
target_link_libraries(test_li_x1_gating PRIVATE li_core GTest::gtest GTest::gtest_main)
target_compile_features(test_li_x1_gating PRIVATE cxx_std_23)
gtest_discover_tests(test_li_x1_gating)
