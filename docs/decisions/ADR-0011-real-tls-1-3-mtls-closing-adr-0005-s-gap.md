## ADR-0011: Real TLS 1.3 + mTLS, closing ADR-0005's gap

**Date:** 2026-08-04
**Status:** Accepted

**Context:** ADR-0009 raised the project's target to production-grade, which made ADR-0005's h2c-
only transport explicitly unacceptable as a permanent state, and the user confirmed TLS/mTLS must
land before Phase 2's NRF work starts (not retrofitted after).

**Decision:**
- **Lab PKI**: `scripts/gen-lab-pki.sh` generates a self-signed root CA and one leaf cert per NF
  (EC P-256/prime256v1, SHA-256, 365-day validity), each with **both** `serverAuth` and
  `clientAuth` EKU since every NF is simultaneously an SBI server and client. SAN covers
  `127.0.0.1`/`localhost`/the NF's own name. Output to `certs/` at repo root (gitignored --
  regenerable dev material, not secrets worth version-controlling); the script itself is
  committed.
- **`http2::Server`**: the accepted TCP socket is wrapped in `boost::asio::ssl::stream`, using an
  `ssl::context::tlsv13_server` context (method-table-level restriction to TLS 1.3, not just an
  option flag). Loads the NF's cert+key, sets `verify_peer | verify_fail_if_no_peer_cert` against
  the configured CA (real mTLS enforcement), and installs an ALPN select callback
  (`SSL_CTX_set_alpn_select_cb` + `SSL_select_next_proto`) that accepts only `h2` --
  `SSL_TLSEXT_ERR_ALERT_FATAL` on anything else, no HTTP/1.1 or no-protocol fallback.
  `TlsConfig{cert_path, key_path, ca_path}` (shared with the client, `tls_config.hpp`) is a
  required constructor argument, not optional/defaulted -- a caller cannot construct an
  unauthenticated or unencrypted `Server` by omission; the constructor throws if any path is
  missing or the material fails to load.
- **`http2::Client`**: almost entirely libcurl configuration -- `CURLOPT_SSLVERSION` pinned to
  `CURL_SSLVERSION_TLSv1_3`, `CURLOPT_SSLCERT`/`SSLKEY` (client cert for mTLS),
  `CURLOPT_CAINFO` (verify server), `CURLOPT_SSL_VERIFYPEER`/`VERIFYHOST` left on. HTTP version
  switched from `CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE` (h2c) to `CURL_HTTP_VERSION_2TLS` (ALPN-
  negotiated over TLS).
- **stub-nrf/hello-nf**: both load their own cert/key + the shared CA (paths baked in at compile
  time via `CERTS_DIR` -- see below), URLs switched to `https://`.

**Proof, not assertion:**
- `openssl s_client -connect 127.0.0.1:7777 -alpn h2 -tls1_3 -CAfile certs/ca/ca.crt` **without**
  a client cert: server sends `tlsv13 alert certificate required` -- mTLS is actually enforced,
  not just configured and silently unchecked.
- Same command **with** `-cert certs/hello-nf/cert.pem -key certs/hello-nf/key.pem`: succeeds,
  `New, TLSv1.3, Cipher is TLS_AES_256_GCM_SHA384`, `ALPN protocol: h2`, `Verify return code: 0
  (ok)`.
- `tests/integration/test_hello_nf_registration.cpp` (real subprocess-to-subprocess, not mocked)
  passes: hello-nf obtains an OAuth2 token, registers (201), heartbeats (200), deregisters (204),
  all over the real TLS 1.3 + mTLS handshake above.

**Disclosed simplifications / known gaps, not hidden:**
- **No graceful TLS shutdown**: `Connection::close()` closes the underlying TCP socket directly
  rather than performing `async_shutdown` (TLS `close_notify`). A fully correct version needs its
  own timeout handling to avoid an unresponsive peer leaking the connection indefinitely; that
  timeout machinery wasn't built. Every current `close()` call site is a teardown/error path, not
  a graceful reuse handoff, so the practical impact today is limited -- but this is real,
  disclosed debt, not fixed.
- **Cert paths are compile-time absolute paths** (`CERTS_DIR` baked in via
  `target_compile_definitions` from `CMAKE_SOURCE_DIR`), not a runtime config system. Acceptable
  for dev/lab binaries run in-place; will need real config (env vars, a config file, whatever
  Phase 2's NF config story ends up being) once these aren't just Phase 0 throwaways.
- **No cert rotation/revocation** (no CRL/OCSP checking, no rotation tooling) -- single static lab
  CA and leaf certs, regenerate-and-restart is the only "rotation" story right now.
- **stub-nrf's OAuth2 token is still unsigned** (ADR-0009 already flagged this separately) --
  TLS/mTLS secures the transport; it doesn't address token signature validation, which is
  unrelated and still Phase 2 NRF work.

**Rejected alternative:** Defer TLS/mTLS until Phase 2, retrofitting it once NRF exists. Rejected
per explicit user decision when the sequencing question was asked directly -- TLS/mTLS first so
Phase 2 starts on solid transport rather than needing a retrofit across whatever NF code lands in
the meantime.

**Note:** the "over actual h2c HTTP/2" phrasing in ADR-0008's PHASE0-NOTES appendix below describes
the Phase 0 build-validation run at the time it was written and is left as a historical record, not
updated to match current behavior -- the transport it describes has since changed per this ADR.

---

