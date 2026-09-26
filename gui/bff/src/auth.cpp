#include "auth.hpp"

#include "util.hpp"

#include <jwt-cpp/traits/nlohmann-json/traits.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <utility>

namespace oam_gui_bff {

using nlohmann::json;
using Traits = jwt::traits::nlohmann_json;

OidcAuthenticator::OidcAuthenticator(OidcConfig config, IamStore& iam,
                                     sbi_core::http2::TlsConfig idp_tls)
    : config_(std::move(config)), iam_(iam), idp_(std::move(idp_tls)) {}

std::string OidcAuthenticator::begin(const std::string& browser_binding,
                                     const std::string& terminal_cn) {
    const std::string state = random_token(32);
    const std::string nonce = random_token(32);
    const std::string verifier = random_token(48); // 64 chars, RFC 7636 §4.1: 43..128
    iam_.put_login_state(state, nonce, verifier, browser_binding, terminal_cn);
    std::map<std::string, std::string> q{
        {"response_type", "code"},
        {"client_id", config_.client_id},
        {"redirect_uri", config_.redirect_uri},
        {"scope", config_.scope},
        {"state", state},
        {"nonce", nonce},
        {"code_challenge", pkce_challenge(verifier)},
        {"code_challenge_method", "S256"},
    };
    if (!config_.acr_values.empty()) q["acr_values"] = config_.acr_values;
    return config_.authorization_endpoint + "?" + form_encode(q);
}

std::string OidcAuthenticator::logout_url() const {
    if (config_.end_session_endpoint.empty()) return {};
    std::map<std::string, std::string> q{{"client_id", config_.client_id}};
    if (!config_.post_logout_redirect_uri.empty()) {
        q["post_logout_redirect_uri"] = config_.post_logout_redirect_uri;
    }
    return config_.end_session_endpoint + "?" + form_encode(q);
}

bool OidcAuthenticator::fetch_jwks() {
    auto r = idp_.send({"GET", config_.jwks_uri, {{"accept", "application/json"}}, ""});
    if (!r.has_value() || r->status != 200) {
        spdlog::warn("oam-gui-bff: JWKS fetch failed ({})",
                     r.has_value() ? std::to_string(r->status) : r.error());
        return false;
    }
    try {
        auto j = json::parse(r->body);
        if (!j.contains("keys") || !j["keys"].is_array()) return false;
        jwks_ = std::move(j);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::optional<std::pair<std::string, std::string>>
OidcAuthenticator::key_for(const std::string& kid, const std::string& alg) {
    std::lock_guard lock(jwks_mutex_);
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt == 1 || jwks_.is_null()) {
            if (!fetch_jwks()) return std::nullopt;
        }
        for (const auto& k : jwks_["keys"]) {
            if (k.value("kid", "") != kid) continue;
            if (k.contains("use") && k["use"] != "sig") continue;
            const std::string kty = k.value("kty", "");
            try {
                if (kty == "RSA" && alg == "RS256") {
                    return std::make_pair(alg, jwt::helper::create_public_key_from_rsa_components(
                                                   k.at("n").get<std::string>(),
                                                   k.at("e").get<std::string>()));
                }
                if (kty == "EC" && alg == "ES256" && k.value("crv", "") == "P-256") {
                    return std::make_pair(alg, jwt::helper::create_public_key_from_ec_components(
                                                   "P-256", k.at("x").get<std::string>(),
                                                   k.at("y").get<std::string>()));
                }
            } catch (const std::exception&) {
                return std::nullopt;
            }
        }
    }
    return std::nullopt;
}

LoginOutcome OidcAuthenticator::complete(const std::string& code, const std::string& state,
                                         const std::string& browser_binding) {
    LoginOutcome out;
    const auto ls = iam_.take_login_state(state, browser_binding);
    if (!ls) {
        out.reason = "unknown, expired, replayed or foreign-browser login state";
        return out;
    }
    // Code for tokens (confidential client, client_secret_post, PKCE verifier).
    auto tr = idp_.send({"POST",
                         config_.token_endpoint,
                         {{"content-type", "application/x-www-form-urlencoded"},
                          {"accept", "application/json"}},
                         form_encode({{"grant_type", "authorization_code"},
                                      {"code", code},
                                      {"redirect_uri", config_.redirect_uri},
                                      {"client_id", config_.client_id},
                                      {"client_secret", config_.client_secret},
                                      {"code_verifier", ls->code_verifier}})});
    if (!tr.has_value() || tr->status != 200) {
        out.reason = "token endpoint refused the code (" +
                     (tr.has_value() ? std::to_string(tr->status) : std::string("unreachable")) +
                     ")";
        return out;
    }
    std::string id_token;
    try {
        id_token = json::parse(tr->body).at("id_token").get<std::string>();
    } catch (const std::exception&) {
        out.reason = "token response carries no id_token";
        return out;
    }
    try {
        const auto decoded = jwt::decode<Traits>(id_token);
        const std::string alg = decoded.get_algorithm();
        if (!decoded.has_key_id()) {
            out.reason = "id_token has no kid";
            return out;
        }
        const auto key = key_for(decoded.get_key_id(), alg);
        if (!key) {
            out.reason = "id_token signed with an unknown key or a refused algorithm (" + alg + ")";
            return out;
        }
        auto verifier = jwt::verify<Traits>()
                            .with_issuer(config_.issuer)
                            .with_audience(config_.client_id)
                            .leeway(static_cast<std::size_t>(config_.leeway_seconds));
        if (key->first == "RS256") {
            verifier.allow_algorithm(jwt::algorithm::rs256(key->second, "", "", ""));
        } else {
            verifier.allow_algorithm(jwt::algorithm::es256(key->second, "", "", ""));
        }
        verifier.verify(decoded); // signature, iss, aud, exp/nbf/iat
        if (!decoded.has_expires_at()) {
            out.reason = "id_token has no exp";
            return out;
        }
        const auto payload = json::parse(decoded.get_payload());
        if (payload.value("nonce", "") != ls->nonce) {
            out.reason = "id_token nonce mismatch";
            return out;
        }
        // Several audiences: the authorized party must be us (OIDC Core §3.1.3.7 item 5).
        if (payload.contains("aud") && payload["aud"].is_array() && payload["aud"].size() > 1 &&
            payload.value("azp", "") != config_.client_id) {
            out.reason = "id_token azp is not this client";
            return out;
        }
        out.subject = payload.value("sub", "");
        out.idp_session_id = payload.value("sid", "");
        out.acr = payload.value("acr", "");
        if (payload.contains("amr") && payload["amr"].is_array()) {
            for (const auto& a : payload["amr"]) {
                if (a.is_string()) out.amr.push_back(a.get<std::string>());
            }
        }
    } catch (const std::exception& e) {
        out.reason = std::string("id_token rejected: ") + e.what();
        return out;
    }
    if (out.subject.empty()) {
        out.reason = "id_token has no sub";
        return out;
    }
    auto user = iam_.find_user(config_.issuer, out.subject);
    if (!user) {
        out.reason = "IdP subject is not an operator_iam user";
        return out;
    }
    out.user = user;
    if (user->status != "ACTIVE") {
        out.reason = "user status is " + user->status;
        return out;
    }
    if (user->dormant) {
        iam_.mark_dormant(user->id);
        out.reason = "account dormant (no login within the policy window); now locked";
        return out;
    }
    if (user->mfa_required) {
        const bool amr_ok = std::any_of(out.amr.begin(), out.amr.end(), [this](const auto& a) {
            return std::find(config_.mfa_amr.begin(), config_.mfa_amr.end(), a) !=
                   config_.mfa_amr.end();
        });
        const bool acr_ok = !out.acr.empty() && std::find(config_.mfa_acr.begin(),
                                                          config_.mfa_acr.end(),
                                                          out.acr) != config_.mfa_acr.end();
        if (!amr_ok && !acr_ok) {
            out.reason = "no MFA evidence in the id_token (amr/acr) for an mfa_required user";
            return out;
        }
    }
    out.ok = true;
    return out;
}

} // namespace oam_gui_bff
