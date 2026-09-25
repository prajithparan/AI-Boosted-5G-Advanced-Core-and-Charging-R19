#include "store.hpp"

#include "sbi_core/datetime.hpp"

#include <nlohmann/json.hpp>

#include <map>

// ADR-0384: TMF620 ProductOffering / ProductOfferingPrice / ProductSpecification persisted in the
// NORMALIZED product_catalog schema of the consolidated charging DB
// (deploy/db/charging/30-product.sql made lossless by 31-product-lossless.sql), replacing the
// per-service JSONB rows.
//
// Round-trip contract: whatever the API accepted comes back field for field, lists in posted order
// (every child row carries `ordinal`). Two deliberate canonicalisations, both disclosed in
// ADR-0384:
//   * date-times (lastUpdate, validFor) are stored as TIMESTAMPTZ and returned as UTC RFC 3339 with
//     milliseconds ("2026-01-02T03:04:05.000Z"), the format sbi_core::format_rfc3339 emits;
//   * an object whose members are all absent (e.g. `"price": {}`) comes back absent.
//
// Reads are set-based: list() issues a fixed number of queries (one per table), never one per
// entity -- the CHF fetches the whole offering list on every rating decision.
//
// Every statement is schema-qualified: the charging DB is shared by several NFs' schemas.

