## ADR-0391: resolve three port collisions in config/*.json

**Date:** 2026-09-26. Found while merging the GUI's derived NF-config schemas (ADR-0425), which
read every `config/*.json`: `nssf` and `product-catalog` both used SBI port 7785 and metrics 9473;
`nef` and `provisioning` both used 7790; `adrf` and `provisioning` both used metrics 9490. Any
deployment running those pairs on one host would fail to bind. Fixed by moving the NF with fewer
references: **NSSF -> 7801 / metrics 9486** (compose ports updated; no test hardcodes NSSF's port),
**provisioning -> 7802 / metrics 9492** (and the GUI BFF's `provisioning_base_url`). NEF (7790,
referenced by ~60 test URLs) and product-catalog (7785, the CHF's catalog base) keep theirs. No
remaining duplicate SBI or metrics port across config/*.json. Follow-up: a CI check that fails on
duplicate ports, so this cannot recur silently.
