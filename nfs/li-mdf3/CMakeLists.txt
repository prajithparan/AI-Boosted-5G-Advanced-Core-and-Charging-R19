# MDF3 -- Mediation and Delivery Function for CC (TS 33.127 5.3.4, ADR-0464). Terminates LI_X1
# (XML over the sbi_core HTTP/2 server, which is where the TLS 1.3 + mTLS listener already lives),
# LI_X3 and LI_HI3 (both li_core, raw TLS byte streams). Warrant state in Valkey.
#
# Links sbi_core for the HTTP/2 server ONLY -- this NF registers with no NRF and exposes no 3GPP
# service API (TS 33.127 keeps the LI architecture off the service-based plane).
find_package(redis++ CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

add_executable(li-mdf3 src/main.cpp)
target_link_libraries(li-mdf3 PRIVATE
    sbi_core li_core li_mdf_store nf_config redis++::redis++_static OpenSSL::SSL OpenSSL::Crypto)
target_compile_definitions(li-mdf3 PRIVATE
    CERTS_DIR="${CMAKE_SOURCE_DIR}/certs" CONFIG_DIR="${CMAKE_SOURCE_DIR}/config")
