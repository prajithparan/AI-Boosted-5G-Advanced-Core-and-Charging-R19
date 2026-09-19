# MDF2 -- Mediation and Delivery Function for IRI (TS 33.127 5.3.4, ADR-0377). Terminates LI_X1
# (XML over the sbi_core HTTP/2 server, which is where the TLS 1.3 + mTLS listener already lives),
# LI_X2 and LI_HI2 (both li_core, raw TLS byte streams). Warrant state in Valkey.
#
# Links sbi_core for the HTTP/2 server ONLY -- this NF registers with no NRF and exposes no 3GPP
# service API (TS 33.127 keeps the LI architecture off the service-based plane).
find_package(redis++ CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

add_executable(li-mdf src/main.cpp src/task_store.cpp)
target_link_libraries(li-mdf PRIVATE
    sbi_core li_core nf_config redis++::redis++_static OpenSSL::SSL OpenSSL::Crypto)
target_compile_definitions(li-mdf PRIVATE
    CERTS_DIR="${CMAKE_SOURCE_DIR}/certs" CONFIG_DIR="${CMAKE_SOURCE_DIR}/config")
