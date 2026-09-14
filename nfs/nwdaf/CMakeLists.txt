# NWDAF -- TS 23.288 / TS 29.520, Phase A (ADR-0358). Two of ten Nnwdaf services; see src/main.cpp.
# libmariadb: NWDAF reads the CHF feature store in Doris over the MySQL protocol -- its own reader
# (src/feature_store.cpp), not CHF's, since no NF includes another's private headers.
find_package(unofficial-libmariadb CONFIG REQUIRED)

add_executable(nwdaf src/main.cpp src/analytics.cpp src/feature_store.cpp)
target_link_libraries(nwdaf PRIVATE sbi_core sbi_generated nf_config unofficial::libmariadb)
target_compile_definitions(nwdaf PRIVATE CERTS_DIR="${CMAKE_SOURCE_DIR}/certs" CONFIG_DIR="${CMAKE_SOURCE_DIR}/config")
