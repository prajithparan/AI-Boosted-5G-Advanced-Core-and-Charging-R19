#pragma once

// NF static-configuration management (ADR-0425): read, validate against the DERIVED schema,
// mask credentials, and apply (atomic file replace). Every NF in this repository reads its
// config/<nf>.json once at start-up -- none re-reads it -- so "apply" writes the file and marks
// the version restart-required; it never pretends to reload a running process.

#include <nlohmann/json.hpp>

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace oam_gui_bff {

inline constexpr const char* kCredentialMask = "********";

class NfConfigManager {
public:
    NfConfigManager(std::string config_dir, std::string schema_dir, std::set<std::string> editable);

    bool editable(const std::string& nf) const { return editable_.count(nf) != 0; }
    const std::set<std::string>& editable_nfs() const { return editable_; }

    std::optional<nlohmann::json> schema(const std::string& nf) const;
    std::optional<nlohmann::json> current(const std::string& nf) const;

    // Subset of JSON Schema draft-07 the deriver emits: type, properties, required,
    // additionalProperties:false, items. Returns human-readable errors (paths + rule, never
    // values -- a value may be a credential).
    static std::vector<std::string> validate(const nlohmann::json& schema,
                                             const nlohmann::json& doc);

    // Replaces every x-sensitivity:credential value with kCredentialMask.
    static nlohmann::json mask(const nlohmann::json& schema, nlohmann::json doc);
    // For a proposed document: a credential still equal to kCredentialMask means "unchanged" and
    // takes the current value.
    static nlohmann::json unmask_unchanged(const nlohmann::json& schema, nlohmann::json proposed,
                                           const nlohmann::json& current);

    // Writes <config_dir>/<nf>.json atomically (temp file + rename). Throws on failure.
    void apply(const std::string& nf, const nlohmann::json& content) const;

private:
    std::string config_dir_;
    std::string schema_dir_;
    std::set<std::string> editable_;
};

} // namespace oam_gui_bff