namespace product_catalog {

namespace {

using nlohmann::json;

constexpr const char* kActor = "bss/product-catalog";

// SQL expression rendering a TIMESTAMPTZ column as sbi_core::format_rfc3339 does.
std::string ts(const std::string& col) {
    return "to_char(" + col + " AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"')";
}

// Validates an RFC 3339 date-time from the API and returns it canonicalised for a TIMESTAMPTZ
// parameter; absent stays absent. A malformed value is a client error, never silently dropped.
std::optional<std::string> ts_in(const std::optional<std::string>& v, const char* field) {
    if (!v.has_value()) {
        return std::nullopt;
    }
    const auto tp = sbi_core::parse_rfc3339(*v);
    if (!tp.has_value()) {
        throw InvalidRequest(std::string(field) + " is not an RFC 3339 date-time");
    }
    return sbi_core::format_rfc3339(*tp);
}

std::optional<std::string> vf_start(const std::optional<bss_sid::TimePeriod>& vf, const char* f) {
    return vf.has_value() ? ts_in(vf->startDateTime, f) : std::nullopt;
}
std::optional<std::string> vf_end(const std::optional<bss_sid::TimePeriod>& vf, const char* f) {
    return vf.has_value() ? ts_in(vf->endDateTime, f) : std::nullopt;
}

template <typename Row>
std::optional<bss_sid::TimePeriod> vf_out(const Row& row, const char* start, const char* end) {
    auto s = row[start].template as<std::optional<std::string>>();
    auto e = row[end].template as<std::optional<std::string>>();
    if (!s && !e) {
        return std::nullopt;
    }
    return bss_sid::TimePeriod{std::move(s), std::move(e)};
}

template <typename T> std::optional<std::string> json_array_or_null(const std::vector<T>& v) {
    return v.empty() ? std::nullopt : std::optional<std::string>(json(v).dump());
}
template <typename T, typename Row> std::vector<T> json_array_out(const Row& row, const char* col) {
    const auto raw = row[col].template as<std::optional<std::string>>();
    if (!raw.has_value()) {
        return {};
    }
    const auto j = json::parse(*raw);
    // 30-product.sql described attachment as a single object; accept one if a row holds it.
    return j.is_array() ? j.template get<std::vector<T>>() : std::vector<T>{j.template get<T>()};
}

template <typename Row> std::optional<std::string> s(const Row& row, const char* col) {
    return row[col].template as<std::optional<std::string>>();
}

template <typename Row>
std::optional<bss_sid::Quantity>
quantity_out(const Row& row, const char* amount, const char* units) {
    auto a = row[amount].template as<std::optional<double>>();
    auto u = s(row, units);
    if (!a && !u) {
        return std::nullopt;
    }
    return bss_sid::Quantity{a, std::move(u)};
}

// A relational constraint violation during a write is the caller's fault; name it for the API.
[[noreturn]] void rethrow_as_request_error(const pqxx::sql_error& e) {
    if (dynamic_cast<const pqxx::foreign_key_violation*>(&e) != nullptr) {
        // Report the offending key ("Key (price_id)=(x) is not present ..."), not table names.
        const std::string what = e.what();
        const auto k = what.find("Key (");
        const auto end = k == std::string::npos ? k : what.find(" is not present", k);
        throw InvalidRequest(
            "a referenced entity does not exist" +
            (end == std::string::npos ? std::string() : ": " + what.substr(k, end - k)));
    }
    if (dynamic_cast<const pqxx::unique_violation*>(&e) != nullptr) {
        throw InvalidRequest("a list contains the same reference twice");
    }
    if (dynamic_cast<const pqxx::not_null_violation*>(&e) != nullptr ||
        dynamic_cast<const pqxx::check_violation*>(&e) != nullptr) {
        throw InvalidRequest("a mandatory field is missing");
    }
    throw;
}

void write_audit(pqxx::work& txn,
                 const std::string& entity_type,
                 const std::string& entity_id,
                 const std::string& action,
                 const std::optional<std::string>& before,
                 const std::optional<std::string>& after) {
    const auto id = txn.exec("SELECT nextval('product_catalog.audit_record_id_seq')::text AS id")
                        .one_row()["id"]
                        .as<std::string>();
    txn.exec("INSERT INTO product_catalog.audit_record (id, entity_type, entity_id, action, actor, "
             "before_snapshot, after_snapshot) VALUES ($1,$2,$3,$4,$5,$6::jsonb,$7::jsonb)",
             pqxx::params{id, entity_type, entity_id, action, kActor, before, after});
}

// ---- CharacteristicValueSpecification (child of a spec characteristic or of a value-use) ------

void insert_value_specs(pqxx::work& txn,
                        const char* fk_col,
                        const std::string& fk,
                        const std::vector<bss_sid::CharacteristicValueSpecification>& values) {
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto& v = values[i];
        const auto uom = v.unitOfMeasure.value_or(bss_sid::Quantity{});
        txn.exec(std::string("INSERT INTO product_catalog.char_value_specification (") + fk_col +
                     ", ordinal, is_default, range_interval, regex, unit_of_measure_amount, "
                     "unit_of_measure_units, value_from, value_to, value_type, value, "
                     "valid_for_start, valid_for_end) VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,"
                     "$11::jsonb,$12::timestamptz,$13::timestamptz)",
                 pqxx::params{fk,
                              static_cast<int>(i),
                              v.isDefault,
                              v.rangeInterval,
                              v.regex,
                              uom.amount,
                              uom.units,
                              v.valueFrom,
                              v.valueTo,
                              v.valueType,
                              v.value.has_value() ? std::optional(v.value->dump()) : std::nullopt,
                              vf_start(v.validFor, "validFor.startDateTime"),
                              vf_end(v.validFor, "validFor.endDateTime")});
    }
}

bss_sid::CharacteristicValueSpecification value_spec_out(const pqxx::row_ref& r) {
    bss_sid::CharacteristicValueSpecification v;
    v.isDefault = r["is_default"].as<std::optional<bool>>();
    v.rangeInterval = s(r, "range_interval");
    v.regex = s(r, "regex");
    v.unitOfMeasure = quantity_out(r, "unit_of_measure_amount", "unit_of_measure_units");
    v.valueFrom = s(r, "value_from");
    v.valueTo = s(r, "value_to");
    v.valueType = s(r, "value_type");
    if (const auto raw = s(r, "value"); raw.has_value()) {
        v.value = json::parse(*raw);
    }
    v.validFor = vf_out(r, "vf_start", "vf_end");
    return v;
}

const std::string kValueSpecCols = "cvs.is_default, cvs.range_interval, cvs.regex, "
                                   "cvs.unit_of_measure_amount::float8 AS unit_of_measure_amount, "
                                   "cvs.unit_of_measure_units, cvs.value_from, cvs.value_to, "
                                   "cvs.value_type, cvs.value::text AS value, " +
                                   ts("cvs.valid_for_start") + " AS vf_start, " +
                                   ts("cvs.valid_for_end") + " AS vf_end";

// ---- ProductSpecificationCharacteristicValueUse (child of an offering or of a price) -----------

void insert_value_uses(
    pqxx::work& txn,
    const char* owner_col,
    const char* owner_tag,
    const std::string& owner_id,
    const std::vector<bss_sid::ProductSpecificationCharacteristicValueUse>& uses) {
    for (std::size_t i = 0; i < uses.size(); ++i) {
        const auto& u = uses[i];
        // Surrogate key: the TMF id is local to its owner (two prices may both use "ratingGroup").
        const std::string key = std::string(owner_tag) + ":" + owner_id + ":u" + std::to_string(i);
        const auto ref = u.productSpecification;
        txn.exec(std::string("INSERT INTO product_catalog.prod_spec_char_value_use (id, ") +
                     owner_col +
                     ", use_id, ordinal, name, description, value_type, min_cardinality, "
                     "max_cardinality, product_specification_id, product_specification_href, "
                     "product_specification_name, product_specification_version, "
                     "product_specification_target_schema, valid_for_start, valid_for_end) VALUES "
                     "($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15::timestamptz,"
                     "$16::timestamptz)",
                 pqxx::params{key,
                              owner_id,
                              u.id,
                              static_cast<int>(i),
                              u.name,
                              u.description,
                              u.valueType,
                              u.minCardinality,
                              u.maxCardinality,
                              ref ? std::optional(ref->id) : std::nullopt,
                              ref ? ref->href : std::nullopt,
                              ref ? ref->name : std::nullopt,
                              ref ? ref->version : std::nullopt,
                              ref ? ref->targetProductSchema : std::nullopt,
                              vf_start(u.validFor, "validFor.startDateTime"),
                              vf_end(u.validFor, "validFor.endDateTime")});
        insert_value_specs(txn, "char_value_use_id", key, u.productSpecCharacteristicValue);
    }
}

// All value-uses of one owner kind (optionally one owner), grouped by owner id, in posted order.
std::map<std::string, std::vector<bss_sid::ProductSpecificationCharacteristicValueUse>>
load_value_uses(pqxx::work& txn, const char* owner_col, const std::optional<std::string>& only) {
    const std::string filter = std::string(" WHERE u.") + owner_col +
                               " IS NOT NULL AND ($1::text "
                               "IS NULL OR u." +
                               owner_col + " = $1)";
    std::map<std::string, std::vector<bss_sid::CharacteristicValueSpecification>> values;
    for (auto r : txn.exec("SELECT cvs.char_value_use_id AS k, " + kValueSpecCols +
                               " FROM product_catalog.char_value_specification cvs JOIN "
                               "product_catalog.prod_spec_char_value_use u ON "
                               "cvs.char_value_use_id = u.id" +
                               filter + " ORDER BY cvs.char_value_use_id, cvs.ordinal",
                           pqxx::params{only})) {
        values[r["k"].as<std::string>()].push_back(value_spec_out(r));
    }
    std::map<std::string, std::vector<bss_sid::ProductSpecificationCharacteristicValueUse>> out;
    for (auto r : txn.exec(std::string("SELECT u.id AS k, u.") + owner_col +
                               " AS owner, u.use_id, u.name, u.description, u.value_type, "
                               "u.min_cardinality, u.max_cardinality, u.product_specification_id, "
                               "u.product_specification_href, u.product_specification_name, "
                               "u.product_specification_version, "
                               "u.product_specification_target_schema, " +
                               ts("u.valid_for_start") + " AS vf_start, " + ts("u.valid_for_end") +
                               " AS vf_end FROM product_catalog.prod_spec_char_value_use u" +
                               filter + " ORDER BY u." + owner_col + ", u.ordinal",
                           pqxx::params{only})) {
        bss_sid::ProductSpecificationCharacteristicValueUse u;
        u.id = s(r, "use_id").value_or("");
        u.name = s(r, "name");
        u.description = s(r, "description");
        u.valueType = s(r, "value_type");
        u.minCardinality = r["min_cardinality"].as<std::optional<int>>();
        u.maxCardinality = r["max_cardinality"].as<std::optional<int>>();
        if (auto sid = s(r, "product_specification_id"); sid.has_value()) {
            u.productSpecification =
                bss_sid::ProductSpecificationRef{*sid,
                                                 s(r, "product_specification_href"),
                                                 s(r, "product_specification_name"),
                                                 s(r, "product_specification_version"),
                                                 s(r, "product_specification_target_schema")};
        }
        u.validFor = vf_out(r, "vf_start", "vf_end");
        if (auto it = values.find(r["k"].as<std::string>()); it != values.end()) {
            u.productSpecCharacteristicValue = std::move(it->second);
        }
        out[r["owner"].as<std::string>()].push_back(std::move(u));
    }
    return out;
}

// Runs a child query ordered by (owner, ordinal) and hands each row to `add` with its owner id.
template <typename Add>
void for_children(pqxx::work& txn,
                  const std::string& sql_select_from,
                  const char* owner_col,
                  const std::optional<std::string>& only,
                  Add add) {
    for (auto r : txn.exec(sql_select_from + " WHERE ($1::text IS NULL OR " + owner_col +
                               " = $1) ORDER BY " + owner_col + ", ordinal",
                           pqxx::params{only})) {
        add(r[owner_col].template as<std::string>(), r);
    }
}

// ================================ ProductOffering ================================================

std::vector<bss_sid::ProductOffering> load_offerings(pqxx::work& txn,
                                                     const std::optional<std::string>& only) {
    std::vector<bss_sid::ProductOffering> out;
    std::map<std::string, std::size_t> idx;
    for (auto r :
         txn.exec("SELECT id, href, name, description, lifecycle_status, " + ts("last_update") +
                      " AS last_update, status_reason, is_bundle, is_sellable, version, "
                      "product_specification_id, product_specification_href, "
                      "product_specification_name, product_specification_version, "
                      "product_specification_target_schema, service_level_agreement_id, "
                      "service_level_agreement_href, service_level_agreement_name, "
                      "resource_candidate_id, resource_candidate_href, resource_candidate_name, "
                      "resource_candidate_version, service_candidate_id, service_candidate_href, "
                      "service_candidate_name, service_candidate_version, attachment::text AS "
                      "attachment, place::text AS place, " +
                      ts("valid_for_start") + " AS vf_start, " + ts("valid_for_end") +
                      " AS vf_end FROM product_catalog.product_offering WHERE ($1::text IS NULL OR "
                      "id = $1) ORDER BY id",
                  pqxx::params{only})) {
        bss_sid::ProductOffering o;
        o.id = s(r, "id");
        o.href = s(r, "href");
        o.name = s(r, "name");
        o.description = s(r, "description");
        o.lifecycleStatus = s(r, "lifecycle_status");
        o.lastUpdate = s(r, "last_update");
        o.statusReason = s(r, "status_reason");
        o.isBundle = r["is_bundle"].as<std::optional<bool>>();
        o.isSellable = r["is_sellable"].as<std::optional<bool>>();
        o.version = s(r, "version");
        if (auto v = s(r, "product_specification_id"); v.has_value()) {
            o.productSpecification =
                bss_sid::ProductSpecificationRef{*v,
                                                 s(r, "product_specification_href"),
                                                 s(r, "product_specification_name"),
                                                 s(r, "product_specification_version"),
                                                 s(r, "product_specification_target_schema")};
        }
        if (auto v = s(r, "service_level_agreement_id"); v.has_value()) {
            o.serviceLevelAgreement = bss_sid::SLARef{
                *v, s(r, "service_level_agreement_href"), s(r, "service_level_agreement_name")};
        }
        if (auto v = s(r, "resource_candidate_id"); v.has_value()) {
            o.resourceCandidate = bss_sid::ResourceCandidateRef{*v,
                                                                s(r, "resource_candidate_href"),
                                                                s(r, "resource_candidate_name"),
                                                                s(r, "resource_candidate_version")};
        }
        if (auto v = s(r, "service_candidate_id"); v.has_value()) {
            o.serviceCandidate = bss_sid::ServiceCandidateRef{*v,
                                                              s(r, "service_candidate_href"),
                                                              s(r, "service_candidate_name"),
                                                              s(r, "service_candidate_version")};
        }
        o.attachment = json_array_out<bss_sid::AttachmentRefOrValue>(r, "attachment");
        o.place = json_array_out<bss_sid::PlaceRef>(r, "place");
        o.validFor = vf_out(r, "vf_start", "vf_end");
        idx[*o.id] = out.size();
        out.push_back(std::move(o));
    }
    if (out.empty()) {
        return out;
    }
    const auto at = [&](const std::string& id) -> bss_sid::ProductOffering* {
        auto it = idx.find(id);
        return it == idx.end() ? nullptr : &out[it->second];
    };

    for_children(
        txn,
        "SELECT offering_id, price_id, href, name FROM product_catalog.offering_price_ref",
        "offering_id",
        only,
        [&](const std::string& o, const auto& r) {
            if (auto* p = at(o)) {
                p->productOfferingPrice.push_back(
                    {r["price_id"].template as<std::string>(), s(r, "href"), s(r, "name")});
            }
        });
    for_children(txn,
                 "SELECT offering_id, ref_id, href, name, version FROM "
                 "product_catalog.offering_category",
                 "offering_id",
                 only,
                 [&](const std::string& o, const auto& r) {
                     if (auto* p = at(o)) {
                         p->category.push_back({r["ref_id"].template as<std::string>(),
                                                s(r, "href"),
                                                s(r, "name"),
                                                s(r, "version")});
                     }
                 });
    for_children(txn,
                 "SELECT offering_id, ref_id, href, name FROM product_catalog.offering_channel",
                 "offering_id",
                 only,
                 [&](const std::string& o, const auto& r) {
                     if (auto* p = at(o)) {
                         p->channel.push_back(
                             {r["ref_id"].template as<std::string>(), s(r, "href"), s(r, "name")});
                     }
                 });
    for_children(txn,
                 "SELECT offering_id, ref_id, href, name FROM "
                 "product_catalog.offering_market_segment",
                 "offering_id",
                 only,
                 [&](const std::string& o, const auto& r) {
                     if (auto* p = at(o)) {
                         p->marketSegment.push_back(
                             {r["ref_id"].template as<std::string>(), s(r, "href"), s(r, "name")});
                     }
                 });
    for_children(txn,
                 "SELECT offering_id, ref_id, href, name FROM product_catalog.offering_agreement",
                 "offering_id",
                 only,
                 [&](const std::string& o, const auto& r) {
                     if (auto* p = at(o)) {
                         p->agreement.push_back(
                             {r["ref_id"].template as<std::string>(), s(r, "href"), s(r, "name")});
                     }
                 });
    for_children(txn,
                 "SELECT parent_offering_id, bundled_id, href, lifecycle_status, name FROM "
                 "product_catalog.bundled_offering",
                 "parent_offering_id",
                 only,
                 [&](const std::string& o, const auto& r) {
                     if (auto* p = at(o)) {
                         p->bundledProductOffering.push_back(
                             {r["bundled_id"].template as<std::string>(),
                              s(r, "href"),
                              s(r, "lifecycle_status"),
                              s(r, "name")});
                     }
                 });
    for_children(txn,
                 "SELECT offering_id, rel_id, href, name, relationship_type, role, " +
                     ts("valid_for_start") + " AS vf_start, " + ts("valid_for_end") +
                     " AS vf_end FROM product_catalog.offering_relationship",
                 "offering_id",
                 only,
                 [&](const std::string& o, const auto& r) {
                     if (auto* p = at(o)) {
                         p->productOfferingRelationship.push_back(
                             {s(r, "rel_id"),
                              s(r, "href"),
                              s(r, "name"),
                              s(r, "relationship_type"),
                              s(r, "role"),
                              vf_out(r, "vf_start", "vf_end")});
                     }
                 });
    for_children(
        txn,
        "SELECT offering_id, name, description, duration_amount, duration_units, " +
            ts("valid_for_start") + " AS vf_start, " + ts("valid_for_end") +
            " AS vf_end FROM product_catalog.offering_term",
        "offering_id",
        only,
        [&](const std::string& o, const auto& r) {
            if (auto* p = at(o)) {
                std::optional<bss_sid::Duration> d;
                auto amount = r["duration_amount"].template as<std::optional<int>>();
                auto units = s(r, "duration_units");
                if (amount || units) {
                    d = bss_sid::Duration{amount, units};
                }
                p->productOfferingTerm.push_back(
                    {s(r, "description"), s(r, "name"), d, vf_out(r, "vf_start", "vf_end")});
            }
        });
    for (auto& [owner, uses] : load_value_uses(txn, "offering_id", only)) {
        if (auto* p = at(owner)) {
            p->prodSpecCharValueUse = std::move(uses);
        }
    }
    return out;
}

// ================================ ProductOfferingPrice ===========================================

std::vector<bss_sid::ProductOfferingPrice> load_prices(pqxx::work& txn,
                                                       const std::optional<std::string>& only) {
    std::vector<bss_sid::ProductOfferingPrice> out;
    std::map<std::string, std::size_t> idx;
    for (auto r : txn.exec(
             "SELECT id, href, name, description, lifecycle_status, " + ts("last_update") +
                 " AS last_update, price_type, percentage, version, price_unit, "
                 "price_value::float8 AS price_value, recurring_charge_period_length, "
                 "recurring_charge_period_type, unit_of_measure_amount::float8 AS "
                 "unit_of_measure_amount, unit_of_measure_units, constraint_ref::text AS "
                 "constraint_ref, place::text AS place, pricing_logic_algorithm::text AS "
                 "pricing_logic_algorithm, product_offering_term::text AS product_offering_term, " +
                 ts("valid_for_start") + " AS vf_start, " + ts("valid_for_end") +
                 " AS vf_end FROM product_catalog.product_offering_price WHERE ($1::text IS NULL "
                 "OR id = $1) ORDER BY id",
             pqxx::params{only})) {
        bss_sid::ProductOfferingPrice p;
        p.id = s(r, "id");
        p.href = s(r, "href");
        p.name = s(r, "name");
        p.description = s(r, "description");
        p.lifecycleStatus = s(r, "lifecycle_status");
        p.lastUpdate = s(r, "last_update");
        p.priceType = s(r, "price_type");
        p.percentage = r["percentage"].as<std::optional<double>>();
        p.version = s(r, "version");
        auto unit = s(r, "price_unit");
        auto value = r["price_value"].as<std::optional<double>>();
        if (unit || value) {
            p.price = bss_sid::Money{std::move(unit), value};
        }
        p.recurringChargePeriodLength =
            r["recurring_charge_period_length"].as<std::optional<int>>();
        p.recurringChargePeriodType = s(r, "recurring_charge_period_type");
        p.unitOfMeasure = quantity_out(r, "unit_of_measure_amount", "unit_of_measure_units");
        p.constraint = json_array_out<bss_sid::ConstraintRef>(r, "constraint_ref");
        p.place = json_array_out<bss_sid::PlaceRef>(r, "place");
        p.pricingLogicAlgorithm =
            json_array_out<bss_sid::PricingLogicAlgorithm>(r, "pricing_logic_algorithm");
        p.productOfferingTerm =
            json_array_out<bss_sid::ProductOfferingTerm>(r, "product_offering_term");
        p.validFor = vf_out(r, "vf_start", "vf_end");
        idx[*p.id] = out.size();
        out.push_back(std::move(p));
    }
    if (out.empty()) {
        return out;
    }
    const auto at = [&](const std::string& id) -> bss_sid::ProductOfferingPrice* {
        auto it = idx.find(id);
        return it == idx.end() ? nullptr : &out[it->second];
    };
    for_children(txn,
                 "SELECT price_id, kind, rel_id, href, name, relationship_type, role FROM "
                 "product_catalog.price_relationship",
                 "price_id",
                 only,
                 [&](const std::string& o, const auto& r) {
                     if (auto* p = at(o)) {
                         if (r["kind"].template as<std::string>() == "BUNDLED_POP") {
                             p->bundledPopRelationship.push_back(
                                 {s(r, "rel_id"), s(r, "href"), s(r, "name")});
                         } else {
                             p->popRelationship.push_back({s(r, "rel_id"),
                                                           s(r, "href"),
                                                           s(r, "name"),
                                                           s(r, "relationship_type"),
                                                           s(r, "role")});
                         }
                     }
                 });
    for_children(txn,
                 "SELECT price_id, tax_id, href, tax_category, tax_rate, tax_amount_unit, "
                 "tax_amount_value::float8 AS tax_amount_value FROM product_catalog.price_tax_item",
                 "price_id",
                 only,
                 [&](const std::string& o, const auto& r) {
                     if (auto* p = at(o)) {
                         std::optional<bss_sid::Money> amount;
                         auto unit = s(r, "tax_amount_unit");
                         auto value = r["tax_amount_value"].template as<std::optional<double>>();
                         if (unit || value) {
                             amount = bss_sid::Money{unit, value};
                         }
                         p->tax.push_back({s(r, "tax_id"),
                                           s(r, "href"),
                                           s(r, "tax_category"),
                                           r["tax_rate"].template as<std::optional<double>>(),
                                           amount});
                     }
                 });
    for (auto& [owner, uses] : load_value_uses(txn, "price_id", only)) {
        if (auto* p = at(owner)) {
            p->prodSpecCharValueUse = std::move(uses);
        }
    }
    return out;
}

// ================================ ProductSpecification ===========================================

std::vector<bss_sid::ProductSpecification> load_specs(pqxx::work& txn,
                                                      const std::optional<std::string>& only) {
    std::vector<bss_sid::ProductSpecification> out;
    std::map<std::string, std::size_t> idx;
    for (auto r : txn.exec("SELECT id, href, brand, description, is_bundle, lifecycle_status, " +
                               ts("last_update") +
                               " AS last_update, name, product_number, version, "
                               "target_product_schema::text AS target_product_schema, "
                               "attachment::text AS attachment, " +
                               ts("valid_for_start") + " AS vf_start, " + ts("valid_for_end") +
                               " AS vf_end FROM product_catalog.product_specification WHERE "
                               "($1::text IS NULL OR id = $1) ORDER BY id",
                           pqxx::params{only})) {
        bss_sid::ProductSpecification sp;
        sp.id = s(r, "id");
        sp.href = s(r, "href");
        sp.brand = s(r, "brand");
        sp.description = s(r, "description");
        sp.isBundle = r["is_bundle"].as<std::optional<bool>>();
        sp.lifecycleStatus = s(r, "lifecycle_status");
        sp.lastUpdate = s(r, "last_update");
        sp.name = s(r, "name");
        sp.productNumber = s(r, "product_number");
        sp.version = s(r, "version");
        if (auto raw = s(r, "target_product_schema"); raw.has_value()) {
            sp.targetProductSchema = json::parse(*raw);
        }
        sp.attachment = json_array_out<bss_sid::AttachmentRefOrValue>(r, "attachment");
        sp.validFor = vf_out(r, "vf_start", "vf_end");
        idx[*sp.id] = out.size();
        out.push_back(std::move(sp));
    }
    if (out.empty()) {
        return out;
    }
    const auto at = [&](const std::string& id) -> bss_sid::ProductSpecification* {
        auto it = idx.find(id);
        return it == idx.end() ? nullptr : &out[it->second];
    };

    std::map<std::string, std::vector<bss_sid::CharacteristicValueSpecification>> values;
    for (auto r : txn.exec("SELECT cvs.spec_characteristic_id AS k, " + kValueSpecCols +
                               " FROM product_catalog.char_value_specification cvs JOIN "
                               "product_catalog.product_spec_characteristic c ON "
                               "cvs.spec_characteristic_id = c.id WHERE ($1::text IS NULL OR "
                               "c.specification_id = $1) ORDER BY cvs.spec_characteristic_id, "
                               "cvs.ordinal",
                           pqxx::params{only})) {
        values[r["k"].as<std::string>()].push_back(value_spec_out(r));
    }
    for_children(txn,
                 "SELECT id, specification_id, char_id, name, description, value_type, regex, "
                 "configurable, extensible, is_unique, min_cardinality, max_cardinality, " +
                     ts("valid_for_start") + " AS vf_start, " + ts("valid_for_end") +
                     " AS vf_end FROM product_catalog.product_spec_characteristic",
                 "specification_id",
                 only,
                 [&](const std::string& o, const auto& r) {
                     if (auto* p = at(o)) {
                         bss_sid::ProductSpecificationCharacteristic c;
                         c.id = s(r, "char_id").value_or("");
                         c.configurable = r["configurable"].template as<std::optional<bool>>();
                         c.description = s(r, "description");
                         c.extensible = r["extensible"].template as<std::optional<bool>>();
                         c.isUnique = r["is_unique"].template as<std::optional<bool>>();
                         c.maxCardinality = r["max_cardinality"].template as<std::optional<int>>();
                         c.minCardinality = r["min_cardinality"].template as<std::optional<int>>();
                         c.name = s(r, "name");
                         c.regex = s(r, "regex");
                         c.valueType = s(r, "value_type");
                         c.validFor = vf_out(r, "vf_start", "vf_end");
                         if (auto it = values.find(r["id"].template as<std::string>());
                             it != values.end()) {
                             c.productSpecCharacteristicValue = std::move(it->second);
                         }
                         p->productSpecCharacteristic.push_back(std::move(c));
                     }
                 });
    for_children(
        txn,
        "SELECT parent_specification_id, bundled_id, href, lifecycle_status, name FROM "
        "product_catalog.bundled_specification",
        "parent_specification_id",
        only,
        [&](const std::string& o, const auto& r) {
            if (auto* p = at(o)) {
                p->bundledProductSpecification.push_back(
                    {s(r, "bundled_id"), s(r, "href"), s(r, "lifecycle_status"), s(r, "name")});
            }
        });
    for_children(txn,
                 "SELECT specification_id, rel_id, href, name, relationship_type, " +
                     ts("valid_for_start") + " AS vf_start, " + ts("valid_for_end") +
                     " AS vf_end FROM product_catalog.spec_relationship",
                 "specification_id",
                 only,
                 [&](const std::string& o, const auto& r) {
                     if (auto* p = at(o)) {
                         p->productSpecificationRelationship.push_back(
                             {s(r, "rel_id"),
                              s(r, "href"),
                              s(r, "name"),
                              s(r, "relationship_type"),
                              vf_out(r, "vf_start", "vf_end")});
                     }
                 });
    for_children(txn,
                 "SELECT specification_id, party_id, href, name, role FROM "
                 "product_catalog.spec_related_party",
                 "specification_id",
                 only,
                 [&](const std::string& o, const auto& r) {
                     if (auto* p = at(o)) {
                         p->relatedParty.push_back({r["party_id"].template as<std::string>(),
                                                    s(r, "href"),
                                                    s(r, "name"),
                                                    s(r, "role")});
                     }
                 });
    for_children(txn,
                 "SELECT specification_id, candidate_kind, ref_id, href, name, version FROM "
                 "product_catalog.spec_candidate_ref",
                 "specification_id",
                 only,
                 [&](const std::string& o, const auto& r) {
                     if (auto* p = at(o)) {
                         if (r["candidate_kind"].template as<std::string>() == "RESOURCE") {
                             p->resourceSpecification.push_back(
                                 {s(r, "ref_id"), s(r, "href"), s(r, "name"), s(r, "version")});
                         } else {
                             p->serviceSpecification.push_back(
                                 {s(r, "ref_id"), s(r, "href"), s(r, "name"), s(r, "version")});
                         }
                     }
                 });
    return out;
}

// Deletes one row; a still-referenced row is a 409, an absent one returns false.
bool delete_or_conflict(pqxx::work& txn,
                        const char* table,
                        const std::string& id,
                        const char* what) {
    try {
        pqxx::subtransaction sub(txn);
        const auto res =
            sub.exec(std::string("DELETE FROM product_catalog.") + table + " WHERE id = $1",
                     pqxx::params{id});
        sub.commit();
        return res.affected_rows() > 0;
    } catch (const pqxx::foreign_key_violation&) {
        throw Conflict(std::string(what) + " " + id + " is still referenced and cannot be deleted");
    }
}

} // namespace

