# oam-gui-bff: backend-for-frontend between the operator's browser and the BSS services
# (ADR-0422). Library + binary so the tests exercise the real route table.
add_library(oam_gui_bff_lib STATIC src/bff.cpp)
target_link_libraries(oam_gui_bff_lib PUBLIC sbi_core)
target_include_directories(oam_gui_bff_lib PUBLIC src)

add_executable(oam-gui-bff src/main.cpp)
target_link_libraries(oam-gui-bff PRIVATE oam_gui_bff_lib sbi_core nf_config)
# Relative paths in config/oam-gui-bff.json resolve against the repository root.
target_compile_definitions(oam-gui-bff PRIVATE REPO_ROOT="${CMAKE_SOURCE_DIR}")

if(5GC_BUILD_TESTS AND BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
    add_executable(oam_gui_bff_tests
        tests/test_bff_proxy.cpp
        tests/test_tmf620_schema_roundtrip.cpp)
    target_link_libraries(oam_gui_bff_tests PRIVATE
        oam_gui_bff_lib sbi_core bss_sid GTest::gtest_main)
    target_compile_definitions(oam_gui_bff_tests PRIVATE
        TEST_PKI_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/tests/make_test_pki.sh"
        TEST_WORK_DIR="${CMAKE_CURRENT_BINARY_DIR}/test-work"
        TMF620_SCHEMA_PATH="${CMAKE_SOURCE_DIR}/gui/web/src/schemas/tmf620.derived.json")
    include(GoogleTest)
    gtest_discover_tests(oam_gui_bff_tests DISCOVERY_MODE PRE_TEST)
endif()
