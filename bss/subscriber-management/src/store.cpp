#include "store.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace subscriber_management {

namespace {

using nlohmann::json;

// Same small put_optional/get_optional pattern libs/bss-sid/src/party.cpp's own anonymous
// namespace already uses -- not shared across translation units, so re-declared locally here for
// these two project-internal (non-TMF632) structs.
template <typename T> void put_optional(json& j, const char* key, const std::optional<T>& v) {
    if (v.has_value()) {
        j[key] = *v;
    }
}

template <typename T> void get_optional(const json& j, const char* key, std::optional<T>& v) {
    if (const auto it = j.find(key); it != j.end() && !it->is_null()) {
        v = it->template get<T>();
    } else {
        v = std::nullopt;
    }
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

// Templated (not `const pqxx::row&`) -- same real, verified libpqxx 8.x `row_ref`/`row` shape
// difference this project's other stores already needed (ADR-0054).
template <typename Row> bss_sid::Individual row_to_individual(const Row& row) {
    bss_sid::Individual v;
    v.id = row["id"].template as<std::optional<std::string>>();
    v.href = row["href"].template as<std::optional<std::string>>();
    v.aristocraticTitle = row["aristocratic_title"].template as<std::optional<std::string>>();
    v.birthDate = row["birth_date"].template as<std::optional<std::string>>();
    v.countryOfBirth = row["country_of_birth"].template as<std::optional<std::string>>();
    v.deathDate = row["death_date"].template as<std::optional<std::string>>();
    v.familyName = row["family_name"].template as<std::optional<std::string>>();
    v.familyNamePrefix = row["family_name_prefix"].template as<std::optional<std::string>>();
    v.formattedName = row["formatted_name"].template as<std::optional<std::string>>();
    v.fullName = row["full_name"].template as<std::optional<std::string>>();
    v.gender = row["gender"].template as<std::optional<std::string>>();
    v.generation = row["generation"].template as<std::optional<std::string>>();
    v.givenName = row["given_name"].template as<std::optional<std::string>>();
    v.legalName = row["legal_name"].template as<std::optional<std::string>>();
    v.location = row["location"].template as<std::optional<std::string>>();
    v.maritalStatus = row["marital_status"].template as<std::optional<std::string>>();
    v.middleName = row["middle_name"].template as<std::optional<std::string>>();
    v.nationality = row["nationality"].template as<std::optional<std::string>>();
    v.placeOfBirth = row["place_of_birth"].template as<std::optional<std::string>>();
    v.preferredGivenName = row["preferred_given_name"].template as<std::optional<std::string>>();
    v.title = row["title"].template as<std::optional<std::string>>();
    v.contactMedium =
        parse_array<bss_sid::ContactMedium>(row["contact_medium"].template as<std::string>());
    v.creditRating =
        parse_array<bss_sid::PartyCreditProfile>(row["credit_rating"].template as<std::string>());
    v.disability = parse_array<bss_sid::Disability>(row["disability"].template as<std::string>());
    v.externalReference = parse_array<bss_sid::ExternalReference>(
        row["external_reference"].template as<std::string>());
    v.individualIdentification = parse_array<bss_sid::IndividualIdentification>(
        row["individual_identification"].template as<std::string>());
    v.languageAbility =
        parse_array<bss_sid::LanguageAbility>(row["language_ability"].template as<std::string>());
    v.otherName =
        parse_array<bss_sid::OtherNameIndividual>(row["other_name"].template as<std::string>());
    v.partyCharacteristic = parse_array<bss_sid::Characteristic>(
        row["party_characteristic"].template as<std::string>());
    v.relatedParty =
        parse_array<bss_sid::RelatedParty>(row["related_party"].template as<std::string>());
    v.skill = parse_array<bss_sid::Skill>(row["skill"].template as<std::string>());
    v.status = row["status"].template as<std::optional<std::string>>();
    v.taxExemptionCertificate = parse_array<bss_sid::TaxExemptionCertificate>(
        row["tax_exemption_certificate"].template as<std::string>());
    return v;
}

template <typename Row> bss_sid::Organization row_to_organization(const Row& row) {
    bss_sid::Organization v;
    v.id = row["id"].template as<std::optional<std::string>>();
    v.href = row["href"].template as<std::optional<std::string>>();
    v.isHeadOffice = row["is_head_office"].template as<std::optional<bool>>();
    v.isLegalEntity = row["is_legal_entity"].template as<std::optional<bool>>();
    v.name = row["name"].template as<std::optional<std::string>>();
    v.nameType = row["name_type"].template as<std::optional<std::string>>();
    v.organizationType = row["organization_type"].template as<std::optional<std::string>>();
    v.tradingName = row["trading_name"].template as<std::optional<std::string>>();
    v.contactMedium =
        parse_array<bss_sid::ContactMedium>(row["contact_medium"].template as<std::string>());
    v.creditRating =
        parse_array<bss_sid::PartyCreditProfile>(row["credit_rating"].template as<std::string>());
    v.existsDuring =
        time_period_from(row["exists_during_from"].template as<std::optional<std::string>>(),
                         row["exists_during_to"].template as<std::optional<std::string>>());
    v.externalReference = parse_array<bss_sid::ExternalReference>(
        row["external_reference"].template as<std::string>());
    v.organizationChildRelationship = parse_array<bss_sid::OrganizationChildRelationship>(
        row["organization_child_relationship"].template as<std::string>());
    v.organizationIdentification = parse_array<bss_sid::OrganizationIdentification>(
        row["organization_identification"].template as<std::string>());
    v.organizationParentRelationship = parse_optional<bss_sid::OrganizationParentRelationship>(
        row["organization_parent_relationship"].template as<std::optional<std::string>>());
    v.otherName =
        parse_array<bss_sid::OtherNameOrganization>(row["other_name"].template as<std::string>());
    v.partyCharacteristic = parse_array<bss_sid::Characteristic>(
        row["party_characteristic"].template as<std::string>());
    v.relatedParty =
        parse_array<bss_sid::RelatedParty>(row["related_party"].template as<std::string>());
    v.status = row["status"].template as<std::optional<std::string>>();
    v.taxExemptionCertificate = parse_array<bss_sid::TaxExemptionCertificate>(
        row["tax_exemption_certificate"].template as<std::string>());
    return v;
}

template <typename Row> Account row_to_account(const Row& row) {
    Account v;
    v.id = row["id"].template as<std::optional<std::string>>();
    v.accountKind = row["account_kind"].template as<std::string>();
    v.parentAccountId = row["parent_account_id"].template as<std::optional<std::string>>();
    v.organizationId = row["organization_id"].template as<std::optional<std::string>>();
    v.billingMode = row["billing_mode"].template as<std::optional<std::string>>();
    v.costCenter = row["cost_center"].template as<std::optional<std::string>>();
    v.contractSlaId = row["contract_sla_id"].template as<std::optional<std::string>>();
    v.provisioningMode = row["provisioning_mode"].template as<std::optional<std::string>>();
    return v;
}

template <typename Row> Subscriber row_to_subscriber(const Row& row) {
    Subscriber v;
    v.id = row["id"].template as<std::optional<std::string>>();
    v.supi = row["supi"].template as<std::string>();
    v.individualId = row["individual_id"].template as<std::optional<std::string>>();
    v.msisdn = row["msisdn"].template as<std::optional<std::string>>();
    v.accountId = row["account_id"].template as<std::optional<std::string>>();
    v.chargingMode = row["charging_mode"].template as<std::optional<std::string>>();
    v.billCycleDay = row["bill_cycle_day"].template as<std::optional<int>>();
    const auto prefs = row["service_preferences"].template as<std::string>();
    v.servicePreferences = prefs.empty() ? json::object() : json::parse(prefs);
    return v;
}

} // namespace

// --- PartyIndividualStore ---

PartyIndividualStore::PartyIndividualStore(std::string resource_url, const std::string& conninfo)
    : resource_url_(std::move(resource_url)), conn_(conninfo) {}

std::string PartyIndividualStore::create(bss_sid::Individual individual) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto id = txn.exec("SELECT nextval('party_individual_id_seq')::text AS id")
                        .one_row()["id"]
                        .as<std::string>();
    individual.id = id;
    individual.href = resource_url_ + "/" + id;
    txn.exec(
        "INSERT INTO party_individual "
        "(id, href, aristocratic_title, birth_date, country_of_birth, death_date, family_name, "
        "family_name_prefix, formatted_name, full_name, gender, generation, given_name, "
        "legal_name, location, marital_status, middle_name, nationality, place_of_birth, "
        "preferred_given_name, title, contact_medium, credit_rating, disability, "
        "external_reference, individual_identification, language_ability, other_name, "
        "party_characteristic, related_party, skill, status, tax_exemption_certificate) "
        "VALUES "
        "($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17,$18,$19,$20,$21,$22::jsonb,"
        "$23::jsonb,$24::jsonb,$25::jsonb,$26::jsonb,$27::jsonb,$28::jsonb,$29::jsonb,$30::jsonb,"
        "$31::jsonb,$32,$33::jsonb)",
        pqxx::params{id,
                     individual.href,
                     individual.aristocraticTitle,
                     individual.birthDate,
                     individual.countryOfBirth,
                     individual.deathDate,
                     individual.familyName,
                     individual.familyNamePrefix,
                     individual.formattedName,
                     individual.fullName,
                     individual.gender,
                     individual.generation,
                     individual.givenName,
                     individual.legalName,
                     individual.location,
                     individual.maritalStatus,
                     individual.middleName,
                     individual.nationality,
                     individual.placeOfBirth,
                     individual.preferredGivenName,
                     individual.title,
                     dump_array(individual.contactMedium),
                     dump_array(individual.creditRating),
                     dump_array(individual.disability),
                     dump_array(individual.externalReference),
                     dump_array(individual.individualIdentification),
                     dump_array(individual.languageAbility),
                     dump_array(individual.otherName),
                     dump_array(individual.partyCharacteristic),
                     dump_array(individual.relatedParty),
                     dump_array(individual.skill),
                     individual.status,
                     dump_array(individual.taxExemptionCertificate)});
    txn.commit();
    return id;
}

