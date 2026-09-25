# libpqxx: real PostgreSQL persistence for this service's stores (store.hpp/store.cpp) -- see
# docs/DATA_MODEL.md's E2 / docs/DECISIONS.md ADR-0053/ADR-0054 for the persistence-choice
# rationale. First PostgreSQL-backed component in this repo.
find_package(libpqxx CONFIG REQUIRED)

# Split out as its own static library (rather than compiling store.cpp straight into the
# product-catalog executable, as before ADR-0054) so tests/integration's real-Postgres test
# (test_product_catalog_postgres.cpp) can link against the actual store classes directly, instead
# of re-implementing them or only testing through the HTTP layer.
add_library(product_catalog_store STATIC src/store.cpp)
target_link_libraries(product_catalog_store PUBLIC bss_sid libpqxx::pqxx nf_config sbi_core)
target_include_directories(product_catalog_store PUBLIC src)

add_executable(product-catalog src/main.cpp)
target_link_libraries(product-catalog PRIVATE sbi_core bss_sid product_catalog_store nf_config)
# See nfs/hello-nf/CMakeLists.txt for why this is a compile-time absolute path.
target_compile_definitions(product-catalog PRIVATE CERTS_DIR="${CMAKE_SOURCE_DIR}/certs" CONFIG_DIR="${CMAKE_SOURCE_DIR}/config")
