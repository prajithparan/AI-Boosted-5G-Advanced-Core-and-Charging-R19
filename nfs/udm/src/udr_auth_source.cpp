#include "udr_auth_source.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <aka_crypto/hex.hpp>

namespace udm {

using nlohmann::json;

namespace {

// Nudr_DataRepository API root (TS29505_Subscription_Data.yaml is served under nudr-dr/v2).
constexpr const char* kUdrApiRoot = "/nudr-dr/v2";
// Bounded CAS retries: contention only comes from concurrent vector requests for one SUPI.
constexpr int kMaxCasAttempts = 8;

std::uint64_t sqn_to_u64(const aka_crypto::Sqn& sqn) {
    std::uint64_t v = 0;
    for (auto b : sqn) {
        v = (v << 8) | b;
    }
    return v;
}

aka_crypto::Sqn u64_to_sqn(std::uint64_t v) {
    aka_crypto::Sqn sqn{};
    for (size_t i = sqn.size(); i-- > 0;) {
        sqn[i] = static_cast<std::uint8_t>(v & 0xFF);
        v >>= 8;
    }
    return sqn;
}

std::string str_or_empty(const json& doc, const char* key) {
    auto it = doc.find(key);
    return (it != doc.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

} // namespace

aka_crypto::Sqn sqn_add(const aka_crypto::Sqn& sqn, std::uint64_t n) {
    return u64_to_sqn((sqn_to_u64(sqn) + n) & 0xFFFFFFFFFFFFULL); // mod 2^48
}

UdrAuthSubscriptionSource::UdrAuthSubscriptionSource(sbi_core::http2::Client& client,
                                                     sbi_core::OAuth2Client& oauth,
                                                     std::string udr_base_url)
    : client_(client), oauth_(oauth), udr_base_url_(std::move(udr_base_url)) {}

std::string UdrAuthSubscriptionSource::resource_url(const std::string& supi) const {
    return udr_base_url_ + kUdrApiRoot + "/subscription-data/" + supi +
           "/authentication-data/authentication-subscription";
}

tl::expected<UdrAuthSubscriptionSource::Fetched, AuthDataError>
UdrAuthSubscriptionSource::fetch(const std::string& supi) {
    auto token = oauth_.get_bearer_token();
    if (!token.has_value()) {
        spdlog::warn("udm: OAuth2 token for UDR QueryAuthSubsData failed: {}", token.error());
        return tl::unexpected(AuthDataError::Unavailable);
    }
    sbi_core::http2::ClientRequest req;
    req.method = "GET";
    req.url = resource_url(supi);
    req.headers.emplace("authorization", "Bearer " + *token);
    auto resp = client_.send(req);
    if (!resp.has_value()) {
        spdlog::warn("udm: UDR QueryAuthSubsData unreachable: {}", resp.error());
        return tl::unexpected(AuthDataError::Unavailable);
    }
    if (resp->status == 404) {
        return tl::unexpected(AuthDataError::NotFound);
    }
    if (resp->status != 200) {
        // The body is not logged: a UDR error body is harmless, but a 2xx-ish one is not.
        spdlog::warn("udm: UDR QueryAuthSubsData returned HTTP {}", resp->status);
        return tl::unexpected(AuthDataError::Unavailable);
    }
    json doc;
    try {
        doc = json::parse(resp->body);
    } catch (const json::parse_error&) {
        return tl::unexpected(AuthDataError::Invalid);
    }

    // AuthenticationSubscription (TS29505_Subscription_Data.yaml). The AKA path needs K and OPc
    // (encPermanentKey/encOpcKey -- clear hex in this lab, ADR-0382), the AMF field and the SQN.
    const auto k = aka_crypto::from_hex<16>(str_or_empty(doc, "encPermanentKey"));
    const auto opc = aka_crypto::from_hex<16>(str_or_empty(doc, "encOpcKey"));
    const auto amf = aka_crypto::from_hex<2>(str_or_empty(doc, "authenticationManagementField"));
    const std::string method = str_or_empty(doc, "authenticationMethod");
    std::string sqn_text;
    if (auto sn = doc.find("sequenceNumber"); sn != doc.end() && sn->is_object()) {
        sqn_text = str_or_empty(*sn, "sqn");
    }
    const auto sqn = aka_crypto::from_hex<6>(sqn_text);
    if (!k || !opc || !amf || !sqn || method.empty()) {
        spdlog::error("udm: UDR authentication-subscription for {} is missing or has malformed "
                      "AKA material (K/OPc/AMF/SQN/method)",
                      supi);
        return tl::unexpected(AuthDataError::Invalid);
    }
    return Fetched{
        AuthenticationSubscription{
            .k = *k, .opc = *opc, .sqn = *sqn, .amf = *amf, .authentication_method = method},
        sqn_text};
}

tl::expected<bool, AuthDataError> UdrAuthSubscriptionSource::compare_and_swap_sqn(
    const std::string& supi, const std::string& expected_sqn, const aka_crypto::Sqn& new_sqn) {
    auto token = oauth_.get_bearer_token();
    if (!token.has_value()) {
        return tl::unexpected(AuthDataError::Unavailable);
    }
    sbi_core::http2::ClientRequest req;
    req.method = "PATCH";
    req.url = resource_url(supi);
    req.headers.emplace("authorization", "Bearer " + *token);
    req.headers.emplace("content-type", "application/json-patch+json");
    req.body =
        json::array(
            {
                json{{"op", "test"}, {"path", "/sequenceNumber/sqn"}, {"value", expected_sqn}},
                json{{"op", "replace"},
                     {"path", "/sequenceNumber/sqn"},
                     {"value", aka_crypto::to_hex(new_sqn)}},
            })
            .dump();
    auto resp = client_.send(req);
    if (!resp.has_value()) {
        return tl::unexpected(AuthDataError::Unavailable);
    }
    // ModifyAuthenticationSubscription: 200 (PatchResult/document) or 204 on success. A failed
    // `test` op makes the UDR reject the whole patch with 400 -- the only 400 this well-formed,
    // two-op patch can produce -- so it means "lost the race".
    if (resp->status == 200 || resp->status == 204) {
        return true;
    }
    if (resp->status == 400) {
        return false;
    }
    spdlog::warn("udm: UDR ModifyAuthenticationSubscription returned HTTP {}", resp->status);
    return tl::unexpected(AuthDataError::Unavailable);
}

tl::expected<AuthenticationSubscription, AuthDataError>
UdrAuthSubscriptionSource::get_and_advance_sqn(const std::string& supi, std::uint64_t count) {
    for (int attempt = 0; attempt < kMaxCasAttempts; ++attempt) {
        auto fetched = fetch(supi);
        if (!fetched.has_value()) {
            return tl::unexpected(fetched.error());
        }
        auto swapped =
            compare_and_swap_sqn(supi, fetched->sqn_text, sqn_add(fetched->sub.sqn, count));
        if (!swapped.has_value()) {
            return tl::unexpected(swapped.error());
        }
        if (*swapped) {
            return fetched->sub; // carries the pre-advance SQN: the first vector's
        }
    }
    spdlog::warn("udm: SQN compare-and-swap for {} lost {} times in a row", supi, kMaxCasAttempts);
    return tl::unexpected(AuthDataError::Unavailable);
}

tl::expected<bool, AuthDataError> UdrAuthSubscriptionSource::resync_sqn(
    const std::string& supi, const aka_crypto::Key128& rand, const aka_crypto::Auts& auts) {
    for (int attempt = 0; attempt < kMaxCasAttempts; ++attempt) {
        auto fetched = fetch(supi);
        if (!fetched.has_value()) {
            return tl::unexpected(fetched.error());
        }
        const auto sqn_ms =
            aka_crypto::verify_and_decode_auts(fetched->sub.opc, fetched->sub.k, rand, auts);
        if (!sqn_ms.has_value()) {
            return false; // failed verification never moves the stored SQN
        }
        // SQN_MS + 2^16, not +1 -- TS 33.102 Annex C.3 IND/SEQ split; rationale in ADR-0037.
        auto swapped = compare_and_swap_sqn(supi, fetched->sqn_text, sqn_add(*sqn_ms, 0x10000));
        if (!swapped.has_value()) {
            return tl::unexpected(swapped.error());
        }
        if (*swapped) {
            return true;
        }
    }
    return tl::unexpected(AuthDataError::Unavailable);
}

} // namespace udm
