#include "tools.hpp"

#include "sbi_core/http2_client.hpp"

#include <spdlog/spdlog.h>

#include <ctime>

#include "analytics.hpp"
#include "cdr.hpp"
#include "rating_decision_store.hpp"

namespace mcp {
namespace {

using nlohmann::json;

std::string cfg_str(const ToolContext& ctx, const char* key) {
    return ctx.config.contains(key) && ctx.config.at(key).is_string()
               ? ctx.config.at(key).get<std::string>()
               : std::string{};
}

// One place where every backend GET happens, so the failure shape is identical for every tool: a
// backend that is down produces `ok=false` with the reason, never an empty object that a caller
// could read as "this subscriber has no balance".
ToolResult backend_get(ToolContext& ctx, const std::string& url, const char* what) {
    ToolResult out;
    if (ctx.client == nullptr) {
        out.error = "no HTTP client configured";
        return out;
    }
    sbi_core::http2::ClientRequest req;
    req.method = "GET";
    req.url = url;
    auto resp = ctx.client->send(req);
    if (!resp.has_value()) {
        out.error = std::string("could not reach ") + what;
        return out;
    }
    if (resp->status == 404) {
        out.error = std::string("no such ") + what;
        return out;
    }
    if (resp->status < 200 || resp->status >= 300) {
        out.error = std::string(what) + " returned HTTP " + std::to_string(resp->status);
        return out;
    }
    try {
        out.content = json::parse(resp->body);
    } catch (const std::exception& e) {
        out.error = std::string("malformed response from ") + what + ": " + e.what();
        return out;
    }
    out.ok = true;
    return out;
}

std::string arg_str(const json& args, const char* key) {
    return args.contains(key) && args.at(key).is_string() ? args.at(key).get<std::string>()
                                                          : std::string{};
}

} // namespace

namespace {
// Shared by both analytics tools: the subscriber's own recorded usage over a window, newest last.
std::vector<mcp::UsagePoint>
usage_points(ToolContext& ctx, const std::string& subscriber, int days) {
    std::vector<mcp::UsagePoint> points;
    if (ctx.cdrs == nullptr) {
        return points;
    }
    const auto now = std::time(nullptr);
    const auto from = now - static_cast<std::time_t>(days) * 86400;
    auto fmt = [](std::time_t t) {
        std::tm tm{};
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", gmtime_r(&t, &tm));
        return std::string(buf);
    };
    chf::CdrWriter::CdrQuery q;
    q.period_start = fmt(from);
    q.period_end = fmt(now + 1);
    q.subscriber_identifier = subscriber;
    for (const auto& c : ctx.cdrs->query(q)) {
        // Both fields are optional on a CdrRecord. An absent value means "not measured", which
        // contributes nothing to a rate or a baseline -- treating it as zero is correct here and
        // is NOT the same as inventing a zero-usage session.
        points.push_back(
            mcp::UsagePoint{.at_unix_sec = static_cast<std::int64_t>(c.invocation_time_stamp),
                            .used_octets = static_cast<double>(c.used_total_volume.value_or(0)),
                            .spend = c.reserved_cost.value_or(0.0)});
    }
    return points;
}
} // namespace

std::vector<Tool> build_tool_registry() {
    std::vector<Tool> tools;

    // ---- PII: the subscriber's own balance -------------------------------------------------
    tools.push_back(Tool{
        .name = "get_balance",
        .description = "Remaining balance and bucket details for one subscriber (TMF654). "
                       "Returns the subscriber's own allowance only.",
        .input_schema = json{{"type", "object"},
                             {"properties",
                              json{{"subscriberId",
                                    json{{"type", "string"},
                                         {"description", "SUPI, e.g. imsi-999700000000001"}}}}},
                             {"required", json::array({"subscriberId"})}},
        .returns_pii = true,
        .subject_arg = "subscriberId",
        .invoke = [](const json& args, ToolContext& ctx) {
            const auto subject = arg_str(args, "subscriberId");
            auto r = backend_get(ctx,
                                 cfg_str(ctx, "balance_management_base_url") +
                                     "/tmf-api/prepayBalanceManagement/v4/bucket"
                                     "?relatedParty.id=" +
                                     subject,
                                 "balance management");
            if (r.ok) {
                r.field_classes = {"subscriber_id", "balance", "bucket_validity"};
            }
            return r;
        }});

    // ---- PII: why a charge happened ---------------------------------------------------------
    //
    // This is the tool that makes a customer agent honest. "Why was I charged this?" is answerable
    // from a record -- tariff, version, the rule that fired, and the AI advisory if a model
    // influenced the grant -- rather than from a language model's reconstruction of a bill.
    tools.push_back(Tool{
        .name = "explain_charge",
        .description = "The recorded rating decision behind one charge: tariff, tariff version, "
                       "the offering/price rule that fired, and the AI advisory if a model "
                       "influenced it. An explanation from records, not a reconstruction.",
        .input_schema = json{{"type", "object"},
                             {"properties",
                              json{{"subscriberId", json{{"type", "string"}}},
                                   {"chargingDataRef", json{{"type", "string"}}}}},
                             {"required", json::array({"subscriberId", "chargingDataRef"})}},
        .returns_pii = true,
        .subject_arg = "subscriberId",
        .invoke = [](const json& args, ToolContext& ctx) {
            ToolResult r;
            if (ctx.rating_decisions == nullptr) {
                r.error = "rating-decision store not configured";
                return r;
            }
            const auto ref = arg_str(args, "chargingDataRef");
            const auto decisions = ctx.rating_decisions->find_by_charging_data_ref(ref);
            if (decisions.empty()) {
                // Explicitly NOT an empty success. "No decision was recorded for this
                // reference" and "this charge had no reason" are different statements, and an
                // agent handed an empty object would be free to narrate the second.
                r.error = "no rating decision recorded for " + ref;
                return r;
            }
            r.ok = true;
            r.content = json{{"chargingDataRef", ref}, {"decisions", decisions}};
            r.field_classes = {"subscriber_id", "tariff", "rated_amount", "ai_advisory"};
            return r;
        }});

    // ---- PII: recent usage ------------------------------------------------------------------
    tools.push_back(Tool{
        .name = "get_recent_charges",
        .description = "Recent completed charging records for one subscriber, most recent first.",
        .input_schema = json{{"type", "object"},
                             {"properties",
                              json{{"subscriberId", json{{"type", "string"}}},
                                   {"lookbackDays",
                                    json{{"type", "integer"},
                                         {"minimum", 1},
                                         {"maximum", 365},
                                         {"default", 30}}}}},
                             {"required", json::array({"subscriberId"})}},
        .returns_pii = true,
        .subject_arg = "subscriberId",
        .invoke = [](const json& args, ToolContext& ctx) {
            ToolResult r;
            if (ctx.cdrs == nullptr) {
                r.error = "CDR store not configured";
                return r;
            }
            const int days =
                args.contains("lookbackDays") && args.at("lookbackDays").is_number_integer()
                    ? std::min(365, std::max(1, args.at("lookbackDays").get<int>()))
                    : 30;
            const auto now = std::time(nullptr);
            const auto from = now - static_cast<std::time_t>(days) * 86400;
            auto fmt = [](std::time_t t) {
                std::tm tm{};
                char buf[32];
                std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", gmtime_r(&t, &tm));
                return std::string(buf);
            };
            chf::CdrWriter::CdrQuery q;
            q.period_start = fmt(from);
            q.period_end = fmt(now + 1);
            q.subscriber_identifier = arg_str(args, "subscriberId");
            const auto rows = ctx.cdrs->query(q);
            json out = json::array();
            for (const auto& c : rows) {
                // Deliberately narrow. serving_plmn is location-adjacent -- it says which
                // country the subscriber was in -- and a "what did I spend" question never
                // needs it, so it is withheld and RECORDED as withheld.
                out.push_back(json{{"chargingDataRef", c.charging_data_ref},
                                   {"ratingGroup", c.rating_group},
                                   {"usedTotalVolume", c.used_total_volume},
                                   {"reservedCost", c.reserved_cost},
                                   {"currency", c.reserved_cost_currency},
                                   {"isRoaming", c.is_roaming}});
            }
            r.ok = true;
            r.content = json{{"subscriberId", arg_str(args, "subscriberId")},
                             {"lookbackDays", days},
                             {"charges", out}};
            r.field_classes = {"subscriber_id", "usage_volume", "spend", "roaming_flag"};
            r.withheld_classes = {"serving_plmn", "precise_location"};
            return r;
        }});

    // ---- PII: quota-exhaustion forecast (ADR-0334) ------------------------------------------
    tools.push_back(Tool{
        .name = "forecast_quota_exhaustion",
        .description = "Projects when this subscriber's remaining allowance runs out, from their "
                       "own observed burn rate. Reports the basis and sample count; says so "
                       "plainly when there is not enough history rather than guessing.",
        .input_schema =
            json{
                {"type", "object"},
                {"properties",
                 json{
                     {"subscriberId", json{{"type", "string"}}},
                     {"remainingOctets", json{{"type", "number"}}},
                     {"lookbackDays",
                      json{{"type", "integer"}, {"minimum", 1}, {"maximum", 90}, {"default", 7}}}}},
                {"required", json::array({"subscriberId", "remainingOctets"})}},
        .returns_pii = true,
        .subject_arg = "subscriberId",
        .invoke = [](const json& args, ToolContext& ctx) {
            ToolResult r;
            const int days =
                args.contains("lookbackDays") && args.at("lookbackDays").is_number_integer()
                    ? std::min(90, std::max(1, args.at("lookbackDays").get<int>()))
                    : 7;
            const double remaining =
                args.contains("remainingOctets") && args.at("remainingOctets").is_number()
                    ? args.at("remainingOctets").get<double>()
                    : 0.0;
            const auto points = usage_points(ctx, arg_str(args, "subscriberId"), days);
            const auto f = mcp::forecast_exhaustion(points, remaining);
            r.ok = true;
            r.content =
                json{{"computable", f.computable},
                     {"reason", f.reason},
                     {"basis", "observed burn rate over this subscriber's own CDRs"},
                     {"samples", f.samples},
                     {"observedSpanSeconds", f.observed_span_sec},
                     {"burnOctetsPerHour", f.burn_octets_per_hour},
                     {"remainingOctets", f.remaining_octets},
                     {"hoursRemaining", f.computable ? json(f.hours_remaining) : json(nullptr)},
                     {"lookbackDays", days}};
            r.field_classes = {"subscriber_id", "usage_volume", "balance"};
            return r;
        }});

    // ---- PII: spend anomaly (ADR-0334) ------------------------------------------------------
    tools.push_back(Tool{
        .name = "detect_spend_anomaly",
        .description = "Compares this subscriber's most recent spend against their OWN earlier "
                       "history. Per-subscriber baseline, so no population model and no shared "
                       "bias. One-sided: only unusually HIGH spend is flagged.",
        .input_schema =
            json{
                {"type", "object"},
                {"properties",
                 json{{"subscriberId", json{{"type", "string"}}},
                      {"lookbackDays",
                       json{{"type", "integer"}, {"minimum", 1}, {"maximum", 90}, {"default", 30}}},
                      {"zThreshold", json{{"type", "number"}, {"default", 3.0}}}}},
                {"required", json::array({"subscriberId"})}},
        .returns_pii = true,
        .subject_arg = "subscriberId",
        .invoke = [](const json& args, ToolContext& ctx) {
            ToolResult r;
            const int days =
                args.contains("lookbackDays") && args.at("lookbackDays").is_number_integer()
                    ? std::min(90, std::max(1, args.at("lookbackDays").get<int>()))
                    : 30;
            // The sensitivity is the operator's commercial choice, not this code's: what
            // counts as bill shock differs by market and by tariff.
            const double z = args.contains("zThreshold") && args.at("zThreshold").is_number()
                                 ? args.at("zThreshold").get<double>()
                                 : 3.0;
            const auto points = usage_points(ctx, arg_str(args, "subscriberId"), days);
            const auto a = mcp::detect_spend_anomaly(points, z);
            r.ok = true;
            r.content = json{{"computable", a.computable},
                             {"reason", a.reason},
                             {"basis", "this subscriber's own spend history"},
                             {"isAnomalous", a.is_anomalous},
                             {"recentSpend", a.recent_spend},
                             {"baselineMean", a.baseline_mean},
                             {"baselineStdDev", a.baseline_stddev},
                             {"zScore", a.z_score},
                             {"zThreshold", z},
                             {"baselineSamples", a.baseline_samples}};
            r.field_classes = {"subscriber_id", "spend"};
            return r;
        }});

    // ---- Non-PII: the catalog ---------------------------------------------------------------
    tools.push_back(
        Tool{.name = "get_offering",
             .description = "A product offering and its prices from the catalog (TMF620). Operator "
                            "domain data; no subscriber involved.",
             .input_schema = json{{"type", "object"},
                                  {"properties", json{{"offeringId", json{{"type", "string"}}}}},
                                  {"required", json::array({"offeringId"})}},
             .returns_pii = false,
             .invoke = [](const json& args, ToolContext& ctx) {
                 return backend_get(ctx,
                                    cfg_str(ctx, "product_catalog_base_url") +
                                        "/tmf-api/productCatalogManagement/v4/productOffering/" +
                                        arg_str(args, "offeringId"),
                                    "product catalog");
             }});

    // ---- Non-PII: NF inventory (the ops-agent half that is NOT blocked on NWDAF) -------------
    tools.push_back(Tool{
        .name = "list_nf_instances",
        .description = "Registered network functions of a given type, from NRF. Live inventory "
                       "state; the 'is this abnormal?' analytics half needs NWDAF and does not "
                       "exist yet.",
        .input_schema =
            json{
                {"type", "object"},
                {"properties",
                 json{{"nfType", json{{"type", "string"}, {"description", "e.g. UDM, SMF, PCF"}}}}},
                {"required", json::array({"nfType"})}},
        .returns_pii = false,
        .invoke = [](const json& args, ToolContext& ctx) {
            return backend_get(ctx,
                               cfg_str(ctx, "nrf_base_url") +
                                   "/nnrf-disc/v1/nf-instances?target-nf-type=" +
                                   arg_str(args, "nfType") + "&requester-nf-type=NEF",
                               "NRF");
        }});

    return tools;
}

const Tool* find_tool(const std::vector<Tool>& tools, const std::string& name) {
    for (const auto& t : tools) {
        if (t.name == name) {
            return &t;
        }
    }
    return nullptr;
}

} // namespace mcp
