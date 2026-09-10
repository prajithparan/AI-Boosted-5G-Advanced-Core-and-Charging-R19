# ADR-0331: read-only MCP tool server. Links sbi_core so it reaches backends over the SAME
# production HTTP/2 + mTLS client path every NF uses, and libpqxx for the PII access audit.
# find_package results are directory-scoped, so libpqxx must be located here even though
# nfs/chf already finds it -- an imported target from another directory is not visible.
find_package(libpqxx CONFIG REQUIRED)

add_executable(mcp-server
    src/main.cpp
    src/pii_audit.cpp
    src/agent_scope.cpp
    src/tools.cpp
)
target_link_libraries(mcp-server PRIVATE sbi_core nf_config libpqxx::pqxx)
target_compile_definitions(mcp-server PRIVATE
    CERTS_DIR="${CMAKE_SOURCE_DIR}/certs"
    CONFIG_DIR="${CMAKE_SOURCE_DIR}/config"
)
