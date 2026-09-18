#include "li_core/hi2_client.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <netdb.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

// ETSI TS 102 232-1 clause 6.3 Delivery Function (ADR-0376). OpenSSL directly, as the X2/X3
// client and server are: li_core does not depend on the SBI stack, and HI2 is a byte stream of
// BER PS-PDUs, not HTTP.

namespace li_core {

namespace {

std::string ssl_err(const std::string& what) {
    const unsigned long e = ERR_get_error();
    char buf[256] = {0};
    if (e != 0) {
        ERR_error_string_n(e, buf, sizeof buf);
        return what + ": " + buf;
    }
    return what;
}

} // namespace

struct Hi2Client::Impl {
    Hi2ClientConfig cfg;
    hi2::PsHeader keepalive_header;
    Hi2EventHandler on_event;

    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr;
    int fd = -1;

    std::thread worker;
    std::atomic<bool> running{false};
    std::atomic<bool> is_connected{false};

    mutable std::mutex mutex;
    std::condition_variable cv;
    // The clause-6.3.3 cyclic buffer. A PDU stays here until the transport has taken it; on a
    // reconnect everything still present is re-sent before any newer PDU (FIFO, so the deque
    // order IS the resynchronisation order).
    std::deque<std::vector<std::uint8_t>> buffer;
    std::size_t buffered = 0;
    // Index of the first PDU not yet handed to an open socket. Everything before it has been
    // sent once; it stays buffered until the buffer needs the room, so a reconnect can re-send it.
    std::size_t unsent_from = 0;

    std::uint32_t keepalive_sequence = 0;
    std::chrono::steady_clock::time_point last_send{};
    std::chrono::steady_clock::time_point keepalive_sent_at{};
    bool awaiting_keepalive_response = false;

    std::atomic<std::uint64_t> sent{0};
    std::atomic<std::uint64_t> discarded{0};
    std::atomic<std::uint64_t> reconnect_count{0};
    std::atomic<std::uint64_t> keepalives{0};

    Impl(Hi2ClientConfig c, hi2::PsHeader h, Hi2EventHandler e)
        : cfg(std::move(c)), keepalive_header(std::move(h)), on_event(std::move(e)) {}
    ~Impl() {
        close_connection();
        if (ctx != nullptr) {
            SSL_CTX_free(ctx);
        }
    }

    void report(Hi2Event event, std::string_view detail) {
        if (on_event) {
            on_event(event, detail);
        }
    }

    void close_connection() {
        if (ssl != nullptr) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
            ssl = nullptr;
        }
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
        is_connected.store(false);
    }

    tl::expected<void, std::string> ensure_ctx() {
        if (ctx != nullptr) {
            return {};
        }
        static std::once_flag sigpipe_once;
        std::call_once(sigpipe_once, [] { ::signal(SIGPIPE, SIG_IGN); });
        ctx = SSL_CTX_new(TLS_client_method());
        if (ctx == nullptr) {
            return tl::make_unexpected(ssl_err("SSL_CTX_new"));
        }
        SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        if (SSL_CTX_load_verify_locations(ctx, cfg.ca_path.c_str(), nullptr) != 1) {
            return tl::make_unexpected(ssl_err("load CA " + cfg.ca_path));
        }
        if (!cfg.client_cert_path.empty() &&
            SSL_CTX_use_certificate_file(ctx, cfg.client_cert_path.c_str(), SSL_FILETYPE_PEM) !=
                1) {
            return tl::make_unexpected(ssl_err("client cert " + cfg.client_cert_path));
        }
        if (!cfg.client_key_path.empty() &&
            SSL_CTX_use_PrivateKey_file(ctx, cfg.client_key_path.c_str(), SSL_FILETYPE_PEM) != 1) {
            return tl::make_unexpected(ssl_err("client key " + cfg.client_key_path));
        }
        return {};
    }

