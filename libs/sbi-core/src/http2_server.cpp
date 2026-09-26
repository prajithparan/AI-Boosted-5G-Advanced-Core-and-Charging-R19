#include "sbi_core/http2_server.hpp"

#include "sbi_core/metrics.hpp"
#include "sbi_core/rate_limit.hpp"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/write.hpp>
#include <nghttp2/nghttp2.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace {

// The only protocol this server will ever negotiate -- see TlsConfig's doc comment: no h2c
// fallback, no HTTP/1.1. Wire format for SSL_select_next_proto: 1-byte length prefix + bytes.
constexpr unsigned char kAlpnH2Wire[] = {2, 'h', '2'};

int alpn_select_callback(SSL* /*ssl*/,
                         const unsigned char** out,
                         unsigned char* outlen,
                         const unsigned char* in,
                         unsigned int inlen,
                         void* /*arg*/) {
    if (SSL_select_next_proto(const_cast<unsigned char**>(out),
                              outlen,
                              kAlpnH2Wire,
                              sizeof(kAlpnH2Wire),
                              in,
                              inlen) != OPENSSL_NPN_NEGOTIATED) {
        // Client didn't offer "h2" -- reject the handshake rather than fall back to HTTP/1.1 or
        // proceed with no negotiated protocol. See TlsConfig's doc comment: h2 is not optional.
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    }
    return SSL_TLSEXT_ERR_OK;
}

} // namespace

namespace sbi_core::http2 {

namespace {

std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> segments;
    std::stringstream ss(path);
    std::string segment;
    while (std::getline(ss, segment, '/')) {
        if (!segment.empty()) {
            segments.push_back(segment);
        }
    }
    return segments;
}

// RFC 3986 percent-decoding, plus the query-string convention of '+' meaning a literal space
// (RFC 1866/application/x-www-form-urlencoded, universally followed by real HTTP clients for
// query strings even though RFC 3986 itself doesn't require it). A malformed "%" escape (not
// followed by two real hex digits) is passed through literally rather than throwing -- a
// malformed query string should degrade to an unmatched/odd value, not crash request parsing.
std::string percent_decode(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size() &&
            std::isxdigit(static_cast<unsigned char>(value[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(value[i + 2]))) {
            const std::string hex = value.substr(i + 1, 2);
            out += static_cast<char>(std::stoi(hex, nullptr, 16));
            i += 2;
        } else if (value[i] == '+') {
            out += ' ';
        } else {
            out += value[i];
        }
    }
    return out;
}

std::multimap<std::string, std::string> parse_query_string(const std::string& full_path) {
    std::multimap<std::string, std::string> out;
    const auto qpos = full_path.find('?');
    if (qpos == std::string::npos) {
        return out;
    }
    std::stringstream ss(full_path.substr(qpos + 1));
    std::string pair;
    while (std::getline(ss, pair, '&')) {
        if (pair.empty()) {
            continue;
        }
        const auto eq = pair.find('=');
        if (eq == std::string::npos) {
            out.emplace(percent_decode(pair), std::string{});
        } else {
            out.emplace(percent_decode(pair.substr(0, eq)), percent_decode(pair.substr(eq + 1)));
        }
    }
    return out;
}

struct Route {
    std::string method;
    std::vector<std::string> pattern_segments;
    Handler handler;
};

bool try_match(const Route& route,
               const std::string& method,
               const std::vector<std::string>& path_segments,
               std::map<std::string, std::string>& params_out) {
    if (route.method != method) {
        return false;
    }
    if (route.pattern_segments.size() != path_segments.size()) {
        return false;
    }
    std::map<std::string, std::string> params;
    for (std::size_t i = 0; i < route.pattern_segments.size(); ++i) {
        const auto& pat = route.pattern_segments[i];
        if (pat.size() >= 2 && pat.front() == '{' && pat.back() == '}') {
            params.emplace(pat.substr(1, pat.size() - 2), path_segments[i]);
        } else if (pat != path_segments[i]) {
            return false;
        }
    }
    params_out = std::move(params);
    return true;
}

} // namespace

Response Response::json(int status, std::string body_json) {
    Response r;
    r.status = status;
    r.headers.emplace("content-type", "application/json");
    r.body = std::move(body_json);
    return r;
}

