#include "li_core/x2x3_client.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

// TS 103 221-2 delivery over blocking mTLS (ADR-0372). OpenSSL directly, not sbi_core -- li_core
// must not depend on the SBI stack (the AMF that hosts the POI links both, and li_core stays a
// standalone shared object). One socket, guarded by X2X3Client::mutex_ in the header.

namespace li_core {

namespace {
std::string ssl_err(const std::string& what) {
    unsigned long e = ERR_get_error();
    char buf[256] = {0};
    if (e != 0) {
        ERR_error_string_n(e, buf, sizeof buf);
        return what + ": " + buf;
    }
    return what;
}
} // namespace

struct X2X3Client::Impl {
    X2X3ClientConfig cfg;
    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr;
    int fd = -1;
    std::uint32_t seq = 0;
    std::chrono::steady_clock::time_point last_send{};

    explicit Impl(X2X3ClientConfig c) : cfg(std::move(c)) {}
    ~Impl() {
        close_conn();
        if (ctx != nullptr) {
            SSL_CTX_free(ctx);
        }
    }

    void close_conn() {
        if (ssl != nullptr) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
            ssl = nullptr;
        }
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }

    tl::expected<void, std::string> ensure_ctx() {
        if (ctx != nullptr) {
            return {};
        }
        ctx = SSL_CTX_new(TLS_client_method());
        if (ctx == nullptr) {
            return tl::make_unexpected(ssl_err("SSL_CTX_new"));
        }
        SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        if (SSL_CTX_load_verify_locations(ctx, cfg.ca_path.c_str(), nullptr) != 1) {
            return tl::make_unexpected(ssl_err("load CA " + cfg.ca_path));
        }
        if (SSL_CTX_use_certificate_file(ctx, cfg.client_cert_path.c_str(), SSL_FILETYPE_PEM) !=
            1) {
            return tl::make_unexpected(ssl_err("client cert " + cfg.client_cert_path));
        }
        if (SSL_CTX_use_PrivateKey_file(ctx, cfg.client_key_path.c_str(), SSL_FILETYPE_PEM) != 1) {
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
        SSL_set1_host(ssl, sni.c_str()); // verify the server cert names this host
        if (SSL_connect(ssl) != 1) {
            SSL_free(ssl);
            ssl = nullptr;
            ::close(sock);
            return tl::make_unexpected(ssl_err("TLS handshake with " + cfg.host));
        }
        fd = sock;
        return {};
    }

    tl::expected<void, std::string> write_all(const std::vector<std::uint8_t>& bytes) {
        std::size_t off = 0;
        while (off < bytes.size()) {
            const int n = SSL_write(ssl, bytes.data() + off, static_cast<int>(bytes.size() - off));
            if (n <= 0) {
                return tl::make_unexpected(ssl_err("SSL_write"));
            }
            off += static_cast<std::size_t>(n);
        }
        last_send = std::chrono::steady_clock::now();
        return {};
    }
};

X2X3Client::X2X3Client(X2X3ClientConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
X2X3Client::~X2X3Client() = default;

tl::expected<void, std::string> X2X3Client::send(const Pdu& pdu) {
    if (auto v = validate(pdu)) {
        return tl::make_unexpected("X2/X3 PDU not conformant: " + *v);
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    if (impl_->ssl == nullptr) {
        if (auto r = impl_->connect(); !r) {
            return r;
        }
    }
    auto r = impl_->write_all(encode(pdu));
    if (!r) {
        impl_->close_conn(); // reconnect on next send
    }
    return r;
}

tl::expected<void, std::string> X2X3Client::send_keepalive() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (impl_->ssl == nullptr) {
        if (auto r = impl_->connect(); !r) {
            return r;
        }
    }
    const Pdu ka = make_keepalive(++impl_->seq);
    auto r = impl_->write_all(encode(ka));
    if (!r) {
        impl_->close_conn();
    }
    return r;
}

void X2X3Client::disconnect() {
    const std::lock_guard<std::mutex> lock(mutex_);
    impl_->close_conn();
}

bool X2X3Client::connected() const {
    return impl_->ssl != nullptr;
}

} // namespace li_core
