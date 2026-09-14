# Generates the TS 33.128 X2/X3 payload codec (Lawful Interception, TS 33.128 Annex A ASN.1) at
# configure time via asn1c and compiles it into the li_generated library. Same CMake shape as
# libs/ngap-generated (configure-time execute_process so file(GLOB) has output, add_custom_command
# to regenerate on module change), same patched asn1c -- see that file's header for the tool's
# provenance. See docs/DECISIONS.md ADR-0364.
#
# Two ASN.1 modules go in together, because asn1c resolves IMPORTS across the files on one command
# line, not from a search path:
#   - specs/3gpp/33128-attachments/TS33128Payloads.asn -- TS 33.128 V19.7.0 Annex A, module OID
#     ... ts33128(19) r19(19) version7(7), verbatim from the 3GPP attachment archive.
#   - specs/etsi/102232-3/IPAccessPDU-IPIRIPacketReport-subset.asn -- the one type
#     TS33128Payloads imports from ETSI TS 102 232-3 (IPIRIPacketReport) with its verbatim closure
#     (6 types), extracted by tools/li-asn1/trim_ipaccess.py from the ETSI schema repository at the
#     module version TS33128Payloads names (version18). Feeding asn1c the full IPAccessPDU would pull
#     in the whole legacy LI-PS-PDU/HI2/HI3 module family through its own IMPORTS.
#
# Encoding: X2 payloads are BER-encoded XIRIPayload structures (TS 33.128 table 5.3.2-1), so this
# codec is built for BER/DER only -- -no-gen-PER/-no-gen-OER. The asn1c skeleton still copies its
# per_*.c/oer_*.c support files; they compile, they are just never called.
#
# Symbol hiding -- the reason this library is NOT linked into NF binaries directly (ADR-0364):
# TS33128Payloads reuses 47 type names that TS 38.413 NGAP also defines (AllowedNSSAI, AMFPointer,
# GUAMI, TAI, ...) with different ASN.1 shapes, and both codecs carry a copy of the 36-file asn1c
# runtime skeleton. A binary that links ngap_generated and this library flat (the AMF and SMF will,
# once they host an IRI-POI) would have duplicate, layout-incompatible definitions of asn_DEF_*
# descriptors and struct types. Vanilla asn1c 0.9.29 has no -fprefix. So every symbol here is
# compiled with hidden visibility and the objects are folded into the li_core SHARED library, whose
# exported surface is li_core's own C++ API; the generated headers never reach a consumer's include
# path. tests/conformance/test_li_ngap_coexistence proves the two codecs coexist in one process.

find_program(ASN1C_EXECUTABLE asn1c
    PATHS ${CMAKE_SOURCE_DIR}/build-tools/asn1c/bin
    DOC "Path to vanilla asn1c 0.9.29, patched in place for Aligned PER (BSD-licensed, https://github.com/vlm/asn1c). See ADR-0031."
)
if(NOT ASN1C_EXECUTABLE)
    message(FATAL_ERROR "asn1c not found -- see libs/ngap-generated/CMakeLists.txt for how it is built into build-tools/asn1c/.")
endif()

set(LI_ASN1_MODULES
    ${CMAKE_SOURCE_DIR}/specs/3gpp/33128-attachments/TS33128Payloads.asn
    ${CMAKE_SOURCE_DIR}/specs/etsi/102232-3/IPAccessPDU-IPIRIPacketReport-subset.asn
)
set(LI_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated/li_gen)

# -fcompound-names: TS33128Payloads nests SEQUENCE members whose names repeat across types.
# -fno-include-deps: one flat directory, as for NGAP. -pdu=all: descriptor table for every type.
set(LI_CODEGEN_FLAGS
    -pdu=all -fcompound-names -fno-include-deps
    -no-gen-OER -no-gen-PER -no-gen-example
)

file(MAKE_DIRECTORY ${LI_GENERATED_DIR})

execute_process(
    COMMAND ${ASN1C_EXECUTABLE} ${LI_CODEGEN_FLAGS} -D ${LI_GENERATED_DIR} ${LI_ASN1_MODULES}
    WORKING_DIRECTORY ${LI_GENERATED_DIR}
    RESULT_VARIABLE LI_CODEGEN_RESULT
    OUTPUT_VARIABLE LI_CODEGEN_OUTPUT
    ERROR_VARIABLE LI_CODEGEN_OUTPUT
)
if(NOT LI_CODEGEN_RESULT EQUAL 0)
    message(FATAL_ERROR "asn1c failed generating the TS 33.128 codec at configure time:\n${LI_CODEGEN_OUTPUT}")
endif()

add_custom_command(
    OUTPUT ${LI_GENERATED_DIR}/.stamp
    COMMAND ${ASN1C_EXECUTABLE} ${LI_CODEGEN_FLAGS} -D ${LI_GENERATED_DIR} ${LI_ASN1_MODULES}
    COMMAND ${CMAKE_COMMAND} -E touch ${LI_GENERATED_DIR}/.stamp
    WORKING_DIRECTORY ${LI_GENERATED_DIR}
    DEPENDS ${LI_ASN1_MODULES}
    COMMENT "Regenerating TS 33.128 ASN.1 BER codec (asn1c)"
)
add_custom_target(li_codegen_run DEPENDS ${LI_GENERATED_DIR}/.stamp)

file(GLOB LI_GENERATED_SOURCES ${LI_GENERATED_DIR}/*.c)

add_library(li_generated STATIC ${LI_GENERATED_SOURCES})
add_dependencies(li_generated li_codegen_run)
# PUBLIC only for li_core, which is the sole intended consumer (its link is PRIVATE, so the
# directory does not propagate further -- that is the whole point).
target_include_directories(li_generated PUBLIC ${LI_GENERATED_DIR})
set_target_properties(li_generated PROPERTIES
    LINKER_LANGUAGE C
    C_VISIBILITY_PRESET hidden
    POSITION_INDEPENDENT_CODE ON
)
target_compile_options(li_generated PRIVATE -w)
# Same skeleton, same guard as ngap_generated: constr_TYPE.h includes the OER headers unless told.
target_compile_definitions(li_generated PUBLIC ASN_DISABLE_OER_SUPPORT)
