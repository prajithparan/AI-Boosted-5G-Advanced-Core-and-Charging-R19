add_executable(udm src/main.cpp src/stores.cpp src/map_client.cpp src/map_server.cpp)
target_link_libraries(udm PRIVATE sbi_core sbi_generated aka_crypto ss7_core tcap_core map_core tbcd_core nf_config)
# See nfs/hello-nf/CMakeLists.txt for why this is a compile-time absolute path.
# CONFIG_DIR: docs/DECISIONS.md ADR-0077 -- separate checked-in config/udm.json.
target_compile_definitions(udm PRIVATE CERTS_DIR="${CMAKE_SOURCE_DIR}/certs" CONFIG_DIR="${CMAKE_SOURCE_DIR}/config")
