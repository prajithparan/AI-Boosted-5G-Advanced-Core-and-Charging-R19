#pragma once

#include <nlohmann/json.hpp>

#include <memory>
#include <mutex>
#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <vector>

// Private to nfs/chf -- not shared with any other NF, per CLAUDE.md's "no NF includes another NF's
// private headers" rule.
//
// P4.5/ADR-0060 (E5, Rating Function): CHF's first real PostgreSQL connection (schema:
// ../schema.postgres.sql, real TMF678 AppliedCustomerBillingRate mapping). Same "graceful
// degradation, never crash the higher-priority real-time charging path" design principle already
// established for CdrWriter (ADR-0058's own real eager-connect crash bug and fix) -- a
// rating-decision audit-write failure must never block or crash the real charging response CHF
// has already committed to.

namespace chf {

struct RatingDecisionRecord {
    std::string tariffId;
    std::optional<std::string> tariffVersion;
    std::optional<std::int64_t> ratingGroup;
    nlohmann::json inputSnapshot = nlohmann::json::object();
    std::optional<double> ratedAmount;
    std::optional<std::string> currency;
    std::string ruleFiredId;
    std::optional<std::string>
        acbrType; // appliedBillingCharge | appliedBillingCredit | appliedPenaltyCharge
    // P4.8 (ADR-0074): real governance logging per CHARGING_PROMPT.md's mandatory model-
    // governance rules -- model id/version, input feature vector, output score, and which
    // deterministic bound actually applied. std::nullopt when AI quota sizing didn't run for this
    // decision (kill switch off, cold start, latency budget exceeded, or this rating_group's
    // price simply wasn't AI-adjustable) -- a real, valid "no advisory" state, not an error.
    std::optional<nlohmann::json> aiAdvisory;
};

class RatingDecisionStore {
public:
    // conninfo: a libpq connection string, sourced by the caller (env var, never hardcoded
    // credentials -- same precedent as bss/product-catalog's PRODUCT_CATALOG_DATABASE_URL,
    // ADR-0054). Does not throw on connection failure -- catches it internally and degrades to a
    // logged no-op state, same real-bug-driven design as CdrWriter (ADR-0058).
    explicit RatingDecisionStore(const std::string& conninfo);

    // Real INSERT into `rating_decision` (best-effort -- a write failure is logged and swallowed,
    // never propagated to the caller, same "does not block the real charging response" discipline
    // CdrWriter::write already established). `pqxx::connection` is single-connection, not
    // thread-safe for concurrent use -- P4.5/ADR-0060 Stage 3: `mutex_` below serializes callers
    // now that the real Diameter Gy CCR path (diameter_server.cpp, its own dedicated
    // per-connection thread) shares this same instance with CHF's HTTP io_context thread, same
    // real concurrency-model change disclosed on CdrWriter::write.
    void record(const RatingDecisionRecord& decision);

    // ADR-0332: read back the decisions behind one ChargingDataRef, so a charge can be
    // EXPLAINED rather than merely recorded. Returns one entry per rating group that was rated
    // for that reference, newest first.
    //
    // Looked up through `input_snapshot->>'chargingDataRef'` because that is where the reference
    // actually lives -- `usage_record_id` was reserved for a UsageRecord table that does not
    // exist (see schema.postgres.sql's own note), so keying on it would find nothing.
    std::vector<nlohmann::json> find_by_charging_data_ref(const std::string& charging_data_ref);

    bool is_connected() const { return client_ != nullptr; }

private:
    std::mutex mutex_;
    std::unique_ptr<pqxx::connection> client_;
};

} // namespace chf
