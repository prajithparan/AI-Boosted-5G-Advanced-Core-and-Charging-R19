#pragma once

// JSON Patch (RFC 6902) application for the UDSF's PATCH operations (ADR-0400). The patch
// document is decoded into the generated sbi_gen::PatchItem (TS 29.571) and applied with
// nlohmann::json::patch, one item at a time so a failing item can be reported by its path in a
// TS 29.571 PatchResult.
//
// Two modes, because TS 29.598 asks for both:
//   * item-wise ("discard and report"): UpdateMeta, UpdateNotificationSubscription, UpdateTimer --
//     "if one or more modification instructions have been discarded, 200 OK with the execution
//     report" (5.2.2.4.4, 5.2.2.4.5, 5.3.2.3.2). Each item is kept only if it applies AND the
//     resulting document still validates.
//   * all-or-nothing: PatchRecord -- "If one or more modification instructions cannot be applied,
//     no changes shall be made to record ... 422 with the execution report" (5.2.2.4.8).

#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <tl/expected.hpp>
#include <vector>

#include "TS26510_CommonData_grp.hpp"

namespace udsf {

// Decodes a JSON Patch request body (array of PatchItem, minItems 1).
tl::expected<std::vector<sbi_gen::PatchItem>, std::string> parse_patch_items(const std::string& body);

nlohmann::json to_rfc6902(const sbi_gen::PatchItem& item);

struct ItemwiseResult {
    nlohmann::json document;
    std::vector<sbi_gen::ReportItem> discarded;
};

// `valid` returns an empty string if the candidate document is acceptable, else the reason.
ItemwiseResult apply_itemwise(nlohmann::json document,
                              const std::vector<sbi_gen::PatchItem>& items,
                              const std::function<std::string(const nlohmann::json&)>& valid);

// Applies every item or none; on failure the PatchResult names the first failing item.
tl::expected<nlohmann::json, sbi_gen::PatchResult>
apply_atomic(const nlohmann::json& document, const std::vector<sbi_gen::PatchItem>& items);

} // namespace udsf