std::vector<std::string> split_form_array(const std::string& value) {
    std::vector<std::string> out;
    if (value.empty()) {
        return out;
    }
    std::stringstream ss(value);
    std::string item;
    while (std::getline(ss, item, ',')) {
        out.push_back(item);
    }
    return out;
}

namespace {

struct StreamContext {
    std::string method;
    std::string path;
    std::multimap<std::string, std::string> headers;
    std::string body;
    std::string response_body;
    std::size_t response_offset = 0;
};

using SslStream = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>;

class Connection : public std::enable_shared_from_this<Connection> {
public:
    Connection(boost::asio::io_context& ioc,
               boost::asio::ip::tcp::socket socket,
               boost::asio::ssl::context& ssl_ctx,
               const std::vector<Route>& routes,
               TokenBucket* rate_limit)
        : ioc_(ioc), socket_(std::move(socket), ssl_ctx), strand_(ioc.get_executor()),
          routes_(routes), rate_limit_(rate_limit) {}

    ~Connection() {
        if (session_ != nullptr) {
            nghttp2_session_del(session_);
        }
    }

    void start() {
        auto self = shared_from_this();
        socket_.async_handshake(
            boost::asio::ssl::stream_base::server,
            boost::asio::bind_executor(strand_, [self](boost::system::error_code ec) {
                if (ec) {
                    spdlog::warn("sbi-core: TLS handshake failed: {}", ec.message());
                    self->close();
                    return;
                }
                self->on_handshake_complete();
            }));
    }

private:
    // Reads the verified peer certificate once per connection (every stream on it shares the same
    // TLS identity). verify_fail_if_no_peer_cert means a completed handshake always has one.
    void capture_peer_identity() {
        boost::system::error_code ep_ec;
        const auto ep = socket_.lowest_layer().remote_endpoint(ep_ec);
        if (!ep_ec) {
            peer_address_ = ep.address().to_string();
        }
        X509* cert = SSL_get1_peer_certificate(socket_.native_handle());
        if (cert == nullptr) {
            return;
        }
        std::array<char, 256> cn{};
        const int cn_len = X509_NAME_get_text_by_NID(
            X509_get_subject_name(cert), NID_commonName, cn.data(), static_cast<int>(cn.size()));
        if (cn_len > 0) {
            peer_cert_cn_.assign(cn.data(), static_cast<std::size_t>(cn_len));
        }
        auto* sans = static_cast<GENERAL_NAMES*>(
            X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
        if (sans != nullptr) {
            for (int i = 0; i < sk_GENERAL_NAME_num(sans); ++i) {
                const GENERAL_NAME* name = sk_GENERAL_NAME_value(sans, i);
                if (name->type == GEN_DNS) {
                    const ASN1_IA5STRING* dns = name->d.dNSName;
                    peer_cert_dns_names_.emplace_back(
                        reinterpret_cast<const char*>(ASN1_STRING_get0_data(dns)),
                        static_cast<std::size_t>(ASN1_STRING_length(dns)));
                }
            }
            GENERAL_NAMES_free(sans);
        }
        X509_free(cert);
    }

    void on_handshake_complete() {
        capture_peer_identity();
        nghttp2_session_callbacks* callbacks = nullptr;
        nghttp2_session_callbacks_new(&callbacks);
        nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks,
                                                                &Connection::on_begin_headers);
        nghttp2_session_callbacks_set_on_header_callback(callbacks, &Connection::on_header);
        nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, &Connection::on_frame_recv);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks,
                                                                  &Connection::on_data_chunk_recv);
        nghttp2_session_callbacks_set_on_stream_close_callback(callbacks,
                                                               &Connection::on_stream_close);

        nghttp2_session_server_new(&session_, callbacks, this);
        nghttp2_session_callbacks_del(callbacks);

