# Phase 7 operator GUI (ADR-0420). The web app itself (gui/web, React + JSON Forms) is built by
# npm, not CMake; this tree builds the C++ backend-for-frontend and registers the schema checks.
add_subdirectory(bff)

if(5GC_BUILD_TESTS AND BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    # ADR-0421: the committed TMF620 schema must equal what the generator derives from
    # libs/bss-sid/include/bss_sid/product.hpp + the overlay.
    add_test(NAME gui_tmf620_schema_up_to_date
             COMMAND Python3::Interpreter
                     ${CMAKE_CURRENT_SOURCE_DIR}/schema-gen/derive_tmf620_schema.py
                     ${CMAKE_SOURCE_DIR}
                     ${CMAKE_CURRENT_SOURCE_DIR}/web/src/schemas/tmf620.derived.json --check)
    # ADR-0421: the hand-transcribed provisioning schema, re-derived from the service source.
    add_test(NAME gui_provisioning_schema_matches_service
             COMMAND Python3::Interpreter
                     ${CMAKE_CURRENT_SOURCE_DIR}/schema-gen/check_provisioning_schema.py
                     ${CMAKE_SOURCE_DIR})
    # Both checks must actually reject drift.
    add_test(NAME gui_schema_checks_reject_drift
             COMMAND Python3::Interpreter
                     ${CMAKE_CURRENT_SOURCE_DIR}/schema-gen/test_schema_checks.py
                     ${CMAKE_SOURCE_DIR})
endif()
