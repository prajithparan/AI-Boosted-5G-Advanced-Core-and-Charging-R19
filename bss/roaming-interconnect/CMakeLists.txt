# E7 (Roaming and Interconnect Agreements) real PostgreSQL persistence -- schema.sql's own header
# explains the scoping: store library only this turn, no HTTP/REST service yet
# (CHARGING_PROMPT.md's P4.11 owns the full BSS-layer service surface). Real TMF651 Agreement
# persistence (bss_sid, libs/bss-sid) plus this project's own InterconnectAgreement/RoamingCdrFile
# linking tables.
find_package(libpqxx CONFIG REQUIRED)

add_library(roaming_interconnect_store STATIC src/store.cpp src/tap_batch.cpp)
target_link_libraries(roaming_interconnect_store PUBLIC bss_sid libpqxx::pqxx tap3_core)
target_include_directories(roaming_interconnect_store PUBLIC src)

add_executable(roaming-interconnect src/main.cpp)
target_link_libraries(roaming-interconnect PRIVATE sbi_core bss_sid roaming_interconnect_store nf_config)
# See nfs/hello-nf/CMakeLists.txt for why this is a compile-time absolute path.
# CONFIG_DIR: docs/DECISIONS.md ADR-0077 -- separate checked-in config/roaming-interconnect.json.
target_compile_definitions(roaming-interconnect PRIVATE CERTS_DIR="${CMAKE_SOURCE_DIR}/certs" CONFIG_DIR="${CMAKE_SOURCE_DIR}/config")
