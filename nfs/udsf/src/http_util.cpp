#include "http_util.hpp"

#include "sbi_core/uuid.hpp"

#include <openssl/evp.h>

#include <array>
#include <chrono>
#include <cstring>
#include <ctime>

namespace udsf {

namespace {

std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t");
    if (a == std::string::npos) {
        return "";
    }
    const auto b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

std::string opaque(const std::string& tag) {
    return tag.rfind("W/", 0) == 0 ? tag.substr(2) : tag;
}

} // namespace

std::string new_etag() {
    std::string id = sbi_core::generate_uuid_v4();
    std::string hex;
    for (char c : id) {
        if (c != '-') {
            hex.push_back(c);
        }
    }
    return "\"" + hex + "\"";
}

std::optional<EtagCondition> parse_etag_condition(const std::optional<std::string>& header) {
    if (!header) {
        return std::nullopt;
    }
    EtagCondition c;
    const std::string v = trim(*header);
    if (v == "*") {
        c.any = true;
        return c;
    }
    std::size_t pos = 0;
    while (pos <= v.size()) {
        const auto comma = v.find(',', pos);
        std::string item =
            trim(v.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
        if (!item.empty()) {
            c.tags.push_back(item);
        }
        if (comma == std::string::npos) {
            break;
        }
        pos = comma + 1;
    }
    return c;
}

bool if_match_passes(const EtagCondition& c, const std::optional<std::string>& current_etag) {
    if (!current_etag) {
        return false;
    }
    if (c.any) {
        return true;
    }
    for (const auto& t : c.tags) {
        if (t.rfind("W/", 0) == 0) {
            continue; // strong comparison: weak tags never match
        }
        if (t == *current_etag) {
            return true;
        }
    }
    return false;
}

bool if_none_match_passes(const EtagCondition& c, const std::optional<std::string>& current_etag) {
    if (!current_etag) {
        return true;
    }
    if (c.any) {
        return false;
    }
    for (const auto& t : c.tags) {
        if (opaque(t) == opaque(*current_etag)) {
            return false;
        }
    }
    return true;
}

std::string http_date(std::int64_t epoch_seconds) {
    const std::time_t t = static_cast<std::time_t>(epoch_seconds);
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::array<char, 64> buf{};
    std::strftime(buf.data(), buf.size(), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    return buf.data();
}

std::optional<std::int64_t> parse_http_date(const std::string& text) {
    std::tm tm{};
    const char* end = strptime(text.c_str(), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    if (end == nullptr || *end != '\0') {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(timegm(&tm));
}

std::string base64_encode(const std::string& bytes) {
    std::string out(4 * ((bytes.size() + 2) / 3), '\0');
    const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                  reinterpret_cast<const unsigned char*>(bytes.data()),
                                  static_cast<int>(bytes.size()));
    out.resize(static_cast<std::size_t>(n));
    return out;
}

std::optional<std::string> base64_decode(const std::string& text) {
    if (text.size() % 4 != 0) {
        return std::nullopt;
    }
    std::string out(3 * (text.size() / 4), '\0');
    const int n = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                  reinterpret_cast<const unsigned char*>(text.data()),
                                  static_cast<int>(text.size()));
    if (n < 0) {
        return std::nullopt;
    }
    std::size_t len = static_cast<std::size_t>(n);
    // EVP_DecodeBlock counts padding bytes as data; strip them.
    if (!text.empty() && text.back() == '=') {
        --len;
        if (text.size() >= 2 && text[text.size() - 2] == '=') {
            --len;
        }
    }
    out.resize(len);
    return out;
}

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace udsf
