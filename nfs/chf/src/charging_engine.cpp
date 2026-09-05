#include "charging_engine.hpp"

#include "sbi_core/datetime.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <nf_config/nf_config.hpp>

namespace chf {

namespace {

using nlohmann::json;

} // namespace

// Task #109 (ADR-0274): these two returned in-source literals
// (https://127.0.0.1:7785/7786) when their env var was unset. The env override stays -- it is how
// the existing integration tests point CHF at their own instances -- but the fallback is now
// config/chf.json, because the standing rule is that a deployment endpoint never lives in a .cpp
// literal. nf_config::require applies the env override itself, so the two paths collapse into one.
//
// Read once via a function-local static rather than per call: these sit on CHF's charging hot
// path (six call sites across offering lookup, reserve and adjust), and re-reading a JSON file per
// HTTP request would be a real cost for a value that cannot change while the process runs.
// main() touches both at startup (see its own comment) so a missing key fails fast at boot
// instead of surfacing mid-request.
namespace {

std::string load_peer_base(const char* key, const char* env_name) {
    const auto config = nf_config::load("chf", CONFIG_DIR);
    return nf_config::require<std::string>(config, key, env_name);
}

} // namespace

std::string product_catalog_base() {
    static const std::string base =
        load_peer_base("product_catalog_base_url", "CHF_PRODUCT_CATALOG_BASE");
    return base;
}

std::string balance_management_base() {
    static const std::string base =
        load_peer_base("balance_management_base_url", "CHF_BALANCE_MANAGEMENT_BASE");
    return base;
}

// ADR-0072 (gap-closure: real N40 product-configurability). Real TMF620 extension-point lookup:
// `prodSpecCharValueUse`/`productSpecCharacteristicValue` is the spec's own generic mechanism for
// vendor/domain-specific configurable attributes (this file's own header already establishes this
// project's use of it for S-NSSAI/5QI/SLA-tier-class characteristics) -- 3GPP charging concepts
// like `ratingGroup` and TS 32.291's own quota-policy fields have no NATIVE TMF620 field of their
// own (TMF620 doesn't know what a 3GPP rating group is), so this is the real, correct extension
// point for them, not a fabricated shortcut. Looked up by the characteristic's real `name` field
// (no existing precedent in this codebase for id-vs-name lookup convention before this function,
// `name` chosen as the human-readable, GUI-facing key a real operator/GUI would set).
std::optional<nlohmann::json> find_characteristic_value(
    const std::vector<bss_sid::ProductSpecificationCharacteristicValueUse>& characteristics,
    const std::string& name) {
    for (const auto& c : characteristics) {
        if (c.name.value_or("") != name || c.productSpecCharacteristicValue.empty()) {
            continue;
        }
        return c.productSpecCharacteristicValue.front().value;
    }
    return std::nullopt;
}

nlohmann::json
collect_charging_attributes(const sbi_gen::ChargingDataRequest_Nchf_ConvergedCharging& request,
                            const sbi_gen::MultipleUnitUsage_Nchf_ConvergedCharging& usage) {
    nlohmann::json attributes = nlohmann::json::object();
    attributes["ratingGroup"] = usage.ratingGroup;
    if (usage.uPFID.has_value()) {
        attributes["uPFID"] = *usage.uPFID;
    }
    if (request.subscriberIdentifier.has_value()) {
        attributes["subscriberIdentifier"] = *request.subscriberIdentifier;
    }
    if (request.tenantIdentifier.has_value()) {
        attributes["tenantIdentifier"] = *request.tenantIdentifier;
    }
    if (request.pDUSessionChargingInformation.has_value() &&
        request.pDUSessionChargingInformation->pduSessionInformation.has_value()) {
        const auto& pdu = *request.pDUSessionChargingInformation->pduSessionInformation;
        attributes["dnnId"] = pdu.dnnId;
        attributes["pduSessionID"] = pdu.pduSessionID;
        if (pdu.networkSlicingInfo.has_value()) {
            // The S-NSSAI as the spec shapes it ({sst, sd}), so a catalog scope can constrain the
            // whole thing or -- via an array of them -- a set of slices.
            attributes["sNSSAI"] = nlohmann::json(pdu.networkSlicingInfo->sNSSAI);
        }
        if (pdu.ratType.has_value()) {
            attributes["ratType"] = nlohmann::json(*pdu.ratType);
        }
        if (pdu.chargingCharacteristics.has_value()) {
            attributes["chargingCharacteristics"] = *pdu.chargingCharacteristics;
        }
        // hPlmnId vs servingCNPlmnId is what makes a session roaming or not, so both are exposed:
        // C5's roaming rating is a scope over these two rather than a separate feature.
        if (pdu.hPlmnId.has_value()) {
            attributes["hPlmnId"] = nlohmann::json(*pdu.hPlmnId);
        }
        if (pdu.servingCNPlmnId.has_value()) {
            attributes["servingCNPlmnId"] = nlohmann::json(*pdu.servingCNPlmnId);
        }
    }
    return attributes;
}

RatingResult build_rating_grant(sbi_core::http2::Client& catalog_client,
                                std::int64_t rating_group,
                                const std::string& supi,
                                AiQuotaSizer* ai_quota_sizer,
                                QuotaFeatureStore* quota_feature_store,
                                const nlohmann::json& attributes) {
    sbi_core::http2::ClientRequest offerings_req;
    offerings_req.method = "GET";
    offerings_req.url = product_catalog_base() + kProductCatalogApiRoot + "/productOffering";
    auto offerings_resp = catalog_client.send(offerings_req);
    if (!offerings_resp.has_value() || offerings_resp->status != 200) {
        spdlog::warn("chf: could not reach bss/product-catalog for rating, granting nothing");
        return {};
    }

    std::vector<bss_sid::ProductOffering> offerings;
    try {
        offerings = json::parse(offerings_resp->body).get<std::vector<bss_sid::ProductOffering>>();
    } catch (const json::exception& e) {
        spdlog::warn("chf: malformed ProductOffering list from product-catalog: {}", e.what());
        return {};
    }

    // Real match: the FIRST Active/isSellable offering whose price's own real `ratingGroup`
    // characteristic equals the request's ratingGroup -- see this function's own header comment
    // for why "first Active/isSellable, ratingGroup ignored" (this project's own earlier behavior)
    // was a real correctness gap, not a documented simplification.
    for (const auto& offering : offerings) {
        if (!offering.isSellable.value_or(false) ||
            offering.lifecycleStatus.value_or("") != "Active" ||
            offering.productOfferingPrice.empty()) {
            continue;
        }

        sbi_core::http2::ClientRequest price_req;
        price_req.method = "GET";
        price_req.url = product_catalog_base() + kProductCatalogApiRoot + "/productOfferingPrice/" +
                        offering.productOfferingPrice.front().id;
        auto price_resp = catalog_client.send(price_req);
        if (!price_resp.has_value() || price_resp->status != 200) {
            continue;
        }
        bss_sid::ProductOfferingPrice price;
        try {
            price = json::parse(price_resp->body).get<bss_sid::ProductOfferingPrice>();
        } catch (const json::exception&) {
            continue;
        }

        const auto rg_value = find_characteristic_value(price.prodSpecCharValueUse, "ratingGroup");
        if (!rg_value.has_value() || !rg_value->is_number_integer() ||
            rg_value->get<std::int64_t>() != rating_group) {
            continue;
        }

        // ADR-0303: the offering's own attribute scope, from the catalog. An offering priced for
        // one slice, one UPF or one visited PLMN is skipped for a request that does not match it,
        // so the NEXT matching offering is used instead -- which is what makes "10 GB on slice 1
        // OR 5 GB on slice 10" two real, separately-priced products rather than one.
        const auto scope = find_characteristic_value(price.prodSpecCharValueUse, "chargingScope");
        if (scope.has_value() && !charging_scope_matches(*scope, attributes)) {
            spdlog::debug("chf: ProductOfferingPrice {} matches ratingGroup {} but its "
                          "chargingScope does not match this request's attributes",
                          price.id.value_or(""),
                          rating_group);
            continue;
        }

        if (!price.unitOfMeasure.has_value() || !price.unitOfMeasure->amount.has_value()) {
            spdlog::info("chf: ProductOfferingPrice {} matches ratingGroup {} but has no "
                         "unitOfMeasure, granting nothing",
                         *price.id,
                         rating_group);
            return {};
        }

        RatingResult result;
        sbi_gen::GrantedUnit grant{};
        const auto amount = *price.unitOfMeasure->amount;
        const auto units = price.unitOfMeasure->units.value_or("");
        if (units == "GB") {
            grant.totalVolume = static_cast<std::uint64_t>(amount * 1'000'000'000.0);
        } else if (units == "MB") {
            grant.totalVolume = static_cast<std::uint64_t>(amount * 1'000'000.0);
        } else {
            grant.serviceSpecificUnits = static_cast<std::uint64_t>(amount);
        }
        result.grant = grant;
        result.cost = price.price;
        result.tariffId = price.id;
        result.tariffVersion = price.version;
        result.offeringName = offering.name;
        result.priceName = price.name;

        if (const auto v = find_characteristic_value(price.prodSpecCharValueUse, "validityTime");
            v.has_value() && v->is_number_integer()) {
            result.validityTimeSec = v->get<std::int64_t>();
        }
        if (const auto v =
                find_characteristic_value(price.prodSpecCharValueUse, "quotaHoldingTime");
            v.has_value() && v->is_number_integer()) {
            result.quotaHoldingTimeSec = v->get<std::int64_t>();
        }
        if (const auto v =
                find_characteristic_value(price.prodSpecCharValueUse, "volumeQuotaThreshold");
            v.has_value() && v->is_number_integer()) {
            result.volumeQuotaThreshold = v->get<std::int64_t>();
        }
        if (const auto v =
                find_characteristic_value(price.prodSpecCharValueUse, "timeQuotaThreshold");
            v.has_value() && v->is_number_integer()) {
            result.timeQuotaThreshold = v->get<std::int64_t>();
        }
        if (const auto v =
                find_characteristic_value(price.prodSpecCharValueUse, "unitQuotaThreshold");
            v.has_value() && v->is_number_integer()) {
            result.unitQuotaThreshold = v->get<std::int64_t>();
        }

        // P4.8 (CHARGING_PROMPT.md Angle 1a, ADR-0074): predictive quota sizing. Real, disclosed
        // scope: only totalVolume (GB/MB) grants are AI-adjustable -- serviceSpecificUnits has no
        // meaningful "predicted usage" quantity to compare against in this project's own schema.
        // "This model informs the decision. The deterministic rating engine makes it." -- the
        // model below only ever SUGGESTS a usage figure; the actual grant is always the
        // price-configured base multiplied by a clamp to [0.5x, 2.0x], never the raw prediction.
        if (grant.totalVolume.has_value() && *grant.totalVolume > 0 && !supi.empty() &&
            ai_quota_sizer != nullptr && ai_quota_sizer->is_enabled() &&
            quota_feature_store != nullptr) {
            if (const auto snapshot = quota_feature_store->get(supi, rating_group);
                snapshot.has_value() && !snapshot->recentUsedVolumes.empty()) {
                QuotaSizingFeatures features{};
                double sum = 0.0;
                for (const auto v : snapshot->recentUsedVolumes) {
                    sum += v;
                }
                features[0] = sum / static_cast<double>(snapshot->recentUsedVolumes.size());
                features[1] = snapshot->recentUsedVolumes.size() >= 2
                                  ? snapshot->recentUsedVolumes[0] - snapshot->recentUsedVolumes[1]
                                  : 0.0;
                const auto now_unix = static_cast<std::int64_t>(
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count());
                features[2] = snapshot->lastInvocationUnixSec.has_value()
                                  ? static_cast<double>(now_unix - *snapshot->lastInvocationUnixSec)
                                  : 0.0;
                features[3] = snapshot->lastGrantedTotalVolume.value_or(0.0);

                if (const auto predicted = ai_quota_sizer->predict(features);
                    predicted.has_value()) {
                    const double base = static_cast<double>(*grant.totalVolume);
                    const double raw_multiplier = *predicted / base;
                    const double multiplier = std::clamp(raw_multiplier, 0.5, 2.0);
                    grant.totalVolume = static_cast<std::uint64_t>(base * multiplier);
                    result.grant = grant;
                    result.aiAdvisory = json{
                        {"model_version", ai_quota_sizer->model_version()},
                        {"features",
                         json{{kQuotaSizingFeatureNames[0], features[0]},
                              {kQuotaSizingFeatureNames[1], features[1]},
                              {kQuotaSizingFeatureNames[2], features[2]},
                              {kQuotaSizingFeatureNames[3], features[3]}}},
                        {"predicted_usage_octets", *predicted},
                        {"base_grant_octets", base},
                        {"raw_multiplier", raw_multiplier},
                        {"applied_multiplier", multiplier},
                        {"clamped_low", raw_multiplier < 0.5},
                        {"clamped_high", raw_multiplier > 2.0},
                        {"deterministic_bound", "[0.5x, 2.0x] of price-configured grant"},
                    };
                    spdlog::info("chf: AI quota sizing adjusted grant for SUPI={} ratingGroup={} "
                                 "by {:.3f}x (raw {:.3f}x, base {} octets -> {} octets)",
                                 supi,
                                 rating_group,
                                 multiplier,
                                 raw_multiplier,
                                 static_cast<std::uint64_t>(base),
                                 *grant.totalVolume);
                }
            }
        }

        spdlog::info(
            "chf: rating engine granted {} from ProductOffering '{}' / ProductOfferingPrice "
            "'{}' (ratingGroup={})",
            units == "GB" || units == "MB"
                ? std::to_string(*grant.totalVolume) + " octets"
                : std::to_string(*grant.serviceSpecificUnits) + " service-specific units",
            offering.name.value_or(""),
            price.name.value_or(""),
            rating_group);
        return result;
    }

    spdlog::info(
        "chf: no Active/isSellable ProductOfferingPrice configures ratingGroup {}, granting "
        "nothing this call",
        rating_group);
    return {};
}

bool reserve_subscriber_balance(sbi_core::http2::Client& balance_client,
                                const std::string& supi,
                                const bss_sid::Money& cost,
                                const std::string& description) {
    if (!cost.value.has_value() || *cost.value <= 0.0) {
        // A real, valid TMF620 state (price with no monetary value, or a zero-cost promotional
        // price) -- nothing to reserve, not a failure.
        return true;
    }

    bss_sid::ReserveBalance reserve_req{};
    bss_sid::Quantity amount{};
    amount.amount = *cost.value;
    amount.units = cost.unit;
    reserve_req.amount = amount;
    bss_sid::BucketRef bucket{};
    bucket.id = supi;
    reserve_req.bucket = bucket;
    reserve_req.description = description;
    reserve_req.usageType = "monetary";

    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = balance_management_base() + kBalanceManagementApiRoot + "/reserveBalance";
    req.headers.emplace("content-type", "application/json");
    req.body = json(reserve_req).dump();

    auto resp = balance_client.send(req);
    if (!resp.has_value() || resp->status != 201) {
        spdlog::warn("chf: reserveBalance call to bss/balance-management failed (SUPI={})", supi);
        return false;
    }
    try {
        const auto result = json::parse(resp->body).get<bss_sid::ReserveBalance>();
        return result.status.value_or("") == "completed";
    } catch (const json::exception& e) {
        spdlog::warn("chf: malformed ReserveBalance response: {}", e.what());
        return false;
    }
}

// Real bug found and fixed via live verification, not caught by unit-level reasoning alone:
// bss/balance-management's `ReserveBalance` (positive amount) moves money remainingValue ->
// reservedValue; `AdjustBalance` only ever touches remainingValue directly and its own atomic
// floor check (`remaining_value + amount >= 0`) has no visibility into reservedValue at all.
// Neither operation alone can "commit" a reservation (decrease reservedValue permanently WITHOUT
// crediting remainingValue back first) -- TMF654's own modeled resource set (this project's
// scope, see bss_sid/balance.hpp) has no dedicated "commit" action. The correct way to compose
// the two real primitives into a net "commit" is ORDER-DEPENDENT: unreserve FIRST (ReserveBalance,
// negative amount -- returns the money to remainingValue, always succeeds since exactly this much
// is known to be reserved), THEN debit (AdjustBalance, negative amount -- now succeeds, since the
// money is back in remainingValue) -- net effect: reservedValue permanently decreases by the full
// amount, remainingValue is unchanged (temporarily credited then immediately re-debited the same
// amount). Doing this in the OPPOSITE order (debit before unreserve, this function's first,
// buggy version) makes the debit's own atomic floor check fail (the money is still sitting in
// reservedValue, not yet in remainingValue), so only the unreserve half succeeds -- silently
// refunding the entire reservation instead of committing it. Caught by this ADR's own real
// end-to-end live verification (a real $50 topup, $40 total reserved across Create+Update, then
// Release incorrectly returned the bucket to $50/$0 instead of the correct $10/$0) -- exactly the
// kind of bug this project's "live-verify over self-consistency" discipline exists to catch.
void finalize_subscriber_balance(sbi_core::http2::Client& balance_client,
                                 const std::string& supi,
                                 double total_reserved,
                                 double amount_to_debit,
                                 const std::string& description) {
    if (total_reserved <= 0.0) {
        return;
    }
    amount_to_debit = std::clamp(amount_to_debit, 0.0, total_reserved);

    bss_sid::BucketRef bucket{};
    bucket.id = supi;

    bss_sid::ReserveBalance unreserve_req{};
    bss_sid::Quantity unreserve_amount{};
    unreserve_amount.amount = -total_reserved;
    unreserve_req.amount = unreserve_amount;
    unreserve_req.bucket = bucket;
    unreserve_req.description = description + " (unreserve before commit)";
    unreserve_req.usageType = "monetary";

    sbi_core::http2::ClientRequest unreserve_http_req;
    unreserve_http_req.method = "POST";
    unreserve_http_req.url =
        balance_management_base() + kBalanceManagementApiRoot + "/reserveBalance";
    unreserve_http_req.headers.emplace("content-type", "application/json");
    unreserve_http_req.body = json(unreserve_req).dump();
    auto unreserve_resp = balance_client.send(unreserve_http_req);
    if (!unreserve_resp.has_value() || unreserve_resp->status != 201) {
        spdlog::warn("chf: finalize unreserve call failed (SUPI={})", supi);
        return;
    }

    // ADR-0297: the reservation was released in FULL above; only the consumed part is debited.
    if (amount_to_debit <= 0.0) {
        spdlog::info(
            "chf: finalize released {} with nothing consumed (SUPI={})", total_reserved, supi);
        return;
    }

    bss_sid::AdjustBalance adjust_req{};
    bss_sid::Quantity debit{};
    debit.amount = -amount_to_debit;
    adjust_req.amount = debit;
    adjust_req.bucket = bucket;
    adjust_req.adjustType = "oneTime";
    adjust_req.description = description + " (commit)";
    adjust_req.usageType = "monetary";

    sbi_core::http2::ClientRequest adjust_http_req;
    adjust_http_req.method = "POST";
    adjust_http_req.url = balance_management_base() + kBalanceManagementApiRoot + "/adjustBalance";
    adjust_http_req.headers.emplace("content-type", "application/json");
    adjust_http_req.body = json(adjust_req).dump();
    auto adjust_resp = balance_client.send(adjust_http_req);
    if (!adjust_resp.has_value() || adjust_resp->status != 201) {
        // Real, disclosed gap: the unreserve above already succeeded, so if this commit debit
        // fails (e.g. balance-management becomes unreachable mid-finalize), the money is left
        // sitting back in remainingValue, uncommitted -- not a lost-money bug (the ledger still
        // has both real action records to reconcile from), but not a fully atomic two-call
        // sequence either. A real distributed-transaction/outbox mechanism would close this;
        // not built here, disclosed rather than silently assumed safe.
        spdlog::warn("chf: finalize commit AdjustBalance call failed (SUPI={})", supi);
    }
}

void write_converged_charging_cdr(chf::CdrWriter& cdr_writer,
                                  const std::string& ref,
                                  const std::string& operation,
                                  const std::string& supi,
                                  const std::string& node_functionality,
                                  const std::string& recording_network_function_id,
                                  std::int64_t invocation_sequence_number,
                                  const sbi_gen::MultipleUnitUsage_Nchf_ConvergedCharging& usage,
                                  const RatingResult& rating,
                                  bool reserved,
                                  std::optional<std::time_t> invocation_time_stamp) {
    chf::CdrRecord cdr{};
    cdr.charging_data_ref = ref;
    cdr.invocation_sequence_number = invocation_sequence_number;
    cdr.service_type = "ConvergedCharging";
    cdr.operation = operation;
    cdr.subscriber_identifier = supi;
    cdr.nf_consumer_node_functionality = node_functionality;
    cdr.recording_network_function_id = recording_network_function_id;
    cdr.rating_group = static_cast<std::int64_t>(usage.ratingGroup);
    if (reserved && rating.grant.has_value()) {
        cdr.granted_total_volume = rating.grant->totalVolume;
        cdr.granted_service_specific_units = rating.grant->serviceSpecificUnits;
    }
    if (usage.usedUnitContainer.has_value() && !usage.usedUnitContainer->empty()) {
        cdr.used_total_volume = usage.usedUnitContainer->front().totalVolume;
    }
    if (reserved && rating.cost.has_value()) {
        cdr.reserved_cost = rating.cost->value;
        cdr.reserved_cost_currency = rating.cost->unit;
    }
    // Real TS 32.291 `invocationTimeStamp` the consumer actually sent, when the caller has it.
    // This used to be unconditionally system_clock::now(), which silently overwrote a real 3GPP
    // field with CHF's own write time -- a genuine defect: `recorded_at` (schema.doris.sql) is
    // already the write-time column, so the two were duplicating each other while the real event
    // time was lost. The now() path survives only as the disclosed fallback for callers with no
    // consumer-supplied event time (Diameter Gy, CAP -- see this function's header comment).
    cdr.invocation_time_stamp = invocation_time_stamp.value_or(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    // Real, disclosed gap: a Doris write failure does not block or fail the real charging
    // response CHF already committed to (the balance reservation above already happened) -- CDR
    // generation is best-effort in this build, matching this project's existing "no real
    // distributed-transaction guarantee across CHF's several real dependencies" disclosure (see
    // finalize_subscriber_balance's own comment for the balance-management analogue). ADR-0192:
    // CdrWriter::write() itself already catches every real Doris error surface and logs a
    // warning -- it never throws, so no try/catch is needed here (unlike the pre-migration
    // version, whose own client could throw mid-insert; see ADR-0192).
    cdr_writer.write(cdr);
}

void write_rating_decision(chf::RatingDecisionStore& rating_decision_store,
                           const std::string& ref,
                           const sbi_gen::MultipleUnitUsage_Nchf_ConvergedCharging& usage,
                           const RatingResult& rating,
                           bool reserved) {
    if (!rating.tariffId.has_value()) {
        return;
    }
    chf::RatingDecisionRecord decision{};
    decision.tariffId = *rating.tariffId;
    decision.tariffVersion = rating.tariffVersion;
    decision.ratingGroup = static_cast<std::int64_t>(usage.ratingGroup);
    // Real, disclosed gap (schema.postgres.sql's own header): balance-at-decision-time is not
    // captured here -- would need an extra bss/balance-management call on every rating decision,
    // not added this pass.
    decision.inputSnapshot = {
        {"chargingDataRef", ref},
        {"ratingGroup", usage.ratingGroup},
        {"offeringName", rating.offeringName.value_or("")},
        {"priceName", rating.priceName.value_or("")},
        {"reserved", reserved},
        {"timestamp", sbi_core::format_rfc3339(std::chrono::system_clock::now())},
    };
    if (reserved && rating.cost.has_value()) {
        decision.ratedAmount = rating.cost->value;
        decision.currency = rating.cost->unit;
    }
    decision.ruleFiredId = *rating.tariffId;
    decision.acbrType = "appliedBillingCharge";
    decision.aiAdvisory = rating.aiAdvisory;
    rating_decision_store.record(decision);
}

ChargeUsageResult
charge_one_usage(sbi_core::http2::Client& catalog_client,
                 sbi_core::http2::Client& balance_client,
                 chf::CdrWriter& cdr_writer,
                 chf::RatingDecisionStore& rating_decision_store,
                 chf::ChargingDataStore& charging_data_store,
                 opentelemetry::metrics::Counter<std::uint64_t>* grant_counter,
                 opentelemetry::metrics::Counter<std::uint64_t>* reserve_rejected_counter,
                 const std::string& ref,
                 const std::string& operation,
                 const std::string& supi,
                 const std::string& node_functionality,
                 const std::string& recording_network_function_id,
                 std::int64_t invocation_sequence_number,
                 const sbi_gen::MultipleUnitUsage_Nchf_ConvergedCharging& usage,
                 chf::AiQuotaSizer* ai_quota_sizer,
                 chf::QuotaFeatureStore* quota_feature_store,
                 std::optional<std::time_t> invocation_time_stamp,
                 const nlohmann::json& attributes) {
    ChargeUsageResult result;
    result.rating = build_rating_grant(catalog_client,
                                       static_cast<std::int64_t>(usage.ratingGroup),
                                       supi,
                                       ai_quota_sizer,
                                       quota_feature_store,
                                       attributes);

    if (result.rating.cost.has_value() && !supi.empty()) {
        result.reserved =
            reserve_subscriber_balance(balance_client,
                                       supi,
                                       *result.rating.cost,
                                       "Nchf_ConvergedCharging_" + operation + " " + ref);
        if (result.reserved) {
            charging_data_store.add_reserved(ref, *result.rating.cost->value);
            // ADR-0297: record what those units WERE, in the same place and at the same moment as
            // the money. Release cannot compute a consumed fraction from a monetary total alone,
            // and reconstructing the grant later would mean re-rating against a catalog that may
            // have changed since -- so it is captured here, at the one point where grant and cost
            // are known together.
            if (result.rating.grant.has_value()) {
                if (result.rating.grant->totalVolume.has_value()) {
                    charging_data_store.add_granted_volume(
                        ref, static_cast<double>(*result.rating.grant->totalVolume));
                }
                if (result.rating.grant->serviceSpecificUnits.has_value()) {
                    charging_data_store.add_granted_service_units(
                        ref, static_cast<double>(*result.rating.grant->serviceSpecificUnits));
                }
            }
        } else if (reserve_rejected_counter != nullptr) {
            reserve_rejected_counter->Add(1);
        }
    }
    if (result.reserved && result.rating.grant.has_value() && grant_counter != nullptr) {
        grant_counter->Add(1);
    }

    write_converged_charging_cdr(cdr_writer,
                                 ref,
                                 operation,
                                 supi,
                                 node_functionality,
                                 recording_network_function_id,
                                 invocation_sequence_number,
                                 usage,
                                 result.rating,
                                 result.reserved,
                                 invocation_time_stamp);
    write_rating_decision(rating_decision_store, ref, usage, result.rating, result.reserved);

    // P4.8 (ADR-0074): record this request's own real reported usage as history for the NEXT
    // prediction -- only when usage was actually reported (Create has none yet, real TS 32.291
    // state) and a QuotaFeatureStore was actually supplied (Diameter/CAP callers pass nullptr,
    // see this function's own header comment).
    if (quota_feature_store != nullptr && !supi.empty() && usage.usedUnitContainer.has_value() &&
        !usage.usedUnitContainer->empty()) {
        const auto now_unix =
            static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
        std::optional<double> granted_total_volume;
        if (result.reserved && result.rating.grant.has_value() &&
            result.rating.grant->totalVolume.has_value()) {
            granted_total_volume = static_cast<double>(*result.rating.grant->totalVolume);
        }
        quota_feature_store->record_usage(
            supi,
            static_cast<std::int64_t>(usage.ratingGroup),
            static_cast<double>(usage.usedUnitContainer->front().totalVolume.value_or(0)),
            granted_total_volume,
            now_unix);
    }

    return result;
}

sbi_gen::SpendingLimitStatus
build_spending_limit_status(const sbi_gen::SpendingLimitContext& context,
                            PolicyCounterConfigStore& config_store) {
    sbi_gen::SpendingLimitStatus status{};
    status.supi = context.supi;
    status.notifId = context.notifId;
    status.expiry = context.expiry;
    status.supportedFeatures = context.supportedFeatures;

    json status_infos = json::object();
    if (context.policyCounterIds.has_value()) {
        for (const auto& counter_id : *context.policyCounterIds) {
            sbi_gen::PolicyCounterInfo info{};
            info.policyCounterId = counter_id;
            info.currentStatus = config_store.get_status(counter_id).value_or("unknown");
            status_infos[counter_id] = info;
        }
    }
    status.statusInfos = status_infos;
    return status;
}

} // namespace chf
