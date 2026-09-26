#include "util.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace oam_gui_bff {

std::string base64url(const std::string& raw) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((raw.size() * 4 + 2) / 3);
    std::size_t i = 0;
    const auto byte = [&raw](std::size_t k) { return static_cast<unsigned char>(raw[k]); };
    for (; i + 3 <= raw.size(); i += 3) {
        const unsigned v = (byte(i) << 16) | (byte(i + 1) << 8) | byte(i + 2);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
    }
    if (raw.size() - i == 1) {
        const unsigned v = byte(i) << 16;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
    } else if (raw.size() - i == 2) {
        const unsigned v = (byte(i) << 16) | (byte(i + 1) << 8);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
    }
    return out;
}

std::string random_token(std::size_t n) {
    std::string raw(n, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(raw.data()), static_cast<int>(n)) != 1) {
        throw std::runtime_error("oam-gui-bff: CSPRNG failure");
    }
    return base64url(raw);
}

namespace {
std::string sha256_raw(const std::string& data) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> md{};
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), md.data());
    return std::string(reinterpret_cast<const char*>(md.data()), md.size());
}
} // namespace

std::string sha256_hex(const std::string& data) {
    const auto raw = sha256_raw(data);
    std::string out;
    out.reserve(raw.size() * 2);
    for (const unsigned char c : raw) {
        std::array<char, 3> b{};
        std::snprintf(b.data(), b.size(), "%02x", c);
        out += b.data();
    }
    return out;
}

std::string pkce_challenge(const std::string& verifier) { return base64url(sha256_raw(verifier)); }

std::string url_encode(const std::string& s) {
    std::string out;
    for (const unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '.' || c == '_' || c == '~') {
            out += static_cast<char>(c);
        } else {
            std::array<char, 4> b{};
            std::snprintf(b.data(), b.size(), "%%%02X", c);
            out += b.data();
        }
    }
    return out;
}

std::string form_encode(const std::map<std::string, std::string>& fields) {
    std::string out;
    for (const auto& [k, v] : fields) {
        if (!out.empty()) out += '&';
        out += url_encode(k) + "=" + url_encode(v);
    }
    return out;
}

std::optional<std::string> cookie(const sbi_core::http2::Request& req, const std::string& name) {
    const auto [lo, hi] = req.headers.equal_range("cookie");
    for (auto it = lo; it != hi; ++it) {
        const std::string& line = it->second;
        std::size_t pos = 0;
        while (pos < line.size()) {
            auto end = line.find(';', pos);
            if (end == std::string::npos) end = line.size();
            std::string pair = line.substr(pos, end - pos);
            const auto first = pair.find_first_not_of(' ');
            pair = first == std::string::npos ? std::string() : pair.substr(first);
            const auto eq = pair.find('=');
            if (eq != std::string::npos && pair.substr(0, eq) == name) {
                return pair.substr(eq + 1);
            }
            pos = end + 1;
        }
    }
    return std::nullopt;
}

std::string header(const sbi_core::http2::Request& req, const std::string& name) {
    const auto it = req.headers.find(name);
    return it == req.headers.end() ? std::string() : it->second;
}

std::optional<std::string> query(const sbi_core::http2::Request& req, const std::string& name) {
    const auto it = req.query_params.find(name);
    if (it == req.query_params.end()) return std::nullopt;
    return it->second;
}

} // namespace oam_gui_bff
