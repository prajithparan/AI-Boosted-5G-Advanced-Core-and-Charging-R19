#include "redact.hpp"

#include <cctype>
#include <sstream>

namespace oam_gui_bff {

using nlohmann::json;

namespace {

std::vector<std::string> split(const std::string& path) {
    std::vector<std::string> out;
    std::stringstream ss(path);
    std::string seg;
    while (std::getline(ss, seg, '.')) out.push_back(seg);
    return out;
}

// Applies `fn` to the value at `path` if present (objects only; arrays are walked element-wise).
template <typename Fn> void at_path(json& node, const std::vector<std::string>& segs,
                                    std::size_t i, Fn&& fn) {
    if (node.is_array()) {
        for (auto& el : node) at_path(el, segs, i, fn);
        return;
    }
    if (!node.is_object() || i >= segs.size()) return;
    const auto it = node.find(segs[i]);
    if (it == node.end()) return;
    if (i + 1 == segs.size()) {
        fn(*it);
    } else {
        at_path(*it, segs, i + 1, fn);
    }
}

void scrub(json& node, const std::vector<std::string>& identifiers,
           const std::vector<std::string>& secrets) {
    if (node.is_string()) {
        auto s = node.get<std::string>();
        bool changed = false;
        for (const auto& sec : secrets) {
            if (sec.empty()) continue;
            for (auto p = s.find(sec); p != std::string::npos; p = s.find(sec, p)) {
                s.replace(p, sec.size(), "[secret: not retained]");
                changed = true;
            }
        }
        for (const auto& id : identifiers) {
            if (id.size() < 5) continue; // too short to be an identifier worth scrubbing
            for (auto p = s.find(id); p != std::string::npos; p = s.find(id, p + 1)) {
                s.replace(p, id.size(), mask_value(id));
                changed = true;
            }
        }
        if (changed) node = s;
    } else if (node.is_structured()) {
        for (auto& el : node) scrub(el, identifiers, secrets);
    }
}

} // namespace

std::string mask_value(const std::string& v) {
    if (v.size() <= 4) return std::string(v.size(), '*');
    return std::string(v.size() - 4, '*') + v.substr(v.size() - 4);
}

json strip_secrets(json doc, const std::vector<FieldRule>& rules) {
    for (const auto& r : rules) {
        if (r.classification != "SECRET_WRITE_ONLY") continue;
        at_path(doc, split(r.path), 0, [](json& v) { v = "[secret: not retained]"; });
    }
    return doc;
}

std::vector<std::string> secret_values(const json& doc, const std::vector<FieldRule>& rules) {
    std::vector<std::string> out;
    json copy = doc;
    for (const auto& r : rules) {
        if (r.classification != "SECRET_WRITE_ONLY") continue;
        at_path(copy, split(r.path), 0, [&out](json& v) {
            if (!v.is_string() || v.get<std::string>().empty()) return;
            // Hex keys may come back in either case.
            std::string lo = v.get<std::string>(), up = lo;
            for (auto& ch : lo) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            for (auto& ch : up) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            out.push_back(v.get<std::string>());
            out.push_back(lo);
            out.push_back(up);
        });
    }
    return out;
}

json mask_pii(json doc, const std::vector<FieldRule>& rules,
              const std::vector<std::string>& identifiers, const std::vector<std::string>& secrets) {
    for (const auto& r : rules) {
        if (r.classification == "SECRET_WRITE_ONLY") {
            at_path(doc, split(r.path), 0, [](json& v) { v = "[secret: not retained]"; });
        } else if (r.classification == "PII" || r.classification == "CREDENTIAL") {
            at_path(doc, split(r.path), 0, [](json& v) {
                if (v.is_string()) v = mask_value(v.get<std::string>());
            });
        }
    }
    scrub(doc, identifiers, secrets);
    return doc;
}

} // namespace oam_gui_bff
