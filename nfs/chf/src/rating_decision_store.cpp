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
            // ADR-0388: chf_rating schema of the consolidated charging DB; idx_rating_ref.
            txn.exec("SELECT id, tariff_id, tariff_version, rating_group, monetary_amount AS "
                     "rated_amount, currency, rule_fired_id, ai_advisory::text AS ai_advisory, "
                     "input_snapshot::text AS input_snapshot, decided_at::text AS decided_at "
                     "FROM chf_rating.rating_decision WHERE charging_data_ref = $1 "
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
        const auto id = txn.exec("SELECT nextval('chf_rating.rating_decision_id_seq')::text AS id")
                            .one_row()["id"]
                            .as<std::string>();
        const std::optional<std::string> ai_advisory_json =
            decision.aiAdvisory.has_value() ? std::make_optional(decision.aiAdvisory->dump())
                                            : std::nullopt;
        // ADR-0388: the decision in the time-partitioned chf_rating.rating_decision ...
        txn.exec("INSERT INTO chf_rating.rating_decision (id, charging_data_ref, "
                 "subscriber_identifier, rating_group, input_snapshot, monetary_amount, currency, "
                 "tariff_id, tariff_version, rule_fired_id, ai_advisory) VALUES ($1,$2,$3,$4,"
                 "$5::jsonb,$6,$7,$8,$9,$10,$11::jsonb)",
                 pqxx::params{id,
                              decision.chargingDataRef,
                              decision.subscriberIdentifier,
                              decision.ratingGroup,
                              decision.inputSnapshot.dump(),
                              decision.ratedAmount,
                              decision.currency,
                              decision.tariffId,
                              decision.tariffVersion,
                              decision.ruleFiredId,
                              ai_advisory_json});
        // ... and its TMF678 AppliedCustomerBillingRate in its own table, same id. Disclosed
        // simplification kept from the per-service store: taxExcluded == taxIncluded (no tax
        // engine), isBilled false until a bill run marks it.
        if (decision.acbrType.has_value()) {
            txn.exec("INSERT INTO chf_rating.applied_customer_billing_rate (id, rate_type, "
                     "is_billed, description, product_id, tax_excluded_unit, tax_excluded_amount, "
                     "tax_included_unit, tax_included_amount) VALUES ($1,$2,false,$3,$4,$5,$6,$5,"
                     "$6)",
                     pqxx::params{id,
                                  decision.acbrType,
                                  "rating decision " + id,
                                  decision.tariffId,
                                  decision.currency,
                                  decision.ratedAmount});
        }
        const std::optional<std::string> ai_advisory_ref =
            decision.aiAdvisory.has_value() && decision.aiAdvisory->contains("model_version")
                ? std::make_optional(decision.aiAdvisory->at("model_version").get<std::string>())
                : std::nullopt;
        nlohmann::json detail{{"entityType", "RATING_DECISION"},
                              {"entityId", id},
                              {"afterSnapshot", decision.inputSnapshot}};
        if (ai_advisory_ref.has_value()) {
            detail["aiAdvisoryRef"] = *ai_advisory_ref;
        }
        txn.exec("INSERT INTO chf_rating.audit_record (actor, action, detail) VALUES "
                 "('chf','ratingDecision.record',$1::jsonb)",
                 pqxx::params{detail.dump()});

        txn.commit();
    } catch (const std::exception& e) {
        // Best-effort, same discipline as CdrWriter::write -- a rating-decision audit-write
        // failure must never block or fail the real charging response CHF already committed to.
        spdlog::warn(
            "chf: RatingDecision write failed for tariffId={}: {}", decision.tariffId, e.what());
    }
}

} // namespace chf
