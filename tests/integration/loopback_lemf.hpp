#pragma once

// A loopback LEMF: one TLS 1.3 listener collecting whole BER PS-PDUs (TS 102 232-1 HI2), answering
// the MDF2's clause-6.3 keep-alives. Shared by the MDF2 test and the ADMF end-to-end test.

#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <netinet/in.h>
#include <optional>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "li_core/hi2.hpp"

namespace nf_test {

namespace hi2 = li_core::hi2;

// A loopback LEMF: one TLS 1.3 listener collecting whole BER PS-PDUs.
class LoopbackLemf {
public:
    LoopbackLemf() {
        ctx_ = SSL_CTX_new(TLS_server_method());
        SSL_CTX_set_min_proto_version(ctx_, TLS1_3_VERSION);
        SSL_CTX_use_certificate_file(ctx_, CERTS_DIR "/hello-nf/cert.pem", SSL_FILETYPE_PEM);
        SSL_CTX_use_PrivateKey_file(ctx_, CERTS_DIR "/hello-nf/key.pem", SSL_FILETYPE_PEM);
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
        socklen_t len = sizeof addr;
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        ::listen(listen_fd_, 4);
        running_.store(true);
        thread_ = std::thread([this] { serve(); });
    }
    ~LoopbackLemf() {
        running_.store(false);
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        if (ctx_ != nullptr) {
            SSL_CTX_free(ctx_);
        }
    }
    std::uint16_t port() const { return port_; }
    bool wait_for_iri(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return !iri_.empty(); });
    }
    std::vector<std::vector<std::uint8_t>> iri() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return iri_;
    }

private:
    static std::size_t tlv_length(const std::vector<std::uint8_t>& buf) {
        if (buf.size() < 2) {
            return 0;
        }
        const std::uint8_t first = buf[1];
        if ((first & 0x80U) == 0) {
            return 2 + first;
        }
        const std::size_t count = first & 0x7FU;
        if (count == 0 || count > 4 || buf.size() < 2 + count) {
            return 0;
        }
        std::size_t len = 0;
        for (std::size_t i = 0; i < count; ++i) {
            len = (len << 8) | buf[2 + i];
        }
        return 2 + count + len;
    }

    void serve() {
        while (running_.load()) {
            const int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) {
                if (!running_.load()) {
                    return;
                }
                continue;
            }
            SSL* ssl = SSL_new(ctx_);
            SSL_set_fd(ssl, fd);
            if (SSL_accept(ssl) != 1) {
                SSL_free(ssl);
                ::close(fd);
                continue;
            }
            std::vector<std::uint8_t> buffer;
            std::uint8_t chunk[4096];
            while (running_.load()) {
                const int n = SSL_read(ssl, chunk, static_cast<int>(sizeof chunk));
                if (n <= 0) {
                    break;
                }
                buffer.insert(buffer.end(), chunk, chunk + n);
                for (;;) {
                    const std::size_t len = tlv_length(buffer);
                    if (len == 0 || buffer.size() < len) {
                        break;
                    }
                    std::vector<std::uint8_t> pdu(buffer.begin(),
                                                  buffer.begin() + static_cast<long>(len));
                    buffer.erase(buffer.begin(), buffer.begin() + static_cast<long>(len));
                    const auto kind = hi2::payload_kind(pdu);
                    if (kind && *kind == hi2::PayloadKind::Tri) {
                        const auto tri = hi2::decode_tri_message(pdu);
                        if (tri && tri->type == hi2::TriType::KeepAlive) {
                            hi2::TriMessage response = *tri;
                            response.type = hi2::TriType::KeepAliveResponse;
                            if (const auto encoded = hi2::encode_tri_message(response)) {
                                SSL_write(ssl, encoded->data(), static_cast<int>(encoded->size()));
                            }
                        }
                        continue;
                    }
                    const std::lock_guard<std::mutex> lock(mutex_);
                    iri_.push_back(std::move(pdu));
                    cv_.notify_all();
                }
            }
            SSL_shutdown(ssl);
            SSL_free(ssl);
            ::close(fd);
        }
    }

    SSL_CTX* ctx_ = nullptr;
    int listen_fd_ = -1;
    std::uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::vector<std::uint8_t>> iri_;
};

} // namespace nf_test
