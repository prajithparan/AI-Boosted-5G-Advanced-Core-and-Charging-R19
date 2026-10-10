## ADR-0357: every port and tunable in config, and a shutdown watcher that reaches every loop

**Date:** 2026-09-14. **Status:** accepted. User mandate, stated three times and now recorded as
a standing rule: *any port or configuration data MUST be in a config text file, changeable any
time* -- never a source literal, never a compile-time define.

### The audit, and what it found

Every NF and BSS config already declared its SBI port and read it through `nf_config::require`.
The sweep found what did not:

- **UPF bound PFCP on `pfcp_core::kPfcpPort`** and **SMF dialled the UPF on it at five sites.** The
  IANA value, cited to TS 29.244 -- and still a hardcoded port. Now `pfcp_bind_port` in
  `config/upf.json` and `upf_pfcp_port` in `config/smf.json`, each with an environment override.
  Verified: `UPF_PFCP_BIND_PORT=18805` moves the listener.
- **CHF logged the dictionary constants while binding the configured ports**, so an instance moved
  by `CHF_DIAMETER_PORT` reported 3868 while listening on 3968. The constants are gone; the log
  prints what is bound.
- **`DorisOptions::port = 9030`** -- a literal default in a struct, waiting for a caller that forgot
  to set it. Now 0, with the comment saying so.
- **Four tunables I had added this week as literals**: the two shutdown-flush timeouts, and the
  idempotency wait's poll and maximum. All in `config/chf.json` now.
- **`config.value()` in the CHF batching reads.** It ignores the environment -- precisely what left
  the 3M-CDR run on a dead Postgres port (ADR-0352). Zero `config.value()` calls remain in any NF
  or BSS `main`. `nf_config::optional<>` (ADR-0355) is the env-aware way to read a key that may
  be absent.

Protocol dictionaries keep their IANA constants as *spec citations*. None is what an NF binds or
dials any more; the config file carries the same number as its default.

**Known remaining retrofit, not done here:** `CERTS_DIR` and `CONFIG_DIR` are compile-time defines
(task #109). They are paths, not ports, and the mandate names "configuration data" -- they fall
under it and are tracked.

### The shutdown watcher, finished properly

ADR-0353's SIGTERM handler was a `signal_set` inside `run_multi_threaded`, tied to that one
io_context. Verifying the port change exposed the hole: UPF runs its SBI server in a detached
thread and blocks `main` in the PFCP loop -- a *synchronous* `receive_from` on a second
io_context. The signal stopped the SBI loop; the process sat in `__skb_wait_for_more_packets`
until SIGKILL. Half a shutdown handler is a hang, which is worse than none.

Now: one process-wide `ShutdownWatcher` on its own thread, independent of any registered
context's lifetime; `stop_on_shutdown_signal(ioc)` registers a loop; `on_shutdown_signal(fn)`
registers a callback for a loop that `stop()` cannot reach. UPF's callback raises a flag and sends
the PFCP socket a datagram from the loopback, so the blocked receive returns and the loop's
condition ends it. The first version of the watcher detached its thread and the process SIGSEGVed
at exit -- static destruction under a running thread -- so it joins.

Verified: UPF, AMF and CHF each exit **0** on SIGTERM, none needing SIGKILL.

### Rejected

- Converting UPF's 130-line receive loop to `async_receive_from` so `stop()` would work directly.
  Correct in principle; every `continue` in the body becomes a re-arm, and that is where the bugs
  would have gone. The flag-and-nudge keeps the body untouched.
- A leaked heap singleton for the watcher to dodge static destruction. It works and it violates
  the no-raw-`new` rule; joining is RAII and just as safe.

