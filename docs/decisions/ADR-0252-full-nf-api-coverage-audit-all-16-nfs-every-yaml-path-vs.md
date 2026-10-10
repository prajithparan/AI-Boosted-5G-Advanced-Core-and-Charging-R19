## ADR-0252: Full NF API coverage audit -- all 16 NFs, every YAML path vs the compiled binary

### Method, and why the obvious one was abandoned

`docs/NF_API_COVERAGE_AUDIT.md` is the deliverable. For each NF: extract every path from its R19
YAML, then test that path against the string table of the **compiled binary**.

The regex-over-source approach ADR-0251 attempted was abandoned as unreliable, not tuned: routes
are registered by concatenating API-root constants and by assigning path patterns to *variables*
(`amf_ctx_path_pattern`), so no regex can see them. It reported 12 routes for UDR, which has 204
`add_route` calls. Publishing numbers from it would have been worse than publishing nothing.

Paths always come from the YAML, never from operationIds -- ADR-0251 recorded a false negative
caused by guessing `supi-or-gpsi` from `GetSupiOrGpsi` when the real path is
`/{ueId}/id-translation-result`.

### Result

**12 of 16 NFs are at 100% path coverage**: AMF, BSF, CHF, 5G-EIR, GMLC, LMF, NEF, NSSF, PCF, SCP,
SMSF (and UPF, which has no SBI YAML). **32 unrouted paths of 328 total (90% coverage).**

- **NRF (5)** and **AUSF (1)**: already disclosed as deliberately deferred in their own source
  headers. Verified: those strings appear in the binaries only inside comments.
- **UDM (1)**: `/{supi}/am-data/update-sor`, genuinely absent (zero source occurrences).
- **SMF (2)**: `transfer-mo-data`, `send-mo-data`, genuinely absent.
- **UDR (23)**: the one large, real, previously-undisclosed gap.

### The method's false-negative mode, found before publishing

Dynamically-assembled routes are invisible to it. UDM registers four ack endpoints in a **loop**
over `{"sor-ack", "upu-ack", "subscribed-snssais-ack", "cag-ack"}`, so the full path literal never
appears in the binary although the route exists. This was caught by cross-checking every candidate
against source **before** writing the report, and those four are excluded from the 32.

15 paths total are flagged likely-dynamic and deliberately **not counted either way**: 4 UDM
(confirmed loop-registered), 3 SMF and 8 UDR (consistent with being routed -- `provisioned-data`
occurs 18x in UDR source -- but not individually confirmed, and said to be unconfirmed rather than
assumed in either direction).

### The finding that matters: UDR's 23

Unlike the NRF/AUSF gaps, these were not disclosed anywhere, and they are coherent rather than
scattered -- three entire resource families: `/application-data/*` (12 paths -- bdtPolicyData,
influenceData and its subs-to-notify tree, iptvConfigData, pfds, serviceParamData, subs-to-notify),
`/exposure-data/*` (4), `/aiot-data/*` (3, R19 Ambient IoT), plus 4 singles including
`/data-restoration-events`.

This bears directly on the free5GC/open5GS parity mandate: `/application-data/influenceData` is
the traffic-influence data store a real NEF/AF path depends on, and this project has built all 14
NEF YAML files. It also contradicts the impression left by `docs/CAPABILITY_GAP_ANALYSIS.md`'s UDR
row, which reads as "past parity" -- true for `subscription-data`, which is where the closure work
went, but silent on `application-data`/`exposure-data`/`aiot-data`.

### Honest limits

Coverage here means **a route exists at that path** -- not that every HTTP method on it is
implemented, nor that behaviour is spec-correct. Per-method and per-behaviour conformance is a
separate, larger audit. UPF is excluded (no SBI YAML; its N4/PFCP surface is not path-based).

