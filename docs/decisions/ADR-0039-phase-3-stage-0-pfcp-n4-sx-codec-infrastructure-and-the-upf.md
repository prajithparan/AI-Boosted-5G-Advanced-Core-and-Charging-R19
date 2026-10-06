## ADR-0039: Phase 3 Stage 0 -- PFCP (N4/Sx) codec infrastructure, and the UPF datapath evaluation

**Date:** 2026-08-08
**Status:** Accepted

**Context:** Phase 2 is complete (ADR-0032 through ADR-0038). CLAUDE.md's Phase 3 scope is "User
plane: N4/PFCP, UPF datapath," explicitly flagging the datapath choice ("DPDK, VPP, or eBPF/XDP for
the UPF datapath -- evaluate and justify") as a real decision, not a default to assume. Staged with
the user before any code, mirroring the AMF NGAP/NAS staged plan's shape (ADR-0030 onward):
Stage 0 (this ADR) infra + datapath decision; Stage 1 UPF skeleton (Association Setup + Heartbeat);
Stage 2 SMF becomes a PFCP client (real Association Setup at startup); Stage 3 real N4 Session
Establishment wired into the existing `CreateSMContext` flow; Stage 4 UPF datapath (actual packet
forwarding).

**No OpenAPI YAML exists for PFCP/TS 29.244** -- it is a binary protocol over UDP (port 8805,
IANA-assigned), entirely outside the `specs/5G_APIs-REL-19/` corpus this project's SBI codegen
spine processes. This is not a gap to route around silently: per CLAUDE.md's source-of-truth rule,
a schema-driven codec requires the schema, and none exists here, so this is the same class of
justified exception NAS-5GS's hand-rolled codec already established
(`nfs/amf/src/nas_codec.hpp`) -- `tools/sbi-codegen` does not apply, a hand-rolled TLV codec does.

**Real spec source, verified before writing any code, not from memory:** the actual, official
3GPP TS 29.244 V14.3.0 (2018-03) specification PDF, located via `WebSearch` and fetched via
`WebFetch` from an ARIB (a genuine 3GPP Organizational Partner) archive mirror
(`arib.or.jp/.../29244-e30.pdf`). `WebFetch`'s HTML-conversion pipeline could not parse the PDF's
compressed content streams, but it saved the raw PDF locally, which this project's PDF-reading
tooling (`Read` with a `pages` range) parsed directly -- clause 7.1 (Transmission Order and Bit
Definitions), 7.2.2 (Message Header, both node-related and session-related forms), 7.3 (Message
Types, the full numeric enumeration table), and 8.1.1/8.2.x (the generic IE TLV format plus every
individual IE this Stage's messages use: Cause, Node ID, Recovery Time Stamp, UP/CP Function
Features) were read directly from the real spec text, not reconstructed from general protocol
knowledge or an LLM's training-data memory of PFCP. The PDF is vendored at `specs/PFCP/29244-e30.pdf`
(same "cite the exact source artifact this project's byte layouts depend on" convention
`specs/NGAP/ngap-17.9.asn` already established) so this citation stays reproducible in future
sessions rather than depending on an ephemeral tool-fetch cache. **Disclosed version gap**:
V14.3.0 was the release actually available to verify against, not this project's REL-19 baseline.
The core PFCP header/TLV format has been stable since PFCP's Release 14 introduction (CUPS) and no
later-release change to clause 7.2.2/8.1.1 themselves is known -- but this is a disclosed
assumption carried in `libs/pfcp-core/include/pfcp_core/header.hpp`'s own comment, not silently
presented as REL-19-text-verified. If a REL-19 PFCP YAML-equivalent or spec text becomes available,
this should be revisited, same as any other disclosed gap in this project.

**New `libs/pfcp-core`**: a pure codec library (no transport of its own) --
`header.{hpp,cpp}` (the 8-byte node-related / 16-byte session-related PFCP message header),
`ie.{hpp,cpp}` (the generic Type-Length-Value IE codec every PFCP IE uses, confirmed against the
real spec's Figure 8.1.1-1/8.1.1-2), and `common_ies.{hpp,cpp}` (Cause, Recovery Time Stamp, Node ID
IPv4-only form, UP/CP Function Features -- the specific IEs Stage 1's Heartbeat and Association
Setup messages need). Recovery Time Stamp's NTP-epoch-to-Unix-epoch conversion
(`2208988800` seconds) is standard RFC 5905 knowledge, not a 3GPP-specific fact, used without
further citation. **No dedicated UDP transport wrapper was written**, unlike `libs/ngap-core`'s
hand-written SCTP socket class: Boost.Asio (already this project's event-loop library for SBI/HTTP2
and the choice `ngap-core`'s own header cites as lacking SCTP support) supports UDP natively
(`boost::asio::ip::udp`), so NFs using this library talk UDP directly rather than needing a
purpose-built wrapper -- a genuine simplification, not a shortcut around a real need. 12 new unit
tests (`tests/conformance/test_pfcp_core.cpp`) cover header/IE round-trips, byte-exact layout
checks against the real spec figures, and malformed-input rejection. Node ID's IPv4-only scope is
a disclosed narrowing (this project only ever speaks IPv4, matching `libs/sbi-core`'s own existing
IPv4-only scope) -- not the full IPv4/IPv6/FQDN union TS 29.244 §8.2.38 supports.

**UPF datapath evaluation (DPDK vs. VPP vs. eBPF/XDP), decided: eBPF/XDP.** Real tradeoffs, not
guessed: DPDK gives the highest raw throughput via full kernel-bypass polling, but needs hugepages
and either a dedicated NIC bound to a DPDK-compatible driver or a paravirtualized one configured
for it -- a real hardware/lab-tier dependency this project's current dev environment (bare-metal
Ubuntu, MX450, no NIC reserved for kernel bypass) does not have, and CLAUDE.md's own "Reality check"
section already anticipates this ("UPF datapath... will want a larger lab tier"). VPP is itself
DPDK-based plus a full vector-packet-processing framework on top -- more moving parts to stand up
correctly than this turn's scope justifies, for the same underlying hardware dependency. eBPF/XDP
runs in-kernel, attaches to a normal interface (a TUN/veth pair is sufficient for a lab, no
dedicated NIC or hugepages needed), and is a real, modern, production-used technique (this is
Cilium's entire design, not a toy). Given this project's actual dev environment and CLAUDE.md's own
disclosed constraint, eBPF/XDP is the correct choice for Stage 4's initial implementation --
DPDK/VPP remain a legitimate later swap-in once a real dedicated-NIC lab tier exists, not a redo of
this decision, since N4/PFCP control-plane work (Stages 1-3) is entirely datapath-agnostic.
Approved by the user (asked explicitly, given CLAUDE.md's "evaluate and justify" instruction for
this specific choice) before any Stage 4 code is written -- Stage 4 itself is not part of this ADR,
which covers Stage 0 only.

**Verification:** 12 new unit tests, full `ctest` suite re-run clean, 107/107 passing, zero
regressions.

**Consequence:** Phase 3's control-plane groundwork can now begin for real (Stage 1: UPF skeleton
answering Heartbeat/Association Setup). No PFCP byte layout in this project is fabricated or
guessed -- every one is either a direct citation of the real V14.3.0 spec text (disclosed version
gap noted above) or explicitly marked as this project's own disclosed scope narrowing.

