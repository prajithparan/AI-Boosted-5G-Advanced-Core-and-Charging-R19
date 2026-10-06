# ADMF -- the Administration Function of TS 33.127 clause 5.3.5: LICF (warrant -> provisioning) and
# LIPF (the proxy that provisions POIs/TFs/MDFs over LI_X1). ADR-0462. Terminates LI_HI1 (ETSI
# TS 103 120, XML over HTTPS + mTLS) from the LEA and is the X1 client of the POIs and the MDF2.
#
# Like li-mdf this is deliberately NOT an SBI NF: it registers with no NRF and exposes no 3GPP
# service API (TS 33.127 keeps the LI architecture off the service-based plane). It links sbi_core
# for the TLS 1.3 + mTLS HTTP/2 server and client only. Warrant state lives in its own PostgreSQL.
find_package(OpenSSL REQUIRED)
find_package(libpqxx CONFIG REQUIRED)

add_library(li_admf_core STATIC src/hi1_service.cpp src/hi1_store.cpp src/lipf.cpp src/lifecycle.cpp)
target_include_directories(li_admf_core PUBLIC src)
target_link_libraries(li_admf_core PUBLIC li_core nf_config sbi_core libpqxx::pqxx OpenSSL::Crypto)

add_executable(li-admf src/main.cpp)
target_link_libraries(li-admf PRIVATE li_admf_core OpenSSL::SSL OpenSSL::Crypto)
target_compile_definitions(li-admf PRIVATE
    CERTS_DIR="${CMAKE_SOURCE_DIR}/certs" CONFIG_DIR="${CMAKE_SOURCE_DIR}/config")
