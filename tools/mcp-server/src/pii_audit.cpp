#include "pii_audit.hpp"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <pqxx/pqxx>

namespace mcp {
namespace {

// Monotonic within a process, prefixed with the start time so ids do not collide across restarts.
// Not a UUID: this is an audit key, and a readable, sortable id is more useful to whoever reads
// the table than an opaque one.
std::string next_audit_id() {
    static const auto boot = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
    static std::atomic<std::uint64_t> seq{0};
    return "pii-" + std::to_string(boot) + "-" + std::to_string(seq.fetch_add(1));
}

} // namespace

const char* to_string(AccessOutcome outcome) {
    switch (outcome) {
        case AccessOutcome::Granted:
            return "granted";
        case AccessOutcome::DeniedScope:
            return "denied_scope";
        case AccessOutcome::DeniedPolicy:
            return "denied_policy";
    }
    return "denied_policy";
}

PiiAuditStore::PiiAuditStore(const std::string& conninfo) {
    try {
        client_ = std::make_unique<pqxx::connection>(conninfo);
    } catch (const std::exception& e) {
        // Left null on purpose. is_connected() is then false and the server refuses to start,
        // rather than coming up able to serve PII with nowhere to log it. Same reasoning as
        // record()'s own contract: degrading to "no audit" is the one degradation not allowed.
        spdlog::error("mcp: PII audit store could not connect: {}", e.what());
        client_.reset();
    }
}

PiiAuditStore::~PiiAuditStore() = default;

bool PiiAuditStore::record(const PiiAccess& access) {
    if (client_ == nullptr) {
        return false;
    }
    try {
        pqxx::work txn(*client_);
        txn.exec("INSERT INTO pii_access_audit (id, agent_id, principal, tool_name, request_id, "
                 "subject_id, field_classes, withheld_classes, outcome) "
                 "VALUES ($1,$2,$3,$4,$5,$6,$7::jsonb,$8::jsonb,$9)",
                 pqxx::params{next_audit_id(),
                              access.agent_id,
                              access.principal,
                              access.tool_name,
                              access.request_id,
                              access.subject_id,
                              nlohmann::json(access.field_classes).dump(),
                              nlohmann::json(access.withheld_classes).dump(),
                              std::string(to_string(access.outcome))});
        // Committed before the caller is allowed to return data. An uncommitted audit is not an
        // audit, so this must not be moved after the read completes.
        txn.commit();
        return true;
    } catch (const std::exception& e) {
        spdlog::error("mcp: PII audit write FAILED for tool {} subject {} -- the read must be "
                      "refused: {}",
                      access.tool_name,
                      access.subject_id,
                      e.what());
        return false;
    }
}

} // namespace mcp
