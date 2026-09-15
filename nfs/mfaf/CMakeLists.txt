# MFAF -- TS 23.288 5A.3.2 / TS 29.576 (ADR-0365). The 3GPP adaptor over the Kafka Messaging
# Framework; state in Valkey, delivery by consumer group. See src/main.cpp.
find_package(redis++ CONFIG REQUIRED)

add_executable(mfaf src/main.cpp src/config_store.cpp src/classify.cpp)
target_link_libraries(mfaf PRIVATE sbi_core sbi_generated nf_config event_bus redis++::redis++_static)
target_compile_definitions(mfaf PRIVATE CERTS_DIR="${CMAKE_SOURCE_DIR}/certs" CONFIG_DIR="${CMAKE_SOURCE_DIR}/config")
