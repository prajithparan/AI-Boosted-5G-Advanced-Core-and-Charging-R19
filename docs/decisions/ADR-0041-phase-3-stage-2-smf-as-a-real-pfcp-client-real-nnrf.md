## ADR-0041: Phase 3 Stage 2 -- SMF as a real PFCP client, real Nnrf_NFDiscovery, real Sx Association

**Date:** 2026-08-08
**Status:** Accepted

**Context:** ADR-0040 (Stage 1) left one disclosed gap: UPF's Heartbeat/Association Setup handling
had only been verified against a hand-crafted test script, not an independent third-party PFCP
implementation. This stage closes that gap for real by making SMF a genuine PFCP client that
performs a real Association Setup against real UPF -- two independently-built processes in this
project's own codebase, neither aware of the other's internals, actually interoperating over the
wire. This is also the next step TS 23.502's real PDU Session Establishment flow needs: SMF must
have an established Sx/N4 Association with a UPF before Stage 3's Session Establishment can target
one.

**Real `Nnrf_NFDiscovery`, not a hardcoded address -- and the first real use of an NRF capability
this project built but never actually called.** Checked NRF's own code (`nfs/nrf/src/main.cpp`)
before assuming either way: `SearchNFInstances` (`GET /nnrf-disc/v1/nf-instances?target-nf-type=...`)
has been implemented since NRF's own turn, but every NF-to-NF call in this project so far
(SMF->PCF, SMF->AMF, AMF->PCF/AUSF/SMF, ...) has used a hardcoded `kXxxBase` constant instead of
ever calling it -- a real, pre-existing gap this stage closes, not a new one introduced. SMF now
calls the real endpoint (`discover_upf_ipv4` in `nfs/smf/src/main.cpp`), parses the real
`NFInstances` response, and extracts UPF's real registered `ipv4Addresses` entry -- the exact value
`nfs/upf/src/main.cpp`'s own `run_nrf_lifecycle` (ADR-0040) put there. Retries forever with a 2s
backoff if no UPF is registered yet, same discipline `run_nrf_lifecycle` itself already uses for
NRF-not-up-yet.

**New `run_pfcp_lifecycle` in `nfs/smf/src/main.cpp`**, a dedicated thread (same "blocking
transport gets its own thread" discipline as every prior blocking-I/O thread in this project --
ADR-0006/ADR-0030/ADR-0039's own UDP-instead-of-a-wrapper-class choice) that builds and sends a
real PFCP Association Setup Request (Node ID, Recovery Time Stamp, CP Function Features -- the same
three IE types UPF's own Association Setup Response already uses, now exercised from the other
side), decodes UPF's real response, and confirms `Cause=Request accepted (1)` before proceeding.
Retry structure: T1 timer (2s, via `SO_RCVTIMEO` on the raw UDP socket -- Boost.Asio's synchronous
API has no built-in receive timeout, so this uses the same direct-POSIX-call discipline
`libs/ngap-core`'s SCTP wrapper already established where Asio doesn't cover something) and N1=3
retries per TS 29.244 §6.4's reliable-delivery model, with the whole procedure restarting after a
5s backoff if all N1 retries are exhausted -- both T1/N1 values are this build's own reasonable
fixed choices, since the spec itself leaves them implementation-specific, not a citable fixed
number.

**Disclosed, deliberate scope limit**: the established UPF endpoint is not yet persisted anywhere
other than a log line -- no cross-thread storage was added for `CreateSMContext`'s handler to read,
since nothing reads it yet. Stage 3 (real N4 Session Establishment wired into `CreateSMContext`)
is where that storage becomes genuinely needed, and adding it now would be exactly the kind of
"design for a hypothetical future requirement" CLAUDE.md's engineering rules warn against.

**Verification:** real end-to-end interop, first attempt -- real `nrf` + real `upf` + real `smf`,
three independent processes. `smf`'s log: `discovered UPF at 127.0.0.1 via Nnrf_NFDiscovery`
immediately followed by `PFCP Sx Association established with UPF at 127.0.0.1`. `upf`'s own,
independently-generated log confirms the same exchange from its side: `Sx Association Setup
accepted from 127.0.0.1`. This is the independent-peer verification ADR-0040 disclosed as missing
-- SMF and UPF are separately-linked binaries built from separate source files, with no shared
in-process state, genuinely exchanging real PFCP wire bytes over a real UDP socket. Full `ctest`
suite re-run clean, 107/107 passing, zero regressions (no new unit tests this stage -- the codec
layer this stage exercises was already covered by ADR-0039's 12 tests, and this stage's own new
code is a client-orchestration loop around that codec, verified via the real interop run above
rather than a second layer of mocked-transport unit tests).

**Consequence:** SMF and UPF have a real, independently-verified Sx/N4 Association. Stage 3 (real
PFCP Session Establishment wired into `CreateSMContext`, closing Phase 3's control-plane arc) is
the next increment -- it can now build on a real, proven Association rather than a hardcoded
assumption.

