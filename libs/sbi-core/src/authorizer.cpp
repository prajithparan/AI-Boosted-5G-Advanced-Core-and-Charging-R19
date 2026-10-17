#include "sbi_core/authorizer.hpp"

#include "sbi_core/api_catalog.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sbi_core::http2 {
namespace {

std::vector<std::string> path_segments(const std::string& path) {
    std::vector<std::string> out;
    const auto end = path.find('?');
    std::size_t i = 0;
    const std::string_view p(path.data(), end == std::string::npos ? path.size() : end);
    while (i < p.size()) {
        const auto next = p.find('/', i);
        const auto stop = next == std::string_view::npos ? p.size() : next;
        if (stop > i) {
            out.emplace_back(p.substr(i, stop - i));
        }
        i = stop + 1;
    }
    return out;
}

// RFC 9110 11.6.1 / TS 29.500 6.7.3: the realm is the URI of the API ({apiRoot}/<apiName>/<ver>).
std::string realm_of(const Request& req) {
    const auto seg = path_segments(req.path);
    std::string uri = "https://" + req.authority;
    for (std::size_t i = 0; i < seg.size() && i < 2; ++i) {
        uri += "/" + seg[i];
    }
    return uri;
}

// RFC 6750 3: auth-param values are quoted-strings. Only the realm comes from the request (its
// :authority and path segments); a '"', '\\' or control character in it is dropped.
Response challenge(int status,
                   const Request& req,
                   std::string_view error,
                   std::string_view scope,
                   std::string detail,
                   std::optional<std::string> cause = std::nullopt,
                   std::vector<std::string> invalid_claims = {}) {
    std::string realm = realm_of(req);
    std::erase_if(realm, [](char c) {
        return c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20;
    });
    std::string value = "Bearer realm=\"" + realm + "\"";
    if (!error.empty()) {
        value += ", error=\"" + std::string(error) + "\"";
    }
    if (!scope.empty()) {
        value += ", scope=\"" + std::string(scope) + "\"";
    }
    nlohmann::json body{{"status", status},
                        {"title", status == 401 ? "Unauthorized" : "Forbidden"},
                        {"detail", std::move(detail)}};
    if (cause) {
        body["cause"] = *cause;
    }
    if (!invalid_claims.empty()) {
        auto& params = body["invalidParams"] = nlohmann::json::array();
        for (auto& c : invalid_claims) {
            params.push_back({{"param", std::move(c)}});
        }
    }
    Response resp;
    resp.status = status;
    resp.headers.emplace("www-authenticate", std::move(value));
    resp.headers.emplace("content-type", "application/problem+json");
    resp.body = body.dump();
    return resp;
}

bool has_scope(const std::string& scopes, const std::string& wanted) {
    std::size_t i = 0;
    while (i <= scopes.size()) {
        const auto next = scopes.find(' ', i);
        const auto stop = next == std::string::npos ? scopes.size() : next;
        if (std::string_view(scopes).substr(i, stop - i) == wanted) {
            return true;
        }
        i = stop + 1;
    }
    return false;
}

} // namespace

Authorizer::Authorizer(AuthorizerConfig config, std::shared_ptr<jwt::Verifier> verifier)
    : config_(std::move(config)), verifier_(std::move(verifier)) {}

std::string Authorizer::required_scope(const std::string& path) {
    const auto name = api_catalog::api_name_of(path);
    if (name.empty()) {
        return {};
    }
    const auto info = api_catalog::find(name);
    return info ? std::string(info->scope) : name;
}

std::optional<Response> Authorizer::check(const Request& req) const {
    const auto header = req.headers.find("authorization");
    if (header == req.headers.end()) {
        if (!config_.required) {
            return std::nullopt;
        }
        return challenge(401, req, "", "", "the request carries no OAuth2 access token");
    }
    // RFC 6750 2.1 / RFC 9110 11.1: the auth-scheme is case-insensitive.
    constexpr std::string_view kScheme = "bearer ";
    const std::string& value = header->second;
    const bool bearer =
        value.size() > kScheme.size() &&
        std::equal(kScheme.begin(), kScheme.end(), value.begin(), [](char a, char b) {
            return a == std::tolower(static_cast<unsigned char>(b));
        });
    if (!bearer || verifier_ == nullptr) {
        return challenge(
            401, req, "invalid_token", "", "the Authorization header is not a Bearer token");
    }
    const auto result = verifier_->verify(value.substr(kScheme.size()));
    if (!result.valid) {
        return challenge(401, req, "invalid_token", "", "the access token failed verification");
    }
    if (!result.missing_claims.empty()) {
        return challenge(401,
                         req,
                         "invalid_token",
                         "",
                         "the access token lacks a required claim",
                         "ACCESS_TOKEN_CLAIM_MISSING",
                         result.missing_claims);
    }
    const bool for_me = std::ranges::any_of(result.audiences, [&](const std::string& a) {
        return (!config_.nf_type.empty() && a == config_.nf_type) ||
               (!config_.nf_instance_id.empty() && a == config_.nf_instance_id);
    });
    if (!for_me) {
        return challenge(
            401, req, "invalid_token", "", "the access token is not for this NF (aud)");
    }
    const auto scope = required_scope(req.path);
    if (!scope.empty() && !has_scope(result.scope, scope)) {
        return challenge(403,
                         req,
                         "insufficient_scope",
                         scope,
                         "the access token's scope does not cover this API");
    }
    return std::nullopt;
}

} // namespace sbi_core::http2