std::optional<bss_sid::Individual> PartyIndividualStore::get(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT * FROM party_individual WHERE id = $1", pqxx::params{id});
    if (result.empty()) {
        return std::nullopt;
    }
    return row_to_individual(result.front());
}

std::vector<bss_sid::Individual> PartyIndividualStore::list() {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT * FROM party_individual ORDER BY id");
    std::vector<bss_sid::Individual> out;
    out.reserve(static_cast<std::size_t>(result.size()));
    for (const auto& row : result) {
        out.push_back(row_to_individual(row));
    }
    return out;
}

// --- PartyOrganizationStore ---

PartyOrganizationStore::PartyOrganizationStore(std::string resource_url,
                                               const std::string& conninfo)
    : resource_url_(std::move(resource_url)), conn_(conninfo) {}

std::string PartyOrganizationStore::create(bss_sid::Organization organization) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto id = txn.exec("SELECT nextval('party_organization_id_seq')::text AS id")
                        .one_row()["id"]
                        .as<std::string>();
    organization.id = id;
    organization.href = resource_url_ + "/" + id;
    txn.exec(
        "INSERT INTO party_organization "
        "(id, href, is_head_office, is_legal_entity, name, name_type, organization_type, "
        "trading_name, contact_medium, credit_rating, exists_during_from, exists_during_to, "
        "external_reference, organization_child_relationship, organization_identification, "
        "organization_parent_relationship, other_name, party_characteristic, related_party, "
        "status, tax_exemption_certificate) "
        "VALUES "
        "($1,$2,$3,$4,$5,$6,$7,$8,$9::jsonb,$10::jsonb,$11,$12,$13::jsonb,$14::jsonb,$15::jsonb,"
        "$16::jsonb,$17::jsonb,$18::jsonb,$19::jsonb,$20,$21::jsonb)",
        pqxx::params{
            id,
            organization.href,
            organization.isHeadOffice,
            organization.isLegalEntity,
            organization.name,
            organization.nameType,
            organization.organizationType,
            organization.tradingName,
            dump_array(organization.contactMedium),
            dump_array(organization.creditRating),
            organization.existsDuring.has_value() ? organization.existsDuring->startDateTime
                                                  : std::optional<std::string>{std::nullopt},
            organization.existsDuring.has_value() ? organization.existsDuring->endDateTime
                                                  : std::optional<std::string>{std::nullopt},
            dump_array(organization.externalReference),
            dump_array(organization.organizationChildRelationship),
            dump_array(organization.organizationIdentification),
            dump_optional(organization.organizationParentRelationship),
            dump_array(organization.otherName),
            dump_array(organization.partyCharacteristic),
            dump_array(organization.relatedParty),
            organization.status,
            dump_array(organization.taxExemptionCertificate)});
    txn.commit();
    return id;
}

