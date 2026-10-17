#pragma once

#include "sbi_core/tls_config.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <tl/expected.hpp>
#include <vector>

// HTTP/2 client built on libcurl (which already has mature native HTTP/2 support), used for all
// outbound SBI calls (NF -> NRF, NF -> NF). Synchronous/blocking per call -- see docs/DECISIONS.md
// ADR-0006 for why that's an acceptable Phase 0 simplification and what integrating it with the
// Boost.Asio event loop (curl_multi + asio) would take later.
//
// TLS 1.3 + mTLS only (see ADR-0011): every request presents the client's own certificate and
// verifies the server's against the configured CA. There is no cleartext fallback -- pass a `url`
// with anything other than an https:// scheme and the request fails.
//
// Thread-safety: `send()` is safe to call concurrently from any number of threads. libcurl's own
// contract is explicit that one easy handle must never be driven by two threads at once, so each
// concurrent call gets its OWN easy handle, checked out of an internal pool.
//
// Why this exists at all (ADR-0050 Stage 5): every NF's convention had been "one Client instance
// per thread", so the single-handle design was never exercised concurrently until that stage's
// detached per-report thread design gave `nfs/smf`'s `chf_report_client` a second, genuinely
// concurrent caller -- caught via a real, live, malformed-response failure (`curl_easy_perform`
// returning `CURLE_FAILED_INIT`/empty responses under real concurrent access), not by inspection.
// The original fix serialized every caller on one mutex held across `curl_easy_perform`, which was
// correct but made every outbound SBI call queue behind every other one on the same Client.
//
// ADR-0241 replaced that with the handle pool: the mutex is now held only for the pool
// checkout/checkin (microseconds), never across the blocking network round-trip. Handle REUSE is
// preserved deliberately -- a checked-in handle keeps its live connection, TLS session cache and
// DNS cache, which is what makes HTTP/2 connection keepalive real, so the pool hands back the same
// handles rather than creating fresh ones per call.
//
// Lifetime contract (pre-existing, stated here rather than newly introduced): a Client must
// outlive every in-flight `send()` call on it. Destroying one while another thread is inside
// `send()` was already undefined with the single-handle design (the destructor never took the
// mutex either); every NF holds its Clients for process lifetime, which is why this has never
// bitten. See ADR-0241 for full disclosure.

namespace sbi_core::http2 {

struct ClientRequest {
    std::string method;
    std::string url;
    std::multimap<std::string, std::string> headers;
    std::string body;
    // Optional "host:port:address" entries (curl's CURLOPT_RESOLVE): connect to `address` for
    // `host:port` while TLS still verifies `host`. Stands in for DNS of 3gppnetwork.org FQDNs the
    // lab does not run (e.g. a telescopic FQDN served by the SEPP, ADR-0492). Empty = normal DNS.
    std::vector<std::string> resolve;
    // TLS exporters (label, octets; empty context) to run on the connection this request used,
    // results in ClientResponse::tls_exports (ADR-0493, the SEPP's N32-f master key).
    std::vector<std::pair<std::string, std::size_t>> tls_exports;
};

struct ClientResponse {
    long status = 0;
    std::multimap<std::string, std::string> headers;
    std::string body;
    std::map<std::string, std::vector<std::uint8_t>> tls_exports;
};

class Client {
public:
    explicit Client(TlsConfig tls);
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    tl::expected<ClientResponse, std::string> send(const ClientRequest& request);

    // ADR-0499 phase 3: called with the URL of every request sent without an Authorization
    // header; a token it returns is sent as "Bearer <token>". Set once, before the first send()
    // (not synchronised against concurrent sends). See sbi_core::attach_tokens.
    using TokenSourceFn = std::function<std::optional<std::string>(const std::string& url)>;
    void set_token_source(TokenSourceFn source);
    // The process-wide fallback for every Client with no source of its own (safe to call while
    // other threads send; empty clears it). See sbi_core::install_default_token_source.
    static void set_default_token_source(TokenSourceFn source);

    [[nodiscard]] const TlsConfig& tls() const { return tls_; }

private:
    // RAII lease: guarantees a checked-out handle returns to the pool on every exit path from
    // send(), including the early returns on a libcurl error.
    class HandleLease {
    public:
        HandleLease(Client& owner, void* handle) : owner_(owner), handle_(handle) {}
        ~HandleLease();
        HandleLease(const HandleLease&) = delete;
        HandleLease& operator=(const HandleLease&) = delete;
        void* get() const { return handle_; }

    private:
        Client& owner_;
        void* handle_;
    };

    void* checkout(); // pops a pooled handle, or creates one; nullptr if curl_easy_init fails
    void checkin(void* handle); // returns a handle to the pool, or destroys it if the pool is full

    TlsConfig tls_;
    TokenSourceFn token_source_;
    std::mutex mutex_;        // guards idle_ only -- never held across curl_easy_perform
    std::vector<void*> idle_; // CURL*, opaque here to avoid leaking <curl/curl.h> into includers
};

} // namespace sbi_core::http2
