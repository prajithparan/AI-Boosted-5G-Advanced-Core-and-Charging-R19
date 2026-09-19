#include "li_core/x2x3_server.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

// The MDF2/MDF3 receiving half of LI_X2/LI_X3 (ADR-0375). Mirrors x2x3_client: OpenSSL directly
// (li_core does not depend on the SBI stack), blocking sockets, one thread per connection.

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

// Version..Payload Length: enough of the header to know the whole frame's length (clause 5.2.3).
constexpr std::size_t kLengthsKnownAfter = 12;

std::string peer_name(int fd) {
    sockaddr_storage addr{};
    socklen_t len = sizeof addr;
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return "?";
    }
    char host[INET6_ADDRSTRLEN] = {0};
    std::uint16_t port = 0;
    if (addr.ss_family == AF_INET) {
        const auto* in4 = reinterpret_cast<const sockaddr_in*>(&addr);
        ::inet_ntop(AF_INET, &in4->sin_addr, host, sizeof host);
        port = ntohs(in4->sin_port);
    } else if (addr.ss_family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&addr);
        ::inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof host);
        port = ntohs(in6->sin6_port);
    }
    return std::string(host) + ":" + std::to_string(port);
}

} // namespace

struct X2X3Server::Impl {
    X2X3ServerConfig cfg;
    X2X3PduHandler handler;
    SSL_CTX* ctx = nullptr;
    int listen_fd = -1;
    std::uint16_t port = 0;
    std::atomic<bool> running{false};
    std::thread accept_thread;
    std::mutex conn_mutex;
    // Each connection's thread flips its flag on the way out, so the accept loop can join the
    // finished ones instead of holding every thread object until stop(). A POI that reconnects in
    // a loop would otherwise accumulate them for the life of the MDF.
    struct Connection {
        std::thread thread;
        // Owned here, read through a raw pointer by the thread: the Connection outlives its
        // thread (it is erased only after join), and the vector's reallocation moves the
        // unique_ptr, never the flag it points at.
        std::unique_ptr<std::atomic<bool>> finished;
        int fd = -1; // shut down by stop(), so a thread blocked in SSL_read wakes and exits
    };
    std::vector<Connection> conn_threads;
    std::atomic<std::uint64_t> pdus{0};
    std::atomic<std::uint64_t> keepalives{0};
    std::atomic<std::uint64_t> rejected{0};

    Impl(X2X3ServerConfig c, X2X3PduHandler h) : cfg(std::move(c)), handler(std::move(h)) {}
    ~Impl() {
        if (ctx != nullptr) {
            SSL_CTX_free(ctx);
        }
    }

    tl::expected<void, std::string> make_ctx() {
        // Same reason as the client: a peer that vanishes mid-write must produce an error, never
        // a SIGPIPE that kills the MDF.
        static std::once_flag sigpipe_once;
        std::call_once(sigpipe_once, [] { ::signal(SIGPIPE, SIG_IGN); });
        ctx = SSL_CTX_new(TLS_server_method());
        if (ctx == nullptr) {
            return tl::make_unexpected(ssl_err("SSL_CTX_new"));
        }
        SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
        if (SSL_CTX_use_certificate_file(ctx, cfg.server_cert_path.c_str(), SSL_FILETYPE_PEM) !=
            1) {
            return tl::make_unexpected(ssl_err("server cert " + cfg.server_cert_path));
        }
        if (SSL_CTX_use_PrivateKey_file(ctx, cfg.server_key_path.c_str(), SSL_FILETYPE_PEM) != 1) {
            return tl::make_unexpected(ssl_err("server key " + cfg.server_key_path));
        }
        if (SSL_CTX_load_verify_locations(ctx, cfg.ca_path.c_str(), nullptr) != 1) {
            return tl::make_unexpected(ssl_err("load CA " + cfg.ca_path));
        }
        // TS 33.128 clause 5.3: the POI authenticates to the MDF. An unauthenticated peer must not
        // be able to inject intercepted material, so a verified client certificate is required.
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
        return {};
    }