std::optional<bss_sid::Organization> PartyOrganizationStore::get(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT * FROM party_organization WHERE id = $1", pqxx::params{id});
    if (result.empty()) {
        return std::nullopt;
    }
    return row_to_organization(result.front());
}

std::vector<bss_sid::Organization> PartyOrganizationStore::list() {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT * FROM party_organization ORDER BY id");
    std::vector<bss_sid::Organization> out;
    out.reserve(static_cast<std::size_t>(result.size()));
    for (const auto& row : result) {
        out.push_back(row_to_organization(row));
    }
    return out;
}

// --- AccountStore ---

AccountStore::AccountStore(std::string resource_url, const std::string& conninfo)
    : resource_url_(std::move(resource_url)), conn_(conninfo) {}

std::string AccountStore::create(Account account) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto id =
        txn.exec("SELECT nextval('account_id_seq')::text AS id").one_row()["id"].as<std::string>();
    account.id = id;
    txn.exec("INSERT INTO account "
             "(id, account_kind, parent_account_id, organization_id, billing_mode, cost_center, "
             "contract_sla_id, provisioning_mode) "
             "VALUES ($1,$2,$3,$4,$5,$6,$7,$8)",
             pqxx::params{id,
                          account.accountKind,
                          account.parentAccountId,
                          account.organizationId,
                          account.billingMode,
                          account.costCenter,
                          account.contractSlaId,
                          account.provisioningMode});
    txn.commit();
    return id;
}

