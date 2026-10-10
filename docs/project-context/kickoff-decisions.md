# Project decisions (resolved at kickoff)

Moved verbatim from CLAUDE.md (section "Project decisions..."); see `docs/optimization/MOVE_LOG.md`.

- **Build strategy**: Greenfield. No fork of Open5GS or free5GC; they are
  reference reading only, not a starting codebase.
- **Spec source**: R19 OpenAPI YAML confirmed present, archive commit
  `bca84b60a37773133bcae97e5c6c0d10a93b47b6`, branch REL-19, release
  status Frozen, API version March 2026. 531 files. To be extracted into
  `specs/5G_APIs-REL-19/` with the commit hash recorded in
  `docs/DECISIONS.md` and `docs/TRACEABILITY.md`.
- **Phase 2 order**: full brief order — NRF -> AMF -> SMF -> UDM -> UDR ->
  AUSF -> PCF — ending with UE registration (TS 23.502 §4.2.2.2.2) and PDU
  session establishment (§4.3.2.2.1) end-to-end. No narrowed slice.
- **Hosting/license**: public GitHub repository, Apache-2.0 license
  (patent grant matters for a standards-adjacent project with likely
  corporate forks/contributors).
- **Dev environment**: bare-metal Ubuntu 24.04, MX450 GPU, CUDA 12.6.
  Not WSL2/VM. UPF datapath and serious model training will still want a
  larger lab tier when the time comes (see Reality check below).
- **CI**: GitHub-hosted free runners. Sanitizer (ASan/UBSan/TSan) and
  libFuzzer jobs must be designed to fit free-tier time/resource limits —
  keep them fast and targeted rather than exhaustive; revisit if runners
  become a bottleneck.
- **Cadence**: long-running, multi-session project worked in small
  increments — one NF or one subsystem per turn, matching the working
  style rules above. Not an accelerated single-shot demo.
- **AI/ML compute**: local MX450 is not the ceiling — larger training
  compute is expected later. Design the training-sidecar interface
  (Phase 5) to be swappable (e.g. pluggable backend/executor) rather than
  hardcoding for toy-scale local training only.