    tl::expected<void, std::string> connect() {
        if (auto r = ensure_ctx(); !r) {
            return r;
        }
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        const std::string port = std::to_string(cfg.port);
        if (getaddrinfo(cfg.host.c_str(), port.c_str(), &hints, &res) != 0 || res == nullptr) {
            return tl::make_unexpected("getaddrinfo " + cfg.host + ":" + port + " failed");
        }
        int sock = -1;
        for (addrinfo* a = res; a != nullptr; a = a->ai_next) {
            sock = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (sock < 0) {
                continue;
            }
            if (::connect(sock, a->ai_addr, a->ai_addrlen) == 0) {
                break;
            }
            ::close(sock);
            sock = -1;
        }
        freeaddrinfo(res);
        if (sock < 0) {
            return tl::make_unexpected("TCP connect to " + cfg.host + ":" + port +
                                       " failed: " + std::strerror(errno));
        }
        ssl = SSL_new(ctx);
        if (ssl == nullptr) {
            ::close(sock);
            return tl::make_unexpected(ssl_err("SSL_new"));
        }
        SSL_set_fd(ssl, sock);
        const std::string& sni = cfg.sni.empty() ? cfg.host : cfg.sni;
        SSL_set_tlsext_host_name(ssl, sni.c_str());
        SSL_set1_host(ssl, sni.c_str());
        if (SSL_connect(ssl) != 1) {
            SSL_free(ssl);
            ssl = nullptr;
            ::close(sock);
            return tl::make_unexpected(ssl_err("TLS handshake with " + cfg.host));
        }
        fd = sock;
        is_connected.store(true);
        // A read timeout bounded by TIME3 lets the worker notice a missing keep-aliveResponse
        // without a second thread: SSL_read returns 0/-1 on timeout and the loop re-checks.
        timeval tv{};
        tv.tv_sec = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        last_send = std::chrono::steady_clock::now();
        awaiting_keepalive_response = false;
        return {};
    }

    bool write_all(const std::vector<std::uint8_t>& bytes) {
        std::size_t off = 0;
        while (off < bytes.size()) {
            const int n = SSL_write(ssl, bytes.data() + off, static_cast<int>(bytes.size() - off));
            if (n <= 0) {
                return false;
            }
            off += static_cast<std::size_t>(n);
        }
        last_send = std::chrono::steady_clock::now();
        return true;
    }

    // Clause 6.3.4: the keep-alive TRI carries a fresh timestamp and a sequence number that
    // increments per keep-alive within this instance of the Delivery Function.
    bool send_keepalive() {
        hi2::TriMessage message;
        message.header = keepalive_header;
        message.header.sequence_number = ++keepalive_sequence;
        message.header.timestamp = hi2::Timestamp{static_cast<std::uint64_t>(::time(nullptr)), 0};
        message.type = hi2::TriType::KeepAlive;
        auto encoded = hi2::encode_tri_message(message);
        if (!encoded) {
            report(Hi2Event::ConnectFailed, encoded.error());
            return false;
        }
        if (!write_all(*encoded)) {
            return false;
        }
        keepalives.fetch_add(1);
        keepalive_sent_at = std::chrono::steady_clock::now();
        awaiting_keepalive_response = true;
        return true;
    }

    // Drain whatever the LEMF sends us. The only message this DF acts on is the
    // keep-aliveResponse of clause 6.3.4; anything else is read and ignored so the socket does
    // not fill.
    void read_available() {
        std::uint8_t chunk[4096];
        const int n = SSL_read(ssl, chunk, static_cast<int>(sizeof chunk));
        if (n > 0) {
            // A keep-aliveResponse is a small PS-PDU; decode what arrived and clear the wait on
            // any well-formed TRI response. A partial or unknown read is not an error here.
            const auto message = hi2::decode_tri_message(
                std::span<const std::uint8_t>(chunk, static_cast<std::size_t>(n)));
            if (message && message->type == hi2::TriType::KeepAliveResponse) {
                awaiting_keepalive_response = false;
            }
            return;
        }
        const int err = SSL_get_error(ssl, n);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            return; // the SO_RCVTIMEO tick, not a failure
        }
        if (n == 0 || err == SSL_ERROR_ZERO_RETURN || err == SSL_ERROR_SYSCALL) {
            close_connection();
            report(Hi2Event::Disconnected, "the LEMF closed the transport connection");
        }
    }

    void run() {
        while (running.load()) {
            if (ssl == nullptr) {
                auto r = connect();
                if (!r) {
                    report(Hi2Event::ConnectFailed, r.error());
                    // 6.3.2: keep trying at the configured interval.
                    std::unique_lock<std::mutex> lock(mutex);
                    cv.wait_for(lock, cfg.reconnect_interval, [this] { return !running.load(); });
                    continue;
                }
                reconnect_count.fetch_add(1);
                report(Hi2Event::Connected, cfg.host + ":" + std::to_string(cfg.port));
                // 6.3.3: resynchronise -- everything still in the buffer goes before new data.
                std::size_t to_resend = 0;
                {
                    const std::lock_guard<std::mutex> lock(mutex);
                    unsent_from = 0;
                    to_resend = buffer.size();
                }
                if (to_resend > 0) {
                    report(Hi2Event::Resynchronised,
                           std::to_string(to_resend) + " buffered PDU(s) re-sent");
                }
            }

            // Send whatever is pending.
            std::vector<std::uint8_t> next;
            bool have_next = false;
            {
                std::unique_lock<std::mutex> lock(mutex);
                if (unsent_from < buffer.size()) {
                    next = buffer[unsent_from];
                    have_next = true;
                } else {
                    // Idle: wake for new data, for TIME1, or for stop().
                    cv.wait_for(lock, std::chrono::seconds(1), [this] {
                        return !running.load() || unsent_from < buffer.size();
                    });
                }
            }
            if (have_next) {
                if (!write_all(next)) {
                    close_connection();
                    report(Hi2Event::Disconnected, "write failed; the connection was dropped");
                    continue;
                }
                const std::lock_guard<std::mutex> lock(mutex);
                ++unsent_from;
                sent.fetch_add(1);
                // A sent PDU stays in the cyclic buffer; only a full buffer evicts it (see
                // send()). That is what makes clause 6.3.3's resynchronisation real: after a
                // reconnect the DF re-sends everything still held, not merely what it never got
                // to. NOTE 2 of that clause is explicit that this can duplicate delivery and that
                // the LEMF must cope.
                continue;
            }

            if (ssl == nullptr) {
                continue;
            }
            read_available();
            if (ssl == nullptr) {
                continue;
            }

            const auto now = std::chrono::steady_clock::now();
            if (awaiting_keepalive_response) {
                if (now - keepalive_sent_at > cfg.keepalive_time3) {
                    // 6.3.4: no response within TIME3 -- terminate and open a new connection.
                    close_connection();
                    report(Hi2Event::KeepaliveTimeout,
                           "no keep-aliveResponse within TIME3; reopening the connection");
                }
            } else if (now - last_send > cfg.keepalive_time1) {
                if (!send_keepalive()) {
                    close_connection();
                    report(Hi2Event::Disconnected, "keep-alive write failed");
                }
            }
        }
        close_connection();
    }
};