std::optional<Account> AccountStore::get(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT * FROM account WHERE id = $1", pqxx::params{id});
    if (result.empty()) {
        return std::nullopt;
    }
    return row_to_account(result.front());
}

std::vector<Account> AccountStore::list() {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT * FROM account ORDER BY id");
    std::vector<Account> out;
    out.reserve(static_cast<std::size_t>(result.size()));
    for (const auto& row : result) {
        out.push_back(row_to_account(row));
    }
    return out;
}

// --- SubscriberStore ---

SubscriberStore::SubscriberStore(std::string resource_url, const std::string& conninfo)
    : resource_url_(std::move(resource_url)), conn_(conninfo) {}

std::string SubscriberStore::create(Subscriber subscriber) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto id = txn.exec("SELECT nextval('subscriber_id_seq')::text AS id")
                        .one_row()["id"]
                        .as<std::string>();
    subscriber.id = id;
    txn.exec("INSERT INTO subscriber "
             "(id, supi, individual_id, msisdn, account_id, charging_mode, bill_cycle_day, "
             "service_preferences) "
             "VALUES ($1,$2,$3,$4,$5,$6,$7,$8::jsonb)",
             pqxx::params{id,
                          subscriber.supi,
                          subscriber.individualId,
                          subscriber.msisdn,
                          subscriber.accountId,
                          subscriber.chargingMode,
                          subscriber.billCycleDay,
                          subscriber.servicePreferences.dump()});
    txn.commit();
    return id;
}

