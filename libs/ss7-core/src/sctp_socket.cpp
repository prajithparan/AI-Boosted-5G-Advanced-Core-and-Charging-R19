#include "ss7_core/sctp_socket.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <netinet/sctp.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

#include "ss7_core/m3ua_dictionary.hpp"

namespace ss7_core {

namespace {

// Matches ngap_core's own real receive-buffer sizing precedent (same file header's own
// disclosure) -- M3UA DATA messages carrying a real SCCP UDT are comfortably under this.
constexpr std::size_t kReceiveBufferSize = 8192;

[[noreturn]] void throw_errno(const char* what) {
    throw std::runtime_error(std::string(what) + ": " + std::strerror(errno));
}

} // namespace

SctpSocket::SctpSocket() {
    fd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP);
    if (fd_ < 0) {
        throw_errno("socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP) failed");
    }

    const int reuse = 1;
    if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        const int saved_errno = errno;
        ::close(fd_);
        errno = saved_errno;
        throw_errno("setsockopt(SO_REUSEADDR) failed");
    }

    sctp_initmsg init{};
    init.sinit_num_ostreams = 16;
    init.sinit_max_instreams = 16;
    init.sinit_max_attempts = 4;
    if (::setsockopt(fd_, IPPROTO_SCTP, SCTP_INITMSG, &init, sizeof(init)) < 0) {
        const int saved_errno = errno;
        ::close(fd_);
        errno = saved_errno;
        throw_errno("setsockopt(SCTP_INITMSG) failed");
    }
}

SctpSocket::SctpSocket(int fd) : fd_(fd) {}

SctpSocket::~SctpSocket() {
    close_if_open();
}

SctpSocket::SctpSocket(SctpSocket&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

SctpSocket& SctpSocket::operator=(SctpSocket&& other) noexcept {
    if (this != &other) {
        close_if_open();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

void SctpSocket::close_if_open() {
    if (fd_ >= 0) {
        // ADR-0353: shutdown(2) before close(2), so a thread blocked in accept() or recv() on this
        // socket actually wakes.
        //
        // Every SCTP listener in this project -- AMF's NGAP task, UDM's MAP server, CHF's CAP
        // server -- runs a dedicated thread in a blocking accept, and every one of their
        // destructors closes the socket and then joins that thread. close() alone does not wake a
        // blocked accept on Linux, so the join never returns. That cost nothing while no NF ever
        // exited cleanly; the moment SIGTERM became a clean shutdown (same ADR) it hung AMF, UDM
        // and CHF, and hung 19 integration tests with them, because the spawn harness reaps with a
        // blocking waitpid.
        //
        // Done HERE rather than at each call site deliberately: a wake-before-close that callers
        // have to remember is one every future listener will forget. On an already-connected
        // socket this is also the correct teardown -- it starts SCTP's own SHUTDOWN exchange
        // instead of aborting the association. Errors are ignored: the peer may already be gone,
        // and there is nothing useful to do about it while closing.
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        fd_ = -1;
    }
}

void SctpSocket::close() {
    close_if_open();
}

void SctpSocket::bind_and_listen(const std::string& address, std::uint16_t port, int backlog) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1) {
        throw std::runtime_error("invalid IPv4 address for SCTP bind: " + address);
    }

    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        throw_errno(("bind(" + address + ":" + std::to_string(port) + ") failed").c_str());
    }
    if (::listen(fd_, backlog) < 0) {
        throw_errno("listen() failed");
    }
}

SctpSocket SctpSocket::accept() {
    const int client_fd = ::accept(fd_, nullptr, nullptr);
    if (client_fd < 0) {
        throw_errno("accept() failed");
    }
    return SctpSocket(client_fd);
}

void SctpSocket::connect(const std::string& address, std::uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1) {
        throw std::runtime_error("invalid IPv4 address for SCTP connect: " + address);
    }
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        throw_errno(("connect(" + address + ":" + std::to_string(port) + ") failed").c_str());
    }
}

void SctpSocket::send(const std::vector<std::uint8_t>& data, std::uint16_t stream) {
    const int result = ::sctp_sendmsg(
        fd_, data.data(), data.size(), nullptr, 0, htonl(dictionary::kSctpPpid), 0, stream, 0, 0);
    if (result < 0) {
        throw_errno("sctp_sendmsg() failed");
    }
}

std::vector<std::uint8_t> SctpSocket::receive() {
    std::vector<std::uint8_t> buffer(kReceiveBufferSize);
    sockaddr_in from{};
    sctp_sndrcvinfo info{};
    int flags = 0;
    auto from_len = static_cast<socklen_t>(sizeof(from));

    const int received = ::sctp_recvmsg(fd_,
                                        buffer.data(),
                                        buffer.size(),
                                        reinterpret_cast<sockaddr*>(&from),
                                        &from_len,
                                        &info,
                                        &flags);
    if (received < 0) {
        if (errno == ECONNRESET) {
            return {};
        }
        throw_errno("sctp_recvmsg() failed");
    }
    if (received == 0) {
        return {};
    }
    if (flags & MSG_NOTIFICATION) {
        return {};
    }
    if (!(flags & MSG_EOR)) {
        throw std::runtime_error("SCTP partial message received (not handled -- message exceeds "
                                 "the receive buffer)");
    }

    buffer.resize(static_cast<std::size_t>(received));
    return buffer;
}

} // namespace ss7_core
