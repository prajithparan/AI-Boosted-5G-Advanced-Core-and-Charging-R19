add_executable(nrf src/main.cpp src/registry.cpp src/discovery_authz.cpp)
target_link_libraries(nrf PRIVATE sbi_core sbi_generated nf_config)
# See nfs/hello-nf/CMakeLists.txt for why this is a compile-time absolute path.
# CONFIG_DIR: docs/DECISIONS.md ADR-0077 -- separate checked-in config/nrf.json.
target_compile_definitions(nrf PRIVATE CERTS_DIR="${CMAKE_SOURCE_DIR}/certs" CONFIG_DIR="${CMAKE_SOURCE_DIR}/config")