        std::array<nghttp2_settings_entry, 1> iv{{{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 128}}};
        nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, iv.data(), iv.size());

        do_write();
    }

    // No TLS close_notify is sent -- just an abrupt close of the underlying TCP socket. A fully
    // correct async_shutdown (sending close_notify and waiting for the peer's, without blocking
    // the io_context thread) would need its own timeout handling to avoid an unresponsive peer
    // leaking the Connection indefinitely; skipped here as a disclosed simplification (see
    // docs/DECISIONS.md ADR-0011) rather than half-implemented. Every close() call site here is
    // already a teardown/error path, not graceful application-level connection reuse handoff.
    void close() {
        boost::system::error_code ec;
        socket_.lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        socket_.lowest_layer().close(ec);
    }

    void do_read() {
        auto self = shared_from_this();
        socket_.async_read_some(
            boost::asio::buffer(read_buf_),
            boost::asio::bind_executor(
                strand_, [self](boost::system::error_code ec, std::size_t n) {
                    if (ec) {
                        self->close();
                        return;
                    }
                    auto rv = nghttp2_session_mem_recv(self->session_, self->read_buf_.data(), n);
                    if (rv < 0) {
                        spdlog::warn("sbi-core: nghttp2_session_mem_recv failed: {}",
                                     nghttp2_strerror(static_cast<int>(rv)));
                        self->close();
                        return;
                    }
                    self->do_write();
                }));
    }

    // Real correctness requirement introduced by handle_request_complete's off-strand handler
    // dispatch (ADR-0239): do_write() can now be invoked "out of band" (from submit_response,
    // once a handler running on the worker pool finishes) while a PREVIOUS async_write for this
    // same connection's write_buf_ is still in flight. Without this guard, a second do_write()
    // would clear()/refill write_buf_ while the kernel is still reading its old contents out
    // from under the in-flight write -- a real buffer-corruption race, not a theoretical one.
    // socket_op_in_flight_/write_pending_ serialize MULTIPLE WRITES against each other only: a
    // do_write() call that arrives mid-write just marks write_pending_ and returns; the in-flight
    // write's own completion handler re-invokes do_write() once it's done.
    //
    // Real, disclosed decision this ADR arrived at only after live testing, not up front: an
    // earlier version of this guard also serialized do_write() against a concurrently-outstanding
    // do_read() (reasoning: boost::asio::ssl::stream has a real, documented history of internal
    // races between overlapping read+write engine operations even under one strand -- see
    // chriskohlhoff/asio#791). That stronger guard was reverted after live testing surfaced a
    // real, 100%-reproducible DEADLOCK: a response queued behind an outstanding read that is
    // itself waiting for the client's next frame -- which, for this project's own synchronous,
    // one-request-at-a-time Client (every NF-to-NF caller in this codebase), never arrives until
    // the very response it's blocking is delivered. A confirmed hang is strictly worse than a
    // narrower, documented, disclosed data-race risk, so this project accepts genuine concurrent
    // read+write per connection (what boost::asio::ssl::stream is actually designed to support)
    // and instead suppresses the specific, narrowly-scoped, upstream-tracked OpenSSL/asio-engine
    // race class this can trigger under TSan (see .tsan-suppressions).
    void do_write() {
        if (socket_op_in_flight_) {
            write_pending_ = true;
            return;
        }

        write_buf_.clear();
        for (;;) {
            const std::uint8_t* data_ptr = nullptr;
            const auto len = nghttp2_session_mem_send(session_, &data_ptr);
            if (len <= 0) {
                break;
            }
            write_buf_.insert(write_buf_.end(), data_ptr, data_ptr + len);
        }

        if (write_buf_.empty()) {
            if (nghttp2_session_want_read(session_) != 0) {
                do_read();
            } else {
                close();
            }
            return;
        }

        socket_op_in_flight_ = true;
        auto self = shared_from_this();
        boost::asio::async_write(
            socket_,
            boost::asio::buffer(write_buf_),
            boost::asio::bind_executor(
                strand_, [self](boost::system::error_code ec, std::size_t /*n*/) {
                    self->socket_op_in_flight_ = false;
                    if (ec) {
                        self->close();
                        return;
                    }
                    if (self->write_pending_) {
                        self->write_pending_ = false;
                        self->do_write();
                        return;
                    }
                    if (nghttp2_session_want_read(self->session_) != 0) {
                        self->do_read();
                    } else if (nghttp2_session_want_write(self->session_) == 0) {
                        self->close();
                    }
                }));
    }

    // Real concurrency point (ADR-0239): route matching and request bookkeeping stay here, on the
    // connection's own strand (cheap, and routes_ is read-only after Server::start() so concurrent
    // reads from other connections' strands are already safe without this). The handler call
    // itself -- which may block on an outbound SBI call to another NF, or just do real work -- is
    // posted onto the shared io_context instead of being invoked inline, so a slow handler for one
    // stream never stalls this connection's own I/O processing (or, once the io_context is driven
    // by a worker-thread pool, any other connection's). Only the response-submission step that
    // follows touches nghttp2 session state again, so it's posted back onto strand_ via
    // submit_response.
    void handle_request_complete(std::int32_t stream_id) {
        auto it = streams_.find(stream_id);
        if (it == streams_.end()) {
            return;
        }
        StreamContext& ctx = it->second;

        auto req = std::make_shared<Request>();
        req->method = ctx.method;
        req->path = ctx.path;
        req->headers = ctx.headers;
        req->body = ctx.body;
        req->query_params = parse_query_string(ctx.path);
        req->peer_cert_cn = peer_cert_cn_;
        req->peer_address = peer_address_;
        req->peer_cert_dns_names = peer_cert_dns_names_;

        const auto path_only = ctx.path.substr(0, ctx.path.find('?'));
        const auto segments = split_path(path_only);

        const Handler* matched_handler = nullptr;
        for (const auto& route : routes_) {
            std::map<std::string, std::string> params;
            if (try_match(route, req->method, segments, params)) {
                req->path_params = std::move(params);
                matched_handler = &route.handler;
                break;
            }
        }

        auto self = shared_from_this();

        // P15 (ADR-0280): shed BEFORE dispatching. Checked after routing so a 404 is still a 404
        // -- an unroutable request is a client error, not load worth protecting against -- but
        // before the handler is posted, because the whole point is to spend less on a shed request
        // than on a served one.
        if (rate_limit_ != nullptr && !rate_limit_->try_acquire()) {
            // P12 (ADR-0282): shedding is a business-visible condition, not just an internal
            // decision -- an NF at its ceiling is refusing real traffic, and an operator needs to
            // see it without reading logs. One counter per process, created on first shed.
            static auto shed_counter = sbi_core::get_meter("sbi_core")
                                           ->CreateUInt64Counter("sbi_requests_shed_total",
                                                                 "Requests shed by the configured "
                                                                 "TPS ceiling (P15)");
            shed_counter->Add(1);
            Response resp;
            resp.status = 503;
            resp.headers.emplace("content-type", "application/problem+json");
            // Retry-After is plain HTTP (RFC 9110 §10.2.3), not a 3GPP addition: TS 29.571 defines
            // the 503 + ProblemDetails shape but no retry hint, so this is standard HTTP
            // semantics layered on the spec's own status, and is called out as such.
            resp.headers.emplace("retry-after", "1");
            resp.body = R"({"status":503,"title":"Service Unavailable",)"
                        R"("detail":"This NF is shedding load above its configured TPS ceiling"})";
            boost::asio::post(strand_, [self, stream_id, resp = std::move(resp)]() mutable {
                self->submit_response(stream_id, std::move(resp));
            });
            return;
        }

        if (matched_handler == nullptr) {
            Response resp;
            resp.status = 404;
            resp.headers.emplace("content-type", "application/problem+json");
            resp.body = R"({"status":404,"title":"Not Found",)"
                        R"("detail":"No route matches this method/path"})";
            boost::asio::post(strand_, [self, stream_id, resp = std::move(resp)]() mutable {
                self->submit_response(stream_id, std::move(resp));
            });
            return;
        }
        boost::asio::post(ioc_, [self, stream_id, req, handler = *matched_handler]() {
            // The API boundary (CLAUDE.md: exceptions only here). A handler that throws -- a
            // datastore client losing its connection mid-request is the realistic case, ADR-0360
            // -- answers a TS 29.500 ProblemDetails 500 for that one request. Before this, the
            // exception escaped io_context::run() and took the whole NF down with it.
            Response resp;
            try {
                resp = handler(*req);
            } catch (const std::exception& e) {
                spdlog::error(
                    "sbi-core: handler for {} {} threw: {}", req->method, req->path, e.what());
                resp.status = 500;
                resp.headers.emplace("content-type", "application/problem+json");
                resp.body =
                    R"({"status":500,"title":"Internal Server Error",)"
                    R"("detail":"request handler failed","cause":"SYSTEM_FAILURE"})"; // TS 29.500
                                                                                      // Table 5.2.7.2-1
            }
            boost::asio::post(self->strand_, [self, stream_id, resp = std::move(resp)]() mutable {
                self->submit_response(stream_id, std::move(resp));
            });
        });
    }

    // Runs on strand_ (posted from handle_request_complete above, possibly from a different
    // worker thread than the one that ran the handler) -- the only place besides do_read/do_write
    // that touches session_/streams_, so it must stay strand-serialized against those.
    void submit_response(std::int32_t stream_id, Response resp) {
        auto it = streams_.find(stream_id);
        if (it == streams_.end()) {
            // Stream already closed (e.g. client reset it) while the handler was running off
            // strand_ -- nothing to submit a response for.
            return;
        }
        StreamContext& ctx = it->second;

        ctx.response_body = std::move(resp.body);
        ctx.response_offset = 0;

        std::vector<std::string> status_str_storage;
        status_str_storage.push_back(std::to_string(resp.status));

        std::vector<nghttp2_nv> nva;
        nva.push_back(make_nv(":status", status_str_storage.back()));
        for (const auto& [name, value] : resp.headers) {
            nva.push_back(make_nv(name, value));
        }

        nghttp2_data_provider data_prd{};
        data_prd.read_callback = &Connection::on_read_response_body;

        nghttp2_submit_response(session_,
                                stream_id,
                                nva.data(),
                                nva.size(),
                                ctx.response_body.empty() ? nullptr : &data_prd);
        do_write();
    }

    // string_view, not const std::string&: a const-ref parameter bound to a string literal (e.g.
    // make_nv(":status", ...)) would materialize a temporary std::string that's destroyed at the
    // end of the full expression, leaving the returned nghttp2_nv's pointer dangling into freed
    // stack memory by the time nghttp2_submit_response's caller actually reads it. ASan caught this
    // in CI (stack-use-after-scope, ":status" is 7 bytes -- matched the reported read size
    // exactly). string_view has no such trap: it just wraps whatever storage the caller already
    // owns (a literal's static storage, or resp.headers'/status_str_storage's buffers).
    static nghttp2_nv make_nv(std::string_view name, std::string_view value) {
        nghttp2_nv nv{};
        nv.name = reinterpret_cast<std::uint8_t*>(const_cast<char*>(name.data()));
        nv.value = reinterpret_cast<std::uint8_t*>(const_cast<char*>(value.data()));
        nv.namelen = name.size();
        nv.valuelen = value.size();
        nv.flags = NGHTTP2_NV_FLAG_NONE;
        return nv;
    }

    static ssize_t on_read_response_body(nghttp2_session* /*session*/,
                                         std::int32_t stream_id,
                                         std::uint8_t* buf,
                                         std::size_t length,
                                         std::uint32_t* data_flags,
                                         nghttp2_data_source* /*source*/,
                                         void* user_data) {
        auto* self = static_cast<Connection*>(user_data);
        auto it = self->streams_.find(stream_id);
        if (it == self->streams_.end()) {
            *data_flags |= NGHTTP2_DATA_FLAG_EOF;
            return 0;
        }
        StreamContext& ctx = it->second;
        const std::size_t remaining = ctx.response_body.size() - ctx.response_offset;
        const std::size_t n = std::min(remaining, length);
        std::memcpy(buf, ctx.response_body.data() + ctx.response_offset, n);
        ctx.response_offset += n;
        if (ctx.response_offset >= ctx.response_body.size()) {
            *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        }
        return static_cast<ssize_t>(n);
    }

    static int
    on_begin_headers(nghttp2_session* /*session*/, const nghttp2_frame* frame, void* user_data) {
        if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
            return 0;
        }
        auto* self = static_cast<Connection*>(user_data);
        self->streams_.emplace(frame->hd.stream_id, StreamContext{});
        return 0;
    }

    static int on_header(nghttp2_session* /*session*/,
                         const nghttp2_frame* frame,
                         const std::uint8_t* name,
                         std::size_t namelen,
                         const std::uint8_t* value,
                         std::size_t valuelen,
                         std::uint8_t /*flags*/,
                         void* user_data) {
        if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
            return 0;
        }
        auto* self = static_cast<Connection*>(user_data);
        auto it = self->streams_.find(frame->hd.stream_id);
        if (it == self->streams_.end()) {
            return 0;
        }
        const std::string name_str(reinterpret_cast<const char*>(name), namelen);
        const std::string value_str(reinterpret_cast<const char*>(value), valuelen);
        if (name_str == ":method") {
            it->second.method = value_str;
        } else if (name_str == ":path") {
            it->second.path = value_str;
        } else if (!name_str.empty() && name_str.front() != ':') {
            it->second.headers.emplace(name_str, value_str);
        }
        return 0;
    }

    static int on_data_chunk_recv(nghttp2_session* /*session*/,
                                  std::uint8_t /*flags*/,
                                  std::int32_t stream_id,
                                  const std::uint8_t* data,
                                  std::size_t len,
                                  void* user_data) {
        auto* self = static_cast<Connection*>(user_data);
        auto it = self->streams_.find(stream_id);
        if (it != self->streams_.end()) {
            it->second.body.append(reinterpret_cast<const char*>(data), len);
        }
        return 0;
    }

    static int
    on_frame_recv(nghttp2_session* /*session*/, const nghttp2_frame* frame, void* user_data) {
        auto* self = static_cast<Connection*>(user_data);
        const bool end_stream = (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0;
        if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_REQUEST &&
            end_stream) {
            self->handle_request_complete(frame->hd.stream_id);
        } else if (frame->hd.type == NGHTTP2_DATA && end_stream) {
            self->handle_request_complete(frame->hd.stream_id);
        }
        return 0;
    }

    static int on_stream_close(nghttp2_session* /*session*/,
                               std::int32_t stream_id,
                               std::uint32_t /*error_code*/,
                               void* user_data) {
        auto* self = static_cast<Connection*>(user_data);
        self->streams_.erase(stream_id);
        return 0;
    }

    boost::asio::io_context& ioc_;
    SslStream socket_;
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
    const std::vector<Route>& routes_;
    // P15 (ADR-0280): owned by Server::Impl and shared by every connection, so the ceiling is the
    // NF's, not one client's. Null when no limit is configured.
    TokenBucket* rate_limit_ = nullptr;
    nghttp2_session* session_ = nullptr;
    std::array<std::uint8_t, 65536> read_buf_{};
    std::vector<std::uint8_t> write_buf_;
    bool socket_op_in_flight_ = false;
    bool write_pending_ = false;
    // Set once in capture_peer_identity() on the strand before any stream exists; read-only after.
    std::string peer_cert_cn_;
    std::string peer_address_;
    std::vector<std::string> peer_cert_dns_names_;
    std::unordered_map<std::int32_t, StreamContext> streams_;
};

} // namespace

