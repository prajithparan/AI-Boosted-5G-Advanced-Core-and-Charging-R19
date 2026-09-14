# libFuzzer harnesses for this project's codec decode paths -- CLAUDE.md's own mandated tech stack
# names "libFuzzer for codec fuzzing" explicitly; ADR-0059 (P4.5 Stage 3) flagged that zero such
# targets existed anywhere in this repo as a real, disclosed gap. This directory closes that gap
# for the decoders that parse genuinely untrusted, off-the-wire bytes: Diameter (Gy/Rf/Sy),
# TCAP (Q.773 message/component/dialogue-portion), MAP, CAP, and the M3UA/SCCP transport codecs
# underneath them (PFCP already has its own header-only fuzz-worthy surface, deferred -- not part
# of this pass, see the "not yet fuzzed" note at the bottom of this file).
#
# Only built when 5GC_ENABLE_FUZZING=ON (root CMakeLists.txt already enforces Clang + mutual
# exclusion with ASan/TSan there) -- NOT part of the normal `ctest` suite. Each target is a
# standalone libFuzzer executable (`./fuzz_x -max_total_time=N corpus/x`), run manually or from a
# dedicated, time-boxed CI job -- matching CLAUDE.md's own CI decision ("keep sanitizer/fuzz jobs
# fast and targeted, not exhaustive, to fit GitHub-hosted free-tier runner limits").
#
# ADR-0065's own real, sandbox-specific finding: in this project's sandboxed dev environment, a
# libFuzzer "NEW_FUNC" coverage discovery deadlocks in the pipe read from its own spawned
# llvm-symbolizer subprocess (confirmed via /proc/<pid>/wchan, not a guess) -- looks exactly like a
# hung/crashed target from the outside. Fix: run with the sanitizer runtime's symbolizer disabled,
# NOT a libFuzzer CLI flag (`-symbolize=0` does not exist -- confirmed via `-help=1`):
#   ASAN_OPTIONS=symbolize=0 UBSAN_OPTIONS=symbolize=0 ./fuzz_x -max_total_time=N corpus/x
# Not re-tested outside this sandbox (e.g. the CI runner, bare metal) -- may not be needed there.

function(add_fuzz_target name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined -g -O1)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_libraries(${name} PRIVATE ${ARG_LIBS})
    target_compile_features(${name} PRIVATE cxx_std_23)
endfunction()

add_fuzz_target(fuzz_diameter SOURCES fuzz_diameter.cpp LIBS diameter_core)
add_fuzz_target(fuzz_tcap_message SOURCES fuzz_tcap_message.cpp LIBS tcap_core)
add_fuzz_target(fuzz_dialogue_portion SOURCES fuzz_dialogue_portion.cpp LIBS tcap_core)
add_fuzz_target(fuzz_map_operations SOURCES fuzz_map_operations.cpp LIBS map_core tcap_core)
add_fuzz_target(fuzz_cap_operations SOURCES fuzz_cap_operations.cpp LIBS cap_core tcap_core)
add_fuzz_target(fuzz_ss7_transport SOURCES fuzz_ss7_transport.cpp LIBS ss7_core)
# ADR-0364: X2/X3 PDU framing + BER XIRIPayload -- what the MDF2 will parse off the wire.
add_fuzz_target(fuzz_li_x2x3 SOURCES fuzz_li_x2x3.cpp LIBS li_core)

# Not yet fuzzed, real disclosed gap: libs/pfcp-core (N4/Sx, Phase 3) and libs/ngap-generated
# (asn1c-generated Aligned PER, Phase "AMF/NGAP") both also decode untrusted wire bytes but are
# out of this pass's scope -- this pass targets exactly P4.5's own protocol-translator stack
# (Diameter/TCAP/MAP/CAP/M3UA/SCCP), not a repo-wide fuzzing sweep.
