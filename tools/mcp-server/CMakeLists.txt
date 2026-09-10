# ADR-0331: read-only MCP tool server. Links sbi_core so it reaches backends over the SAME
# production HTTP/2 + mTLS client path every NF uses, and libpqxx for the PII access audit.
# find_package results are directory-scoped, so libpqxx must be located here even though
# nfs/chf already finds it -- an imported target from another directory is not visible.
find_package(libpqxx CONFIG REQUIRED)
find_package(unofficial-libmariadb CONFIG REQUIRED)

# Reuses CHF's own store implementations rather than re-implementing their queries here, so an
# MCP read cannot drift from the tested one. Disclosed coupling: this is a TOOL reading project
# stores, not an NF bypassing SBI -- CLAUDE.md's "NFs talk only over SBI" rule is about NFs.
add_executable(mcp-server
    src/main.cpp
    ${CMAKE_SOURCE_DIR}/nfs/chf/src/rating_decision_store.cpp
    ${CMAKE_SOURCE_DIR}/nfs/chf/src/cdr.cpp
    ${CMAKE_SOURCE_DIR}/nfs/chf/src/cdr_asn1.cpp
    src/pii_audit.cpp
    src/agent_scope.cpp
    src/tools.cpp
)
target_link_libraries(mcp-server PRIVATE sbi_core nf_config libpqxx::pqxx unofficial::libmariadb tcap_core tbcd_core)
target_include_directories(mcp-server PRIVATE ${CMAKE_SOURCE_DIR}/nfs/chf/src)
target_compile_definitions(mcp-server PRIVATE
    CERTS_DIR="${CMAKE_SOURCE_DIR}/certs"
    CONFIG_DIR="${CMAKE_SOURCE_DIR}/config"
)
