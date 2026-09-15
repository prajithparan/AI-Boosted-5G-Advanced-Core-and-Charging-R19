# ADRF -- TS 23.288 5B / TS 29.575 (ADR-0367). The analytics data repository: data and analytics
# records in Apache Doris (libmariadb, the MySQL protocol Doris speaks -- same port CHF and NWDAF
# use), ML models in PostgreSQL (libpqxx), control state in Valkey. See src/main.cpp.
find_package(redis++ CONFIG REQUIRED)
find_package(unofficial-libmariadb CONFIG REQUIRED)
find_package(libpqxx CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

add_executable(adrf src/main.cpp src/record_store.cpp src/model_store.cpp src/state_store.cpp)
target_link_libraries(adrf PRIVATE sbi_core sbi_generated nf_config redis++::redis++_static unofficial::libmariadb libpqxx::pqxx OpenSSL::Crypto)
target_compile_definitions(adrf PRIVATE CERTS_DIR="${CMAKE_SOURCE_DIR}/certs" CONFIG_DIR="${CMAKE_SOURCE_DIR}/config")
