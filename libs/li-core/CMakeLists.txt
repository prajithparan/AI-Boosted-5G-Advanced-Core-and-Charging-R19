# Lawful Interception core (ADR-0364): the ETSI TS 103 221-2 X2/X3 PDU codec every POI and the
# MDF2/MDF3 share, and the TS 33.128 xIRI payload facade over the asn1c-generated TS33128Payloads
# codec (libs/li-generated).
#
# SHARED, unlike every other libs/* target, on purpose. li_generated's symbols collide with
# ngap_generated's (47 type names shared between TS 33.128 and TS 38.413, plus two copies of the
# asn1c runtime), and the AMF/SMF link ngap_generated. Folding li_generated's hidden-visibility
# objects into this shared object keeps them out of the executable's symbol space: the NF links
# li_core, sees only the li_core:: C++ API, and the generated headers are never on its include path
# (li_generated is linked PRIVATE). tests/conformance/test_li_ngap_coexistence is the proof.

find_package(tl-expected CONFIG REQUIRED)

add_library(li_core SHARED
    src/x2x3_pdu.cpp
    src/xiri.cpp
)

target_include_directories(li_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(li_core
    PUBLIC 5gc_project_options tl::expected
    PRIVATE li_generated
)

# Everything not part of the li_core:: API stays inside the .so -- including every asn1c symbol.
# The public headers re-export their own declarations with `#pragma GCC visibility push(default)`
# (GCC and Clang both honour it), so nothing else needs an export macro.
set_target_properties(li_core PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

target_compile_features(li_core PUBLIC cxx_std_23)
