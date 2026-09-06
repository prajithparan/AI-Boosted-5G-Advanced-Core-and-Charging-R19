# libpqxx: real PostgreSQL persistence (see schema.sql's own header for the ADR/design rationale).
find_package(libpqxx CONFIG REQUIRED)

add_library(balance_management_store STATIC src/store.cpp src/bill_run.cpp)
target_link_libraries(balance_management_store PUBLIC bss_sid libpqxx::pqxx)
target_include_directories(balance_management_store PUBLIC src)

add_executable(balance-management src/main.cpp)
target_link_libraries(balance-management PRIVATE sbi_core bss_sid balance_management_store nf_config)
# See nfs/hello-nf/CMakeLists.txt for why this is a compile-time absolute path.
# CONFIG_DIR: docs/DECISIONS.md ADR-0077 -- separate checked-in config/balance-management.json.
target_compile_definitions(balance-management PRIVATE CERTS_DIR="${CMAKE_SOURCE_DIR}/certs" CONFIG_DIR="${CMAKE_SOURCE_DIR}/config")
