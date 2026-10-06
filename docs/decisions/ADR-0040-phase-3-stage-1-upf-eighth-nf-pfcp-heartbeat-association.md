## ADR-0040: Phase 3 Stage 1 -- UPF (eighth NF), PFCP Heartbeat + Association Setup

**Date:** 2026-08-08
**Status:** Accepted

**Context:** ADR-0039 (Stage 0) built the PFCP codec; this stage stands up the actual UPF binary
using it. TS 23.502's PDU Session Establishment flow requires a real N4/Sx Association between SMF
and UPF (established via the Association Setup procedure, TS 29.244 §6.2.6) before any Session
Establishment can happen -- this is the node-level bring-up UPF needs before Stage 3 can wire real
sessions through it, the same role NG Setup played for AMF/gNB in the earlier NGAP staging.

**UPF has no SBI service of its own -- confirmed against the real generated types before assuming
either way, not guessed.** No `Nupf_*` API exists anywhere in `specs/5G_APIs-REL-19/`: real 3GPP
architecture has SMF talk to UPF exclusively over N4/PFCP, never SBI. UPF's only real SBI role is
as an NRF *registration client* -- confirmed real (not fabricated) by finding `NFType::UPF` and
`NFProfile.upfInfo` (a genuine `UpfInfo` struct, with `sNssaiUpfInfoList`/`DnnUpfInfoItem` etc.) in
the generated `TS29122_CommonData_grp.hpp` before writing any registration code, so SMF can
discover UPF dynamically via NRF (Stage 2) rather than a hardcoded address. `nfs/upf/src/main.cpp`
therefore has no HTTP2 server at all -- a genuine architectural difference from every other NF in
this project, not an oversight: `run_nrf_lifecycle` (same pattern every NF already uses) runs on a
background thread purely as an outbound client, while the main thread runs a blocking PFCP/UDP
loop instead of `server.start()`.

**UPF's advertised `upfInfo` uses this project's actual configured S-NSSAI/DNN** (sst=1/sd=1,
matching `simulators/ransim/config/gnb.yaml`; dnn="internet", matching SMF's own existing default)
-- not an arbitrary placeholder, so Stage 3's real N4 Session Establishment will be requesting
exactly what UPF already declared it serves.

**PFCP/UDP transport: no dedicated wrapper class, unlike NGAP/SCTP.** `boost::asio::ip::udp::socket`
is used directly and synchronously (blocking `receive_from`/`send_to`) on UPF's main thread -- the
same "blocking I/O gets its own thread" discipline ADR-0006/ADR-0030 established, except here PFCP
is the *only* thing UPF's main thread does, so no explicit second thread is needed for it.

**Implemented this stage:** Heartbeat Request/Response (TS 29.244 §7.4.2, Recovery Time Stamp IE
only) and Association Setup Request/Response (§7.4.4.1/§7.4.4.2: UPF replies with Node ID, Cause=
Request accepted, Recovery Time Stamp, and an all-zero UP Function Features bitmask -- this build
declares no optional PFCP features, a disclosed minimal-viable scope, not a feature evaluated and
rejected). Every other PFCP message type is logged and ignored (disclosed, not silently
mishandled) -- Session Establishment/Modification/Deletion is Stage 3's scope, not this one's.

**Verification:** real interop, not just unit tests -- started real `nrf` + real `upf`, confirmed
real NRF registration (HTTP 201, `nfType=UPF`), then sent real hand-crafted PFCP datagrams (a
Python script constructing genuine wire bytes per this project's own `pfcp_core` codec) for both
Heartbeat Request and Association Setup Request over a real UDP socket to port 8805. Both responses
decoded byte-for-byte as expected: Heartbeat Response with the correct message type and Recovery
Time Stamp; Association Setup Response with Node ID (127.0.0.1), Cause=1 (accepted), Recovery Time
Stamp, and UP Function Features=0x0000, in the exact TLV order this build encodes them. This is not
interop against a second independent PFCP implementation (unlike the AMF/NGAP staging, which had
real UERANSIM as a genuine third-party peer) -- disclosed: no third-party PFCP client/CP-function
reference implementation is vendored in this project yet, so this verification proves UPF's own
codec and message handling are internally consistent and match this project's own spec-derived
understanding, not that it interops with an independent real-world PFCP peer. That remains a gap
Stage 2 (SMF as a real PFCP client) will close for real, the same way UERANSIM closed it for NGAP.
12 unit tests were already added in ADR-0039 for the codec layer this stage's message handlers use;
full `ctest` suite re-run clean, 107/107 passing, zero regressions.

**Consequence:** UPF exists, registers with NRF for real, and answers the two PFCP procedures
needed before any N4 session can be established. Stage 2 (SMF becomes a real PFCP client,
discovering UPF via NRF and performing a real Association Setup at SMF startup) is the next
increment -- that is what will finally provide the independent-peer verification this stage's own
disclosed limitation calls out as missing.

