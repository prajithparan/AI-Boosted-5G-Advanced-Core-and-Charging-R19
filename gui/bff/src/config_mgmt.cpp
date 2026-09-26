#include "config_mgmt.hpp"

#include "util.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace oam_gui_bff {

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

bool safe_name(const std::string& nf) {
    if (nf.empty() || nf.size() > 64) return false;
    for (const char c : nf) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

std::optional<json> read_json(const fs::path& p) {
    std::ifstream in(p);
    if (!in) return std::nullopt;
    try {
        return std::optional<json>(json::parse(in));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool type_ok(const std::string& t, const json& v) {
    if (t == "integer") return v.is_number_integer();
    if (t == "number") return v.is_number();
    if (t == "string") return v.is_string();
    if (t == "boolean") return v.is_boolean();
    if (t == "object") return v.is_object();
    if (t == "array") return v.is_array();
    return true;
}

void check(const json& s, const json& v, const std::string& path, std::vector<std::string>& errs) {
    if (s.contains("type") && !type_ok(s["type"].get<std::string>(), v)) {
        errs.push_back((path.empty() ? "<root>" : path) + ": must be " +
                       s["type"].get<std::string>());
        return;
    }
    if (v.is_object() && s.contains("properties")) {
        const auto& props = s["properties"];
        for (const auto& r : s.value("required", json::array())) {
            if (!v.contains(r.get<std::string>())) {
                errs.push_back((path.empty() ? "" : path + ".") + r.get<std::string>() +
                               ": required");
            }
        }
        for (const auto& [k, sub] : v.items()) {
            const std::string p = path.empty() ? k : path + "." + k;
            if (!props.contains(k)) {
                if (s.value("additionalProperties", true) == false) {
                    errs.push_back(p + ": not a key of this component's configuration");
                }
                continue;
            }
            check(props[k], sub, p, errs);
        }
    }
    if (v.is_array() && s.contains("items")) {
        for (std::size_t i = 0; i < v.size(); ++i) {
            check(s["items"], v[i], path + "[" + std::to_string(i) + "]", errs);
        }
    }
}

template <typename Fn> void each_credential(const json& s, json& v, const json* other, Fn&& fn) {
    if (!v.is_object() || !s.contains("properties")) return;
    for (const auto& [k, sub] : s["properties"].items()) {
        if (!v.contains(k)) continue;
        const json* o = (other && other->is_object() && other->contains(k)) ? &(*other)[k] : nullptr;
        if (sub.value("x-sensitivity", "") == "credential") {
            fn(v[k], o);
        } else if (sub.value("type", "") == "object") {
            each_credential(sub, v[k], o, fn);
        }
    }
}

} // namespace

NfConfigManager::NfConfigManager(std::string config_dir, std::string schema_dir,
                                 std::set<std::string> editable)
    : config_dir_(std::move(config_dir)), schema_dir_(std::move(schema_dir)),
      editable_(std::move(editable)) {}

std::optional<json> NfConfigManager::schema(const std::string& nf) const {
    if (!safe_name(nf)) return std::nullopt;
    return read_json(fs::path(schema_dir_) / (nf + ".schema.json"));
}

std::optional<json> NfConfigManager::current(const std::string& nf) const {
    if (!safe_name(nf)) return std::nullopt;
    return read_json(fs::path(config_dir_) / (nf + ".json"));
}

std::vector<std::string> NfConfigManager::validate(const json& schema, const json& doc) {
    std::vector<std::string> errs;
    check(schema, doc, "", errs);
    return errs;
}

json NfConfigManager::mask(const json& schema, json doc) {
    each_credential(schema, doc, nullptr, [](json& v, const json*) {
        if (v.is_string()) v = kCredentialMask;
    });
    return doc;
}

json NfConfigManager::unmask_unchanged(const json& schema, json proposed, const json& current) {
    each_credential(schema, proposed, &current, [](json& v, const json* cur) {
        if (v.is_string() && v.get<std::string>() == kCredentialMask && cur != nullptr) v = *cur;
    });
    return proposed;
}

void NfConfigManager::apply(const std::string& nf, const json& content) const {
    if (!safe_name(nf)) throw std::runtime_error("invalid component name");
    const fs::path target = fs::path(config_dir_) / (nf + ".json");
    const fs::path tmp = fs::path(config_dir_) / ("." + nf + ".json." + random_token(6));
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write the configuration file");
        out << content.dump(2) << "\n";
        out.flush();
        if (!out) throw std::runtime_error("cannot write the configuration file");
    }
    std::error_code ec;
    fs::rename(tmp, target, ec); // atomic on the same filesystem
    if (ec) {
        fs::remove(tmp, ec);
        throw std::runtime_error("cannot replace the configuration file");
    }
}

} // namespace oam_gui_bff
