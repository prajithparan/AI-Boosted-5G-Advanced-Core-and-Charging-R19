## ADR-0016: UERANSIM as an arms-length RAN/UE simulator, fetched not vendored

**Date:** 2026-08-05
**Status:** Accepted

**Context:** AMF's real northbound surface is `Namf_Communication` (SBI/REST, generated from
`TS29518_Namf_Communication.yaml`), but the actual UE Registration procedure (TS 23.502 clause
4.2.2.2.2) starts over N2 (gNB -> AMF, NGAP/TS 38.413, ASN.1 over SCTP) and N1 (NAS), neither of
which is SBI/REST or expressible in the OpenAPI YAML -- there is nothing to codegen here, and
implementing an NGAP stack is a substantial protocol effort in its own right (comparable to Phase
3's PFCP work), deliberately deferred to a later, dedicated turn (`docs/TRACEABILITY.md` will carry
that gap until it's closed). To eventually test AMF's N2/N1 handling against a real, spec-following
peer rather than a hand-rolled test harness that might silently diverge from the actual NGAP/NAS
wire format, the user asked for a RAN/UE simulator.

**Decision:** UERANSIM (`github.com/aligungr/UERANSIM`, tag `v3.3.0`, commit
`6bf5a1a96aaef6ae8778b9d8b477ac6e2bbf8156`) -- an open-source, 3GPP-Release-15-conformant 5G-SA UE
and gNodeB simulator (NGAP/N2 and NAS/N1 control plane, GTP-U/N3 user plane; the NR radio interface
itself is simulated over UDP, not real RF, which is fine since we have no RF hardware target
either).

**License, checked not assumed (correction to what the user was initially told):** UERANSIM is
**AGPL-3.0** (dual-licensed with a commercial option), *not* GPL-3.0 as first stated in this
session before verifying against the actual GitHub repository metadata and `LICENSE` file --
flagged and corrected before this ADR was written, not silently left wrong. AGPL-3.0 is OSI-approved
(satisfies CLAUDE.md's "OSI-approved open source" bar) but is copyleft with a network-use clause
that would require offering source to users of a modified version served over a network. That
clause is not triggered here: UERANSIM runs **unmodified**, as its own separate OS process
(`nr-gnb`/`nr-ue` binaries), communicating with our NFs only over the wire (NGAP/SCTP, NAS, GTP-U)
-- never linked into, statically or dynamically, any `libs/` or `nfs/` binary. No derivative work is
created on our side; our Apache-2.0 code and UERANSIM's AGPL-3.0 code remain two separate programs
talking a standard protocol, the same relationship our NFs will have with any other black-box UE/RAN
implementation.

**Not vendored into the repository.** UERANSIM's source is not committed into this git history --
consistent with keeping AGPL-licensed source code out of an Apache-2.0 project's own tree (avoids
any ambiguity about what license governs what part of the repository) and with the existing pattern
of not vendoring vcpkg dependencies either. `simulators/ransim/fetch-and-build.sh` clones the pinned
commit into `simulators/ransim/vendor/UERANSIM/` (gitignored) and builds it there on demand.

**Directory:** `simulators/ransim/` at repo root, sibling to `nfs/`, `libs/`, `tests/` -- not nested
under `tests/`, since it is a standalone external tool (its own build, its own binaries) rather than
test code we author, even though its primary purpose here is testing our NFs.

**Scope of this session, explicitly:** simulator scaffold only -- fetch script, pinned commit,
build verified to actually produce `nr-gnb`/`nr-ue`/`nr-cli` binaries in this environment (see
`docs/TRACEABILITY.md`), and placeholder `gnb.yaml`/`ue.yaml` configs pointed at AMF's future N2
listener address (`127.0.0.5:38412`, not yet bound by anything -- AMF has no NGAP server yet). **Not
wired to AMF this session** -- there is nothing on the other end yet. Running `nr-gnb` against this
config today will fail to connect (SCTP `ECONNREFUSED`), which is the expected, disclosed state
until AMF's NGAP termination is built in a later turn.

**Test PLMN:** `mcc: '999'`, `mnc: '70'` -- not a 3GPP-assigned real-network PLMN; this is the
de facto "not a real network" test PLMN convention used across the open5gs/free5GC/UERANSIM lab
ecosystem (UERANSIM's own `config/open5gs-gnb.yaml` in the same repo uses the identical pair). A
config-value convention, not a fabricated spec fact -- disclosed as a choice we made, not something
transcribed from a 3GPP TS.

**Rejected alternative:** hand-roll a minimal NGAP/NAS test client that only emits the exact
messages our own AMF implementation expects. Rejected -- that risks tautological tests (our fake
peer and our AMF agreeing with each other while both diverge from the real TS 38.413/24.501 wire
format), which a real, independently-implemented, spec-conformant simulator avoids.

**Consequence / follow-up required:** AMF's own turn(s) still need to (a) implement Namf_Communication
per the procedure list already agreed with the user, and (b) implement NGAP/N2 termination (SCTP
transport, ASN.1 PER encode/decode per TS 38.413) before UERANSIM's `nr-gnb` can successfully reach
it. Both tracked as not-yet-done, not silently assumed complete because the simulator now exists.

---

