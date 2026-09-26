#pragma once

// Field-policy application (ADR-0423). Rules come from iam.field_policy, never from code.

#include "iam.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace oam_gui_bff {

// Removes every SECRET_WRITE_ONLY field (replaced by "[secret: not retained]") -- used on
// anything that is stored or audited. PII is kept (the audit trail must say which customer).
nlohmann::json strip_secrets(nlohmann::json doc, const std::vector<FieldRule>& rules);

// Masks every PII field for display ("******0001": all but the last 4 characters), and also
// scrubs any occurrence of `identifiers` (e.g. the SUPI's digits) inside other string values,
// because provisioning's ids and error texts embed them.
// `secrets` (e.g. the K/OPc values the request carried) are replaced WHOLE wherever they appear,
// so even an upstream that echoed a key back could not put it on the operator's screen.
nlohmann::json mask_pii(nlohmann::json doc, const std::vector<FieldRule>& rules,
                        const std::vector<std::string>& identifiers,
                        const std::vector<std::string>& secrets = {});

// The string values at every SECRET_WRITE_ONLY path of `doc` (to scrub them from responses).
std::vector<std::string> secret_values(const nlohmann::json& doc,
                                       const std::vector<FieldRule>& rules);

std::string mask_value(const std::string& v);

} // namespace oam_gui_bff
