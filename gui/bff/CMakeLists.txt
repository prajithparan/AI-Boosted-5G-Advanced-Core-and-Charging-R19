# oam-gui-bff: backend-for-frontend between the operator's browser and the BSS services / NF
# configuration (ADR-0422..0425). Library + binary so the tests exercise the real route table.
find_package(libpqxx CONFIG REQUIRED)

add_library(oam_gui_bff_lib STATIC
    src/app.cpp
    src/auth.cpp
    src/bff.cpp
    src/config_mgmt.cpp
    src/iam.cpp
    src/redact.cpp
    src/util.cpp)
target_link_libraries(oam_gui_bff_lib PUBLIC sbi_core nf_config libpqxx::pqxx)
target_include_directories(oam_gui_bff_lib PUBLIC src)

add_executable(oam-gui-bff src/main.cpp)
target_link_libraries(oam-gui-bff PRIVATE oam_gui_bff_lib sbi_core nf_config)
# Relative paths in config/oam-gui-bff.json resolve against the repository root.
target_compile_definitions(oam-gui-bff PRIVATE REPO_ROOT="${CMAKE_SOURCE_DIR}")

if(5GC_BUILD_TESTS AND BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
    # Pure unit tests: no database, no network beyond loopback.
    add_executable(oam_gui_bff_unit_tests
        tests/test_tmf620_schema_roundtrip.cpp
        tests/test_bff_units.cpp)
    target_link_libraries(oam_gui_bff_unit_tests PRIVATE
        oam_gui_bff_lib sbi_core bss_sid GTest::gtest_main)
    target_compile_definitions(oam_gui_bff_unit_tests PRIVATE
        TMF620_SCHEMA_PATH="${CMAKE_SOURCE_DIR}/gui/web/src/schemas/tmf620.derived.json"
        NF_CONFIG_SCHEMA_DIR="${CMAKE_SOURCE_DIR}/gui/web/src/schemas/nf-config")
    include(GoogleTest)
    gtest_discover_tests(oam_gui_bff_unit_tests DISCOVERY_MODE PRE_TEST)

    # Security tests (ADR-0423/0424): real TLS on every leg, a fake OIDC IdP, a fake upstream,
    # and a PRIVATE PostgreSQL database built from deploy/db/operator_iam/*.sql and dropped
    # afterwards. Needs a reachable PostgreSQL superuser URL: TEST_POSTGRES_URL (CI sets it) or
    # the lab default in the test source. Labelled so a runner without PostgreSQL can -LE it.
    add_executable(oam_gui_bff_security_tests tests/test_bff_security.cpp)
    target_link_libraries(oam_gui_bff_security_tests PRIVATE
        oam_gui_bff_lib sbi_core GTest::gtest_main)
    target_compile_definitions(oam_gui_bff_security_tests PRIVATE
        TEST_PKI_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/tests/make_test_pki.sh"
        TEST_WORK_DIR="${CMAKE_CURRENT_BINARY_DIR}/test-work"
        IAM_DDL_DIR="${CMAKE_SOURCE_DIR}/deploy/db/operator_iam"
        NF_CONFIG_SCHEMA_DIR="${CMAKE_SOURCE_DIR}/gui/web/src/schemas/nf-config")
    gtest_discover_tests(oam_gui_bff_security_tests DISCOVERY_MODE PRE_TEST
                         PROPERTIES LABELS "needs-postgres" RUN_SERIAL TRUE)
endif()
