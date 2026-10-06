## ADR-0005: h2c (cleartext) only for Phase 0, no TLS/mTLS yet

**Date:** 2026-08-04
**Status:** Superseded by ADR-0011 (2026-08-04) -- real TLS 1.3 + mTLS is now in place.

**Context:** The non-negotiable rules mandate OpenSSL 3.x with TLS 1.3 and mTLS for real SBI
traffic. Standing up a lab PKI (CA, per-NF certs, mTLS validation) is real scope on its own and
wasn't needed to prove Phase 0's actual goal: that the hand-rolled HTTP/2 transport and OAuth2 flow
work end-to-end.

**Decision:** `http2::Server` and `http2::Client` speak h2c (HTTP/2 cleartext, "prior knowledge")
only. hello-nf <-> stub-nrf runs entirely over plaintext TCP.

**Consequence -- explicitly non-conformant:** This is a real gap against TS 33.501 and the
non-negotiable rules, not a hidden one. It must be closed with real TLS 1.3 + mTLS (ALPN
negotiating "h2") before any NF-to-NF traffic beyond the Phase 0 stub is considered done. Tracked
as follow-up work for Phase 2.

---

