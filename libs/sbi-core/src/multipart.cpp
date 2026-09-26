#include "sbi_core/multipart.hpp"

#include "sbi_core/uuid.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace sbi_core::multipart {

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string strip_quotes(std::string s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

std::string strip_angle_brackets(std::string s) {
    s = trim(s);
    if (s.size() >= 2 && s.front() == '<' && s.back() == '>') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

std::optional<std::string> extract_boundary(const std::string& content_type_header) {
    std::size_t pos = 0;
    while (pos <= content_type_header.size()) {
        const auto semi = content_type_header.find(';', pos);
        std::string segment =
            trim(semi == std::string::npos ? content_type_header.substr(pos)
                                           : content_type_header.substr(pos, semi - pos));
        constexpr std::string_view kKey = "boundary=";
        if (segment.size() > kKey.size() && to_lower(segment.substr(0, kKey.size())) == kKey) {
            return strip_quotes(segment.substr(kKey.size()));
        }
        if (semi == std::string::npos) {
            break;
        }
        pos = semi + 1;
    }
    return std::nullopt;
}

std::string media_type_of(const std::string& content_type_header) {
    const auto semi = content_type_header.find(';');
    return to_lower(trim(semi == std::string::npos ? content_type_header
                                                   : content_type_header.substr(0, semi)));
}

// The RFC 2046 framing shared by every multipart subtype. `what` only names the subtype in error
// text; the caller has already checked the media type.
tl::expected<std::vector<Part>, std::string> parse_framing(const std::string& content_type_header,
                                                           const std::string& body,
                                                           const std::string& what) {
    const auto boundary = extract_boundary(content_type_header);
    if (!boundary.has_value() || boundary->empty()) {
        return tl::unexpected(what + " content type missing boundary parameter");
    }

    // Wrapped in try/catch: this parses network-supplied, potentially-malformed input. Any
    // std::out_of_range from a bounds-check slip must become a returned error (-> a 400 to the
    // caller), never an uncaught exception that could take the server down.
    try {
        const std::string delimiter = "--" + *boundary;

        auto pos = body.find(delimiter);
        if (pos == std::string::npos) {
            return tl::unexpected("no boundary delimiter found in multipart body");
        }

        std::vector<Part> parts;
        while (true) {
            pos += delimiter.size();
            if (pos > body.size()) {
                return tl::unexpected("truncated multipart body (delimiter cut off)");
            }
            if (body.compare(pos, 2, "--") == 0) {
                break; // closing delimiter: "--boundary--"
            }
            if (body.compare(pos, 2, "\r\n") == 0) {
                pos += 2;
            } else if (pos < body.size() && body[pos] == '\n') {
                pos += 1;
            } else {
                return tl::unexpected(
                    "malformed multipart delimiter line (no CRLF/LF after boundary)");
            }

            Part part;
            while (true) {
                const auto line_end = body.find('\n', pos);
                if (line_end == std::string::npos) {
                    return tl::unexpected("unterminated multipart part headers");
                }
                std::string line = body.substr(pos, line_end - pos);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                pos = line_end + 1;
                if (line.empty()) {
                    break; // blank line -- end of this part's headers
                }
                const auto colon = line.find(':');
                if (colon == std::string::npos) {
                    continue; // ignore a malformed header line rather than fail the whole part
                }
                const std::string key = to_lower(trim(line.substr(0, colon)));
                const std::string value = trim(line.substr(colon + 1));
                if (key == "content-type") {
                    part.content_type = value;
                } else if (key == "content-id") {
                    part.content_id = strip_angle_brackets(value);
                } else if (key == "content-transfer-encoding") {
                    part.content_transfer_encoding = value;
                }
            }

            // A zero-length part body (RFC 2046 allows one): the delimiter starts right where the
            // body would, and the '\n' that precedes it is the blank line's own. Searching from
            // `pos` would skip it and swallow the next part.
            const auto next = (pos > 0 && body.compare(pos, delimiter.size(), delimiter) == 0)
                                  ? pos - 1
                                  : body.find("\n" + delimiter, pos);
            if (next == std::string::npos) {
                return tl::unexpected("unterminated multipart body part (no closing delimiter)");
            }
            std::string part_body = next < pos ? std::string() : body.substr(pos, next - pos);
            // `next` is the index of the '\n' that precedes the delimiter, deliberately excluded
            // from part_body by substr's exclusive end -- only a lone trailing '\r' (present with
            // CRLF line endings) can be left over, never '\n' itself.
            if (!part_body.empty() && part_body.back() == '\r') {
                part_body.pop_back();
            }
            part.body = std::move(part_body);
            parts.push_back(std::move(part));

            pos = next + 1; // start of the next delimiter's "--boundary"
        }

        if (parts.empty()) {
            return tl::unexpected("multipart body contained no parts");
        }
        return parts;
    } catch (const std::exception& e) {
        return tl::unexpected(std::string("malformed multipart body: ") + e.what());
    }
}

std::string encode_parts(const std::vector<Part>& parts, const std::string& boundary) {
    std::string body;
    for (const auto& part : parts) {
        body += "--" + boundary + "\r\n";
        body += "Content-Type: " + part.content_type + "\r\n";
        if (part.content_id.has_value()) {
            body += "Content-Id: " + *part.content_id + "\r\n";
        }
        if (part.content_transfer_encoding.has_value()) {
            body += "Content-Transfer-Encoding: " + *part.content_transfer_encoding + "\r\n";
        }
        body += "\r\n";
        body += part.body;
        body += "\r\n";
    }
    body += "--" + boundary + "--\r\n";
    return body;
}

} // namespace

bool is_multipart_related(const std::string& content_type_header) {
    return media_type_of(content_type_header) == "multipart/related";
}

bool is_multipart(const std::string& content_type_header) {
    return media_type_of(content_type_header).rfind("multipart/", 0) == 0;
}

tl::expected<std::vector<Part>, std::string> parse(const std::string& content_type_header,
                                                   const std::string& body) {
    if (!is_multipart_related(content_type_header)) {
        return tl::unexpected("not a multipart/related content type: " + content_type_header);
    }
    return parse_framing(content_type_header, body, "multipart/related");
}

tl::expected<std::vector<Part>, std::string> parse_any(const std::string& content_type_header,
                                                       const std::string& body) {
    if (!is_multipart(content_type_header)) {
        return tl::unexpected("not a multipart content type: " + content_type_header);
    }
    return parse_framing(content_type_header, body, media_type_of(content_type_header));
}

Encoded encode_subtype(const std::string& subtype, const std::vector<Part>& parts) {
    const std::string boundary = "5gc-r19-" + generate_uuid_v4();
    Encoded result;
    result.content_type_header = "multipart/" + subtype + "; boundary=\"" + boundary + "\"";
    result.body = encode_parts(parts, boundary);
    return result;
}

Encoded encode(const std::vector<Part>& parts) {
    const std::string boundary = "5gc-r19-" + generate_uuid_v4();
    std::string body = encode_parts(parts, boundary);

    const std::string root_type = parts.empty() ? "application/json" : parts.front().content_type;
    Encoded result;
    result.content_type_header =
        "multipart/related; boundary=\"" + boundary + "\"; type=\"" + root_type + "\"";
    result.body = std::move(body);
    return result;
}

} // namespace sbi_core::multipart
