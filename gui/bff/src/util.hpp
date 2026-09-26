#pragma once

// Small crypto / HTTP helpers for oam-gui-bff (ADR-0423/0424). OpenSSL only.

#include "sbi_core/http2_server.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <string>

namespace oam_gui_bff {

// `n` bytes from the OpenSSL CSPRNG, base64url without padding. Throws if the RNG fails.
std::string random_token(std::size_t n = 32);

std::string sha256_hex(const std::string& data);
std::string base64url(const std::string& raw);

// PKCE S256 challenge for a verifier (RFC 7636 §4.2).
std::string pkce_challenge(const std::string& verifier);

// application/x-www-form-urlencoded / query component encoding (RFC 3986 unreserved kept).
std::string url_encode(const std::string& s);
// Inverse of url_encode; a malformed escape is kept literally.
std::string url_decode(const std::string& s);
std::string form_encode(const std::map<std::string, std::string>& fields);

// A cookie's value from every `cookie` header (HTTP/2 may split cookies across several).
std::optional<std::string> cookie(const sbi_core::http2::Request& req, const std::string& name);

std::string header(const sbi_core::http2::Request& req, const std::string& name);
std::optional<std::string> query(const sbi_core::http2::Request& req, const std::string& name);

} // namespace oam_gui_bff