std::optional<Subscriber> SubscriberStore::get(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT * FROM subscriber WHERE id = $1", pqxx::params{id});
    if (result.empty()) {
        return std::nullopt;
    }
    return row_to_subscriber(result.front());
}

std::optional<Subscriber> SubscriberStore::get_by_supi(const std::string& supi) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT * FROM subscriber WHERE supi = $1", pqxx::params{supi});
    if (result.empty()) {
        return std::nullopt;
    }
    return row_to_subscriber(result.front());
}

bool SubscriberStore::record_lifecycle_transition(const std::string& subscriber_id,
                                                  const std::string& to_status,
                                                  const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        pqxx::work txn(conn_);
        // Read the current status inside the transaction: taking it from the caller would let a
        // stale value be written as the `from` side of a transition that never happened.
        const auto rows =
            txn.exec("SELECT status FROM subscriber WHERE id = $1", pqxx::params{subscriber_id});
        if (rows.empty()) {
            return false;
        }
        const auto from_status = rows[0][0].as<std::string>("");
        txn.exec("UPDATE subscriber SET status = $1, updated_at = now() WHERE id = $2",
                 pqxx::params{to_status, subscriber_id});
        txn.exec("INSERT INTO subscriber_lifecycle_event (subscriber_id, from_status, to_status, "
                 "reason) VALUES ($1,$2,$3,$4)",
                 pqxx::params{subscriber_id, from_status, to_status, reason});
        txn.commit();
        return true;
    } catch (const std::exception& e) {
        spdlog::warn("subscriber-management: lifecycle transition for {} failed: {}",
                     subscriber_id,
                     e.what());
        return false;
    }
}

std::vector<Subscriber> SubscriberStore::list() {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT * FROM subscriber ORDER BY id");
    std::vector<Subscriber> out;
    out.reserve(static_cast<std::size_t>(result.size()));
    for (const auto& row : result) {
        out.push_back(row_to_subscriber(row));
    }
    return out;
}

void to_json(nlohmann::json& j, const Account& v) {
    j = nlohmann::json::object();
    put_optional(j, "id", v.id);
    j["accountKind"] = v.accountKind;
    put_optional(j, "parentAccountId", v.parentAccountId);
    put_optional(j, "organizationId", v.organizationId);
    put_optional(j, "billingMode", v.billingMode);
    put_optional(j, "costCenter", v.costCenter);
    put_optional(j, "contractSlaId", v.contractSlaId);
    put_optional(j, "provisioningMode", v.provisioningMode);
}

void from_json(const nlohmann::json& j, Account& v) {
    get_optional(j, "id", v.id);
    j.at("accountKind").get_to(v.accountKind);
    get_optional(j, "parentAccountId", v.parentAccountId);
    get_optional(j, "organizationId", v.organizationId);
    get_optional(j, "billingMode", v.billingMode);
    get_optional(j, "costCenter", v.costCenter);
    get_optional(j, "contractSlaId", v.contractSlaId);
    get_optional(j, "provisioningMode", v.provisioningMode);
}

void to_json(nlohmann::json& j, const Subscriber& v) {
    j = nlohmann::json::object();
    put_optional(j, "id", v.id);
    j["supi"] = v.supi;
    put_optional(j, "individualId", v.individualId);
    put_optional(j, "msisdn", v.msisdn);
    put_optional(j, "accountId", v.accountId);
    put_optional(j, "chargingMode", v.chargingMode);
    put_optional(j, "billCycleDay", v.billCycleDay);
    j["servicePreferences"] = v.servicePreferences;
}

void from_json(const nlohmann::json& j, Subscriber& v) {
    get_optional(j, "id", v.id);
    j.at("supi").get_to(v.supi);
    get_optional(j, "individualId", v.individualId);
    get_optional(j, "msisdn", v.msisdn);
    get_optional(j, "accountId", v.accountId);
    get_optional(j, "chargingMode", v.chargingMode);
    get_optional(j, "billCycleDay", v.billCycleDay);
    if (const auto it = j.find("servicePreferences"); it != j.end()) {
        v.servicePreferences = *it;
    }
}

} // namespace subscriber_management
