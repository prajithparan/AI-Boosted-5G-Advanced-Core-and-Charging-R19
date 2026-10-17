#pragma once

#include "sbi_core/http2_client.hpp"

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tl/expected.hpp>

// OAuth2 client-credentials flow against NRF's Nnrf_AccessToken service
// (specs/5G_APIs-REL-19/TS29510_Nnrf_AccessToken.yaml, POST /oauth2/token). Request/response field
// names below are transcribed from that file's AccessTokenReq/AccessTokenRsp schemas, not invented.
//
// Fields modeled: grant_type, nfInstanceId, scope, targetNfType on the request (targetNfType added
// in Phase 2 -- NRF's JWT `aud` claim needs it, see AccessTokenClaims.aud in
// TS29510_Nnrf_AccessToken.yaml); access_token, token_type, expires_in on the response.
// AccessTokenReq has more optional fields (requesterPlmn, requesterSnssaiList, ...) that further
// NF-to-NF token requests will need; those are added when the NF procedures that populate them are
// implemented, per CLAUDE.md's "no speculative abstraction" rule.

namespace sbi_core {

struct OAuth2Token {
    std::string access_token;
    std::string token_type;
    std::chrono::steady_clock::time_point expires_at;
};

class OAuth2Client {
public:
    // nrf_token_endpoint e.g. "http://127.0.0.1:7777/oauth2/token"
    OAuth2Client(http2::Client& http_client,
                 std::string nrf_token_endpoint,
                 std::string nf_instance_id,
                 std::string scope,
                 std::string target_nf_type);

    // Returns a cached token if still valid (with a small safety margin), otherwise fetches a new
    // one synchronously.
    tl::expected<std::string, std::string> get_bearer_token();

private:
    tl::expected<OAuth2Token, std::string> fetch_token();

    http2::Client& http_client_;
    std::string nrf_token_endpoint_;
    std::string nf_instance_id_;
    std::string scope_;
    std::string target_nf_type_;

    std::mutex mutex_;
    std::optional<OAuth2Token> cached_token_;
};

// ADR-0499 phase 3: the consumer side. For a request URL, the token for its API: the scope and
// producer NF type (targetNfType) come from api_catalog.hpp; one OAuth2Client per (scope, NF
// type), each caching its token until expiry. std::nullopt -- send the request without a token --
// for an API the catalog gives no NF type or no scope, the NRF's own token endpoint, or a fetch the
// NRF refused (then not retried for `retry_after`). The tokenless fallback is what lets an NF
// register before it has a token: the NRF's NFManagement API admits requests without one
// (`security: - {}`), and TS 33.501 13.4.1.1.2 makes registration the pre-requisite of a token.
// Everywhere else the producer decides (ADR-0499 phase 1).
class TokenSource {
public:
    TokenSource(http2::Client& nrf_client,
                std::string nrf_token_endpoint,
                std::string nf_instance_id,
                std::chrono::seconds retry_after = std::chrono::seconds(5));

    std::optional<std::string> token_for(const std::string& url);

    // Forgets every refusal's back-off: call it once the NF is registered, so a token the NRF
    // refused before NFRegister (TS 33.501 13.4.1.1.2) is asked for again on the next request.
    void clear_backoff();

private:
    struct Entry {
        std::unique_ptr<OAuth2Client> client;
        std::chrono::steady_clock::time_point retry_at{};
    };

    http2::Client& nrf_client_;
    std::string nrf_token_endpoint_;
    std::string nf_instance_id_;
    std::chrono::seconds retry_after_;
    std::mutex mutex_; // guards entries_ only; never held across a token fetch
    std::map<std::pair<std::string, std::string>, Entry> entries_;
};

// Installs `source` on `client`: every request sent without an Authorization header gets the
// token for its API (http2::Client::set_token_source). `source` must outlive `client`'s use.
void attach_tokens(http2::Client& client, TokenSource& source);

// Makes a TokenSource the process-wide default (http2::Client::set_default_token_source), with its
// own NRF client presenting `tls` (the NF's client certificate): every Client of the process that
// has no source of its own then sends each API's token. Call it from the NRF lifecycle, before
// registering. Not for an NF that relays other NFs' requests (SCP, SEPP): a relayed request sent
// without a token would leave with the relay's -- those use attach_tokens on their own client.
void install_default_token_source(const http2::TlsConfig& tls,
                                  std::string nrf_token_endpoint,
                                  std::string nf_instance_id);

// TokenSource::clear_backoff on the source install_default_token_source installed (no-op if none).
void clear_default_token_backoff();

} // namespace sbi_core
