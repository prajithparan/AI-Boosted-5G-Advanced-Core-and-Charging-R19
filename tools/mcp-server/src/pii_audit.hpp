#pragma once

// ADR-0331: PII access audit. See schema.postgres.sql for the table and the reasoning.
//
// The one rule that shapes this interface: `record()` returns false when the audit could not be
// written, and every caller MUST refuse the read in that case. It is deliberately not a
// fire-and-forget logger and deliberately does not swallow its own failures -- a best-effort
// audit is indistinguishable from no audit at the moment it matters.

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

namespace pqxx {
class connection;
}

namespace mcp {

enum class AccessOutcome { Granted, DeniedScope, DeniedPolicy };

struct PiiAccess {
    std::string agent_id;   // the persona that asked
    std::string principal;  // the human or system it acted for
    std::string tool_name;  // why: which tool
    std::string request_id; // why: the caller's request
    std::string subject_id; // whose data
    // Classes, never values: "msisdn", "balance", "spend". An audit row that copies the PII it
    // audits doubles the exposure it exists to control.
    std::vector<std::string> field_classes;
    std::vector<std::string> withheld_classes;
    AccessOutcome outcome = AccessOutcome::Granted;
};

class PiiAuditStore {
public:
    explicit PiiAuditStore(const std::string& conninfo);
    ~PiiAuditStore();

    PiiAuditStore(const PiiAuditStore&) = delete;
    PiiAuditStore& operator=(const PiiAuditStore&) = delete;

    // True only when the row is durably written. False means the caller must not return PII.
    bool record(const PiiAccess& access);

    bool is_connected() const { return client_ != nullptr; }

private:
    std::unique_ptr<pqxx::connection> client_;
};

const char* to_string(AccessOutcome outcome);

} // namespace mcp
