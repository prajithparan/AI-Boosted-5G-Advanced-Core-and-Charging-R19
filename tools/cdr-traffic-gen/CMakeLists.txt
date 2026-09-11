# ADR-0337: drives CHF's real N40 path over the same production HTTP/2 + mTLS client the NFs use,
# so the CDRs it produces are made by the real charging engine rather than written to the table.
add_executable(cdr-traffic-gen src/main.cpp src/profiles.cpp)
target_link_libraries(cdr-traffic-gen PRIVATE sbi_core)
