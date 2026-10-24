add_executable(nef src/main.cpp src/stores.cpp src/li_poi.cpp src/qos_mapping.cpp)
target_link_libraries(nef PRIVATE sbi_core sbi_generated nf_config li_core li_poi)
# See nfs/hello-nf/CMakeLists.txt for why this is a compile-time absolute path.
# CONFIG_DIR: docs/DECISIONS.md ADR-0077 -- separate checked-in config/nef.json.
target_compile_definitions(nef PRIVATE CERTS_DIR="${CMAKE_SOURCE_DIR}/certs" CONFIG_DIR="${CMAKE_SOURCE_DIR}/config")
