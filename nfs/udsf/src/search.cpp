#include "search.hpp"

#include <algorithm>
#include <iterator>

#include "TS29598_Nudsf_DataRepository.hpp"

namespace udsf {

namespace {

constexpr int kMaxDepth = 32;

bool is_comparison_op(const std::string& op) {
    return op == "EQ" || op == "NEQ" || op == "GT" || op == "GTE" || op == "LT" || op == "LTE";
}

tl::expected<SearchExpr, std::string> parse(const nlohmann::json& j, int depth) {
    if (depth > kMaxDepth) {
        return tl::unexpected("SearchExpression nested deeper than " + std::to_string(kMaxDepth));
    }
    if (!j.is_object()) {
        return tl::unexpected(std::string("SearchExpression must be a JSON object"));
    }
    // oneOf discrimination by each alternative's REQUIRED members (YAML: SearchCondition requires
    // cond+units, SearchComparison op+tag+value, RecordIdList recordIdList). Exactly one shall
    // match.
    const bool is_cond = j.contains("cond") || j.contains("units");
    const bool is_cmp = j.contains("op") || j.contains("tag") || j.contains("value");
    const bool is_ids = j.contains("recordIdList");
    if (static_cast<int>(is_cond) + static_cast<int>(is_cmp) + static_cast<int>(is_ids) != 1) {
        return tl::unexpected(std::string(
            "SearchExpression shall be exactly one of SearchCondition, SearchComparison, "
            "RecordIdList (TS 29.598 6.1.6.4.1)"));
    }
    SearchExpr out;
    try {
        if (is_cmp) {
            const auto c = j.get<sbi_gen::SearchComparison>();
            if (!is_comparison_op(c.op.value)) {
                return tl::unexpected("unknown ComparisonOperator \"" + c.op.value + "\"");
            }
            out.kind = SearchExpr::Kind::Comparison;
            out.op = c.op.value;
            out.tag = c.tag;
            out.value = c.value;
            return out;
        }
        if (is_ids) {
            const auto l = j.get<sbi_gen::RecordIdList>();
            if (l.recordIdList.empty()) {
                return tl::unexpected(std::string("recordIdList shall have at least one item"));
            }
            out.kind = SearchExpr::Kind::IdList;
            out.ids = l.recordIdList;
            return out;
        }
        const auto c = j.get<sbi_gen::SearchCondition>(); // cond + schemaId; units walked below
        out.kind = SearchExpr::Kind::Condition;
        out.cond = c.cond.value;
        out.schema_id = c.schemaId;
    } catch (const std::exception& e) {
        return tl::unexpected(std::string("invalid SearchExpression: ") + e.what());
    }
    if (out.cond != "AND" && out.cond != "OR" && out.cond != "NOT") {
        return tl::unexpected("unknown ConditionOperator \"" + out.cond + "\"");
    }
    const auto& units = j.at("units");
    if (!units.is_array()) {
        return tl::unexpected(std::string("SearchCondition.units shall be an array"));
    }
    if (out.cond == "NOT" && units.size() != 1) {
        return tl::unexpected(
            std::string("NOT takes exactly one unit (TS 29.598 table 6.1.6.2.8-1)"));
    }
    if (out.cond != "NOT" && units.size() < 2) {
        return tl::unexpected(
            std::string("AND/OR take at least two units (TS 29.598 table 6.1.6.2.8-1)"));
    }
    for (const auto& u : units) {
        auto sub = parse(u, depth + 1);
        if (!sub) {
            return tl::unexpected(sub.error());
        }
        out.units.push_back(std::move(*sub));
    }
    return out;
}

std::set<std::string> intersect(const std::set<std::string>& a, const std::set<std::string>& b) {
    std::set<std::string> r;
    std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::inserter(r, r.end()));
    return r;
}

std::set<std::string> minus(const std::set<std::string>& a, const std::set<std::string>& b) {
    std::set<std::string> r;
    std::set_difference(a.begin(), a.end(), b.begin(), b.end(), std::inserter(r, r.end()));
    return r;
}

} // namespace

tl::expected<SearchExpr, std::string> parse_search_expression(const nlohmann::json& j) {
    return parse(j, 0);
}

tl::expected<SearchExpr, std::string> parse_filter_param(const std::string& text) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
    } catch (const nlohmann::json::parse_error& e) {
        return tl::unexpected(std::string("filter is not valid JSON: ") + e.what());
    }
    return parse(j, 0);
}

bool uses_advanced_query(const SearchExpr& e) {
    switch (e.kind) {
        case SearchExpr::Kind::Comparison:
            return e.op != "EQ";
        case SearchExpr::Kind::IdList:
            return false;
        case SearchExpr::Kind::Condition:
            return true; // "shall not use the cond attribute" without AdvancedQuery (table 6.1.8-1)
    }
    return false;
}

bool uses_id_list(const SearchExpr& e) {
    if (e.kind == SearchExpr::Kind::IdList) {
        return true;
    }
    return std::any_of(
        e.units.begin(), e.units.end(), [](const auto& u) { return uses_id_list(u); });
}

tl::expected<std::set<std::string>, std::string> evaluate(const SearchExpr& e, TagIndex& index) {
    switch (e.kind) {
        case SearchExpr::Kind::Comparison:
            if (e.tag.empty() && e.op == "GTE") {
                return index.all(); // 6.1.3.2.3.2: select every record
            }
            if (e.op == "EQ") {
                return index.eq(e.tag, e.value);
            }
            if (e.op == "NEQ") {
                return minus(index.all(), index.eq(e.tag, e.value));
            }
            return index.range(e.tag, e.op, e.value);
        case SearchExpr::Kind::IdList:
            return index.existing(e.ids);
        case SearchExpr::Kind::Condition:
            break;
    }
    std::set<std::string> acc;
    bool first = true;
    for (const auto& u : e.units) {
        auto sub = evaluate(u, index);
        if (!sub) {
            return sub;
        }
        if (e.cond == "NOT") {
            acc = minus(index.all(), *sub);
        } else if (first) {
            acc = std::move(*sub);
        } else if (e.cond == "AND") {
            acc = intersect(acc, *sub);
        } else {
            acc.insert(sub->begin(), sub->end());
        }
        first = false;
    }
    if (e.schema_id) {
        auto scoped = index.with_schema(*e.schema_id);
        if (!scoped) {
            return tl::unexpected(std::string(
                "SearchCondition.schemaId cannot be evaluated for this resource type (see "
                "ADR-0400: TS29598 RecordMeta carries no schemaId in the R19 YAML)"));
        }
        acc = intersect(acc, *scoped);
    }
    return acc;
}

} // namespace udsf