// =================================================================================================

ProductOfferingStore::ProductOfferingStore(std::string resource_url,
                                           const std::string& conninfo,
                                           std::size_t pool_size)
    : resource_url_(std::move(resource_url)), pool_(conninfo, pool_size) {}

std::string ProductOfferingStore::create(bss_sid::ProductOffering o) {
    if (!o.name.has_value() || o.name->empty()) {
        throw InvalidRequest("name is required (TMF620 ProductOffering_Create)");
    }
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    const auto id =
        txn.exec("SELECT nextval('product_catalog.product_offering_id_seq')::text AS id")
            .one_row()["id"]
            .as<std::string>();
    o.id = id;
    o.href = resource_url_ + "/" + id;
    try {
        const auto& spec = o.productSpecification;
        const auto& sla = o.serviceLevelAgreement;
        const auto& rc = o.resourceCandidate;
        const auto& sc = o.serviceCandidate;
        txn.exec(
            "INSERT INTO product_catalog.product_offering (id, href, name, description, "
            "lifecycle_status, last_update, status_reason, is_bundle, is_sellable, version, "
            "product_specification_id, product_specification_href, product_specification_name, "
            "product_specification_version, product_specification_target_schema, "
            "service_level_agreement_id, service_level_agreement_href, "
            "service_level_agreement_name, resource_candidate_id, resource_candidate_href, "
            "resource_candidate_name, resource_candidate_version, service_candidate_id, "
            "service_candidate_href, service_candidate_name, service_candidate_version, "
            "attachment, place, valid_for_start, valid_for_end) VALUES ($1,$2,$3,$4,$5,"
            "$6::timestamptz,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17,$18,$19,$20,$21,$22,$23,$24,"
            "$25,$26,$27::jsonb,$28::jsonb,$29::timestamptz,$30::timestamptz)",
            pqxx::params{id,
                         o.href,
                         o.name,
                         o.description,
                         o.lifecycleStatus,
                         ts_in(o.lastUpdate, "lastUpdate"),
                         o.statusReason,
                         o.isBundle,
                         o.isSellable,
                         o.version,
                         spec ? std::optional(spec->id) : std::nullopt,
                         spec ? spec->href : std::nullopt,
                         spec ? spec->name : std::nullopt,
                         spec ? spec->version : std::nullopt,
                         spec ? spec->targetProductSchema : std::nullopt,
                         sla ? std::optional(sla->id) : std::nullopt,
                         sla ? sla->href : std::nullopt,
                         sla ? sla->name : std::nullopt,
                         rc ? std::optional(rc->id) : std::nullopt,
                         rc ? rc->href : std::nullopt,
                         rc ? rc->name : std::nullopt,
                         rc ? rc->version : std::nullopt,
                         sc ? std::optional(sc->id) : std::nullopt,
                         sc ? sc->href : std::nullopt,
                         sc ? sc->name : std::nullopt,
                         sc ? sc->version : std::nullopt,
                         json_array_or_null(o.attachment),
                         json_array_or_null(o.place),
                         vf_start(o.validFor, "validFor.startDateTime"),
                         vf_end(o.validFor, "validFor.endDateTime")});
        for (std::size_t i = 0; i < o.productOfferingPrice.size(); ++i) {
            const auto& p = o.productOfferingPrice[i];
            txn.exec("INSERT INTO product_catalog.offering_price_ref (offering_id, price_id, href, "
                     "name, ordinal) VALUES ($1,$2,$3,$4,$5)",
                     pqxx::params{id, p.id, p.href, p.name, static_cast<int>(i)});
        }
        for (std::size_t i = 0; i < o.category.size(); ++i) {
            const auto& c = o.category[i];
            txn.exec("INSERT INTO product_catalog.offering_category (offering_id, ref_id, href, "
                     "name, version, ordinal) VALUES ($1,$2,$3,$4,$5,$6)",
                     pqxx::params{id, c.id, c.href, c.name, c.version, static_cast<int>(i)});
        }
        for (std::size_t i = 0; i < o.channel.size(); ++i) {
            const auto& c = o.channel[i];
            txn.exec("INSERT INTO product_catalog.offering_channel (offering_id, ref_id, href, "
                     "name, ordinal) VALUES ($1,$2,$3,$4,$5)",
                     pqxx::params{id, c.id, c.href, c.name, static_cast<int>(i)});
        }
        for (std::size_t i = 0; i < o.marketSegment.size(); ++i) {
            const auto& c = o.marketSegment[i];
            txn.exec("INSERT INTO product_catalog.offering_market_segment (offering_id, ref_id, "
                     "href, name, ordinal) VALUES ($1,$2,$3,$4,$5)",
                     pqxx::params{id, c.id, c.href, c.name, static_cast<int>(i)});
        }
        for (std::size_t i = 0; i < o.agreement.size(); ++i) {
            const auto& a = o.agreement[i];
            txn.exec("INSERT INTO product_catalog.offering_agreement (offering_id, ordinal, "
                     "ref_id, href, name) VALUES ($1,$2,$3,$4,$5)",
                     pqxx::params{id, static_cast<int>(i), a.id, a.href, a.name});
        }
        for (std::size_t i = 0; i < o.bundledProductOffering.size(); ++i) {
            const auto& b = o.bundledProductOffering[i];
            txn.exec("INSERT INTO product_catalog.bundled_offering (id, parent_offering_id, "
                     "bundled_id, href, lifecycle_status, name, ordinal) VALUES "
                     "($1,$2,$3,$4,$5,$6,$7)",
                     pqxx::params{"o:" + id + ":b" + std::to_string(i),
                                  id,
                                  b.id,
                                  b.href,
                                  b.lifecycleStatus,
                                  b.name,
                                  static_cast<int>(i)});
        }
        for (std::size_t i = 0; i < o.productOfferingRelationship.size(); ++i) {
            const auto& r = o.productOfferingRelationship[i];
            txn.exec("INSERT INTO product_catalog.offering_relationship (id, offering_id, rel_id, "
                     "href, name, relationship_type, role, valid_for_start, valid_for_end, "
                     "ordinal) VALUES ($1,$2,$3,$4,$5,$6,$7,$8::timestamptz,$9::timestamptz,$10)",
                     pqxx::params{"o:" + id + ":r" + std::to_string(i),
                                  id,
                                  r.id,
                                  r.href,
                                  r.name,
                                  r.relationshipType,
                                  r.role,
                                  vf_start(r.validFor, "validFor.startDateTime"),
                                  vf_end(r.validFor, "validFor.endDateTime"),
                                  static_cast<int>(i)});
        }
        for (std::size_t i = 0; i < o.productOfferingTerm.size(); ++i) {
            const auto& t = o.productOfferingTerm[i];
            const auto d = t.duration.value_or(bss_sid::Duration{});
            txn.exec("INSERT INTO product_catalog.offering_term (id, offering_id, name, "
                     "description, duration_amount, duration_units, valid_for_start, "
                     "valid_for_end, ordinal) VALUES ($1,$2,$3,$4,$5,$6,$7::timestamptz,"
                     "$8::timestamptz,$9)",
                     pqxx::params{"o:" + id + ":t" + std::to_string(i),
                                  id,
                                  t.name,
                                  t.description,
                                  d.amount,
                                  d.units,
                                  vf_start(t.validFor, "validFor.startDateTime"),
                                  vf_end(t.validFor, "validFor.endDateTime"),
                                  static_cast<int>(i)});
        }
        insert_value_uses(txn, "offering_id", "o", id, o.prodSpecCharValueUse);
    } catch (const pqxx::sql_error& e) {
        rethrow_as_request_error(e);
    }
    write_audit(
        txn, "PRODUCT_OFFERING", id, "productOffering.create", std::nullopt, json(o).dump());
    txn.commit();
    return id;
}

