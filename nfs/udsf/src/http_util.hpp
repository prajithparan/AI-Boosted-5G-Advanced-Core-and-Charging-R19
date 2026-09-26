#pragma once

// HTTP conditional-request plumbing for the UDSF (TS 29.598 6.1.2.2.3-6.1.2.2.9, RFC 9110
// clauses 8.8.2, 8.8.3, 13.1.1-13.1.3) plus small encoding helpers (ADR-0400).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace udsf {

// A fresh strong validator, quoted per RFC 9110 8.8.3 (e.g. "\"3f2a...\"").
std::string new_etag();

// Parsed If-Match / If-None-Match header: "*" or a list of entity tags.
struct EtagCondition {
    bool any = false;              // "*"
    std::vector<std::string> tags; // quoted as received; W/ prefixes kept
};
std::optional<EtagCondition> parse_etag_condition(const std::optional<std::string>& header);

// RFC 9110 13.1.1 If-Match: strong comparison; false if the resource does not exist.
bool if_match_passes(const EtagCondition& c, const std::optional<std::string>& current_etag);
// RFC 9110 13.1.2 If-None-Match (weak comparison); false if a listed tag matches or if "*" and
// the resource exists.
bool if_none_match_passes(const EtagCondition& c, const std::optional<std::string>& current_etag);

// IMF-fixdate (RFC 9110 5.6.7), e.g. "Sun, 06 Nov 1994 08:49:37 GMT".
std::string http_date(std::int64_t epoch_seconds);
std::optional<std::int64_t> parse_http_date(const std::string& text);

std::string base64_encode(const std::string& bytes);
std::optional<std::string> base64_decode(const std::string& text);

std::int64_t now_ms();

} // namespace udsf
