## ADR-0456: three red CI tests, root-caused -- and one open TSan finding

**Date:** 2026-10-05. **Status:** two fixed, one worked around in the test with the underlying hang
recorded as OPEN. From CI runs 37220853366 and 37258571754 (the latter cancelled after both
sanitizer jobs finished, so the runner was free for ADR-0454's benchmark).

1. **`gui_nf_config_schemas_up_to_date` (ASan + TSan).** Caused by this session's own commit
   f85b0e0, which deleted `advertised_ipv4` from `config/nwdaf.json` without regenerating the GUI
   schema. The key is restored (ADR-0451's correction: nwdaf now resolves it with `getaddrinfo`),
   so the committed schema matches again -- verified locally, passes.
2. **`SmfHandoverN2SmInfoIntegration.HandoverRequiredReturnsRealUpfTunnelNotAPlaceholder` (ASan).**
   A harness race, not an SMF bug: the test spawned PCF but only waited for SMF, and
   CreateSMContext needs PCF for the SM Policy Association, so a run where PCF came up last got
   "500 could not reach PCF". The test now also waits for PCF. Passes locally.
3. **`ChfDiameterRar.*` (TSan only).** My first fix (fe96db7, a 40 s wait budget, on the theory
   that TSan made startup slow) was **wrong**, and CI proved it: the next TSan run still failed,
   now after the full 40 s, while the identical test passed in 4.5 s under ASan. It is a hang, not
   slowness. CHF's last log line is always "CDR event bus ENABLED"; the next step is `CdrWriter`'s
   `mysql_real_connect` to Doris (no connect/read timeout set) with librdkafka's producer threads
   already running against a dead broker. These two tests were the only CHF tests configuring the
   Kafka event bus; every other CHF integration test uses the default Doris direct-insert sink and
   passes under TSan. RAR/RAA does not depend on CDR delivery, so the tests now use the default
   sink, and the 40 s bump is reverted to the harness default.
   **OPEN, not hidden:** CHF can hang at startup under TSan when the event bus is configured. Not
   reproduced locally (`build-tsan` is stale and a full TSan rebuild was not run in this pass), so
   which of the two candidates -- an unbounded Doris handshake, or an interaction between
   librdkafka's threads and TSan's runtime -- is not established. Separately and certainly: the
   Doris connection having no `MYSQL_OPT_CONNECT_TIMEOUT`/`READ_TIMEOUT` means a stalled Doris can
   hang CHF's startup in production too. That is real debt against ADR-0009's production bar,
   recorded here; the fix needs a config key (no hardcoded timeouts) and a GUI schema regen.

