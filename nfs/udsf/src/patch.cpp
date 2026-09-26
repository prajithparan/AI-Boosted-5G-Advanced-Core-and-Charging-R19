#include "patch.hpp"

namespace udsf {

tl::expected<std::vector<sbi_gen::PatchItem>, std::string>
parse_patch_items(const std::string& body) {
    try {
        const auto j = nlohmann::json::parse(body);
        if (!j.is_array() || j.empty()) {
            return tl::unexpected(std::string("the JSON Patch body shall be a non-empty array"));
        }
        return j.get<std::vector<sbi_gen::PatchItem>>();
    } catch (const std::exception& e) {
        return tl::unexpected(std::string("invalid JSON Patch body: ") + e.what());
    }
}

nlohmann::json to_rfc6902(const sbi_gen::PatchItem& item) {
    nlohmann::json op{{"op", item.op.value}, {"path", item.path}};
    if (item.from) {
        op["from"] = *item.from;
    }
    if (item.value) {
        op["value"] = *item.value;
    } else if (item.op.value == "add" || item.op.value == "replace" || item.op.value == "test") {
        op["value"] = nullptr; // PatchItem.value is nullable; RFC 6902 requires the member
    }
    return op;
}

ItemwiseResult apply_itemwise(nlohmann::json document,
                              const std::vector<sbi_gen::PatchItem>& items,
                              const std::function<std::string(const nlohmann::json&)>& valid) {
    ItemwiseResult r;
    for (const auto& item : items) {
        nlohmann::json candidate;
        try {
            candidate = document.patch(nlohmann::json::array({to_rfc6902(item)}));
        } catch (const std::exception& e) {
            sbi_gen::ReportItem ri;
            ri.path = item.path;
            ri.reason = std::string(item.op.value) + " not applied: " + e.what();
            r.discarded.push_back(std::move(ri));
            continue;
        }
        if (auto why = valid(candidate); !why.empty()) {
            sbi_gen::ReportItem ri;
            ri.path = item.path;
            ri.reason = std::string(item.op.value) + " discarded: " + why;
            r.discarded.push_back(std::move(ri));
            continue;
        }
        document = std::move(candidate);
    }
    r.document = std::move(document);
    return r;
}

tl::expected<nlohmann::json, sbi_gen::PatchResult>
apply_atomic(const nlohmann::json& document, const std::vector<sbi_gen::PatchItem>& items) {
    nlohmann::json current = document;
    for (const auto& item : items) {
        try {
            current = current.patch(nlohmann::json::array({to_rfc6902(item)}));
        } catch (const std::exception& e) {
            sbi_gen::ReportItem ri;
            ri.path = item.path;
            ri.reason = std::string(item.op.value) + " cannot be applied: " + e.what();
            sbi_gen::PatchResult pr;
            pr.report.push_back(std::move(ri));
            return tl::unexpected(std::move(pr));
        }
    }
    return current;
}

} // namespace udsf
