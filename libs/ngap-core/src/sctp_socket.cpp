#include "ngap_core/sctp_socket.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <netinet/sctp.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace ngap_core {

namespace {

// 8192 bytes matches simulators/ransim/vendor/UERANSIM/src/lib/sctp/internal.cpp's own
// RECEIVE_BUFFER_SIZE -- real evidence from a working reference implementation on this exact
// system, not a guessed constant. NGAP PDUs in this build's scope (NG Setup, Initial UE Message,
// NAS Transport) are well under this.
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

    // Real NGAP deployments use multiple SCTP streams (stream 0 for non-UE-associated signaling,
    // dynamic per-UE streams otherwise -- see sctp_socket.hpp's disclosed simplification). This
    // build only ever sends/receives on stream 0, but the association itself is still negotiated
    // with room for more, matching a real peer's expectations during NG Setup.
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

SctpSocket::SctpSocket(SctpSocket&& other) noexcept : fd_(other.fd_.exchange(-1)) {}

SctpSocket& SctpSocket::operator=(SctpSocket&& other) noexcept {
    if (this != &other) {
        close_if_open();
        fd_ = other.fd_.exchange(-1);
    }
    return *this;
}

void SctpSocket::close_if_open() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
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

SctpSocket SctpSocket::accept() {
    const int client_fd = ::accept(fd_, nullptr, nullptr);
    if (client_fd < 0) {
        throw_errno("accept() failed");
    }
    return SctpSocket(client_fd);
}

std::optional<SctpSocket> SctpSocket::accept_or_timeout() {
    const int client_fd = ::accept(fd_, nullptr, nullptr);
    if (client_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return std::nullopt;
        }
        throw_errno("accept() failed");
    }
    return SctpSocket(client_fd);
}

void SctpSocket::shutdown_now() {
    const int fd = fd_.load();
    if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
    }
}

void SctpSocket::send(const std::vector<std::uint8_t>& data, std::uint16_t stream) {
    // sctp_sendmsg's exact parameter shape (0 for the unused destination-address/addrlen since
    // this is a connected one-to-one socket, htonl(ppid), context=0, timetolive=0) matches
    // simulators/ransim/vendor/UERANSIM/src/lib/sctp/internal.cpp's own SendMessage, a working
    // reference implementation on this exact system -- not guessed from the sctp_sendmsg man page
    // alone.
    const int result = ::sctp_sendmsg(
        fd_, data.data(), data.size(), nullptr, 0, htonl(kNgapPpid), 0, stream, 0, 0);
    if (result < 0) {
        throw_errno("sctp_sendmsg() failed");
    }
}

void SctpSocket::set_receive_timeout(std::chrono::milliseconds timeout) {
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(timeout.count() / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
    if (::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, static_cast<socklen_t>(sizeof(tv))) < 0) {
        throw_errno("setsockopt(SO_RCVTIMEO) failed");
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
        // SO_RCVTIMEO expiry -- reported as "nothing came back", the same answer a closed
        // association already gives, rather than as an error. Only reachable if a caller asked
        // for a deadline; the default is an indefinite block.
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return {};
        }
        throw_errno("sctp_recvmsg() failed");
    }
    if (received == 0) {
        return {};
    }
    if (flags & MSG_NOTIFICATION) {
        // SCTP association-lifecycle notifications (only relevant for one-to-many sockets in the
        // general case, but the kernel can still deliver e.g. a shutdown notification on a
        // one-to-one socket if notifications were subscribed to -- they weren't here, so this
        // path is defensive, not expected to trigger). Treated as "no application data this call"
        // rather than an error.
        return {};
    }
    if (!(flags & MSG_EOR)) {
        throw std::runtime_error("SCTP partial message received (not handled -- message exceeds "
                                 "the receive buffer)");
    }

    buffer.resize(static_cast<std::size_t>(received));
    return buffer;
}

} // namespace ngap_core
