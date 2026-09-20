#include "store.hpp"

#include <nlohmann/json.hpp>

namespace product_catalog {

namespace {

using nlohmann::json;

// P4.5/ADR-0060 (E8, Security): real audit trail, written in the SAME transaction as the mutation
// it records (schema.sql's own header explains why this is a local, per-service table rather than
// one shared cross-service table). `actor` is a fixed service-name string -- this project has no
// human-operator identity/auth path for BSS mutations yet (disclosed, same gap noted in
// schema.sql).
void write_audit_record(pqxx::work& txn,
                        const std::string& entity_type,
                        const std::string& entity_id,
                        const std::string& action,
                        const std::optional<std::string>& after_snapshot) {
    const auto id = txn.exec("SELECT nextval('audit_record_id_seq')::text AS id")
                        .one_row()["id"]
                        .as<std::string>();
    txn.exec("INSERT INTO audit_record (id, entity_type, entity_id, action, actor, "
             "after_snapshot) VALUES ($1,$2,$3,$4,'bss/product-catalog',$5::jsonb)",
             pqxx::params{id, entity_type, entity_id, action, after_snapshot});
}

std::optional<std::string> valid_from_of(const std::optional<bss_sid::TimePeriod>& vf) {
    return vf.has_value() ? vf->startDateTime : std::nullopt;
}

std::optional<std::string> valid_to_of(const std::optional<bss_sid::TimePeriod>& vf) {
    return vf.has_value() ? vf->endDateTime : std::nullopt;
}

std::optional<bss_sid::TimePeriod> time_period_from(const std::optional<std::string>& from,
                                                    const std::optional<std::string>& to) {
    if (!from.has_value() && !to.has_value()) {
        return std::nullopt;
    }
    bss_sid::TimePeriod tp;
    tp.startDateTime = from;
    tp.endDateTime = to;
    return tp;
}

template <typename T> std::string dump_array(const std::vector<T>& v) {
    return json(v).dump();
}

template <typename T> std::vector<T> parse_array(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    return json::parse(s).get<std::vector<T>>();
}

template <typename T> std::optional<std::string> dump_optional(const std::optional<T>& v) {
    if (!v.has_value()) {
        return std::nullopt;
    }
    return json(*v).dump();
}

template <typename T> std::optional<T> parse_optional(const std::optional<std::string>& s) {
    if (!s.has_value()) {
        return std::nullopt;
    }
    return json::parse(*s).get<T>();
}

// Templated (not `const pqxx::row&`) because libpqxx 8.x's `pqxx::result::operator[]`/`front()`
// return the lightweight `pqxx::row_ref` view type, while `result::one_row()` returns an owning
// `pqxx::row` -- both support the same named-field `operator[]` access used below, so one template
// serves both call sites without a copy.
template <typename Row> bss_sid::ProductOffering row_to_offering(const Row& row) {
    bss_sid::ProductOffering v;
    v.id = row["id"].template as<std::optional<std::string>>();
    v.href = row["href"].template as<std::optional<std::string>>();
    v.name = row["name"].template as<std::optional<std::string>>();
    v.description = row["description"].template as<std::optional<std::string>>();
    v.lifecycleStatus = row["lifecycle_status"].template as<std::optional<std::string>>();
    v.lastUpdate = row["last_update"].template as<std::optional<std::string>>();
    v.statusReason = row["status_reason"].template as<std::optional<std::string>>();
    v.isBundle = row["is_bundle"].template as<std::optional<bool>>();
    v.isSellable = row["is_sellable"].template as<std::optional<bool>>();
    v.version = row["version"].template as<std::optional<std::string>>();
    v.validFor = time_period_from(row["valid_from"].template as<std::optional<std::string>>(),
                                  row["valid_to"].template as<std::optional<std::string>>());
    v.productOfferingPrice = parse_array<bss_sid::ProductOfferingPriceRef>(
        row["product_offering_price"].template as<std::string>());
    v.category = parse_array<bss_sid::CategoryRef>(row["category"].template as<std::string>());
    v.channel = parse_array<bss_sid::ChannelRef>(row["channel"].template as<std::string>());
    v.marketSegment =
        parse_array<bss_sid::MarketSegmentRef>(row["market_segment"].template as<std::string>());
    v.prodSpecCharValueUse = parse_array<bss_sid::ProductSpecificationCharacteristicValueUse>(
        row["prod_spec_char_value_use"].template as<std::string>());
    v.productSpecification = parse_optional<bss_sid::ProductSpecificationRef>(
        row["product_specification"].template as<std::optional<std::string>>());
    v.resourceCandidate = parse_optional<bss_sid::ResourceCandidateRef>(
        row["resource_candidate"].template as<std::optional<std::string>>());
    v.serviceCandidate = parse_optional<bss_sid::ServiceCandidateRef>(
        row["service_candidate"].template as<std::optional<std::string>>());
    v.serviceLevelAgreement = parse_optional<bss_sid::SLARef>(
        row["service_level_agreement"].template as<std::optional<std::string>>());
    v.agreement = parse_array<bss_sid::AgreementRef>(row["agreement"].template as<std::string>());
    v.bundledProductOffering = parse_array<bss_sid::BundledProductOffering>(
        row["bundled_product_offering"].template as<std::string>());
    v.attachment =
        parse_array<bss_sid::AttachmentRefOrValue>(row["attachment"].template as<std::string>());
    v.place = parse_array<bss_sid::PlaceRef>(row["place"].template as<std::string>());
    v.productOfferingRelationship = parse_array<bss_sid::ProductOfferingRelationship>(
        row["product_offering_relationship"].template as<std::string>());
    v.productOfferingTerm = parse_array<bss_sid::ProductOfferingTerm>(
        row["product_offering_term"].template as<std::string>());
    return v;
}

template <typename Row> bss_sid::ProductOfferingPrice row_to_price(const Row& row) {
    bss_sid::ProductOfferingPrice v;
    v.id = row["id"].template as<std::optional<std::string>>();
    v.href = row["href"].template as<std::optional<std::string>>();
    v.name = row["name"].template as<std::optional<std::string>>();
    v.description = row["description"].template as<std::optional<std::string>>();
    v.lifecycleStatus = row["lifecycle_status"].template as<std::optional<std::string>>();
    v.lastUpdate = row["last_update"].template as<std::optional<std::string>>();
    v.priceType = row["price_type"].template as<std::optional<std::string>>();
    v.percentage = row["percentage"].template as<std::optional<double>>();
    v.version = row["version"].template as<std::optional<std::string>>();
    v.price =
        parse_optional<bss_sid::Money>(row["price"].template as<std::optional<std::string>>());
    v.recurringChargePeriodLength =
        row["recurring_charge_period_length"].template as<std::optional<int>>();
    v.recurringChargePeriodType =
        row["recurring_charge_period_type"].template as<std::optional<std::string>>();
    v.unitOfMeasure = parse_optional<bss_sid::Quantity>(
        row["unit_of_measure"].template as<std::optional<std::string>>());
    v.prodSpecCharValueUse = parse_array<bss_sid::ProductSpecificationCharacteristicValueUse>(
        row["prod_spec_char_value_use"].template as<std::string>());
    v.bundledPopRelationship = parse_array<bss_sid::BundledProductOfferingPriceRelationship>(
        row["bundled_pop_relationship"].template as<std::string>());
    v.constraint =
        parse_array<bss_sid::ConstraintRef>(row["constraint_ref"].template as<std::string>());
    v.place = parse_array<bss_sid::PlaceRef>(row["place"].template as<std::string>());
    v.popRelationship = parse_array<bss_sid::ProductOfferingPriceRelationship>(
        row["pop_relationship"].template as<std::string>());
    v.pricingLogicAlgorithm = parse_array<bss_sid::PricingLogicAlgorithm>(
        row["pricing_logic_algorithm"].template as<std::string>());
    v.productOfferingTerm = parse_array<bss_sid::ProductOfferingTerm>(
        row["product_offering_term"].template as<std::string>());
    v.tax = parse_array<bss_sid::TaxItem>(row["tax"].template as<std::string>());
    v.validFor = time_period_from(row["valid_from"].template as<std::optional<std::string>>(),
                                  row["valid_to"].template as<std::optional<std::string>>());
    return v;
}

template <typename Row> bss_sid::ProductSpecification row_to_spec(const Row& row) {
    bss_sid::ProductSpecification v;
    v.id = row["id"].template as<std::optional<std::string>>();
    v.href = row["href"].template as<std::optional<std::string>>();
    v.brand = row["brand"].template as<std::optional<std::string>>();
    v.description = row["description"].template as<std::optional<std::string>>();
    v.isBundle = row["is_bundle"].template as<std::optional<bool>>();
    v.lifecycleStatus = row["lifecycle_status"].template as<std::optional<std::string>>();
    v.lastUpdate = row["last_update"].template as<std::optional<std::string>>();
    v.name = row["name"].template as<std::optional<std::string>>();
    v.productNumber = row["product_number"].template as<std::optional<std::string>>();
    v.version = row["version"].template as<std::optional<std::string>>();
    v.productSpecCharacteristic = parse_array<bss_sid::ProductSpecificationCharacteristic>(
        row["product_spec_characteristic"].template as<std::string>());
    v.attachment =
        parse_array<bss_sid::AttachmentRefOrValue>(row["attachment"].template as<std::string>());
    v.bundledProductSpecification = parse_array<bss_sid::BundledProductSpecification>(
        row["bundled_product_specification"].template as<std::string>());
    v.productSpecificationRelationship = parse_array<bss_sid::ProductSpecificationRelationship>(
        row["product_specification_relationship"].template as<std::string>());
    v.relatedParty =
        parse_array<bss_sid::RelatedParty>(row["related_party"].template as<std::string>());
    v.resourceSpecification = parse_array<bss_sid::ResourceSpecificationRef>(
        row["resource_specification"].template as<std::string>());
    v.serviceSpecification = parse_array<bss_sid::ServiceSpecificationRef>(
        row["service_specification"].template as<std::string>());
    // Not routed through the generic parse_optional<T>() helper: T=nlohmann::json creates a real
    // overload-resolution ambiguity between std::optional's own converting constructor and
    // nlohmann::json's own templated operator ValueType() -- both real, valid conversions for this
    // one specific T, harmless (json::parse's result already IS the value, so either resolution
    // gives the same result) but a compiler warning worth silencing at the one real call site
    // rather than complicating the shared helper for every other T that doesn't have this
    // ambiguity.
    if (const auto raw = row["target_product_schema"].template as<std::optional<std::string>>();
        raw.has_value()) {
        v.targetProductSchema = nlohmann::json::parse(*raw);
    }
    v.validFor = time_period_from(row["valid_from"].template as<std::optional<std::string>>(),
                                  row["valid_to"].template as<std::optional<std::string>>());
    return v;
}

} // namespace

// --- ProductOfferingStore ---

ProductOfferingStore::ProductOfferingStore(std::string resource_url, const std::string& conninfo, std::size_t pool_size)
    : resource_url_(std::move(resource_url)), pool_(conninfo, pool_size) {}

std::string ProductOfferingStore::create(bss_sid::ProductOffering offering) {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    const auto id = txn.exec("SELECT nextval('product_offering_id_seq')::text AS id")
                        .one_row()["id"]
                        .as<std::string>();
    offering.id = id;
    offering.href = resource_url_ + "/" + id;
    txn.exec(
        "INSERT INTO product_offering "
        "(id, href, name, description, lifecycle_status, last_update, status_reason, is_bundle, "
        "is_sellable, version, valid_from, valid_to, product_offering_price, category, channel, "
        "market_segment, prod_spec_char_value_use, product_specification, resource_candidate, "
        "service_candidate, service_level_agreement, agreement, bundled_product_offering, "
        "attachment, place, product_offering_relationship, product_offering_term) "
        "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13::jsonb,$14::jsonb,$15::jsonb,"
        "$16::jsonb,$17::jsonb,$18::jsonb,$19::jsonb,$20::jsonb,$21::jsonb,$22::jsonb,$23::jsonb,"
        "$24::jsonb,$25::jsonb,$26::jsonb,$27::jsonb)",
        pqxx::params{id,
                     offering.href,
                     offering.name,
                     offering.description,
                     offering.lifecycleStatus,
                     offering.lastUpdate,
                     offering.statusReason,
                     offering.isBundle,
                     offering.isSellable,
                     offering.version,
                     valid_from_of(offering.validFor),
                     valid_to_of(offering.validFor),
                     dump_array(offering.productOfferingPrice),
                     dump_array(offering.category),
                     dump_array(offering.channel),
                     dump_array(offering.marketSegment),
                     dump_array(offering.prodSpecCharValueUse),
                     dump_optional(offering.productSpecification),
                     dump_optional(offering.resourceCandidate),
                     dump_optional(offering.serviceCandidate),
                     dump_optional(offering.serviceLevelAgreement),
                     dump_array(offering.agreement),
                     dump_array(offering.bundledProductOffering),
                     dump_array(offering.attachment),
                     dump_array(offering.place),
                     dump_array(offering.productOfferingRelationship),
                     dump_array(offering.productOfferingTerm)});
    write_audit_record(
        txn, "PRODUCT_OFFERING", id, "productOffering.create", json(offering).dump());
    txn.commit();
    return id;
}

std::optional<bss_sid::ProductOffering> ProductOfferingStore::get(const std::string& id) {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    const auto result = txn.exec("SELECT * FROM product_offering WHERE id = $1", pqxx::params{id});
    if (result.empty()) {
        return std::nullopt;
    }
    return row_to_offering(result.front());
}

std::vector<bss_sid::ProductOffering> ProductOfferingStore::list() {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    const auto result = txn.exec("SELECT * FROM product_offering ORDER BY id");
    std::vector<bss_sid::ProductOffering> out;
    out.reserve(static_cast<std::size_t>(result.size()));
    for (const auto& row : result) {
        out.push_back(row_to_offering(row));
    }
    return out;
}

bool ProductOfferingStore::remove(const std::string& id) {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    const auto result = txn.exec("DELETE FROM product_offering WHERE id = $1", pqxx::params{id});
    const bool removed = result.affected_rows() > 0;
    if (removed) {
        write_audit_record(txn, "PRODUCT_OFFERING", id, "productOffering.remove", std::nullopt);
    }
    txn.commit();
    return removed;
}

// --- ProductOfferingPriceStore ---

ProductOfferingPriceStore::ProductOfferingPriceStore(std::string resource_url,
                                                     const std::string& conninfo, std::size_t pool_size)
    : resource_url_(std::move(resource_url)), pool_(conninfo, pool_size) {}

std::string ProductOfferingPriceStore::create(bss_sid::ProductOfferingPrice price) {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    const auto id = txn.exec("SELECT nextval('product_offering_price_id_seq')::text AS id")
                        .one_row()["id"]
                        .as<std::string>();
    price.id = id;
    price.href = resource_url_ + "/" + id;
    txn.exec(
        "INSERT INTO product_offering_price "
        "(id, href, name, description, lifecycle_status, last_update, price_type, percentage, "
        "version, price, recurring_charge_period_length, recurring_charge_period_type, "
        "unit_of_measure, prod_spec_char_value_use, bundled_pop_relationship, constraint_ref, "
        "place, pop_relationship, pricing_logic_algorithm, product_offering_term, tax, "
        "valid_from, valid_to) "
        "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10::jsonb,$11,$12,$13::jsonb,$14::jsonb,$15::jsonb,"
        "$16::jsonb,$17::jsonb,$18::jsonb,$19::jsonb,$20::jsonb,$21::jsonb,$22,$23)",
        pqxx::params{id,
                     price.href,
                     price.name,
                     price.description,
                     price.lifecycleStatus,
                     price.lastUpdate,
                     price.priceType,
                     price.percentage,
                     price.version,
                     dump_optional(price.price),
                     price.recurringChargePeriodLength,
                     price.recurringChargePeriodType,
                     dump_optional(price.unitOfMeasure),
                     dump_array(price.prodSpecCharValueUse),
                     dump_array(price.bundledPopRelationship),
                     dump_array(price.constraint),
                     dump_array(price.place),
                     dump_array(price.popRelationship),
                     dump_array(price.pricingLogicAlgorithm),
                     dump_array(price.productOfferingTerm),
                     dump_array(price.tax),
                     valid_from_of(price.validFor),
                     valid_to_of(price.validFor)});
    write_audit_record(
        txn, "PRODUCT_OFFERING_PRICE", id, "productOfferingPrice.create", json(price).dump());
    txn.commit();
    return id;
}

std::optional<bss_sid::ProductOfferingPrice> ProductOfferingPriceStore::get(const std::string& id) {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    const auto result =
        txn.exec("SELECT * FROM product_offering_price WHERE id = $1", pqxx::params{id});
    if (result.empty()) {
        return std::nullopt;
    }
    return row_to_price(result.front());
}

std::vector<bss_sid::ProductOfferingPrice> ProductOfferingPriceStore::list() {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    const auto result = txn.exec("SELECT * FROM product_offering_price ORDER BY id");
    std::vector<bss_sid::ProductOfferingPrice> out;
    out.reserve(static_cast<std::size_t>(result.size()));
    for (const auto& row : result) {
        out.push_back(row_to_price(row));
    }
    return out;
}

bool ProductOfferingPriceStore::remove(const std::string& id) {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    const auto result =
        txn.exec("DELETE FROM product_offering_price WHERE id = $1", pqxx::params{id});
    const bool removed = result.affected_rows() > 0;
    if (removed) {
        write_audit_record(
            txn, "PRODUCT_OFFERING_PRICE", id, "productOfferingPrice.remove", std::nullopt);
    }
    txn.commit();
    return removed;
}

// --- ProductSpecificationStore ---

ProductSpecificationStore::ProductSpecificationStore(std::string resource_url,
                                                     const std::string& conninfo, std::size_t pool_size)
    : resource_url_(std::move(resource_url)), pool_(conninfo, pool_size) {}

std::string ProductSpecificationStore::create(bss_sid::ProductSpecification spec) {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    const auto id = txn.exec("SELECT nextval('product_specification_id_seq')::text AS id")
                        .one_row()["id"]
                        .as<std::string>();
    spec.id = id;
    spec.href = resource_url_ + "/" + id;
    txn.exec("INSERT INTO product_specification "
             "(id, href, brand, description, is_bundle, lifecycle_status, last_update, name, "
             "product_number, version, product_spec_characteristic, attachment, "
             "bundled_product_specification, product_specification_relationship, related_party, "
             "resource_specification, service_specification, target_product_schema, valid_from, "
             "valid_to) "
             "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11::jsonb,$12::jsonb,$13::jsonb,$14::jsonb,"
             "$15::jsonb,$16::jsonb,$17::jsonb,$18::jsonb,$19,$20)",
             pqxx::params{id,
                          spec.href,
                          spec.brand,
                          spec.description,
                          spec.isBundle,
                          spec.lifecycleStatus,
                          spec.lastUpdate,
                          spec.name,
                          spec.productNumber,
                          spec.version,
                          dump_array(spec.productSpecCharacteristic),
                          dump_array(spec.attachment),
                          dump_array(spec.bundledProductSpecification),
                          dump_array(spec.productSpecificationRelationship),
                          dump_array(spec.relatedParty),
                          dump_array(spec.resourceSpecification),
                          dump_array(spec.serviceSpecification),
                          dump_optional(spec.targetProductSchema),
                          valid_from_of(spec.validFor),
                          valid_to_of(spec.validFor)});
    write_audit_record(
        txn, "PRODUCT_SPECIFICATION", id, "productSpecification.create", json(spec).dump());
    txn.commit();
    return id;
}

std::optional<bss_sid::ProductSpecification> ProductSpecificationStore::get(const std::string& id) {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    const auto result =
        txn.exec("SELECT * FROM product_specification WHERE id = $1", pqxx::params{id});
    if (result.empty()) {
        return std::nullopt;
    }
    return row_to_spec(result.front());
}

std::vector<bss_sid::ProductSpecification> ProductSpecificationStore::list() {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    const auto result = txn.exec("SELECT * FROM product_specification ORDER BY id");
    std::vector<bss_sid::ProductSpecification> out;
    out.reserve(static_cast<std::size_t>(result.size()));
    for (const auto& row : result) {
        out.push_back(row_to_spec(row));
    }
    return out;
}

bool ProductSpecificationStore::remove(const std::string& id) {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    const auto result =
        txn.exec("DELETE FROM product_specification WHERE id = $1", pqxx::params{id});
    const bool removed = result.affected_rows() > 0;
    if (removed) {
        write_audit_record(
            txn, "PRODUCT_SPECIFICATION", id, "productSpecification.remove", std::nullopt);
    }
    txn.commit();
    return removed;
}

} // namespace product_catalog