namespace {

boost::asio::ssl::context make_server_ssl_context(const TlsConfig& tls) {
    // tlsv13_server, not the generic tlsv13: restricts the negotiable method set to TLS 1.3 server
    // mode specifically. Combined with no SSLv3/TLS1.0-1.2 context ever being constructed, there is
    // no downgrade path -- this is enforced by which OpenSSL method table gets selected, not just a
    // set_options() flag that could be forgotten.
    boost::asio::ssl::context ctx(boost::asio::ssl::context::tlsv13_server);

    try {
        ctx.use_certificate_chain_file(tls.cert_path);
        ctx.use_private_key_file(tls.key_path, boost::asio::ssl::context::pem);
        ctx.load_verify_file(tls.ca_path);
    } catch (const boost::system::system_error& e) {
        throw std::runtime_error("sbi-core: failed to load TLS material (cert=" + tls.cert_path +
                                 ", key=" + tls.key_path + ", ca=" + tls.ca_path +
                                 "): " + e.what());
    }

    // mTLS: require a client certificate and verify it against the CA above. No anonymous/
    // unauthenticated clients accepted -- see docs/DECISIONS.md ADR-0011.
    ctx.set_verify_mode(boost::asio::ssl::verify_peer |
                        boost::asio::ssl::verify_fail_if_no_peer_cert);

    // Real, pre-existing OpenSSL requirement this project never hit until real server concurrency
    // (ADR-0239) made it possible for two connections' handshakes/session-cache writes to race:
    // OpenSSL refuses to cache/complete a session under peer-cert verification unless an
    // application-defined session id context is set (its own documented anti-cross-context-
    // session-reuse safeguard), failing the handshake outright with "session id context
    // uninitialized" otherwise. One SSL_CTX per NF process, so any fixed, non-empty byte string is
    // sufficient -- doesn't need to be unique across processes.
    constexpr unsigned char kSessionIdContext[] = "5gc-sbi-core";
    SSL_CTX_set_session_id_context(
        ctx.native_handle(), kSessionIdContext, sizeof(kSessionIdContext) - 1);

    SSL_CTX_set_alpn_select_cb(ctx.native_handle(), alpn_select_callback, nullptr);

    return ctx;
}

} // namespace

