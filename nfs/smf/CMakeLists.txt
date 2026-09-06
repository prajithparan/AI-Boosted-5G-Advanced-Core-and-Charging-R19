add_executable(smf src/main.cpp src/sm_context_store.cpp src/event_subscription_store.cpp src/nas_5gsm_codec.cpp src/pfcp_peer.cpp src/ambr.cpp)
# ngap_core/ngap_generated: gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #101, ADR-0092) --
# SMF needs the real NGAP PER codec to decode/encode the PathSwitchRequestTransfer/
# PathSwitchRequestAcknowledgeTransfer transparent containers AMF relays verbatim as n2SmInfo (a
# real, legitimate shared-library dependency, not an NF-private-header violation -- same class of
# reuse nfs/amf/src/ngap_codec.cpp already established for AMF).
target_link_libraries(smf PRIVATE sbi_core sbi_generated pfcp_core ngap_core ngap_generated nf_config)
# See nfs/hello-nf/CMakeLists.txt for why this is a compile-time absolute path.
# CONFIG_DIR: docs/DECISIONS.md ADR-0077 -- separate checked-in config/smf.json.
target_compile_definitions(smf PRIVATE CERTS_DIR="${CMAKE_SOURCE_DIR}/certs" CONFIG_DIR="${CMAKE_SOURCE_DIR}/config")
