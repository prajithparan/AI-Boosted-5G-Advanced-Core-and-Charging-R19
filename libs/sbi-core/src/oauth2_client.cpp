#include "sbi_core/oauth2_client.hpp"

#include "sbi_core/api_catalog.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <iomanip>
#include <sstream>

namespace sbi_core {

namespace {

std::string form_urlencode(const std::string& value) {
    std::ostringstream out;
    for (const char raw_c : value) {
        const auto c = static_cast<unsigned char>(raw_c);
        if (std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~') {
            out << c;
        } else if (c == ' ') {
            out << '+';
        } else {
            out << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<int>(c);
        }
    }
    return out.str();
}

} // namespace

OAuth2Client::OAuth2Client(http2::Client& http_client,
                           std::string nrf_token_endpoint,
                           std::string nf_instance_id,
                           std::string scope,
                           std::string target_nf_type)
    : http_client_(http_client), nrf_token_endpoint_(std::move(nrf_token_endpoint)),
      nf_instance_id_(std::move(nf_instance_id)), scope_(std::move(scope)),
      target_nf_type_(std::move(target_nf_type)) {}

tl::expected<std::string, std::string> OAuth2Client::get_bearer_token() {
    std::lock_guard<std::mutex> lock(mutex_);

    constexpr auto kSafetyMargin = std::chrono::seconds(10);
    if (cached_token_.has_value() &&
        std::chrono::steady_clock::now() + kSafetyMargin < cached_token_->expires_at) {
        return cached_token_->access_token;
    }

    auto token = fetch_token();
    if (!token.has_value()) {
        return tl::unexpected(token.error());
    }
    cached_token_ = *token;
    return cached_token_->access_token;
}

tl::expected<OAuth2Token, std::string> OAuth2Client::fetch_token() {
    http2::ClientRequest req;
    req.method = "POST";
    req.url = nrf_token_endpoint_;
    req.headers.emplace("content-type", "application/x-www-form-urlencoded");

    std::ostringstream body;
    body << "grant_type=" << form_urlencode("client_credentials");
    body << "&nfInstanceId=" << form_urlencode(nf_instance_id_);
    body << "&scope=" << form_urlencode(scope_);
    body << "&targetNfType=" << form_urlencode(target_nf_type_);
    req.body = body.str();

    auto resp = http_client_.send(req);
    if (!resp.has_value()) {
        return tl::unexpected("oauth2 token request failed: " + resp.error());
    }
    if (resp->status != 200) {
        return tl::unexpected("oauth2 token request returned status " +
                              std::to_string(resp->status) + ": " + resp->body);
    }

    OAuth2Token token;
    try {
        const auto json = nlohmann::json::parse(resp->body);
        token.access_token = json.at("access_token").get<std::string>();
        token.token_type = json.at("token_type").get<std::string>();
        const auto expires_in =
            json.contains("expires_in") ? json.at("expires_in").get<long>() : 3600L;
        token.expires_at = std::chrono::steady_clock::now() + std::chrono::seconds(expires_in);
    } catch (const nlohmann::json::exception& e) {
        return tl::unexpected(std::string("failed to parse AccessTokenRsp: ") + e.what());
    }

    return token;
}

TokenSource::TokenSource(http2::Client& nrf_client,
                         std::string nrf_token_endpoint,
                         std::string nf_instance_id,
                         std::chrono::seconds retry_after)
    : nrf_client_(nrf_client), nrf_token_endpoint_(std::move(nrf_token_endpoint)),
      nf_instance_id_(std::move(nf_instance_id)), retry_after_(retry_after) {}

std::optional<std::string> TokenSource::token_for(const std::string& url) {
    const auto info = api_catalog::find(api_catalog::api_name_of(url));
    // No producer type, or no scope to ask for (AccessTokenReq.scope is mandatory): no token.
    if (!info || info->nf_type.empty() || info->scope.empty()) {
        return std::nullopt;
    }
    const std::pair<std::string, std::string> key{std::string(info->scope),
                                                  std::string(info->nf_type)};
    OAuth2Client* client = nullptr;
    {
        std::lock_guard lock(mutex_);
        auto& entry = entries_[key];
        if (std::chrono::steady_clock::now() < entry.retry_at) {
            return std::nullopt;
        }
        if (!entry.client) {
            entry.client = std::make_unique<OAuth2Client>(
                nrf_client_, nrf_token_endpoint_, nf_instance_id_, key.first, key.second);
        }
        client = entry.client.get(); // stable: entries are never erased
    }
    auto token = client->get_bearer_token();
    if (token.has_value()) {
        return *token;
    }
    spdlog::warn("sbi-core: no access token for {} ({}): {}; sending without one",
                 key.first,
                 key.second,
                 token.error());
    std::lock_guard lock(mutex_);
    entries_[key].retry_at = std::chrono::steady_clock::now() + retry_after_;
    return std::nullopt;
}

void TokenSource::clear_backoff() {
    std::lock_guard lock(mutex_);
    for (auto& [key, entry] : entries_) {
        entry.retry_at = {};
    }
}

void attach_tokens(http2::Client& client, TokenSource& source) {
    client.set_token_source([&source](const std::string& url) { return source.token_for(url); });
}

namespace {
std::mutex& default_source_mutex() {
    static std::mutex m;
    return m;
}
// The TokenSource install_default_token_source installed, for clear_default_token_backoff.
std::shared_ptr<TokenSource>& default_source() {
    static std::shared_ptr<TokenSource> source;
    return source;
}
} // namespace

void install_default_token_source(const http2::TlsConfig& tls,
                                  std::string nrf_token_endpoint,
                                  std::string nf_instance_id) {
    // Owned by the installed function, so a send() still holding the previous default keeps it
    // alive. The NRF client itself falls back to this source too; the token endpoint is not in
    // the API catalog, so its requests are sent without a token.
    struct State {
        State(const http2::TlsConfig& tls, std::string endpoint, std::string instance_id)
            : nrf_client(tls), source(nrf_client, std::move(endpoint), std::move(instance_id)) {}
        http2::Client nrf_client;
        TokenSource source;
    };
    auto state =
        std::make_shared<State>(tls, std::move(nrf_token_endpoint), std::move(nf_instance_id));
    {
        const std::lock_guard lock(default_source_mutex());
        default_source() = std::shared_ptr<TokenSource>(state, &state->source);
    }
    http2::Client::set_default_token_source(
        [state](const std::string& url) { return state->source.token_for(url); });
}

void clear_default_token_backoff() {
    std::shared_ptr<TokenSource> source;
    {
        const std::lock_guard lock(default_source_mutex());
        source = default_source();
    }
    if (source) {
        source->clear_backoff();
    }
}

} // namespace sbi_core
