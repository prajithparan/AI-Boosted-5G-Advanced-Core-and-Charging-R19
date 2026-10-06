## ADR-0006: Synchronous libcurl calls for the HTTP/2 client (Phase 0)

**Date:** 2026-08-04
**Status:** Accepted (temporary)

**Context:** libcurl's easy interface is blocking by default; integrating it with Boost.Asio's
event loop properly means driving `curl_multi_socket_action` from Asio's reactor.

**Decision:** `http2::Client::send()` blocks the calling thread via `curl_easy_perform`. Fine for
hello-nf, which has nothing else to do while waiting.

**Consequence:** Not viable for a real NF handling concurrent inbound SBI requests while also
making outbound calls -- that would stall the server's io_context if called from the same thread.
Revisit (curl_multi + Asio integration, or a dedicated client thread pool) before any Phase 2 NF
needs to make outbound calls while serving inbound ones.

---

