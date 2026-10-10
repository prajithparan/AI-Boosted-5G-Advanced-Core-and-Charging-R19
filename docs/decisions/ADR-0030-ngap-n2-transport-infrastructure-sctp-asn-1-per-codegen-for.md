## ADR-0030: NGAP/N2 transport infrastructure (SCTP, ASN.1 PER codegen) for AMF's Registration path

**Date:** 2026-08-07
**Status:** Accepted

**Context:** ADR-0029 left AMF->PCF wiring open specifically because there is no real NAS/N1
Registration trigger in this build. The user chose to build that trigger for real: NGAP (N2,
gNB<->AMF, TS 38.413, ASN.1 PER over SCTP) and NAS-5GS (N1, UE<->AMF, TS 24.501), ending in a real
`CreateIndividualAMPolicyAssociation` call to PCF. This ADR covers the transport/codegen
infrastructure stage (Stage 0 of a staged plan); the NGAP/NAS procedure implementation itself is
covered by later ADRs as each stage lands. Planned and approved via Claude Code's plan-mode
workflow after three parallel research passes (AMF/sbi-core conventions, UERANSIM's real NG-Setup
through Registration message sequence with file:line evidence, and the asn1c/SCTP toolchain
state) -- see the plan file's own findings for the full trail; this ADR records the decisions that
came out of it.

**SCTP: system `libsctp-dev` (kernel one-to-one sockets), not vcpkg.** vcpkg has no
kernel-SCTP-socket port -- its only SCTP-adjacent port, `usrsctp`, is Google's userspace-over-UDP
stack (built for WebRTC data channels), not a binding to the Linux kernel SCTP the way NGAP/N2
actually runs. `libsctp-dev`/`libsctp1` were already installed on this system
(`/usr/include/netinet/sctp.h` present). New `libs/ngap-core` wraps a raw
`socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP)` one-to-one socket (`accept()` returns a fully
connected per-association socket directly, the same model as a TCP listening socket -- no
`SCTP_ASSOC_CHANGE` notification handling needed, unlike a one-to-many `SOCK_SEQPACKET` socket
multiplexing several associations over one fd). `sctp_sendmsg`/`sctp_recvmsg` call shapes (PPID
placement, flag handling, `RECEIVE_BUFFER_SIZE`) were copied from
`simulators/ransim/vendor/UERANSIM/src/lib/sctp/internal.cpp` -- a working reference
implementation already building and (per ADR-0016) attempting to connect against this exact lab --
not written from the `sctp_sendmsg` man page alone. NGAP's PPID is 60, confirmed against
UERANSIM's own `src/lib/sctp/types.hpp`, not from memory. Boost.Asio (already this project's
event-loop library for SBI/HTTP2) has no native SCTP support, so `SctpSocket` is meant to be
driven from a dedicated blocking-I/O thread, the same discipline ADR-0006 already established for
`run_nrf_lifecycle`. Disclosed simplification: every message in this build uses SCTP stream 0
only; real deployments reserve stream 0 for non-UE-associated signaling and assign dynamic
per-UE streams otherwise, not needed yet at this build's single-UE scope.

**ASN.1 PER codec: `asn1c` (BSD-licensed, `vlm/asn1c`), generating our own copy from a vendored
ASN.1 module, not from UERANSIM's own generated `.c`/`.h` files.** `tools/sbi-codegen` is entirely
OpenAPI/JSON-shaped and not reusable for ASN.1 -- new `libs/ngap-generated` mirrors only
`libs/sbi-generated`'s *CMake* pattern (configure-time `execute_process`, `add_custom_command` to
regenerate on source change, `file(GLOB)` into a `STATIC` lib), not its Python codegen internals.
The exact `asn1c` invocation flags
(`-pdu=all -fcompound-names -findirect-choice -fno-include-deps -no-gen-OER -gen-PER
-no-gen-example`) were copied from UERANSIM's own generated-file header comments (which cite the
precise command line used to produce them) -- proven correct against this exact `.asn` file, not
guessed.

**Real bug found and fixed during Stage 0's own verification, not assumed away:** `-no-gen-OER`
only suppresses per-type OER function *bodies*; the shared `constr_TYPE.h` skeleton still
unconditionally `#include`s `<oer_decoder.h>`/`<oer_encoder.h>` unless the caller also defines
`ASN_DISABLE_OER_SUPPORT` at *compile* time (a separate, undocumented-in-the-codegen-flags
requirement, discovered only because the generated code was actually compiled against real
`asn1c` output rather than assumed to work from the flag list alone). Confirmed via
`simulators/ransim/vendor/UERANSIM/src/asn/asn1c/CMakeLists.txt`, which sets this exact define for
its own identically-flagged NGAP codec build -- not guessed from the compiler error alone, cross-
checked against a working reference. `libs/ngap-generated/CMakeLists.txt` now sets this
`PUBLIC` on the `ngap_generated` target.

