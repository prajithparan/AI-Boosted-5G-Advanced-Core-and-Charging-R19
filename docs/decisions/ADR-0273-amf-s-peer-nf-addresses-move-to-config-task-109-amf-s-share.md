## ADR-0273: AMF's peer NF addresses move to config (task #109, AMF's share)

**Date:** 2026-09-04
**Status:** Accepted

### What changed

Four compile-time literals in AMF -- `kAusfBase` (7782), `kPcfBase` (7783), `kSmfBase` (7779, in
BOTH `ngap_task.cpp` and `ngap_handover.cpp`) and `kSelfBase` (7778) -- are gone. AMF's peer base
URLs now come from `config/amf.json` (`ausf_base_url`/`pcf_base_url`/`smf_base_url`), each with an
`AMF_*_BASE_URL` env override following `nf_config`'s existing per-key convention.

This is the standing rule this project already applies to every datastore URL: a deployment
endpoint never lives in a `.cpp` literal. `ngap_handover.cpp`'s own comment said as much --
"the real fix for all of them is the config-file retrofit tracked as gap-closure task #109" -- and
this is that fix for AMF.

**One struct, not four parameters.** `amf::ngap::PeerEndpoints` (a 4-string aggregate in the new
`peer_endpoints.hpp`) is passed by const& through the handler chain. `run_ngap_lifecycle` already
took 12 parameters and the handlers it dispatches to take 8+; four separate strings would have been
threaded through every one of them at every call site. The struct is read-only after construction
in `main()`, so it needs no synchronisation, and it is copied **by value** into each detached
association thread rather than `std::ref`'d -- unlike the stores, which `main()` owns for the whole
process lifetime, that thread outlives the frame holding it. Four strings per association costs
nothing and removes the lifetime question entirely.

**`self_base` is derived, not configured.** It is AMF's own advertised address, and `amf.json`
already carries `port`. A fifth key would have to be kept in sync with `port` by hand -- which is
the drift this task exists to remove -- so it is built from `port` instead.

### The check that proves it, since a green suite cannot

Every configured value is identical to the literal it replaced, so passing tests only show nothing
broke; they cannot tell "reads config" from "still reads a constant". The discriminating run:

```
AMF_SMF_BASE_URL=https://127.0.0.1:19999  ->  amf-ngap: SMF CreateSMContext call failed:
                                              Could not connect to server   (test fails)
unset                                     ->  7/7 AMF tests pass
```

AMF really dialled the dead port. Done before this ADR claimed the retrofit works.

### Explicitly NOT in scope, so it reads as decided rather than missed

- **`kMcc`/`kMnc`/`kSst`/`kSd` stay duplicated** across the two translation units. They are this
  lab's PLMN/slice identity, not deployment endpoints, and the TU split that forces the
  duplication exists for a real CI compiler-memory reason (`ngap_handover.hpp`'s own header).
- **The 3GPP service paths** (`/nausf-auth/v1`, `/npcf-am-policy-control/v1`,
  `/nsmf-pdusession/v1`) stay in source. They are spec-defined API roots, not configuration.
- **CHF (2 sites) and product-catalog (1)** still hold `https://127.0.0.1:` literals. They have
  their own config load and are a clean, separate follow-up -- task #109 is not closed by this ADR,
  only AMF's share of it.

### On the parallel-test motivation, corrected

This retrofit is a prerequisite for running `ctest -j` (a test cannot repoint AMF at its own SMF
instance while the address is a compile-time constant), but it is **not** justified by it, and the
throughput claim that prompted it was wrong. Measured from CI run 33878218832's own tsan job:
started 13:28:27, first test at 13:57:44, all 490 tests done at 14:00:14, job completed 14:00:19 --
so the **test phase is under three minutes of a 32-minute job**; the build is the other ~29, and
the asan leg is `-j1`-capped by ADR-0225 on purpose. Parallel tests would buy minutes, not the
"~25 minutes off every CI run" claimed mid-session before anyone measured it.

Other prerequisites remain outstanding regardless: AMF's shared Redis keys, UDR's and
product-catalog's shared PostgreSQL rows, one NRF serving all discovery, and UPF/SMF's fixed PFCP
UDP ports 8805/8806. Recorded so nobody reads this ADR as having unlocked parallelism.

---

