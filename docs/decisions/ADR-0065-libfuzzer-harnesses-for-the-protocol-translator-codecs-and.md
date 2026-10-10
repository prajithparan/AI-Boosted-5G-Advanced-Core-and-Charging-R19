## ADR-0065: libFuzzer harnesses for the protocol-translator codecs, and a real integer-overflow bug found and fixed in libs/tcap-core's BER decoder

**Date:** 2026-08-15
**Status:** Accepted. Closes CLAUDE.md's own mandated-tech-stack gap ("libFuzzer for codec
fuzzing") and ADR-0059's own repeatedly-flagged disclosed gap ("zero existing libFuzzer targets
anywhere in this codebase").

### What was built

`tests/fuzz/` -- six libFuzzer harnesses targeting every decoder in this project's real
protocol-translator stack that parses genuinely untrusted, off-the-wire bytes: `fuzz_diameter`
(RFC 6733 header + AVP), `fuzz_tcap_message` (Q.773 TC-message envelope + component decode),
`fuzz_dialogue_portion` (AARQ/AARE, ADR-0063/0064's own new codec), `fuzz_map_operations`
(TS 29.002 insertSubscriberData), `fuzz_cap_operations` (TS 29.078 gsmSSF/gsmSCF operations), and
`fuzz_ss7_transport` (RFC 4666 M3UA + ITU-T Q.713 SCCP). Gated behind a new
`5GC_ENABLE_FUZZING` CMake option (root `CMakeLists.txt`), mutually exclusive with the existing
`5GC_ENABLE_ASAN`/`5GC_ENABLE_TSAN` options, requires Clang (libFuzzer is Clang/LLVM-only --
CI already uses `clang-18` for its sanitizer/build/lint jobs, so no new toolchain dependency).
NOT part of the normal `ctest` suite -- standalone executables run manually or from a dedicated,
time-boxed job, matching CLAUDE.md's own CI decision ("keep sanitizer/fuzz jobs fast and targeted,
not exhaustive, to fit GitHub-hosted free-tier runner limits"). PFCP (`libs/pfcp-core`) and NGAP
(`libs/ngap-generated`) also decode untrusted wire bytes but are real, disclosed, out-of-scope for
this pass -- this targets exactly P4.5's own protocol-translator stack, not a repo-wide sweep.

### Real bug found: BER long-form length integer overflow, uncaught exception, process crash

Within the first few thousand fuzz iterations, `fuzz_dialogue_portion` crashed with
`terminate called after throwing an instance of 'std::length_error': cannot create std::vector
larger than max_size()`. Root cause, in `libs/tcap-core/src/ber.cpp`'s `decode_tlv` (the single
shared low-level TLV decoder every other decoder in this stack -- TCAP, MAP, CAP, dialogue-portion
-- is built on): the long-form BER length field lets an attacker supply up to 127 length-of-length
bytes, so the decoded `length` (a `std::size_t`) is fully attacker-controlled and can reach
`SIZE_MAX` (confirmed minimal trigger: 8 bytes of `0xFF`). The existing bounds check was
`if (pos + length > bytes.size())` -- with `length` near `SIZE_MAX`, `pos + length` overflows
(wraps) `size_t` arithmetic, which can make the check spuriously pass for an out-of-range length.
The following `tlv.value.assign(bytes.begin()+pos, bytes.begin()+(pos+length))` then builds an
iterator range with `first > last` (since the wrapped `pos+length` can end up smaller than `pos`),
which libstdc++'s `vector::assign` turns into a huge underflowed size request -- an uncaught
`std::length_error`, `std::terminate`, process abort. A real, network-reachable DoS: this exact
decode path is what UDM's MAP client and CHF's CAP server (ADR-0061/0064) run on every inbound
message from an SCTP peer.

**Fix**: compare `length > bytes.size() - pos` instead (`pos <= bytes.size()` is already
established earlier in the same function by the length-of-length bounds check, so
`bytes.size() - pos` cannot itself underflow) -- this is correct regardless of how large the
attacker-supplied `length` is, since no addition/subtraction on attacker-controlled operands
occurs. One-line fix, `libs/tcap-core/src/ber.cpp`. Regression test added:
`TcapBer.RejectsOverflowingLongFormLengthWithoutThrowing` (`tests/conformance/test_tcap_core.cpp`)
-- the same 8-byte trigger, asserts no throw and a real `std::nullopt` rejection.

### Real environment finding: ASan/libFuzzer's external symbolizer subprocess deadlocks in this sandbox

After the fix above, `fuzz_dialogue_portion` runs clean (300K+ exec/s sustained, no crash), but
`fuzz_tcap_message`, `fuzz_map_operations`, and `fuzz_cap_operations` initially appeared to hang
under the fuzzer's coverage-guided mutation -- no forward progress for minutes at a time. Root
cause, found by direct process inspection (`/proc/<pid>/wchan` = `anon_pipe_read`, plus a live
`/usr/bin/llvm-symbolizer-18 --demangle --inlines --default-arch=x86_64` child process sitting
idle in state `S`, confirmed via `ps --ppid`): this project's sandboxed dev environment does not
block the symbolizer subprocess from being *spawned*, but the parent-child pipe interaction
between libFuzzer's ASan-backed coverage reporter (triggered by its own `NEW_FUNC` -- "newly
covered function" -- diagnostic) and that subprocess deadlocks, with both ends left blocked on a
pipe read forever. Confirmed as the true cause, not a guess: the same binaries run cleanly (`Rl`
process state, real CPU time accumulating 1:1 with wall time, steady multi-hundred-thousand-exec/s
throughput, sustained and re-verified) when launched with `ASAN_OPTIONS=symbolize=0
UBSAN_OPTIONS=symbolize=0` in the environment, which stops libFuzzer from invoking the external
symbolizer at all. (The libFuzzer CLI flag `-symbolize=0` does NOT exist -- confirmed by its own
`-help=1` output; the fix is the sanitizer runtime's environment variable, not a fuzzer flag.) This
is a real, disclosed, sandbox-specific fact about running these targets in THIS dev environment, not
a bug in this project's own decode code -- both `gdb -p` (ptrace) and libFuzzer's own per-run
`-timeout` SIGALRM watchdog were also independently found non-functional in this same sandbox
during this investigation, consistent with a broader (but not further characterized here)
subprocess/signal restriction. `tests/fuzz/CMakeLists.txt`'s own header comment now records the
required invocation (`ASAN_OPTIONS=symbolize=0 UBSAN_OPTIONS=symbolize=0 ./fuzz_x ...`) for running
any of these targets in this sandbox; a bare-metal session or the CI runner itself may not need it
-- not re-tested there by this ADR.

Full rebuild + `ctest`: 239/239 tests pass (3 PostgreSQL-backed tests skipped, no live PostgreSQL
in this pass -- pre-existing, unrelated to this ADR), including the new regression test. No
regressions.

