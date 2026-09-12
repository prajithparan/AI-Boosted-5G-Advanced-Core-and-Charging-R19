# Diameter (RFC 6733 + RFC 4006 DCC) message header + AVP TLV codec. Pure codec library, no
# transport of its own yet (Stage 2, ADR-0059, not yet built). Hand-rolled rather than a
# freeDiameter link -- see docs/DECISIONS.md ADR-0059 for the full architecture decision and the
# real, vendored reference material (simulators/reference/freeDiameter/) every AVP/command
# constant in dictionary.hpp is grounded in.

add_library(diameter_core STATIC
    src/header.cpp
    src/avp.cpp
    src/avp_names.cpp
)

target_include_directories(diameter_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(diameter_core PUBLIC
    5gc_project_options
)

target_compile_features(diameter_core PUBLIC cxx_std_23)
