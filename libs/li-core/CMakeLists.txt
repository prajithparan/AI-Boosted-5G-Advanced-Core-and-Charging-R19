# Lawful Interception core (ADR-0364): the ETSI TS 103 221-2 X2/X3 PDU codec every POI and the
# MDF2/MDF3 share, the TS 33.128 xIRI payload facade over the asn1c-generated TS33128Payloads
# codec (libs/li-generated), the X1 provisioning codec and NE server (ADR-0372), and the LI_HI2
# mediation + ETSI TS 102 232-1 PS-PDU envelope the MDF2 delivers to the LEMF (ADR-0374).
#
# SHARED, unlike every other libs/* target, on purpose. li_generated's symbols collide with
# ngap_generated's (47 type names shared between TS 33.128 and TS 38.413, plus two copies of the
# asn1c runtime), and the AMF/SMF link ngap_generated. Folding li_generated's hidden-visibility
# objects into this shared object keeps them out of the executable's symbol space: the NF links
# li_core, sees only the li_core:: C++ API, and the generated headers are never on its include path
# (li_generated is linked PRIVATE). tests/conformance/test_li_ngap_coexistence is the proof.

find_package(tl-expected CONFIG REQUIRED)
# ADR-0372: libxml2 for the X1 XML codec (schema-validated), OpenSSL for the X2/X3 mTLS delivery
# client. Both linked PRIVATE -- no li_core:: header names an libxml2 or OpenSSL type, so a POI
# that links li_core sees only the C++ API.
find_package(LibXml2 REQUIRED)
find_package(OpenSSL REQUIRED)

add_library(li_core SHARED
    src/x2x3_pdu.cpp
    src/xiri.cpp
    src/x1.cpp
    src/x1_server.cpp
    src/x2x3_client.cpp
    src/hi2.cpp
    src/x2x3_server.cpp
)

target_include_directories(li_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(li_core
    PUBLIC 5gc_project_options tl::expected
    PRIVATE li_generated LibXml2::LibXml2 OpenSSL::SSL OpenSSL::Crypto
)

# ADR-0372: the ETSI X1 schema set (committed, BSD-3-Clause) the X1 codec validates against.
target_compile_definitions(li_core PRIVATE
    LI_ETSI_SCHEMA_DIR="${CMAKE_SOURCE_DIR}/specs/etsi"
)

# Everything not part of the li_core:: API stays inside the .so -- including every asn1c symbol.
# The public headers re-export their own declarations with `#pragma GCC visibility push(default)`
# (GCC and Clang both honour it), so nothing else needs an export macro.
set_target_properties(li_core PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# ADR-0372: the PRIVATE static archives (libxml2, OpenSSL, and li_generated's asn1c runtime) are
# compiled with default visibility in their own builds, so without this their symbols would land
# in libli_core.so's dynamic table and collide with the AMF's own OpenSSL/libxml2 (the AMF links
# both li_core and sbi_core). --exclude-libs,ALL drops every archive-originated symbol from the
# export table; the li_core:: API stays exported because it is compiled directly into the .so
# with the visibility-default pragma, not pulled from an archive. Same containment goal as the
# hidden-visibility presets, extended to the third-party archives ADR-0364 did not yet link.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(li_core PRIVATE "LINKER:--exclude-libs,ALL")
endif()

target_compile_features(li_core PUBLIC cxx_std_23)
