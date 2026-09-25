#pragma once

#include "sbi_core/tls_config.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Minimal HTTP/2 server built directly on nghttp2's C session API, driven by Boost.Asio for the
// socket I/O loop. This exists because vcpkg does not package upstream nghttp2's own asio_http2
// ("nghttp2-asio") convenience library -- see docs/DECISIONS.md ADR-0004.
//
// TLS 1.3 + mTLS only (see ADR-0011): every connection is wrapped in boost::asio::ssl::stream, the
// server requires and verifies a client certificate against the configured CA, and ALPN must
// negotiate "h2" or the handshake is rejected outright -- there is no cleartext/h2c fallback and no
// TLS version below 1.3. TlsConfig is a required constructor argument specifically so a caller
// cannot accidentally stand up an unauthenticated or unencrypted server by omission.
//
// Routing is a simple exact-segment matcher supporting "{param}" path segments (e.g.
// "/nnrf-nfm/v1/nf-instances/{nfInstanceId}"), which is what Nnrf_NFManagement needs; it is not a
// general-purpose router and isn't meant to become one -- Phase 1's generated server stubs may
// replace this matcher entirely once real per-NF path sets exist.

namespace sbi_core::http2 {

// TlsConfig (cert_path/key_path/ca_path) is defined in tls_config.hpp, shared with
// http2_client.hpp. Server's constructor throws std::runtime_error if any path is
// missing/unreadable or the certificate/key can't be loaded -- fails loudly rather than silently
// falling back to an unauthenticated/unencrypted listener.

struct Request {
    std::string method;
    std::string path;
    std::multimap<std::string, std::string> headers;
    std::map<std::string, std::string> path_params;
    // Real query-string parsing (added for bss/balance-management's AccumulatedBalance filter,
    // TMF654) -- percent-decoded per RFC 3986; a repeated key (?a=1&a=2) keeps every value, since
    // this is a multimap, matching headers' own convention above.
    std::multimap<std::string, std::string> query_params;
    std::string body;
    // Identity of the mTLS peer, read from the client certificate the handshake already verified
    // against the CA (ADR-0011). Every NF cert chains to the same lab CA, so "verified" alone only
    // proves "some NF of this deployment"; a route that must admit exactly one client (the UDR's
    // project-owned OAM provisioning API, ADR-0382) checks these against its configured allow-list.
    // Empty when the certificate carries no CN / no dNSName SAN.
    std::string peer_cert_cn;
    std::vector<std::string> peer_cert_dns_names;
};

struct Response {
    int status = 200;
    std::multimap<std::string, std::string> headers;
    std::string body;

    static Response json(int status, std::string body_json);
};

// Splits a single already-decoded query_params value on OpenAPI's `style: form, explode: false`
// array convention (comma-separated, e.g. `?dataset-names=AMF_3GPP,SDM_SUBSCRIPTIONS`) into its
// real component values. Real, disclosed limitation: `Request::query_params` values are already
// fully percent-decoded by the time a handler sees them, so a literal comma escaped as `%2C` by a
// spec-compliant client is indistinguishable from a delimiter comma after decoding -- this
// function cannot tell them apart and will over-split such a value. Every real 5G identifier this
// project has needed to split so far (enum values, external group IDs, PLMN/SNSSAI component
// strings) is comma-free by construction, so this is a real but narrow, disclosed simplification,
// not swept under the rug (see docs/DECISIONS.md ADR-0161). Returns an empty vector for an empty
// input string (matching "the query param was present but had no value", not "one empty-string
// element").
std::vector<std::string> split_form_array(const std::string& value);

using Handler = std::function<Response(const Request&)>;

class Server {
public:
    Server(boost::asio::io_context& ioc, std::string address, unsigned short port, TlsConfig tls);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // path_pattern segments wrapped in {} are captured into Request::path_params.
    void add_route(std::string method, std::string path_pattern, Handler handler);

    // P15 / P4.12 (ADR-0280): a TPS ceiling for this server. Beyond it, requests are shed with a
    // real 503 + ProblemDetails (the status TS29571_CommonData.yaml defines for Service
    // Unavailable) and a `Retry-After`, BEFORE the handler runs -- shedding is only protective if
    // it is cheaper than serving.
    //
    // Not called, or called with sustained_tps <= 0, means no ceiling: every NF that has not
    // opted in behaves exactly as it did before.
    void set_tps_limit(double sustained_tps, double burst_capacity);

    // Requests shed by the limit above. 0 when no limit is set.
    std::uint64_t shed_count() const;

    // Begins accepting connections. Returns immediately; actual I/O happens on the io_context
    // passed to the constructor (caller owns the event loop, e.g. via ioc.run()).
    void start();

    unsigned short local_port() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sbi_core::http2
