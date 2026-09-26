#pragma once

// TS 29.598 SearchExpression (clause 6.1.6.4.1) -- parsing, validation and evaluation (ADR-0400).
//
// Why this is not simply the generated DTO: SearchExpression in TS29598_Nudsf_DataRepository.yaml
// is `type: object` + `oneOf: [SearchCondition, SearchComparison, RecordIdList]`, which the
// project's generator (tools/sbi-codegen) renders as an EMPTY struct -- from_json keeps nothing,
// so SearchCondition::units (vector<SearchExpression>) would lose every nested expression. The
// raw JSON is therefore walked here and each LEAF is decoded with the generated types
// (sbi_gen::SearchComparison, sbi_gen::RecordIdList, sbi_gen::SearchCondition for cond/schemaId),
// so no field name below is hand-transcribed from anything but the generated DTOs.
//
// Semantics (TS 29.598 6.1.6.2.8, 6.1.6.2.9, 6.1.6.3.3, 6.1.6.3.4, 6.1.3.2.3.2):
//   * EQ / NEQ / GT / GTE / LT / LTE compare the tag's ARRAY of strings to one value, byte-wise
//     lexicographically; a record matches GT if ANY of its values is greater, and so on.
//     NEQ is "the array does not contain the value" -- a record without the tag matches.
//   * AND / OR need >= 2 units, NOT exactly 1 (6.1.6.2.8). Violations are rejected, not guessed.
//   * SearchComparison{op: GTE, tag: ""} selects every record (6.1.3.2.3.2 "To delete all
//     records ... GTE ... The value of the tag shall be set to ''").
//   * RecordIdList (BulkOperations) selects exactly those ids that exist.
//   * SearchCondition.schemaId restricts to items stored with that schemaId -- evaluated through
//     TagIndex::with_schema(), which a store may refuse (see nfs/udsf/src/store.hpp for why the
//     record store does).

#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <tl/expected.hpp>
#include <vector>

namespace udsf {

struct SearchExpr {
    enum class Kind { Comparison, Condition, IdList };
    Kind kind = Kind::Comparison;
    // Comparison
    std::string op;
    std::string tag;
    std::string value;
    // Condition
    std::string cond;
    std::optional<std::string> schema_id;
    std::vector<SearchExpr> units;
    // IdList
    std::vector<std::string> ids;
};

// Parses and validates one SearchExpression JSON value. Error text is suitable for a 400 detail.
tl::expected<SearchExpr, std::string> parse_search_expression(const nlohmann::json& j);

// Parses the `filter` query parameter, whose YAML encoding is `content: application/json`
// (the whole expression is one JSON text in the query string, already percent-decoded).
tl::expected<SearchExpr, std::string> parse_filter_param(const std::string& text);

// True if the expression (anywhere in its tree) uses an operator outside the always-available
// EQ comparison -- i.e. needs the AdvancedQuery feature (TS 29.598 table 6.1.8-1, feature 1).
bool uses_advanced_query(const SearchExpr& e);
// True if the expression contains a RecordIdList (BulkOperations, feature 4).
bool uses_id_list(const SearchExpr& e);

// The store side of an evaluation: sets of item ids per primitive.
class TagIndex {
public:
    virtual ~TagIndex() = default;
    virtual std::set<std::string> all() = 0;
    virtual std::set<std::string> eq(const std::string& tag, const std::string& value) = 0;
    // op is one of GT, GTE, LT, LTE.
    virtual std::set<std::string>
    range(const std::string& tag, const std::string& op, const std::string& value) = 0;
    // Subset of `ids` that exist.
    virtual std::set<std::string> existing(const std::vector<std::string>& ids) = 0;
    // Items stored with this schemaId; nullopt when the store cannot answer (then the whole
    // evaluation fails with an explanatory error rather than silently ignoring the filter).
    virtual std::optional<std::set<std::string>> with_schema(const std::string& schema_id) = 0;
};

tl::expected<std::set<std::string>, std::string> evaluate(const SearchExpr& e, TagIndex& index);

} // namespace udsf
