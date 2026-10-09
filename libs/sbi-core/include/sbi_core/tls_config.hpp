#pragma once

#include <cstdlib>
#include <stdexcept>
#include <string>

// Shared by http2_server.hpp and http2_client.hpp: every NF is both an SBI server and an SBI
// client (see CLAUDE.md's non-negotiable rules), and in both roles it needs the same three things
// -- its own cert+key to authenticate itself, and a CA bundle to verify the peer -- so this is one
// type, not two near-identical ones.

namespace sbi_core::http2 {

// Paths to this NF's own certificate + private key (PEM), and the CA bundle used to verify the
// peer's certificate during mTLS. All three are required; see scripts/gen-lab-pki.sh for how to
// generate a lab set. Required (not defaulted/optional) specifically so a caller cannot
// accidentally end up with an unauthenticated or unencrypted connection by omission.
struct TlsConfig {
    std::string cert_path;
    std::string key_path;
    std::string ca_path;
};

// ADR-0478. The process-wide TLS version floor. Default (variable unset) is TLS 1.3 only, exactly
// as before. "1.2" is an explicit operator opt-in (config key `tls_min_version` -> nf_config::load
// -> SBI_TLS_MIN_VERSION) for peers that only offer TLS 1.2 (TS 33.210 6.2.1 "shall support").
// Anything else throws: a typo must not silently widen or narrow the floor.
enum class MinTlsVersion { v1_3, v1_2 };

inline MinTlsVersion min_tls_version() {
    const char* v = std::getenv("SBI_TLS_MIN_VERSION");
    if (v == nullptr || std::string(v).empty() || std::string(v) == "1.3") {
        return MinTlsVersion::v1_3;
    }
    if (std::string(v) == "1.2") {
        return MinTlsVersion::v1_2;
    }
    throw std::runtime_error(
        std::string("sbi-core: tls_min_version must be \"1.3\" or \"1.2\", got \"") + v + "\"");
}

// TLS 1.2 cipher suites (TLS 1.3 suites are unaffected): ECDHE key exchange with AEAD only -- no
// RSA key transport, no CBC, no static DH.
inline constexpr const char* kTls12CipherList = "ECDHE+AESGCM:ECDHE+CHACHA20";

} // namespace sbi_core::http2
