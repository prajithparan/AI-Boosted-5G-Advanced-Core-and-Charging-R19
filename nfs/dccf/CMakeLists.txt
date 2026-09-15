# DCCF -- TS 23.288 5A.2 / TS 29.574 (ADR-0366). Coordinates data collection: one source
# subscription per distinct data spec, fanned out through the MFAF; state in Valkey. See src/main.cpp.
find_package(redis++ CONFIG REQUIRED)

add_executable(dccf src/main.cpp src/subscription_store.cpp)
target_link_libraries(dccf PRIVATE sbi_core sbi_generated nf_config redis++::redis++_static)
target_compile_definitions(dccf PRIVATE CERTS_DIR="${CMAKE_SOURCE_DIR}/certs" CONFIG_DIR="${CMAKE_SOURCE_DIR}/config")
