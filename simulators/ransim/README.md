# ransim -- UERANSIM-based RAN/UE simulator

Arms-length external test tool, not part of this project's own source tree. See
`docs/DECISIONS.md` ADR-0016 for the full rationale (license, why it's fetched instead of
vendored, why it's a separate directory, current scope/gaps).

## What this is

[UERANSIM](https://github.com/aligungr/UERANSIM) (tag `v3.3.0`, commit
`6bf5a1a96aaef6ae8778b9d8b477ac6e2bbf8156`), an open-source, 3GPP-Release-15-conformant 5G-SA UE
and gNodeB simulator: real NGAP (N2) and NAS (N1) control-plane messages, real GTP-U (N3) user
plane. The NR radio interface itself is simulated over UDP loopback, not real RF.

**License: AGPL-3.0** (dual-licensed with a commercial option by its author). We run it
**unmodified**, as its own separate process, talking standard protocols (NGAP/SCTP, NAS, GTP-U)
to our NFs over the wire -- never linked into any binary in `libs/` or `nfs/`. Do not vendor its
source into this repository, do not copy code from it into `libs/`/`nfs/`, and do not modify the
checked-out copy in `vendor/` and redistribute that modified copy -- any of those would change the
license analysis in ADR-0016.

## Layout

- `fetch-and-build.sh` -- clones the pinned commit into `vendor/UERANSIM/` (gitignored, not
  committed) and builds it there.
- `vendor/` -- gitignored. Created by `fetch-and-build.sh`. Not part of this repository's git
  history.
- `config/gnb.yaml`, `config/ue.yaml` -- our lab config for `nr-gnb`/`nr-ue`, pointed at AMF's N2
  listener. **Corrected 2026-10-02 (docs-audit): this section was severely stale.** AMF has had a
  real NGAP/SCTP server for a long time (`nfs/amf/src/ngap_task.cpp`, logs "listening for NGAP/N2
  (SCTP)"); registration, PDU session establishment, and N2 handover have all worked end to end
  since early in this project (CLAUDE.md's Phase 2 end-to-end milestone). This file was apparently
  never revisited after that landed. Not independently re-verified in this pass: whether this
  specific `config/gnb.yaml`/`ue.yaml` pair is still the right shape for AMF's current NGAP config,
  or whether this exact UERANSIM pairing is still the project's live/preferred UE-driving tool
  versus something that superseded it -- check both before relying on this file to actually run.

## Usage

```
./fetch-and-build.sh
cd vendor/UERANSIM/build
./nr-gnb -c ../../../config/gnb.yaml
./nr-ue  -c ../../../config/ue.yaml   # separate terminal
```

## Prerequisites

`cmake`, a C++17 compiler, and `libsctp-dev` (Ubuntu/Debian package name; UERANSIM's NGAP
transport is real SCTP, not simulated).