std::optional<bss_sid::ProductOffering> ProductOfferingStore::get(const std::string& id) {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    auto all = load_offerings(txn, id);
    if (all.empty()) {
        return std::nullopt;
    }
    return std::move(all.front());
}

std::vector<bss_sid::ProductOffering> ProductOfferingStore::list() {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    return load_offerings(txn, std::nullopt);
}

bool ProductOfferingStore::remove(const std::string& id) {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    auto before = load_offerings(txn, id);
    if (!delete_or_conflict(txn, "product_offering", id, "ProductOffering")) {
        return false;
    }
    write_audit(txn,
                "PRODUCT_OFFERING",
                id,
                "productOffering.remove",
                before.empty() ? std::nullopt : std::optional(json(before.front()).dump()),
                std::nullopt);
    txn.commit();
    return true;
}

// =================================================================================================

ProductOfferingPriceStore::ProductOfferingPriceStore(std::string resource_url,
                                                     const std::string& conninfo,
                                                     std::size_t pool_size)
    : resource_url_(std::move(resource_url)), pool_(conninfo, pool_size) {}

std::string ProductOfferingPriceStore::create(bss_sid::ProductOfferingPrice p) {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    const auto id =
        txn.exec("SELECT nextval('product_catalog.product_offering_price_id_seq')::text AS id")
            .one_row()["id"]
            .as<std::string>();
    p.id = id;
    p.href = resource_url_ + "/" + id;
    try {
        const auto money = p.price.value_or(bss_sid::Money{});
        const auto uom = p.unitOfMeasure.value_or(bss_sid::Quantity{});
        txn.exec(
            "INSERT INTO product_catalog.product_offering_price (id, href, name, description, "
            "lifecycle_status, last_update, price_type, percentage, version, price_unit, "
            "price_value, recurring_charge_period_length, recurring_charge_period_type, "
            "unit_of_measure_amount, unit_of_measure_units, constraint_ref, place, "
            "pricing_logic_algorithm, product_offering_term, valid_for_start, valid_for_end) "
            "VALUES ($1,$2,$3,$4,$5,$6::timestamptz,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16::jsonb,"
            "$17::jsonb,$18::jsonb,$19::jsonb,$20::timestamptz,$21::timestamptz)",
            pqxx::params{id,
                         p.href,
                         p.name,
                         p.description,
                         p.lifecycleStatus,
                         ts_in(p.lastUpdate, "lastUpdate"),
                         p.priceType,
                         p.percentage,
                         p.version,
                         money.unit,
                         money.value,
                         p.recurringChargePeriodLength,
                         p.recurringChargePeriodType,
                         uom.amount,
                         uom.units,
                         json_array_or_null(p.constraint),
                         json_array_or_null(p.place),
                         json_array_or_null(p.pricingLogicAlgorithm),
                         json_array_or_null(p.productOfferingTerm),
                         vf_start(p.validFor, "validFor.startDateTime"),
                         vf_end(p.validFor, "validFor.endDateTime")});
        int ordinal = 0;
        for (const auto& r : p.popRelationship) {
            txn.exec("INSERT INTO product_catalog.price_relationship (id, price_id, kind, rel_id, "
                     "href, name, relationship_type, role, ordinal) VALUES "
                     "($1,$2,'POP',$3,$4,$5,$6,$7,$8)",
                     pqxx::params{"p:" + id + ":r" + std::to_string(ordinal),
                                  id,
                                  r.id,
                                  r.href,
                                  r.name,
                                  r.relationshipType,
                                  r.role,
                                  ordinal});
            ++ordinal;
        }
        for (const auto& r : p.bundledPopRelationship) {
            txn.exec(
                "INSERT INTO product_catalog.price_relationship (id, price_id, kind, rel_id, "
                "href, name, ordinal) VALUES ($1,$2,'BUNDLED_POP',$3,$4,$5,$6)",
                pqxx::params{
                    "p:" + id + ":r" + std::to_string(ordinal), id, r.id, r.href, r.name, ordinal});
            ++ordinal;
        }
        for (std::size_t i = 0; i < p.tax.size(); ++i) {
            const auto& t = p.tax[i];
            const auto amount = t.taxAmount.value_or(bss_sid::Money{});
            txn.exec("INSERT INTO product_catalog.price_tax_item (id, price_id, tax_id, href, "
                     "tax_category, tax_rate, tax_amount_unit, tax_amount_value, ordinal) VALUES "
                     "($1,$2,$3,$4,$5,$6,$7,$8,$9)",
                     pqxx::params{"p:" + id + ":x" + std::to_string(i),
                                  id,
                                  t.id,
                                  t.href,
                                  t.taxCategory,
                                  t.taxRate,
                                  amount.unit,
                                  amount.value,
                                  static_cast<int>(i)});
        }
        insert_value_uses(txn, "price_id", "p", id, p.prodSpecCharValueUse);
    } catch (const pqxx::sql_error& e) {
        rethrow_as_request_error(e);
    }
    write_audit(txn,
                "PRODUCT_OFFERING_PRICE",
                id,
                "productOfferingPrice.create",
                std::nullopt,
                json(p).dump());
    txn.commit();
    return id;
}

