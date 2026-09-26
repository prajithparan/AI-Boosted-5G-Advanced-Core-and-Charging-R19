#pragma once

// Shared request plumbing for the two UDSF services (ADR-0400): OAuth2 enforcement per TS 29.500
// 6.7.3, realm/storage resolution (TS 29.598 REALM_NOT_FOUND / STORAGE_NOT_FOUND), ProblemDetails
// and ExtendedProblemDetails builders, conditional-request helpers, and the multipart encoding of
// a Record (TS 29.598 6.1.2.4.2).

#include "sbi_core/http2_server.hpp"
#include "sbi_core/jwt.hpp"
#include "sbi_core/multipart.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tl/expected.hpp>
#include <utility>
#include <vector>

#include "http_util.hpp"
#include "store.hpp"

namespace udsf {

using sbi_core::http2::Request;
using sbi_core::http2::Response;

// servers[0].url of each YAML (ADR-0325; checked by tests/conformance/validate_api_roots.py).
inline constexpr const char* kDrRoot = "/nudsf-dr/v1";
inline constexpr const char* kTimerRoot = "/nudsf-timer/v1";

// Supported features (TS 29.598 tables 6.1.8-1 and 6.2.8-1), see ADR-0400 for each decision.
//   DR:    1 AdvancedQuery, 3 CombinedSearchRetrieve, 4 BulkOperations, 6 PartialRecordUpdate,
//          7 RecordDeletePartialSuccess  -> 0b1101101 = 0x6D.
//          NOT 2 Meta Schema (record<->schema link missing from the YAML), NOT 5 AdvancedCounting.
//   Timer: 1 PeriodicTimer, 3 TimerDeletePartialSuccess -> 0b101 = 0x5. NOT 2 Meta Schema.
inline constexpr const char* kDrFeatures = "6D";
inline constexpr const char* kTimerFeatures = "5";

struct Settings {
    std::set<std::pair<std::string, std::string>> storages; // (realmId, storageId)
    std::int64_t max_record_ttl_seconds = 0;                // 0 = no operator ceiling
    std::int64_t max_subscription_seconds = 0;              // 0 = no operator ceiling
    std::int64_t cache_max_age_seconds = 0;
    bool oauth2_required = false;
    std::string self_base; // https://<advertised>:<port>
};

struct Ctx {
    Store& store;
    sbi_core::jwt::Verifier& verifier;
    Settings settings;
};

// ---- errors -------------------------------------------------------------------------------------
Response problem(int status,
                 const std::string& title,
                 const std::string& detail,
                 const std::optional<std::string>& cause = std::nullopt);
// ExtendedProblemDetails = ProblemDetails + ProblemDetailsExtension (anyOf Record | RecordMeta).
// The Record alternative is used with only its (required) `meta`: blocks are opaque bytes and have
// no JSON representation.
Response extended_problem(int status,
                          const std::string& title,
                          const std::string& detail,
                          const std::string& cause,
                          const nlohmann::json& stored_meta);

// ---- request helpers ----------------------------------------------------------------------------
std::optional<std::string> header(const Request& req, const std::string& name);
std::optional<std::string> query(const Request& req, const std::string& name);
bool query_flag(const Request& req, const std::string& name); // "true" -> true
// Positive integer query parameter; error text if present but malformed.
tl::expected<std::optional<std::int64_t>, std::string> query_uint(const Request& req,
                                                                  const std::string& name);

// TS 29.500 6.7.3: 401/403 with WWW-Authenticate when the token is invalid / lacks `scope`.
std::optional<Response>
authorize(const Ctx& ctx, const Request& req, const std::string& scope, const std::string& api_uri);
tl::expected<StorageRef, Response> resolve_storage(const Ctx& ctx, const Request& req);

struct Conditions {
    std::optional<EtagCondition> if_match;
    std::optional<EtagCondition> if_none_match;
    std::optional<std::int64_t> if_modified_since;
    // RFC 9110 13.2.2 order: If-Match, then If-None-Match (for writes).
    bool write_passes(const std::optional<std::string>& etag) const;
    // GET: If-None-Match wins over If-Modified-Since (RFC 9110 13.1.3).
    bool not_modified(const std::optional<std::string>& etag, std::int64_t lm) const;
};
Conditions conditions(const Request& req);

// Adds ETag, Last-Modified and (when `cacheable`) Cache-Control: max-age.
void add_validators(
    const Ctx& ctx, Response& r, const std::string& etag, std::int64_t lm, bool cacheable);
Response not_modified(const Ctx& ctx, const std::string& etag, std::int64_t lm);

// Negotiated features: the intersection of ours and the consumer's supported-features (hex).
std::optional<std::string> negotiate(const std::string& ours,
                                     const std::optional<std::string>& theirs);
bool feature_negotiated(const std::optional<std::string>& theirs, int feature_number);

// ---- representations ----------------------------------------------------------------------------
// Record as multipart/mixed: meta part (Content-Id "meta", application/json) then one part per
// block (Content-Id = blockId, its Content-Type and Content-Transfer-Encoding).
sbi_core::multipart::Encoded record_multipart(const Record& rec);
Response record_response(const Ctx& ctx, int status, const Record& rec, bool cacheable);

// Validates RecordMeta as the YAML shapes it (TS29598 RecordMeta via the generated DTO, plus
// tags: map(array(string)), minProperties 1, items minItems 1 + uniqueItems). Empty string = ok.
std::string validate_record_meta(const nlohmann::json& meta);
// Same map(array(string)) rule for Timer.metaTags.
std::string validate_tag_map(const nlohmann::json& tags, const char* what);

} // namespace udsf
