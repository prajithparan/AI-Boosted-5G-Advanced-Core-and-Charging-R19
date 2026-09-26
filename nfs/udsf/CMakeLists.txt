# UDSF -- TS 29.598 Nudsf_DataRepository + Nudsf_Timer (ADR-0400..ADR-0402). State in Valkey.
find_package(redis++ CONFIG REQUIRED)

# The logic as a library (the NF "shared library" of CLAUDE.md), linked by the binary and by the
# UDSF tests.
add_library(udsf_core STATIC
    src/search.cpp src/patch.cpp src/http_util.cpp src/store.cpp src/service.cpp
    src/dr_routes.cpp src/timer_routes.cpp src/worker.cpp)
target_include_directories(udsf_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(udsf_core PUBLIC sbi_core sbi_generated nf_config redis++::redis++_static)

add_executable(udsf src/main.cpp)
target_link_libraries(udsf PRIVATE udsf_core)
target_compile_definitions(udsf PRIVATE CERTS_DIR="${CMAKE_SOURCE_DIR}/certs" CONFIG_DIR="${CMAKE_SOURCE_DIR}/config")