    tl::expected<void, std::string> bind_listen() {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;
        addrinfo* res = nullptr;
        const std::string service = std::to_string(cfg.port);
        if (getaddrinfo(cfg.bind_address.c_str(), service.c_str(), &hints, &res) != 0 ||
            res == nullptr) {
            return tl::make_unexpected("getaddrinfo " + cfg.bind_address + ":" + service +
                                       " failed");
        }
        int fd = -1;
        for (addrinfo* a = res; a != nullptr; a = a->ai_next) {
            fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (fd < 0) {
                continue;
            }
            int one = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            if (::bind(fd, a->ai_addr, a->ai_addrlen) == 0) {
                break;
            }
            ::close(fd);
            fd = -1;
        }
        freeaddrinfo(res);
        if (fd < 0) {
            return tl::make_unexpected("bind " + cfg.bind_address + ":" + service +
                                       " failed: " + std::strerror(errno));
        }
        if (::listen(fd, cfg.backlog) != 0) {
            const std::string err = std::strerror(errno);
            ::close(fd);
            return tl::make_unexpected("listen failed: " + err);
        }
        sockaddr_storage bound{};
        socklen_t len = sizeof bound;
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
            port = bound.ss_family == AF_INET
                       ? ntohs(reinterpret_cast<const sockaddr_in*>(&bound)->sin_port)
                       : ntohs(reinterpret_cast<const sockaddr_in6*>(&bound)->sin6_port);
        }
        listen_fd = fd;
        return {};
    }

    // Read the framed PDU stream until the peer closes or something is wrong with it.
    void serve(int fd) {
        SSL* ssl = SSL_new(ctx);
        if (ssl == nullptr) {
            ::close(fd);
            return;
        }
        SSL_set_fd(ssl, fd);
        if (SSL_accept(ssl) != 1) {
            rejected.fetch_add(1);
            SSL_free(ssl);
            ::close(fd);
            return;
        }
        const std::string peer = peer_name(fd);

        std::vector<std::uint8_t> buffer;
        std::uint8_t chunk[8192];
        while (running.load()) {
            const int n = SSL_read(ssl, chunk, static_cast<int>(sizeof chunk));
            if (n <= 0) {
                break;
            }
            buffer.insert(buffer.end(), chunk, chunk + n);

            // One read may carry several PDUs, or half of one: drain whole frames only.
            while (buffer.size() >= kLengthsKnownAfter) {
                const auto frame = frame_length(buffer);
                if (!frame) {
                    break;
                }
                if (*frame > cfg.max_pdu_bytes || *frame < kMandatoryHeaderLength) {
                    // A length field that is absurd or below the clause-5.2.3 minimum: the stream
                    // cannot be resynchronised from here, so drop the connection rather than guess
                    // where the next PDU starts. Payload Length is 32 bits from an untrusted peer.
                    SSL_shutdown(ssl);
                    SSL_free(ssl);
                    ::close(fd);
                    return;
                }
                if (buffer.size() < *frame) {
                    break; // wait for the rest
                }
                std::size_t consumed = 0;
                auto pdu = decode(std::span<const std::uint8_t>(buffer.data(), *frame), &consumed);
                if (!pdu || consumed != *frame) {
                    // decode() reports what it consumed; today that is always Header Length +
                    // Payload Length, the same two fields frame_length() reads. Advancing by
                    // `consumed` rather than by the frame length keeps this loop correct if that
                    // ever stops being true -- a mismatch would desynchronise the stream, and
                    // every later PDU on the connection would be garbage.
                    SSL_shutdown(ssl);
                    SSL_free(ssl);
                    ::close(fd);
                    return;
                }
                buffer.erase(buffer.begin(),
                             buffer.begin() + static_cast<std::ptrdiff_t>(consumed));

                if (pdu->type == PduType::Keepalive) {
                    // Clause 6.2.4: acknowledge with the Sequence Number that was sent.
                    const auto seq = sequence_number(*pdu).value_or(0);
                    const auto ack = encode(make_keepalive_ack(seq));
                    std::size_t off = 0;
                    while (off < ack.size()) {
                        const int w =
                            SSL_write(ssl, ack.data() + off, static_cast<int>(ack.size() - off));
                        if (w <= 0) {
                            break;
                        }
                        off += static_cast<std::size_t>(w);
                    }
                    keepalives.fetch_add(1);
                    continue;
                }
                if (pdu->type == PduType::KeepaliveAck) {
                    continue; // an MDF does not send keepalives on this link; ignore
                }
                if (validate(*pdu)) {
                    continue; // non-conformant: counted by the handler's absence, not delivered
                }
                pdus.fetch_add(1);
                if (handler) {
                    handler(*pdu, peer);
                }
            }
        }
        SSL_shutdown(ssl);
        SSL_free(ssl);
        ::close(fd);
    }

    void accept_loop() {
        while (running.load()) {
            const int fd = ::accept(listen_fd, nullptr, nullptr);
            if (fd < 0) {
                if (!running.load()) {
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            const std::lock_guard<std::mutex> lock(conn_mutex);
            for (auto it = conn_threads.begin(); it != conn_threads.end();) {
                if (it->finished->load()) {
                    if (it->thread.joinable()) {
                        it->thread.join();
                    }
                    it = conn_threads.erase(it);
                } else {
                    ++it;
                }
            }
            auto finished = std::unique_ptr<std::atomic<bool>>(new std::atomic<bool>(false));
            auto* finished_flag = finished.get();
            conn_threads.push_back(Connection{std::thread([this, fd, finished_flag] {
                                                  serve(fd);
                                                  finished_flag->store(true);
                                              }),
                                              std::move(finished),
                                              fd});
        }
    }
};

X2X3Server::X2X3Server(X2X3ServerConfig config, X2X3PduHandler handler)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(handler))) {}

