# redis-plus-plus (Valkey-protocol-compatible): real persistence for UeSecurityContextStore
# (gap-closure, docs/CAPABILITY_GAP_ANALYSIS.md task #100/ADR-0075) -- same real dependency every
# other NF's own Redis-backed hot-path store already uses (CHF's ChargingDataStore, PCF's
# SpendingLimitTrackingStore).
find_package(redis++ CONFIG REQUIRED)

add_executable(amf src/main.cpp src/ue_context_store.cpp src/ue_security_context_store.cpp
                   src/amf_ue_id_index_store.cpp src/gnb_association_registry.cpp
                   src/ngap_task.cpp src/ngap_handover.cpp
                   src/nas_codec.cpp src/li_poi.cpp src/li_location.cpp)
# li_core: the AMF's IRI-POI (src/li_poi.cpp, ADR-0377). This is the first binary to host BOTH the
# NGAP asn1c codec (ngap_generated) and li_core's TS33128Payloads codec, whose 83 shared type
# names are contained by li_core being a shared object with hidden visibility -- exactly the link
# shape tests/conformance/test_li_ngap_coexistence.cpp exists to prove. li_core carries its own
# LI_ETSI_SCHEMA_DIR, so the X1 server's schema validation needs specs/etsi at runtime (see
# deploy/docker/amf.Dockerfile).
target_link_libraries(amf PRIVATE sbi_core sbi_generated ngap_core ngap_generated li_core aka_crypto redis++::redis++_static nf_config)
# See nfs/hello-nf/CMakeLists.txt for why this is a compile-time absolute path.
# CONFIG_DIR: docs/DECISIONS.md ADR-0077 -- separate checked-in config/amf.json, not hardcoded
# defaults inside main.cpp.
target_compile_definitions(amf PRIVATE CERTS_DIR="${CMAKE_SOURCE_DIR}/certs" CONFIG_DIR="${CMAKE_SOURCE_DIR}/config")
