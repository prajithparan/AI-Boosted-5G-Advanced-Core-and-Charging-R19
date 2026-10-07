// The IPv4 address the UPF advertises: in its NRF NFProfile (`ipv4Addresses`, which the SMF parses
// as a literal and sends PFCP to) and as its PFCP Node ID. ADR-0466.
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <string>

namespace upf {

using Ipv4 = std::array<std::uint8_t, 4>;

// `setting` is config/upf.json `advertised_ipv4` (env UPF_ADVERTISED_IPV4):
//   - a literal dotted-quad IPv4 (not 0.0.0.0): used as given;
//   - "auto": the local address the kernel would use to reach the NRF named in `nrf_base_url`
//     (UDP connect() + getsockname(); sends no packet). 127.0.0.1 on a host lab where the NRF is on
//     loopback, the container's own address in Compose/Kubernetes.
// Anything else, and any failure to find an address, is an error: the caller must not start with a
// guessed address, because a wrong one is silent (the SMF just never gets an N4 association).
std::expected<Ipv4, std::string> resolve_advertised_ipv4(const std::string& setting,
                                                         const std::string& nrf_base_url);

std::string to_string(const Ipv4& ip);

} // namespace upf