X2X3Server::~X2X3Server() {
    stop();
}

tl::expected<void, std::string> X2X3Server::start() {
    if (impl_->running.load()) {
        return {};
    }
    if (auto r = impl_->make_ctx(); !r) {
        return r;
    }
    if (auto r = impl_->bind_listen(); !r) {
        return r;
    }
    impl_->running.store(true);
    impl_->accept_thread = std::thread([this] { impl_->accept_loop(); });
    return {};
}

void X2X3Server::stop() {
    if (!impl_->running.exchange(false)) {
        return;
    }
    // Wake the accept loop (it is blocked in ::accept) but leave listen_fd's value alone until
    // the thread has joined: closing it and writing -1 here would race the loop's read of it in
    // ::accept (TSan, x2x3_server.cpp:263 vs :325). On Linux shutdown() on a listening socket is
    // what makes the blocked accept() return; the fd is only closed once nothing can read it.
    if (impl_->listen_fd >= 0) {
        ::shutdown(impl_->listen_fd, SHUT_RDWR);
    }
    if (impl_->accept_thread.joinable()) {
        impl_->accept_thread.join();
    }
    if (impl_->listen_fd >= 0) {
        ::close(impl_->listen_fd);
        impl_->listen_fd = -1;
    }
    std::vector<Impl::Connection> connections;
    {
        const std::lock_guard<std::mutex> lock(impl_->conn_mutex);
        connections.swap(impl_->conn_threads);
    }
    // A connection thread is blocked in SSL_read; shutting the socket down makes that return 0
    // so the loop sees !running and unwinds. Without this, stop() would wait for the POI to close.
    for (auto& connection : connections) {
        if (connection.fd >= 0) {
            ::shutdown(connection.fd, SHUT_RDWR);
        }
    }
    for (auto& connection : connections) {
        if (connection.thread.joinable()) {
            connection.thread.join();
        }
    }
}

std::uint16_t X2X3Server::bound_port() const {
    return impl_->port;
}
std::uint64_t X2X3Server::pdus_received() const {
    return impl_->pdus.load();
}
std::uint64_t X2X3Server::keepalives_answered() const {
    return impl_->keepalives.load();
}
std::uint64_t X2X3Server::connections_rejected() const {
    return impl_->rejected.load();
}

} // namespace li_core