Hi2Client::Hi2Client(Hi2ClientConfig config,
                     hi2::PsHeader keepalive_header,
                     Hi2EventHandler on_event)
    : impl_(std::make_unique<Impl>(
          std::move(config), std::move(keepalive_header), std::move(on_event))) {}

Hi2Client::~Hi2Client() {
    stop();
}

void Hi2Client::start() {
    if (impl_->running.exchange(true)) {
        return;
    }
    impl_->worker = std::thread([this] { impl_->run(); });
}

void Hi2Client::stop() {
    if (!impl_->running.exchange(false)) {
        return;
    }
    impl_->cv.notify_all();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
}

tl::expected<void, std::string> Hi2Client::send(std::vector<std::uint8_t> ps_pdu) {
    if (ps_pdu.empty()) {
        return tl::make_unexpected(std::string("refusing to deliver an empty PS-PDU"));
    }
    if (ps_pdu.size() > impl_->cfg.buffer_capacity_bytes) {
        return tl::make_unexpected(std::string("PS-PDU of ") + std::to_string(ps_pdu.size()) +
                                   " octets exceeds the whole cyclic buffer");
    }
    bool overwrote = false;
    {
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        // 6.3.3: "The Delivery Function will only accept PDUs for transport if there is room for
        // them in the cyclic buffer. If the buffer becomes full, the Delivery Function reports
        // this to the Handover Manager; the Delivery Function then discards data by overwriting
        // the oldest data in the buffer."
        while (!impl_->buffer.empty() &&
               impl_->buffered + ps_pdu.size() > impl_->cfg.buffer_capacity_bytes) {
            impl_->buffered -= impl_->buffer.front().size();
            impl_->buffer.pop_front();
            if (impl_->unsent_from > 0) {
                --impl_->unsent_from;
            }
            impl_->discarded.fetch_add(1);
            overwrote = true;
        }
        impl_->buffered += ps_pdu.size();
        impl_->buffer.push_back(std::move(ps_pdu));
    }
    if (overwrote) {
        impl_->report(Hi2Event::BufferFull,
                      "cyclic buffer full; the oldest undelivered PDU(s) were overwritten");
    }
    impl_->cv.notify_all();
    return {};
}

bool Hi2Client::connected() const {
    return impl_->is_connected.load();
}
std::uint64_t Hi2Client::pdus_sent() const {
    return impl_->sent.load();
}
std::uint64_t Hi2Client::pdus_discarded() const {
    return impl_->discarded.load();
}
std::uint64_t Hi2Client::reconnects() const {
    return impl_->reconnect_count.load();
}
std::uint64_t Hi2Client::keepalives_sent() const {
    return impl_->keepalives.load();
}
std::size_t Hi2Client::buffered_bytes() const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->buffered;
}

} // namespace li_core
