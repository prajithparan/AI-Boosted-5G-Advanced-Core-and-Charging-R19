#include "service.hpp"

#include "sbi_core/datetime.hpp"
#include "sbi_core/problem_details.hpp"

#include <algorithm>
#include <cctype>

#include "TS29598_Nudsf_DataRepository.hpp"

namespace udsf {

Response problem(int status, const std::string& title, const std::string& detail,
                 const std::optional<std::string>& cause) {
    Response r;
    r.status = status;
    r.headers.emplace("content-type", "application/problem+json");
    r.body = nlohmann::json(sbi_core::make_problem_details(status, title, detail, cause)).dump();
    return r;
}

Response extended_problem(int status, const std::string& title, const std::string& detail,
                          const std::string& cause, const nlohmann::json& stored_meta) {
    nlohmann::json j = sbi_core::make_problem_details(status, title, detail, cause);
    j["meta"] = stored_meta; // ProblemDetailsExtension -> Record{meta}
    Response r;
    r.status = status;
    r.headers.emplace("content-type", "application/problem+json");
    r.body = j.dump();
    return r;
}

std::optional<std::string> header(const Request& req, const std::string& name) {
    std::string key = name;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto it = req.headers.find(key);
    if (it == req.headers.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<std::string> query(const Request& req, const std::string& name) {
    const auto it = req.query_params.find(name);
    if (it == req.query_params.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool query_flag(const Request& req, const std::string& name) {
    const auto v = query(req, name);
    return v && *v == "true";
}

tl::expected<std::optional<std::int64_t>, std::string> query_uint(const Request& req,
                                                                   const std::string& name) {
    const auto v = query(req, name);
    if (!v) {
        return std::optional<std::int64_t>{};
    }
    if (v->empty() || !std::all_of(v->begin(), v->end(), [](unsigned char c) {
            return std::isdigit(c) != 0;
        }) || v->size() > 18) {
        return tl::unexpected(name + " shall be an unsigned integer (Uinteger)");
    }
    return std::optional<std::int64_t>(std::stoll(*v));
}

std::optional<Response> authorize(const Ctx& ctx, const Request& req, const std::string& scope,
                                  const std::string& api_uri) {
    const auto value = header(req, "authorization");
    if (!value) {
        if (!ctx.settings.oauth2_required) {
            return std::nullopt; // TS 29.500 6.7.3: accepted per local configuration
        }
        auto r = problem(401, "Unauthorized", "an OAuth2 access token is required");
        r.headers.emplace("www-authenticate", "Bearer realm=\"" + api_uri + "\"");
        return r;
    }
    constexpr std::string_view kPrefix = "Bearer ";
    sbi_core::jwt::VerifyResult v;
    if (value->size() > kPrefix.size() && value->compare(0, kPrefix.size(), kPrefix) == 0) {
        v = ctx.verifier.verify(value->substr(kPrefix.size()));
    } else {
        v.valid = false;
        v.error = "Authorization header is not a Bearer token";
    }
    if (!v.valid) {
        auto r = problem(401, "Unauthorized", v.error);
        r.headers.emplace("www-authenticate",
                          "Bearer realm=\"" + api_uri + "\", error=\"invalid_token\"");
        return r;
    }
    // The service-name scope grants the whole API (TS 29.500 6.7.3 first bullet; every operation
    // of both TS 29.598 YAMLs accepts it). Resource/operation scopes are additive, not required.
    std::vector<std::string> scopes;
    std::size_t pos = 0;
    while (pos <= v.scope.size()) {
        const auto sp = v.scope.find(' ', pos);
        scopes.push_back(v.scope.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos));
        if (sp == std::string::npos) {
            break;
        }
        pos = sp + 1;
    }
    if (std::find(scopes.begin(), scopes.end(), scope) == scopes.end()) {
        auto r = problem(403, "Forbidden", "access token scope does not include " + scope,
                         "INSUFFICIENT_SCOPES");
        r.headers.emplace("www-authenticate", "Bearer realm=\"" + api_uri +
                                                  "\", error=\"insufficient_scope\", scope=\"" +
                                                  scope + "\"");
        return r;
    }
    return std::nullopt;
}

tl::expected<StorageRef, Response> resolve_storage(const Ctx& ctx, const Request& req) {
    StorageRef s{req.path_params.at("realmId"), req.path_params.at("storageId")};
    const bool realm_known =
        std::any_of(ctx.settings.storages.begin(), ctx.settings.storages.end(),
                    [&](const auto& p) { return p.first == s.realm; });
    if (!realm_known) {
        return tl::unexpected(
            problem(404, "Not Found", "realm " + s.realm + " is not served", "REALM_NOT_FOUND"));
    }
    if (ctx.settings.storages.count({s.realm, s.storage}) == 0) {
        return tl::unexpected(problem(404, "Not Found",
                                      "storage " + s.storage + " is not served in realm " + s.realm,
                                      "STORAGE_NOT_FOUND"));
    }
    return s;
}

bool Conditions::write_passes(const std::optional<std::string>& etag) const {
    if (if_match && !if_match_passes(*if_match, etag)) {
        return false;
    }
    if (if_none_match && !if_none_match_passes(*if_none_match, etag)) {
        return false;
    }
    return true;
}

bool Conditions::not_modified(const std::optional<std::string>& etag, std::int64_t lm) const {
    if (if_none_match) {
        return !if_none_match_passes(*if_none_match, etag);
    }
    if (if_modified_since && etag) {
        return lm <= *if_modified_since;
    }
    return false;
}

Conditions conditions(const Request& req) {
    Conditions c;
    c.if_match = parse_etag_condition(header(req, "if-match"));
    c.if_none_match = parse_etag_condition(header(req, "if-none-match"));
    if (const auto ims = header(req, "if-modified-since")) {
        c.if_modified_since = parse_http_date(*ims);
    }
    return c;
}

void add_validators(const Ctx& ctx, Response& r, const std::string& etag, std::int64_t lm,
                    bool cacheable) {
    if (!etag.empty()) {
        r.headers.emplace("etag", etag);
    }
    if (lm > 0) {
        r.headers.emplace("last-modified", http_date(lm));
    }
    if (cacheable && ctx.settings.cache_max_age_seconds > 0) {
        r.headers.emplace("cache-control",
                          "max-age=" + std::to_string(ctx.settings.cache_max_age_seconds));
    }
}

Response not_modified(const Ctx& ctx, const std::string& etag, std::int64_t lm) {
    Response r;
    r.status = 304;
    add_validators(ctx, r, etag, lm, true);
    return r;
}

namespace {

std::vector<int> hex_nibbles(const std::string& hex) {
    std::vector<int> out; // least-significant nibble first
    for (auto it = hex.rbegin(); it != hex.rend(); ++it) {
        const int c = std::tolower(static_cast<unsigned char>(*it));
        if (c >= '0' && c <= '9') {
            out.push_back(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            out.push_back(c - 'a' + 10);
        } else {
            return {};
        }
    }
    return out;
}

} // namespace

std::optional<std::string> negotiate(const std::string& ours,
                                     const std::optional<std::string>& theirs) {
    if (!theirs) {
        return std::nullopt;
    }
    const auto a = hex_nibbles(ours);
    const auto b = hex_nibbles(*theirs);
    std::string out;
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
        out.insert(out.begin(), "0123456789ABCDEF"[a[i] & b[i]]);
    }
    while (out.size() > 1 && out.front() == '0') {
        out.erase(out.begin());
    }
    return out.empty() ? std::string("0") : out;
}

bool feature_negotiated(const std::optional<std::string>& theirs, int feature_number) {
    if (!theirs) {
        return false;
    }
    const auto n = hex_nibbles(*theirs);
    const auto idx = static_cast<std::size_t>((feature_number - 1) / 4);
    return idx < n.size() && ((n[idx] >> ((feature_number - 1) % 4)) & 1) != 0;
}

sbi_core::multipart::Encoded record_multipart(const Record& rec) {
    std::vector<sbi_core::multipart::Part> parts;
    parts.push_back({"application/json", std::string("meta"), rec.meta.dump()});
    for (const auto& b : rec.blocks) {
        sbi_core::multipart::Part p{b.content_type, b.id, b.data};
        p.content_transfer_encoding = b.cte;
        parts.push_back(std::move(p));
    }
    return sbi_core::multipart::encode_subtype("mixed", parts);
}

Response record_response(const Ctx& ctx, int status, const Record& rec, bool cacheable) {
    const auto enc = record_multipart(rec);
    Response r;
    r.status = status;
    r.headers.emplace("content-type", enc.content_type_header);
    r.body = enc.body;
    add_validators(ctx, r, rec.etag, rec.lm, cacheable);
    return r;
}

std::string validate_tag_map(const nlohmann::json& tags, const char* what) {
    if (!tags.is_object()) {
        return std::string(what) + " shall be a map of tag name to array of strings";
    }
    if (tags.empty()) {
        return std::string(what) + " shall have at least one tag (minProperties 1)";
    }
    for (auto it = tags.begin(); it != tags.end(); ++it) {
        if (!it->is_array() || it->empty()) {
            return std::string(what) + "." + it.key() + " shall be a non-empty array";
        }
        std::set<std::string> seen;
        for (const auto& v : *it) {
            if (!v.is_string()) {
                return std::string(what) + "." + it.key() + " items shall be strings";
            }
            seen.insert(v.get<std::string>());
        }
        if (seen.size() != it->size()) {
            return std::string(what) + "." + it.key() + " values shall be unique";
        }
    }
    return "";
}

std::string validate_record_meta(const nlohmann::json& meta) {
    if (!meta.is_object()) {
        return "RecordMeta shall be a JSON object";
    }
    try {
        (void)meta.get<sbi_gen::RecordMeta>();
    } catch (const std::exception& e) {
        return std::string("invalid RecordMeta: ") + e.what();
    }
    if (meta.contains("ttl")) {
        if (!meta["ttl"].is_string() || !sbi_core::parse_rfc3339(meta["ttl"].get<std::string>())) {
            return "RecordMeta.ttl shall be a DateTime (RFC 3339)";
        }
    }
    if (meta.contains("tags")) {
        return validate_tag_map(meta["tags"], "RecordMeta.tags");
    }
    return "";
}

} // namespace udsf