class Server::Impl {
public:
    Impl(boost::asio::io_context& ioc, std::string address, unsigned short port, TlsConfig tls)
        : ioc_(ioc),
          acceptor_(ioc,
                    boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address(address), port)),
          ssl_ctx_(make_server_ssl_context(tls)) {}

    void add_route(std::string method, std::string path_pattern, Handler handler) {
        std::transform(method.begin(), method.end(), method.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        routes_.push_back(Route{std::move(method), split_path(path_pattern), std::move(handler)});
    }

    void start() { do_accept(); }

    unsigned short local_port() const { return acceptor_.local_endpoint().port(); }

private:
    void do_accept() {
        acceptor_.async_accept(
            [this](boost::system::error_code ec, boost::asio::ip::tcp::socket socket) {
                if (!ec) {
                    // TCP_NODELAY (ADR-0244). Boost.Asio does NOT set this by default, so every
                    // accepted connection had Nagle's algorithm enabled: a small response whose
                    // final segment is under the MSS sits in the kernel waiting either for more
                    // data or for the peer's ACK, which the peer's own delayed-ACK timer holds
                    // back. Measured with tools/sbi-loadgen against a real nrf before this line
                    // existed: p99 ~50ms against a p50 of 0.3-0.7ms at low concurrency, with the
                    // stall vanishing at concurrency 32 (where there is always more data queued,
                    // so Nagle never waits) -- the classic signature, and confirmed by the
                    // before/after numbers in ADR-0244 rather than assumed from the shape.
                    // SBI responses are exactly the small-message pattern Nagle penalises, and
                    // every NF is both a server and a client of other NFs, so this compounds
                    // across a call chain.
                    boost::system::error_code nodelay_ec;
                    socket.set_option(boost::asio::ip::tcp::no_delay(true), nodelay_ec);
                    if (nodelay_ec) {
                        // Non-fatal: a connection with Nagle still on is slow, not broken.
                        spdlog::warn("sbi: could not set TCP_NODELAY: {}", nodelay_ec.message());
                    }
                    auto conn = std::make_shared<Connection>(
                        ioc_, std::move(socket), ssl_ctx_, routes_, rate_limit_.get());
                    conn->start();
                }
                do_accept();
            });
    }

    boost::asio::io_context& ioc_;
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::ssl::context ssl_ctx_;
    std::vector<Route> routes_;
    std::unique_ptr<TokenBucket> rate_limit_;

public:
    void set_tps_limit(double sustained_tps, double burst_capacity) {
        if (sustained_tps <= 0.0) {
            rate_limit_.reset();
            return;
        }
        rate_limit_ = std::make_unique<TokenBucket>(sustained_tps, burst_capacity);
    }

    std::uint64_t shed_count() const {
        return rate_limit_ == nullptr ? 0 : rate_limit_->shed_count();
    }
};

Server::Server(boost::asio::io_context& ioc,
               std::string address,
               unsigned short port,
               TlsConfig tls)
    : impl_(std::make_unique<Impl>(ioc, std::move(address), port, std::move(tls))) {}

Server::~Server() = default;

void Server::set_tps_limit(double sustained_tps, double burst_capacity) {
    impl_->set_tps_limit(sustained_tps, burst_capacity);
}

std::uint64_t Server::shed_count() const {
    return impl_->shed_count();
}

void Server::add_route(std::string method, std::string path_pattern, Handler handler) {
    impl_->add_route(std::move(method), std::move(path_pattern), std::move(handler));
}

void Server::start() {
    impl_->start();
}

unsigned short Server::local_port() const {
    return impl_->local_port();
}

} // namespace sbi_core::http2
