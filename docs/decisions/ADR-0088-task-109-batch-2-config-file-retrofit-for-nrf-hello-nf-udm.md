## ADR-0088: task #109 batch 2 -- config-file retrofit for NRF, hello-nf, UDM, PCF, SMF, UPF

### Context

Continues ADR-0077's project-wide "no hardcoded DB URL/deployment literal in source" rule and
ADR-0085's batch 1 (the 5 services with confirmed live bugs). This batch closes the six remaining
untouched services from task #109's own backlog: `nfs/nrf`, `nfs/hello-nf`, `nfs/udm`, `nfs/pcf`,
`nfs/smf`, `nfs/upf`. `bss/product-catalog` remains the one deliberately-still-untouched service
(its own hardcoded default happens to be correct, real style debt not an active bug, left for a
future turn per ADR-0085's own disclosure). CHF's own remaining Redis/ClickHouse/AI-env fields
(ADR-0085's own disclosed partial-retrofit scope) also remain untouched -- not part of this batch.

### Scope, by service

- **NRF**: smallest surface -- `port`/`metrics_bind_address` only (NRF has no NRF base URL of its
  own, it *is* the NRF).
- **hello-nf**: not a real NF (a test/demo tool, see its own file header) but still hardcoded a
  literal NRF base URL -- `nrf_base_url` only, same class of gap as every real NF.
- **UDM**: `port`/`metrics_bind_address`/`nrf_base_url`/`udr_base_url` (the real cross-NF call
  `GetAmData`/`GetSmfSelData`/`GetSmData` already make against UDR, ADR-0069).
- **PCF**: `port`/`metrics_bind_address`/`nrf_base_url`/`udr_base_url`/`chf_base_url`/
  `self_base_url` (the real N28 spending-limit wiring from ADR-0072, plus the notifUri CHF calls
  back on).
- **SMF**: the largest surface -- `port`/`metrics_bind_address`/`nrf_base_url`/`self_base_url`/
  `pcf_base_url`/`amf_base_url`/`chf_base_url`. Three free functions
  (`perform_n40_charging_data_create`/`_update`/`_release`) and `discover_upf_ipv4`/
  `run_pfcp_lifecycle` gained a `chf_base`/`nrf_base` parameter each; every lambda capturing them
  by reference (the CreateSMContext and release-route handlers, the Session Report handler and its
  own nested detached-thread lambda) had the corresponding config value added to its capture list.
- **UPF**: `metrics_bind_address`/`nrf_base_url` only (UPF has no HTTP/SBI server -- UDP/PFCP +
  Prometheus only, confirmed by grep, no `kPort` existed to retrofit).

### Real, disclosed process note: a genuine build-directory race, not a code defect

Mid-retrofit, a duplicate `cmake --build` invocation was accidentally launched against an
already-running one targeting the same six binaries (a monitoring tool's wait timed out and was
misread as the build having stalled, when it had actually completed in the background) -- two
concurrent `ninja` processes writing the same object file truncated
`TS29122_CommonData_grp.cpp.o`, breaking `ar`/`ranlib`. Root-caused via the real `ranlib` error
text ("file truncated"), not guessed; fixed by deleting the corrupt `.o` and the stale `.a` and
rebuilding once, cleanly. Disclosed here because it is a real incident this pass hit and fixed, not
because it reflects anything about the retrofit's own correctness -- the actual source edits were
unaffected, confirmed by the clean rebuild that followed.

### Verification

All six rebuilt clean (`cmake --build . --target nrf hello-nf udm pcf smf upf -j4`, zero warnings
from the new code). Live-started a real 7-process lab stack (nrf, udr, chf, udm, pcf, smf, upf)
with **zero environment variable overrides** -- every service registered with NRF successfully.
Real, multi-hop cross-NF proof, not just individual startup: SMF's own log shows
`"discovered UPF at 127.0.0.1 via Nnrf_NFDiscovery"` immediately followed by `"PFCP Sx Association
established with UPF at 127.0.0.1"` -- a genuine real-time NRF discovery (`nrf_base_url`) into a
real PFCP handshake, both ends purely config-driven. `hello-nf`'s own full register/heartbeat/
deregister lifecycle completed successfully (exit 0) against its own config-driven
`nrf_base_url`. Live HTTP: UDM's `GetAmData` returned a real 200 (proves the real UDR cross-call,
`udr_base_url`, still works end-to-end); PCF responded (400 on an intentionally minimal test body,
confirming reachability/config wiring -- PCF's own request-body validation is unrelated to this
ADR's scope and was already verified in ADR-0080).

Full `conformance_tests`: **321/321 pass**, zero regressions, run with the same five batch-1
`*_DATABASE_URL` env vars explicitly unset (`env -u`) to confirm ADR-0085's own config defaults
remain self-sufficient alongside this batch's new ones.

### What this ADR does NOT include

`bss/product-catalog` and CHF's remaining Redis/ClickHouse/AI-env fields (both already disclosed
in ADR-0085 as deliberately out of scope). `nfs/ausf` was retrofitted incidentally in ADR-0081, not
part of either batch. **Task #109 is now closed**: every NF/BSS service in this project has been
retrofitted onto `libs/nf-config`, except the two explicitly-disclosed remainders above, which
are real style debt (not active bugs) tracked for a future turn if ever prioritized.

