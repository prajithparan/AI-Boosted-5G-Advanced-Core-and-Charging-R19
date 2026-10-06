## ADR-0422: Browser-to-NF transport -- a C++ backend-for-frontend, NF mTLS untouched

**Date:** 2026-09-26. **Status:** accepted (amended by ADR-0423: the BFF now parses bodies).

**Decision.** `gui/bff` (`oam-gui-bff`, C++ on `libs/sbi-core`): the browser talks TLS 1.3 + mTLS +
HTTP/2 to the BFF; the BFF calls product-catalog / provisioning with its OWN lab-CA identity
(CN `oam-gui-bff`) via `sbi_core::http2::Client`. Two separate trust anchors: the browser-facing
listener verifies clients against a separate **operator CA** (terminal certificates), so an NF's
lab-CA certificate cannot even open the GUI (tested), and no NF gains a new trust anchor, port or
exception. Only allow-listed routes exist (no generic proxy); only content-type/accept go upstream;
upstream headers are allow-listed back (Location rewritten). CSRF: every write needs
`x-requested-by: oam-gui` + `content-type: application/json`, which force a CORS preflight the BFF
never answers. Static app served from memory with a strict CSP. Ports config-driven
(`config/oam-gui-bff.json`, 8710 -- outside the NF/CI 7700-7899/9400-9499 ranges).
`libs/sbi-core` gained one additive field, `Request::peer_address` (audit "from where").

**Rejected.** *Browser straight to NFs with an operator client cert* -- every NF would have to
trust operator certs and serve CORS; weakens NF mTLS. *Vite dev-server proxy / Node BFF* -- a
second runtime, and http-proxy speaks HTTP/1.1 to h2-only services. *Python BFF* -- no reason
beyond convenience; sbi-core already has the TLS/h2 stack. *Envoy/nginx in front* -- would terminate
mTLS and still need an authorization service; the BFF is where authorization lives (ADR-0423).
**Disclosed:** `sbi_core::http2::Client` sets no timeout, so a hung service holds a BFF worker.