std::optional<bss_sid::ProductOfferingPrice> ProductOfferingPriceStore::get(const std::string& id) {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    auto all = load_prices(txn, id);
    if (all.empty()) {
        return std::nullopt;
    }
    return std::move(all.front());
}

std::vector<bss_sid::ProductOfferingPrice> ProductOfferingPriceStore::list() {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    return load_prices(txn, std::nullopt);
}

bool ProductOfferingPriceStore::remove(const std::string& id) {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    auto before = load_prices(txn, id);
    if (!delete_or_conflict(txn, "product_offering_price", id, "ProductOfferingPrice")) {
        return false;
    }
    write_audit(txn,
                "PRODUCT_OFFERING_PRICE",
                id,
                "productOfferingPrice.remove",
                before.empty() ? std::nullopt : std::optional(json(before.front()).dump()),
                std::nullopt);
    txn.commit();
    return true;
}

// =================================================================================================

ProductSpecificationStore::ProductSpecificationStore(std::string resource_url,
                                                     const std::string& conninfo,
                                                     std::size_t pool_size)
    : resource_url_(std::move(resource_url)), pool_(conninfo, pool_size) {}

std::string ProductSpecificationStore::create(bss_sid::ProductSpecification sp) {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    const auto id =
        txn.exec("SELECT nextval('product_catalog.product_specification_id_seq')::text AS id")
            .one_row()["id"]
            .as<std::string>();
    sp.id = id;
    sp.href = resource_url_ + "/" + id;
    try {
        txn.exec(
            "INSERT INTO product_catalog.product_specification (id, href, brand, description, "
            "is_bundle, lifecycle_status, last_update, name, product_number, version, "
            "target_product_schema, attachment, valid_for_start, valid_for_end) VALUES ($1,$2,$3,"
            "$4,$5,$6,$7::timestamptz,$8,$9,$10,$11::jsonb,$12::jsonb,$13::timestamptz,"
            "$14::timestamptz)",
            pqxx::params{id,
                         sp.href,
                         sp.brand,
                         sp.description,
                         sp.isBundle,
                         sp.lifecycleStatus,
                         ts_in(sp.lastUpdate, "lastUpdate"),
                         sp.name,
                         sp.productNumber,
                         sp.version,
                         sp.targetProductSchema.has_value()
                             ? std::optional(json(*sp.targetProductSchema).dump())
                             : std::nullopt,
                         json_array_or_null(sp.attachment),
                         vf_start(sp.validFor, "validFor.startDateTime"),
                         vf_end(sp.validFor, "validFor.endDateTime")});
        for (std::size_t i = 0; i < sp.productSpecCharacteristic.size(); ++i) {
            const auto& c = sp.productSpecCharacteristic[i];
            const std::string key = "s:" + id + ":c" + std::to_string(i);
            txn.exec("INSERT INTO product_catalog.product_spec_characteristic (id, "
                     "specification_id, char_id, name, description, value_type, regex, "
                     "configurable, extensible, is_unique, min_cardinality, max_cardinality, "
                     "valid_for_start, valid_for_end, ordinal) VALUES ($1,$2,$3,$4,$5,$6,$7,$8,"
                     "$9,$10,$11,$12,$13::timestamptz,$14::timestamptz,$15)",
                     pqxx::params{key,
                                  id,
                                  c.id,
                                  c.name,
                                  c.description,
                                  c.valueType,
                                  c.regex,
                                  c.configurable,
                                  c.extensible,
                                  c.isUnique,
                                  c.minCardinality,
                                  c.maxCardinality,
                                  vf_start(c.validFor, "validFor.startDateTime"),
                                  vf_end(c.validFor, "validFor.endDateTime"),
                                  static_cast<int>(i)});
            insert_value_specs(
                txn, "spec_characteristic_id", key, c.productSpecCharacteristicValue);
        }
        for (std::size_t i = 0; i < sp.bundledProductSpecification.size(); ++i) {
            const auto& b = sp.bundledProductSpecification[i];
            txn.exec("INSERT INTO product_catalog.bundled_specification (id, "
                     "parent_specification_id, bundled_id, href, lifecycle_status, name, ordinal) "
                     "VALUES ($1,$2,$3,$4,$5,$6,$7)",
                     pqxx::params{"s:" + id + ":b" + std::to_string(i),
                                  id,
                                  b.id,
                                  b.href,
                                  b.lifecycleStatus,
                                  b.name,
                                  static_cast<int>(i)});
        }
        for (std::size_t i = 0; i < sp.productSpecificationRelationship.size(); ++i) {
            const auto& r = sp.productSpecificationRelationship[i];
            txn.exec("INSERT INTO product_catalog.spec_relationship (id, specification_id, rel_id, "
                     "href, name, relationship_type, valid_for_start, valid_for_end, ordinal) "
                     "VALUES ($1,$2,$3,$4,$5,$6,$7::timestamptz,$8::timestamptz,$9)",
                     pqxx::params{"s:" + id + ":r" + std::to_string(i),
                                  id,
                                  r.id,
                                  r.href,
                                  r.name,
                                  r.relationshipType,
                                  vf_start(r.validFor, "validFor.startDateTime"),
                                  vf_end(r.validFor, "validFor.endDateTime"),
                                  static_cast<int>(i)});
        }
        for (std::size_t i = 0; i < sp.relatedParty.size(); ++i) {
            const auto& rp = sp.relatedParty[i];
            txn.exec("INSERT INTO product_catalog.spec_related_party (id, specification_id, "
                     "party_id, href, name, role, ordinal) VALUES ($1,$2,$3,$4,$5,$6,$7)",
                     pqxx::params{"s:" + id + ":p" + std::to_string(i),
                                  id,
                                  rp.id,
                                  rp.href,
                                  rp.name,
                                  rp.role,
                                  static_cast<int>(i)});
        }
        for (std::size_t i = 0; i < sp.resourceSpecification.size(); ++i) {
            const auto& r = sp.resourceSpecification[i];
            txn.exec("INSERT INTO product_catalog.spec_candidate_ref (specification_id, "
                     "candidate_kind, ref_id, href, name, version, ordinal) VALUES "
                     "($1,'RESOURCE',$2,$3,$4,$5,$6)",
                     pqxx::params{id, r.id, r.href, r.name, r.version, static_cast<int>(i)});
        }
        for (std::size_t i = 0; i < sp.serviceSpecification.size(); ++i) {
            const auto& r = sp.serviceSpecification[i];
            // SERVICE ordinals follow RESOURCE ones so one (specification, ordinal) order holds.
            txn.exec("INSERT INTO product_catalog.spec_candidate_ref (specification_id, "
                     "candidate_kind, ref_id, href, name, version, ordinal) VALUES "
                     "($1,'SERVICE',$2,$3,$4,$5,$6)",
                     pqxx::params{id,
                                  r.id,
                                  r.href,
                                  r.name,
                                  r.version,
                                  static_cast<int>(sp.resourceSpecification.size() + i)});
        }
    } catch (const pqxx::sql_error& e) {
        rethrow_as_request_error(e);
    }
    write_audit(txn,
                "PRODUCT_SPECIFICATION",
                id,
                "productSpecification.create",
                std::nullopt,
                json(sp).dump());
    txn.commit();
    return id;
}

std::optional<bss_sid::ProductSpecification> ProductSpecificationStore::get(const std::string& id) {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    auto all = load_specs(txn, id);
    if (all.empty()) {
        return std::nullopt;
    }
    return std::move(all.front());
}

std::vector<bss_sid::ProductSpecification> ProductSpecificationStore::list() {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    return load_specs(txn, std::nullopt);
}

bool ProductSpecificationStore::remove(const std::string& id) {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    auto before = load_specs(txn, id);
    if (!delete_or_conflict(txn, "product_specification", id, "ProductSpecification")) {
        return false;
    }
    write_audit(txn,
                "PRODUCT_SPECIFICATION",
                id,
                "productSpecification.remove",
                before.empty() ? std::nullopt : std::optional(json(before.front()).dump()),
                std::nullopt);
    txn.commit();
    return true;
}

} // namespace product_catalog
