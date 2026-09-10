#include "tools.hpp"

#include "sbi_core/http2_client.hpp"

#include <spdlog/spdlog.h>

#include <ctime>

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
