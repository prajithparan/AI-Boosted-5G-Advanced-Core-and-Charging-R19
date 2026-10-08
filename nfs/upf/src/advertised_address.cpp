#include "advertised_address.hpp"

#include <arpa/inet.h>
#include <cstring>
#include <memory>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

namespace upf {

namespace {

struct FdCloser {
    void operator()(const int* fd) const {
        if (fd != nullptr && *fd >= 0) {
            ::close(*fd);
        }
        delete fd;
    }
};
using Fd = std::unique_ptr<int, FdCloser>;

struct AddrInfoFree {
    void operator()(addrinfo* p) const { ::freeaddrinfo(p); }
};

tl::expected<Ipv4, std::string> parse_literal(const std::string& s) {
    in_addr a{};
    // inet_pton accepts exactly four decimal octets and nothing else (no "1.2.3", no hex, no
    // names).
    if (s.empty() || ::inet_pton(AF_INET, s.c_str(), &a) != 1) {
        return tl::unexpected("advertised_ipv4 '" + s +
                              "' is not a literal IPv4 address or \"auto\" (the SMF parses the "
                              "NRF-registered address as a literal, so a hostname cannot work)");
    }
    Ipv4 ip{};
    std::memcpy(ip.data(), &a.s_addr, 4);
    if (ip == Ipv4{0, 0, 0, 0}) {
        return tl::unexpected("advertised_ipv4 0.0.0.0 is not an address anything can send to");
    }
    return ip;
}

// "https://host[:port][/...]" -> {host, port}; port defaults to 443 for the connect() target only.
tl::expected<std::pair<std::string, std::string>, std::string> split_url(const std::string& url) {
    const auto scheme = url.find("://");
    if (scheme == std::string::npos) {
        return tl::unexpected("nrf_base_url '" + url + "' has no scheme://");
    }
    auto rest = url.substr(scheme + 3);
    rest = rest.substr(0, rest.find('/'));
    std::string host = rest;
    std::string port = "443";
    if (const auto colon = rest.rfind(':'); colon != std::string::npos) {
        host = rest.substr(0, colon);
        port = rest.substr(colon + 1);
    }
    if (host.empty() || port.empty()) {
        return tl::unexpected("nrf_base_url '" + url + "' has no host or an empty port");
    }
    return std::pair{host, port};
}

} // namespace

tl::expected<Ipv4, std::string> resolve_advertised_ipv4(const std::string& setting,
                                                        const std::string& nrf_base_url) {
    if (setting != "auto") {
        return parse_literal(setting);
    }
    const auto target = split_url(nrf_base_url);
    if (!target) {
        return tl::unexpected(target.error());
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* raw = nullptr;
    if (const int rc = ::getaddrinfo(target->first.c_str(), target->second.c_str(), &hints, &raw);
        rc != 0 || raw == nullptr) {
        return tl::unexpected("cannot resolve the NRF host '" + target->first +
                              "' to an IPv4 address: " + ::gai_strerror(rc));
    }
    const std::unique_ptr<addrinfo, AddrInfoFree> info(raw);

    Fd fd(new int(::socket(AF_INET, SOCK_DGRAM, 0)));
    if (*fd < 0) {
        return tl::unexpected(std::string("socket(): ") + std::strerror(errno));
    }
    // UDP connect() only selects a route and a source address; it transmits nothing.
    if (::connect(*fd, info->ai_addr, info->ai_addrlen) != 0) {
        return tl::unexpected("no route to the NRF host '" + target->first +
                              "', so no local address can be chosen: " + std::strerror(errno));
    }
    sockaddr_in local{};
    socklen_t len = sizeof(local);
    if (::getsockname(*fd, reinterpret_cast<sockaddr*>(&local), &len) != 0) {
        return tl::unexpected(std::string("getsockname(): ") + std::strerror(errno));
    }
    Ipv4 ip{};
    std::memcpy(ip.data(), &local.sin_addr.s_addr, 4);
    if (ip == Ipv4{0, 0, 0, 0}) {
        return tl::unexpected("the kernel chose 0.0.0.0 as the source address toward the NRF");
    }
    return ip;
}

std::string to_string(const Ipv4& ip) {
    return std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." + std::to_string(ip[2]) + "." +
           std::to_string(ip[3]);
}

} // namespace upf