**`asn1c` itself is not installed system-wide, and this dev environment has no passwordless
sudo.** Built from `asn1c`'s own official GitHub release tarball (`v0.9.29`, matching the version
UERANSIM's own vendored codec was generated with, confirmed via that codec's generated-file
header) into `build-tools/asn1c/` -- a release tarball, not a git checkout, ships a
pre-generated `configure` script needing no `autoconf`/`automake`/`bison`/`flex` (none of which
were available either), only a C compiler and `make`. `build-tools/` is gitignored (already
covered by the existing `build-*/` glob pattern) -- this is a local build tool, not committed,
same category as vcpkg's own downloaded packages. `find_program` in
`libs/ngap-generated/CMakeLists.txt` checks system `PATH` first, falling back to this local
install location, so a CI runner with real `apt-get install asn1c` (the expected real-world path)
needs no special-casing.

**The ASN.1 module itself is now vendored into `specs/NGAP/ngap-17.9.asn`, committed, not read
from the gitignored `simulators/ransim/vendor/` tree at build time.** Reasoning: every NF/lib in
this project must be buildable standalone per CLAUDE.md, and `simulators/ransim/vendor/` only
exists after a separate, large, AGPL-3.0-licensed third-party fetch (`fetch-and-build.sh`) that a
normal `cmake --build .` should not hard-depend on. The ASN.1 module text itself is 3GPP-published
interface specification content, not UERANSIM's own copyrightable work -- the same category as the
OpenAPI YAML already committed under `specs/5G_APIs-REL-19/`, copied here for the identical reason.
**Disclosed version mismatch, not silently treated as equivalent:** this is TS 38.413 v17.9.0
(Release 17), not REL-19 -- no REL-19 NGAP ASN.1 module was available locally (NGAP has no OpenAPI
representation to source from `specs/5G_APIs-REL-19/` the way every other NF's API surface in this
project is sourced). The procedures this build actually uses (NG Setup, Initial UE Message,
Downlink/Uplink NAS Transport) are stable across R17-R19, but this remains a real, disclosed gap
against the project's stated REL-19 target, flagged in the vendored file's own header comment as
well as here. A stray finding while writing that header comment: ASN.1 `--` comments terminate at
the next `--` token even mid-line, not just at a newline -- this project's usual C++ comment style
(using `--` freely as an em dash) is unsafe inside actual `.asn` file comments and had to be
avoided when writing the provenance header.

**Real bug found and fixed in `simulators/ransim/config/ue.yaml`, verified against source, not
assumed correct:** two of its subscriber credential fields did not match `nfs/udm/src/main.cpp`'s
already-seeded `imsi-999700000000001` test subscriber (TS 35.207 Test Set 1). `op` was UERANSIM's
own unrelated published example default, not the TS 35.207 value UDM actually seeds -- would have
failed authentication regardless of NGAP/NAS correctness. `amf` (the Authentication Management
Field) was also wrong, and matters for a reason worth stating explicitly: UERANSIM's own MAC
validation (`simulators/ransim/vendor/UERANSIM/src/ue/nas/mm/auth.cpp`'s `calculateMilenage`,
lines ~472-521) recomputes the expected MAC-A using **this config file's configured `amf` value**,
not the AMF field actually received inside AUTN -- confirmed by reading that function directly,
not assumed from general AKA protocol knowledge (a spec-compliant USIM extracts AMF from the
received AUTN; this particular simulator's implementation does not, for whatever reason of its
own). Both fields now match UDM's seed (`op=CDC202D5123E20F62B6D676AC72CB318`, `amf=B9B9`); `key`
already matched. `protectionScheme: 0` (null-scheme SUCI, plaintext MSIN, no real ECIES needed)
was already correctly configured and did not need changing.

**Verification:** `ngap_generated` (1109 compiled objects) and `ngap_core` build clean under the
project's full `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion` flag set with no
suppressions beyond the standard generated-code `-w` already used for `sbi_generated`. Full project
rebuild from a clean configure succeeds; all 58 pre-existing tests still pass, unchanged -- this
stage adds build infrastructure only, no behavior change to any existing NF.

**Rejected alternative:** skip vendoring the ASN.1 module and require
`simulators/ransim/fetch-and-build.sh` to have run before `libs/ngap-generated` can configure.
Rejected because it would make a core library's buildability depend on a separate, large,
AGPL-licensed third-party fetch that CLAUDE.md's "every NF/lib buildable standalone" rule doesn't
otherwise require of anything else in this project.

**Consequence:** Stages 1 onward (NG Setup, Initial UE Message, Authentication, Security Mode,
Registration Accept/Complete, the real PCF call) build on this transport/codegen foundation. Each
stage gets its own verification against real `nr-gnb`/`nr-ue` processes and, where deterministic,
real `gtest` unit tests -- tracked in the approved plan, reported here as later ADRs as each stage
completes.

