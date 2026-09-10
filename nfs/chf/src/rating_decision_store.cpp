#include "rating_decision_store.hpp"

#include <spdlog/spdlog.h>

namespace chf {

RatingDecisionStore::RatingDecisionStore(const std::string& conninfo) {
    try {
        client_ = std::make_unique<pqxx::connection>(conninfo);
    } catch (const std::exception& e) {
        spdlog::warn("chf: could not connect to PostgreSQL (RatingDecision audit disabled): {}",
                     e.what());
        client_.reset();
    }
}

std::vector<nlohmann::json>
RatingDecisionStore::find_by_charging_data_ref(const std::string& charging_data_ref) {
    std::vector<nlohmann::json> out;
    if (client_ == nullptr) {
        return out;
    }
    try {
        pqxx::work txn(*client_);
        // ai_advisory is returned as-is: when a model influenced the grant, the explanation must
        // say so and show which bound applied. Hiding it would make an AI-adjusted charge
        // indistinguishable from a purely deterministic one.
        const auto rows =
            txn.exec("SELECT id, tariff_id, tariff_version, rating_group, rated_amount, currency, "
                     "rule_fired_id, ai_advisory::text AS ai_advisory, input_snapshot::text AS "
                     "input_snapshot, decided_at::text AS decided_at "
                     "FROM rating_decision WHERE input_snapshot->>'chargingDataRef' = $1 "
                     "ORDER BY decided_at DESC",
                     pqxx::params{charging_data_ref});
        for (const auto& r : rows) {
            nlohmann::json entry;
            entry["id"] = r["id"].as<std::string>("");
            entry["tariffId"] = r["tariff_id"].as<std::string>("");
            entry["tariffVersion"] = r["tariff_version"].as<std::string>("");
            entry["ratingGroup"] = r["rating_group"].as<long>(0);
            entry["ratedAmount"] = r["rated_amount"].as<double>(0.0);
            entry["currency"] = r["currency"].as<std::string>("");
            entry["ruleFiredId"] = r["rule_fired_id"].as<std::string>("");
            entry["decidedAt"] = r["decided_at"].as<std::string>("");
            for (const char* col : {"ai_advisory", "input_snapshot"}) {
                const auto raw = r[col].as<std::string>("");
                if (!raw.empty()) {
                    try {
                        entry[col] = nlohmann::json::parse(raw);
                    } catch (const std::exception&) {
                        // A malformed stored document is reported as absent rather than crashing
                        // an explanation request; the rest of the decision is still useful.
                    }
                }
            }
            out.push_back(std::move(entry));
        }
    } catch (const std::exception& e) {
        spdlog::warn("chf: rating-decision lookup for {} failed: {}", charging_data_ref, e.what());
    }
    return out;
}

void RatingDecisionStore::record(const RatingDecisionRecord& decision) {
    if (!client_) {
        spdlog::warn(
            "chf: RatingDecision write skipped for tariffId={} -- PostgreSQL not connected",
            decision.tariffId);
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    try {
        pqxx::work txn(*client_);
        const auto id = txn.exec("SELECT nextval('rating_decision_id_seq')::text AS id")
                            .one_row()["id"]
                            .as<std::string>();
        // Disclosed simplification: acbr_tax_excluded and acbr_tax_included both bind to the same
        // $10 (decision.ratedAmount) -- this project has no real tax-computation subsystem yet
        // (see rating.hpp's own AppliedBillingTaxRate, modeled but not populated from any real
        // rate table), so the two real TMF678 fields are not yet meaningfully distinguished, not
        // fabricated as more sophisticated than they are.
        // P4.8 (ADR-0074): ai_advisory is a nullable jsonb column -- pqxx::params binds
        // std::optional<std::string> as SQL NULL when empty, same convention already used for
        // tariffVersion/currency/acbrType above.
        const std::optional<std::string> ai_advisory_json =
            decision.aiAdvisory.has_value() ? std::make_optional(decision.aiAdvisory->dump())
                                            : std::nullopt;
        txn.exec("INSERT INTO rating_decision "
                 "(id, tariff_id, tariff_version, rating_group, input_snapshot, rated_amount, "
                 "currency, rule_fired_id, ai_advisory, acbr_type, acbr_is_billed, "
                 "acbr_tax_excluded, acbr_tax_included) "
                 "VALUES ($1,$2,$3,$4,$5::jsonb,$6,$7,$8,$9::jsonb,$10,false,$11,$11)",
                 pqxx::params{id,
                              decision.tariffId,
                              decision.tariffVersion,
                              decision.ratingGroup,
                              decision.inputSnapshot.dump(),
                              decision.ratedAmount,
                              decision.currency,
                              decision.ruleFiredId,
                              ai_advisory_json,
                              decision.acbrType,
                              decision.ratedAmount});

        // P4.5/ADR-0060 (E8, Security): real audit trail, same transaction as the mutation it
        // records -- same "local per-service table" architectural disclosure as
        // bss/product-catalog's own schema.sql header.
        const auto audit_id = txn.exec("SELECT nextval('audit_record_id_seq')::text AS id")
                                  .one_row()["id"]
                                  .as<std::string>();
        // P4.8 (ADR-0074): ai_advisory_ref carries the model id/version (MLflow run id) an AI
        // advisory came from, when one was present -- audit_record's own real, separate
        // governance-traceability field (schema.postgres.sql), distinct from rating_decision's own
        // full ai_advisory JSON blob.
        const std::optional<std::string> ai_advisory_ref =
            decision.aiAdvisory.has_value() && decision.aiAdvisory->contains("model_version")
                ? std::make_optional(decision.aiAdvisory->at("model_version").get<std::string>())
                : std::nullopt;
        txn.exec("INSERT INTO audit_record (id, entity_type, entity_id, action, actor, "
                 "after_snapshot, ai_advisory_ref) VALUES ($1,'RATING_DECISION',$2,"
                 "'ratingDecision.record','chf',$3::jsonb,$4)",
                 pqxx::params{audit_id, id, decision.inputSnapshot.dump(), ai_advisory_ref});

        txn.commit();
    } catch (const std::exception& e) {
        // Best-effort, same discipline as CdrWriter::write -- a rating-decision audit-write
        // failure must never block or fail the real charging response CHF already committed to.
        spdlog::warn(
            "chf: RatingDecision write failed for tariffId={}: {}", decision.tariffId, e.what());
    }
}

} // namespace chf
