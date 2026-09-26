#include "store.hpp"

#include "sbi_core/datetime.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <map>

// ADR-0386: TMF632 Individual / Organization and the project's Account / Subscriber persisted in
// the consolidated charging DB -- schemas `party` (normalized TMF632, 10-party.sql made lossless by
// 21-party-subscriber-lossless.sql) and `subscriber_mgmt` -- replacing the per-service JSONB rows.
//
// Round-trip contract: what the API accepted comes back field for field, lists in posted order.
// Canonicalisations (disclosed): date-times return as UTC RFC 3339 with milliseconds; an object
// whose members are all absent (e.g. a ContactMedium `characteristic: {}`) returns absent.
// Subscriber SUPI / MSISDN are SID Resources (subscriber_mgmt.resource rows), not columns.
// Reads are set-based (one query per table). Every statement is schema-qualified.

namespace subscriber_management {

namespace {

using nlohmann::json;

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

std::string ts(const std::string& col) {
    return "to_char(" + col + " AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"')";
}

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
std::optional<std::string> vf_s(const std::optional<bss_sid::TimePeriod>& vf) {
    return vf.has_value() ? ts_in(vf->startDateTime, "validFor.startDateTime") : std::nullopt;
}
std::optional<std::string> vf_e(const std::optional<bss_sid::TimePeriod>& vf) {
    return vf.has_value() ? ts_in(vf->endDateTime, "validFor.endDateTime") : std::nullopt;
}

template <typename Row> std::optional<std::string> s(const Row& row, const char* col) {
    return row[col].template as<std::optional<std::string>>();
}
template <typename Row> std::optional<bss_sid::TimePeriod> vf_out(const Row& row) {
    auto a = s(row, "vf_start");
    auto b = s(row, "vf_end");
    if (!a && !b) {
        return std::nullopt;
    }
    return bss_sid::TimePeriod{std::move(a), std::move(b)};
}
std::string vf_cols(const std::string& prefix = "") {
    return ts(prefix + "valid_for_start") + " AS vf_start, " + ts(prefix + "valid_for_end") +
           " AS vf_end";
}

template <typename T> std::optional<std::string> j_opt(const std::optional<T>& v) {
    return v.has_value() ? std::optional<std::string>(json(*v).dump()) : std::nullopt;
}
template <typename T, typename Row> std::optional<T> j_opt_out(const Row& row, const char* col) {
    const auto raw = s(row, col);
    return raw.has_value() ? std::optional<T>(json::parse(*raw).template get<T>()) : std::nullopt;
}

[[noreturn]] void rethrow_as_request_error(const pqxx::sql_error& e) {
    const std::string what = e.what();
    if (dynamic_cast<const pqxx::foreign_key_violation*>(&e) != nullptr) {
        const auto k = what.find("Key (");
        const auto end = k == std::string::npos ? k : what.find(" is not present", k);
        throw InvalidRequest(
            "a referenced entity does not exist" +
            (end == std::string::npos ? std::string() : ": " + what.substr(k, end - k)));
    }
    if (dynamic_cast<const pqxx::unique_violation*>(&e) != nullptr) {
        throw Conflict("an identifier (SUPI/MSISDN) is already an active resource of another "
                       "subscriber");
    }
    if (dynamic_cast<const pqxx::check_violation*>(&e) != nullptr) {
        throw InvalidRequest("a field has a value outside its allowed set");
    }
    if (dynamic_cast<const pqxx::not_null_violation*>(&e) != nullptr) {
        throw InvalidRequest("a mandatory field is missing");
    }
    throw;
}

// Owner of a shared party child table: an individual ("i") or an organization ("o").
struct Owner {
    const char* col; // individual_id | organization_id
    const char* tag; // i | o -- keeps surrogate ids of the two sequences apart
};
constexpr Owner kInd{"individual_id", "i"};
constexpr Owner kOrg{"organization_id", "o"};

std::string child_id(const Owner& o, const std::string& owner_id, const char* kind, std::size_t i) {
    return std::string(o.tag) + ":" + owner_id + ":" + kind + std::to_string(i);
}

// Runs `SELECT <cols> FROM party.<table>` restricted to one owner (or all) and ordered by owner,
// ordinal; hands each row to `add(owner_id, row)`.
template <typename Add>
void children(pqxx::work& txn,
              const std::string& cols,
              const char* table,
              const Owner& o,
              const std::optional<std::string>& only,
              Add add) {
    for (auto r :
         txn.exec("SELECT " + std::string(o.col) + " AS owner, " + cols + " FROM party." + table +
                      " WHERE " + o.col + " IS NOT NULL AND ($1::text IS NULL OR " + o.col +
                      " = $1) ORDER BY " + o.col + ", ordinal",
                  pqxx::params{only})) {
        add(r["owner"].template as<std::string>(), r);
    }
}

// ---- Children shared by Individual and Organization ---------------------------------------------

void insert_shared_children(pqxx::work& txn,
                            const Owner& o,
                            const std::string& id,
                            const std::vector<bss_sid::ContactMedium>& contact,
                            const std::vector<bss_sid::PartyCreditProfile>& credit,
                            const std::vector<bss_sid::ExternalReference>& ext,
                            const std::vector<bss_sid::Characteristic>& chars,
                            const std::vector<bss_sid::RelatedParty>& related,
                            const std::vector<bss_sid::TaxExemptionCertificate>& tax) {
    const std::string oc = o.col;
    for (std::size_t i = 0; i < contact.size(); ++i) {
        const auto& c = contact[i];
        const auto m = c.characteristic.value_or(bss_sid::MediumCharacteristic{});
        txn.exec("INSERT INTO party.contact_medium (id, " + oc +
                     ", ordinal, medium_type, preferred, valid_for_start, valid_for_end, city, "
                     "contact_type, country, email_address, fax_number, phone_number, post_code, "
                     "social_network_id, state_or_province) VALUES ($1,$2,$3,$4,$5,$6::timestamptz,"
                     "$7::timestamptz,$8,$9,$10,$11,$12,$13,$14,$15,$16)",
                 pqxx::params{child_id(o, id, "cm", i),
                              id,
                              static_cast<int>(i),
                              c.mediumType,
                              c.preferred,
                              vf_s(c.validFor),
                              vf_e(c.validFor),
                              m.city,
                              m.contactType,
                              m.country,
                              m.emailAddress,
                              m.faxNumber,
                              m.phoneNumber,
                              m.postCode,
                              m.socialNetworkId,
                              m.stateOrProvince});
    }
    for (std::size_t i = 0; i < credit.size(); ++i) {
        const auto& c = credit[i];
        txn.exec("INSERT INTO party.credit_profile (id, " + oc +
                     ", ordinal, credit_agency_name, credit_agency_type, rating_reference, "
                     "rating_score, valid_for_start, valid_for_end) VALUES ($1,$2,$3,$4,$5,$6,$7,"
                     "$8::timestamptz,$9::timestamptz)",
                 pqxx::params{child_id(o, id, "cr", i),
                              id,
                              static_cast<int>(i),
                              c.creditAgencyName,
                              c.creditAgencyType,
                              c.ratingReference,
                              c.ratingScore,
                              vf_s(c.validFor),
                              vf_e(c.validFor)});
    }
    for (std::size_t i = 0; i < ext.size(); ++i) {
        txn.exec("INSERT INTO party.external_reference (id, " + oc +
                     ", ordinal, external_reference_type, name) VALUES ($1,$2,$3,$4,$5)",
                 pqxx::params{child_id(o, id, "er", i),
                              id,
                              static_cast<int>(i),
                              ext[i].externalReferenceType,
                              ext[i].name});
    }
    for (std::size_t i = 0; i < chars.size(); ++i) {
        txn.exec("INSERT INTO party.party_characteristic (id, " + oc +
                     ", ordinal, name, value_type, value) VALUES ($1,$2,$3,$4,$5,$6::jsonb)",
                 pqxx::params{child_id(o, id, "pc", i),
                              id,
                              static_cast<int>(i),
                              chars[i].name,
                              chars[i].valueType,
                              chars[i].value.dump()});
    }
    for (std::size_t i = 0; i < related.size(); ++i) {
        txn.exec("INSERT INTO party.related_party (id, " + oc +
                     ", ordinal, related_party_id, href, name, role) VALUES ($1,$2,$3,$4,$5,$6,$7)",
                 pqxx::params{child_id(o, id, "rp", i),
                              id,
                              static_cast<int>(i),
                              related[i].id,
                              related[i].href,
                              related[i].name,
                              related[i].role});
    }
    for (std::size_t i = 0; i < tax.size(); ++i) {
        const auto& t = tax[i];
        const auto cert = child_id(o, id, "tx", i);
        txn.exec("INSERT INTO party.tax_exemption_certificate (id, " + oc +
                     ", ordinal, cert_id, attachment, valid_for_start, valid_for_end) VALUES "
                     "($1,$2,$3,$4,$5::jsonb,$6::timestamptz,$7::timestamptz)",
                 pqxx::params{cert,
                              id,
                              static_cast<int>(i),
                              t.id,
                              j_opt(t.attachment),
                              vf_s(t.validFor),
                              vf_e(t.validFor)});
        for (std::size_t k = 0; k < t.taxDefinition.size(); ++k) {
            const auto& d = t.taxDefinition[k];
            txn.exec("INSERT INTO party.tax_definition (id, certificate_id, ordinal, def_id, name, "
                     "tax_type) VALUES ($1,$2,$3,$4,$5,$6)",
                     pqxx::params{cert + ":d" + std::to_string(k),
                                  cert,
                                  static_cast<int>(k),
                                  d.id,
                                  d.name,
                                  d.taxType});
        }
    }
}

// Loads the shared children into `set(owner, kind, ...)` targets supplied by the caller.
template <typename Party>
void load_shared_children(pqxx::work& txn,
                          const Owner& o,
                          const std::optional<std::string>& only,
                          const std::map<std::string, Party*>& at) {
    const auto find = [&](const std::string& id) -> Party* {
        auto it = at.find(id);
        return it == at.end() ? nullptr : it->second;
    };
    children(txn,
             "medium_type, preferred, city, contact_type, country, email_address, fax_number, "
             "phone_number, post_code, social_network_id, state_or_province, " +
                 vf_cols(),
             "contact_medium",
             o,
             only,
             [&](const std::string& ow, const auto& r) {
                 if (auto* p = find(ow)) {
                     bss_sid::ContactMedium c;
                     c.mediumType = s(r, "medium_type");
                     c.preferred = r["preferred"].template as<std::optional<bool>>();
                     bss_sid::MediumCharacteristic m{s(r, "city"),
                                                     s(r, "contact_type"),
                                                     s(r, "country"),
                                                     s(r, "email_address"),
                                                     s(r, "fax_number"),
                                                     s(r, "phone_number"),
                                                     s(r, "post_code"),
                                                     s(r, "social_network_id"),
                                                     s(r, "state_or_province")};
                     if (json(m) != json::object()) {
                         c.characteristic = m;
                     }
                     c.validFor = vf_out(r);
                     p->contactMedium.push_back(std::move(c));
                 }
             });
    children(txn,
             "credit_agency_name, credit_agency_type, rating_reference, rating_score, " + vf_cols(),
             "credit_profile",
             o,
             only,
             [&](const std::string& ow, const auto& r) {
                 if (auto* p = find(ow)) {
                     p->creditRating.push_back({s(r, "credit_agency_name"),
                                                s(r, "credit_agency_type"),
                                                s(r, "rating_reference"),
                                                r["rating_score"].template as<std::optional<int>>(),
                                                vf_out(r)});
                 }
             });
    children(txn,
             "external_reference_type, name",
             "external_reference",
             o,
             only,
             [&](const std::string& ow, const auto& r) {
                 if (auto* p = find(ow)) {
                     p->externalReference.push_back(
                         {s(r, "external_reference_type"), s(r, "name")});
                 }
             });
    children(txn,
             "name, value_type, value::text AS value",
             "party_characteristic",
             o,
             only,
             [&](const std::string& ow, const auto& r) {
                 if (auto* p = find(ow)) {
                     p->partyCharacteristic.push_back(
                         {r["name"].template as<std::string>(),
                          s(r, "value_type"),
                          json::parse(r["value"].template as<std::string>())});
                 }
             });
    children(txn,
             "related_party_id, href, name, role",
             "related_party",
             o,
             only,
             [&](const std::string& ow, const auto& r) {
                 if (auto* p = find(ow)) {
                     p->relatedParty.push_back({s(r, "related_party_id").value_or(""),
                                                s(r, "href"),
                                                s(r, "name"),
                                                s(r, "role")});
                 }
             });
    std::map<std::string, std::vector<bss_sid::TaxDefinition>> defs;
    for (auto r : txn.exec("SELECT d.certificate_id, d.def_id, d.name, d.tax_type FROM "
                           "party.tax_definition d JOIN party.tax_exemption_certificate c ON "
                           "d.certificate_id = c.id WHERE c." +
                               std::string(o.col) + " IS NOT NULL AND ($1::text IS NULL OR c." +
                               o.col + " = $1) ORDER BY d.certificate_id, d.ordinal",
                           pqxx::params{only})) {
        defs[r["certificate_id"].as<std::string>()].push_back(
            {s(r, "def_id"), s(r, "name"), s(r, "tax_type")});
    }
    children(txn,
             "id, cert_id, attachment::text AS attachment, " + vf_cols(),
             "tax_exemption_certificate",
             o,
             only,
             [&](const std::string& ow, const auto& r) {
                 if (auto* p = find(ow)) {
                     bss_sid::TaxExemptionCertificate t;
                     t.id = s(r, "cert_id");
                     t.attachment = j_opt_out<bss_sid::AttachmentRefOrValue>(r, "attachment");
                     if (auto it = defs.find(r["id"].template as<std::string>());
                         it != defs.end()) {
                         t.taxDefinition = std::move(it->second);
                     }
                     t.validFor = vf_out(r);
                     p->taxExemptionCertificate.push_back(std::move(t));
                 }
             });
}

// ---- Individual
// ----------------------------------------------------------------------------------

std::vector<bss_sid::Individual> load_individuals(pqxx::work& txn,
                                                  const std::optional<std::string>& only) {
    std::vector<bss_sid::Individual> out;
    for (auto r : txn.exec("SELECT id, href, aristocratic_title, " + ts("birth_date") +
                               " AS birth_date, country_of_birth, " + ts("death_date") +
                               " AS death_date, family_name, family_name_prefix, formatted_name, "
                               "full_name, gender, generation, given_name, legal_name, location, "
                               "marital_status, middle_name, nationality, place_of_birth, "
                               "preferred_given_name, title, status FROM party.individual WHERE "
                               "($1::text IS NULL OR id = $1) ORDER BY id",
                           pqxx::params{only})) {
        bss_sid::Individual v;
        v.id = s(r, "id");
        v.href = s(r, "href");
        v.aristocraticTitle = s(r, "aristocratic_title");
        v.birthDate = s(r, "birth_date");
        v.countryOfBirth = s(r, "country_of_birth");
        v.deathDate = s(r, "death_date");
        v.familyName = s(r, "family_name");
        v.familyNamePrefix = s(r, "family_name_prefix");
        v.formattedName = s(r, "formatted_name");
        v.fullName = s(r, "full_name");
        v.gender = s(r, "gender");
        v.generation = s(r, "generation");
        v.givenName = s(r, "given_name");
        v.legalName = s(r, "legal_name");
        v.location = s(r, "location");
        v.maritalStatus = s(r, "marital_status");
        v.middleName = s(r, "middle_name");
        v.nationality = s(r, "nationality");
        v.placeOfBirth = s(r, "place_of_birth");
        v.preferredGivenName = s(r, "preferred_given_name");
        v.title = s(r, "title");
        v.status = s(r, "status");
        out.push_back(std::move(v));
    }
    if (out.empty()) {
        return out;
    }
    std::map<std::string, bss_sid::Individual*> at;
    for (auto& v : out) {
        at[*v.id] = &v;
    }
    const auto find = [&](const std::string& id) -> bss_sid::Individual* {
        auto it = at.find(id);
        return it == at.end() ? nullptr : it->second;
    };
    load_shared_children(txn, kInd, only, at);
    children(txn,
             "identification_type, identification_id, issuing_authority, " + ts("issuing_date") +
                 " AS issuing_date, attachment::text AS attachment, " + vf_cols(),
             "individual_identification",
             kInd,
             only,
             [&](const std::string& ow, const auto& r) {
                 if (auto* p = find(ow)) {
                     p->individualIdentification.push_back(
                         {s(r, "identification_type"),
                          s(r, "identification_id"),
                          s(r, "issuing_authority"),
                          s(r, "issuing_date"),
                          j_opt_out<bss_sid::AttachmentRefOrValue>(r, "attachment"),
                          vf_out(r)});
                 }
             });
    children(txn,
             "disability_code, disability_name, " + vf_cols(),
             "disability",
             kInd,
             only,
             [&](const std::string& ow, const auto& r) {
                 if (auto* p = find(ow)) {
                     p->disability.push_back(
                         {s(r, "disability_code"), s(r, "disability_name"), vf_out(r)});
                 }
             });
    children(txn,
             "is_favourite_language, language_code, language_name, listening_proficiency, "
             "reading_proficiency, speaking_proficiency, writing_proficiency, " +
                 vf_cols(),
             "language_ability",
             kInd,
             only,
             [&](const std::string& ow, const auto& r) {
                 if (auto* p = find(ow)) {
                     p->languageAbility.push_back(
                         {r["is_favourite_language"].template as<std::optional<bool>>(),
                          s(r, "language_code"),
                          s(r, "language_name"),
                          s(r, "listening_proficiency"),
                          s(r, "reading_proficiency"),
                          s(r, "speaking_proficiency"),
                          s(r, "writing_proficiency"),
                          vf_out(r)});
                 }
             });
    children(txn,
             "comment, evaluated_level, skill_code, skill_name, " + vf_cols(),
             "skill",
             kInd,
             only,
             [&](const std::string& ow, const auto& r) {
                 if (auto* p = find(ow)) {
                     p->skill.push_back({s(r, "comment"),
                                         s(r, "evaluated_level"),
                                         s(r, "skill_code"),
                                         s(r, "skill_name"),
                                         vf_out(r)});
                 }
             });
    children(txn,
             "aristocratic_title, family_name, family_name_prefix, formatted_name, full_name, "
             "generation, given_name, legal_name, middle_name, preferred_given_name, title, " +
                 vf_cols(),
             "other_name_individual",
             kInd,
             only,
             [&](const std::string& ow, const auto& r) {
                 if (auto* p = find(ow)) {
                     p->otherName.push_back({s(r, "aristocratic_title"),
                                             s(r, "family_name"),
                                             s(r, "family_name_prefix"),
                                             s(r, "formatted_name"),
                                             s(r, "full_name"),
                                             s(r, "generation"),
                                             s(r, "given_name"),
                                             s(r, "legal_name"),
                                             s(r, "middle_name"),
                                             s(r, "preferred_given_name"),
                                             s(r, "title"),
                                             vf_out(r)});
                 }
             });
    return out;
}

// ---- Organization
// --------------------------------------------------------------------------------

std::vector<bss_sid::Organization> load_organizations(pqxx::work& txn,
                                                      const std::optional<std::string>& only) {
    std::vector<bss_sid::Organization> out;
    for (auto r :
         txn.exec("SELECT id, href, is_head_office, is_legal_entity, name, name_type, "
                  "organization_type, trading_name, " +
                      ts("exists_during_start") + " AS ed_start, " + ts("exists_during_end") +
                      " AS ed_end, status, parent_relationship_type, "
                      "parent_organization_id, parent_organization_name, "
                      "parent_organization_href FROM party.organization WHERE ($1::text "
                      "IS NULL OR id = $1) ORDER BY id",
                  pqxx::params{only})) {
        bss_sid::Organization v;
        v.id = s(r, "id");
        v.href = s(r, "href");
        v.isHeadOffice = r["is_head_office"].as<std::optional<bool>>();
        v.isLegalEntity = r["is_legal_entity"].as<std::optional<bool>>();
        v.name = s(r, "name");
        v.nameType = s(r, "name_type");
        v.organizationType = s(r, "organization_type");
        v.tradingName = s(r, "trading_name");
        auto eds = s(r, "ed_start");
        auto ede = s(r, "ed_end");
        if (eds || ede) {
            v.existsDuring = bss_sid::TimePeriod{eds, ede};
        }
        v.status = s(r, "status");
        auto prt = s(r, "parent_relationship_type");
        auto pid = s(r, "parent_organization_id");
        auto pname = s(r, "parent_organization_name");
        auto phref = s(r, "parent_organization_href");
        if (prt || pid || pname || phref) {
            bss_sid::OrganizationParentRelationship rel;
            rel.relationshipType = prt;
            if (pid || pname || phref) {
                rel.organization = bss_sid::OrganizationRef{pid, phref, pname};
            }
            v.organizationParentRelationship = rel;
        }
        out.push_back(std::move(v));
    }
    if (out.empty()) {
        return out;
    }
    std::map<std::string, bss_sid::Organization*> at;
    for (auto& v : out) {
        at[*v.id] = &v;
    }
    const auto find = [&](const std::string& id) -> bss_sid::Organization* {
        auto it = at.find(id);
        return it == at.end() ? nullptr : it->second;
    };
    load_shared_children(txn, kOrg, only, at);
    children(txn,
             "identification_type, identification_id, issuing_authority, " + ts("issuing_date") +
                 " AS issuing_date, attachment::text AS attachment, " + vf_cols(),
             "organization_identification",
             kOrg,
             only,
             [&](const std::string& ow, const auto& r) {
                 if (auto* p = find(ow)) {
                     p->organizationIdentification.push_back(
                         {s(r, "identification_type"),
                          s(r, "identification_id"),
                          s(r, "issuing_authority"),
                          s(r, "issuing_date"),
                          j_opt_out<bss_sid::AttachmentRefOrValue>(r, "attachment"),
                          vf_out(r)});
                 }
             });
    children(txn,
             "name, name_type, trading_name, " + vf_cols(),
             "other_name_organization",
             kOrg,
             only,
             [&](const std::string& ow, const auto& r) {
                 if (auto* p = find(ow)) {
                     p->otherName.push_back(
                         {s(r, "name"), s(r, "name_type"), s(r, "trading_name"), vf_out(r)});
                 }
             });
    children(txn,
             "relationship_type, child_organization_id, child_organization_name, "
             "child_organization_href",
             "organization_child_relationship",
             kOrg,
             only,
             [&](const std::string& ow, const auto& r) {
                 if (auto* p = find(ow)) {
                     bss_sid::OrganizationChildRelationship c;
                     c.relationshipType = s(r, "relationship_type");
                     auto cid = s(r, "child_organization_id");
                     auto cname = s(r, "child_organization_name");
                     auto chref = s(r, "child_organization_href");
                     if (cid || cname || chref) {
                         c.organization = bss_sid::OrganizationRef{cid, chref, cname};
                     }
                     p->organizationChildRelationship.push_back(std::move(c));
                 }
             });
    return out;
}

// ---- Account / Subscriber rows
// -------------------------------------------------------------------

template <typename Row> Account account_out(const Row& r) {
    Account a;
    a.id = s(r, "id");
    a.accountKind = r["account_kind"].template as<std::string>();
    a.parentAccountId = s(r, "parent_account_id");
    a.organizationId = s(r, "organization_id");
    a.billingMode = s(r, "billing_mode");
    a.costCenter = s(r, "cost_center");
    a.contractSlaId = s(r, "contract_sla_id");
    a.provisioningMode = s(r, "provisioning_mode");
    return a;
}

// Subscribers with their SUPI / MSISDN resources, set-based; `where_sql` filters subscriber `s`.
std::vector<Subscriber>
load_subscribers(pqxx::work& txn, const std::string& where_sql, const pqxx::params& params) {
    std::vector<Subscriber> out;
    std::map<std::string, std::size_t> idx;
    for (auto r : txn.exec("SELECT s.id, s.individual_id, s.account_id, s.charging_mode, "
                           "s.bill_cycle_day, s.service_preferences::text AS prefs FROM "
                           "subscriber_mgmt.subscriber s " +
                               where_sql,
                           params)) {
        Subscriber v;
        v.id = s(r, "id");
        v.individualId = s(r, "individual_id");
        v.accountId = s(r, "account_id");
        v.chargingMode = s(r, "charging_mode");
        v.billCycleDay = r["bill_cycle_day"].as<std::optional<int>>();
        v.servicePreferences = json::parse(r["prefs"].as<std::string>());
        idx[*v.id] = out.size();
        out.push_back(std::move(v));
    }
    if (out.empty()) {
        return out;
    }
    for (auto r : txn.exec("SELECT subscriber_id, resource_type, resource_value FROM "
                           "subscriber_mgmt.resource WHERE status = 'active' AND resource_type IN "
                           "('SUPI','MSISDN') AND subscriber_id IN (SELECT s.id FROM "
                           "subscriber_mgmt.subscriber s " +
                               where_sql + ")",
                           params)) {
        auto& v = out[idx.at(r["subscriber_id"].as<std::string>())];
        if (r["resource_type"].as<std::string>() == "SUPI") {
            v.supi = r["resource_value"].as<std::string>();
        } else {
            v.msisdn = r["resource_value"].as<std::string>();
        }
    }
    return out;
}

} // namespace

// =================================================================================================

PartyIndividualStore::PartyIndividualStore(std::string resource_url, const std::string& conninfo)
    : resource_url_(std::move(resource_url)), conn_(conninfo) {}

std::string PartyIndividualStore::create(bss_sid::Individual v) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto id = txn.exec("SELECT nextval('party.individual_id_seq')::text AS id")
                        .one_row()["id"]
                        .as<std::string>();
    v.id = id;
    v.href = resource_url_ + "/" + id;
    try {
        txn.exec("INSERT INTO party.individual (id, href, aristocratic_title, birth_date, "
                 "country_of_birth, death_date, family_name, family_name_prefix, formatted_name, "
                 "full_name, gender, generation, given_name, legal_name, location, marital_status, "
                 "middle_name, nationality, place_of_birth, preferred_given_name, title, status) "
                 "VALUES ($1,$2,$3,$4::timestamptz,$5,$6::timestamptz,$7,$8,$9,$10,$11,$12,$13,"
                 "$14,$15,$16,$17,$18,$19,$20,$21,$22)",
                 pqxx::params{id,
                              v.href,
                              v.aristocraticTitle,
                              ts_in(v.birthDate, "birthDate"),
                              v.countryOfBirth,
                              ts_in(v.deathDate, "deathDate"),
                              v.familyName,
                              v.familyNamePrefix,
                              v.formattedName,
                              v.fullName,
                              v.gender,
                              v.generation,
                              v.givenName,
                              v.legalName,
                              v.location,
                              v.maritalStatus,
                              v.middleName,
                              v.nationality,
                              v.placeOfBirth,
                              v.preferredGivenName,
                              v.title,
                              v.status});
        insert_shared_children(txn,
                               kInd,
                               id,
                               v.contactMedium,
                               v.creditRating,
                               v.externalReference,
                               v.partyCharacteristic,
                               v.relatedParty,
                               v.taxExemptionCertificate);
        for (std::size_t i = 0; i < v.individualIdentification.size(); ++i) {
            const auto& x = v.individualIdentification[i];
            txn.exec("INSERT INTO party.individual_identification (id, individual_id, ordinal, "
                     "identification_type, identification_id, issuing_authority, issuing_date, "
                     "attachment, valid_for_start, valid_for_end) VALUES ($1,$2,$3,$4,$5,$6,"
                     "$7::timestamptz,$8::jsonb,$9::timestamptz,$10::timestamptz)",
                     pqxx::params{child_id(kInd, id, "id", i),
                                  id,
                                  static_cast<int>(i),
                                  x.identificationType,
                                  x.identificationId,
                                  x.issuingAuthority,
                                  ts_in(x.issuingDate, "issuingDate"),
                                  j_opt(x.attachment),
                                  vf_s(x.validFor),
                                  vf_e(x.validFor)});
        }
        for (std::size_t i = 0; i < v.disability.size(); ++i) {
            const auto& x = v.disability[i];
            txn.exec("INSERT INTO party.disability (id, individual_id, ordinal, disability_code, "
                     "disability_name, valid_for_start, valid_for_end) VALUES ($1,$2,$3,$4,$5,"
                     "$6::timestamptz,$7::timestamptz)",
                     pqxx::params{child_id(kInd, id, "di", i),
                                  id,
                                  static_cast<int>(i),
                                  x.disabilityCode,
                                  x.disabilityName,
                                  vf_s(x.validFor),
                                  vf_e(x.validFor)});
        }
        for (std::size_t i = 0; i < v.languageAbility.size(); ++i) {
            const auto& x = v.languageAbility[i];
            txn.exec("INSERT INTO party.language_ability (id, individual_id, ordinal, "
                     "is_favourite_language, language_code, language_name, listening_proficiency, "
                     "reading_proficiency, speaking_proficiency, writing_proficiency, "
                     "valid_for_start, valid_for_end) VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,"
                     "$11::timestamptz,$12::timestamptz)",
                     pqxx::params{child_id(kInd, id, "la", i),
                                  id,
                                  static_cast<int>(i),
                                  x.isFavouriteLanguage,
                                  x.languageCode,
                                  x.languageName,
                                  x.listeningProficiency,
                                  x.readingProficiency,
                                  x.speakingProficiency,
                                  x.writingProficiency,
                                  vf_s(x.validFor),
                                  vf_e(x.validFor)});
        }
        for (std::size_t i = 0; i < v.skill.size(); ++i) {
            const auto& x = v.skill[i];
            txn.exec("INSERT INTO party.skill (id, individual_id, ordinal, comment, "
                     "evaluated_level, skill_code, skill_name, valid_for_start, valid_for_end) "
                     "VALUES ($1,$2,$3,$4,$5,$6,$7,$8::timestamptz,$9::timestamptz)",
                     pqxx::params{child_id(kInd, id, "sk", i),
                                  id,
                                  static_cast<int>(i),
                                  x.comment,
                                  x.evaluatedLevel,
                                  x.skillCode,
                                  x.skillName,
                                  vf_s(x.validFor),
                                  vf_e(x.validFor)});
        }
        for (std::size_t i = 0; i < v.otherName.size(); ++i) {
            const auto& x = v.otherName[i];
            txn.exec("INSERT INTO party.other_name_individual (id, individual_id, ordinal, "
                     "aristocratic_title, family_name, family_name_prefix, formatted_name, "
                     "full_name, generation, given_name, legal_name, middle_name, "
                     "preferred_given_name, title, valid_for_start, valid_for_end) VALUES ($1,$2,"
                     "$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15::timestamptz,$16::timestamptz)",
                     pqxx::params{child_id(kInd, id, "on", i),
                                  id,
                                  static_cast<int>(i),
                                  x.aristocraticTitle,
                                  x.familyName,
                                  x.familyNamePrefix,
                                  x.formattedName,
                                  x.fullName,
                                  x.generation,
                                  x.givenName,
                                  x.legalName,
                                  x.middleName,
                                  x.preferredGivenName,
                                  x.title,
                                  vf_s(x.validFor),
                                  vf_e(x.validFor)});
        }
    } catch (const pqxx::sql_error& e) {
        rethrow_as_request_error(e);
    }
    txn.commit();
    return id;
}

std::optional<bss_sid::Individual> PartyIndividualStore::get(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    auto all = load_individuals(txn, id);
    if (all.empty()) {
        return std::nullopt;
    }
    return std::move(all.front());
}

std::vector<bss_sid::Individual> PartyIndividualStore::list() {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    return load_individuals(txn, std::nullopt);
}

// =================================================================================================

PartyOrganizationStore::PartyOrganizationStore(std::string resource_url,
                                               const std::string& conninfo)
    : resource_url_(std::move(resource_url)), conn_(conninfo) {}

std::string PartyOrganizationStore::create(bss_sid::Organization v) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto id = txn.exec("SELECT nextval('party.organization_id_seq')::text AS id")
                        .one_row()["id"]
                        .as<std::string>();
    v.id = id;
    v.href = resource_url_ + "/" + id;
    try {
        const auto& ed = v.existsDuring;
        const auto& parent = v.organizationParentRelationship;
        const auto porg = parent ? parent->organization : std::nullopt;
        txn.exec(
            "INSERT INTO party.organization (id, href, is_head_office, is_legal_entity, name, "
            "name_type, organization_type, trading_name, exists_during_start, "
            "exists_during_end, status, parent_relationship_type, parent_organization_id, "
            "parent_organization_name, parent_organization_href) VALUES ($1,$2,$3,$4,$5,$6,"
            "$7,$8,$9::timestamptz,$10::timestamptz,$11,$12,$13,$14,$15)",
            pqxx::params{id,
                         v.href,
                         v.isHeadOffice,
                         v.isLegalEntity,
                         v.name,
                         v.nameType,
                         v.organizationType,
                         v.tradingName,
                         ed ? ts_in(ed->startDateTime, "existsDuring.startDateTime") : std::nullopt,
                         ed ? ts_in(ed->endDateTime, "existsDuring.endDateTime") : std::nullopt,
                         v.status,
                         parent ? parent->relationshipType : std::nullopt,
                         porg ? porg->id : std::nullopt,
                         porg ? porg->name : std::nullopt,
                         porg ? porg->href : std::nullopt});
        insert_shared_children(txn,
                               kOrg,
                               id,
                               v.contactMedium,
                               v.creditRating,
                               v.externalReference,
                               v.partyCharacteristic,
                               v.relatedParty,
                               v.taxExemptionCertificate);
        for (std::size_t i = 0; i < v.organizationIdentification.size(); ++i) {
            const auto& x = v.organizationIdentification[i];
            txn.exec("INSERT INTO party.organization_identification (id, organization_id, ordinal, "
                     "identification_type, identification_id, issuing_authority, issuing_date, "
                     "attachment, valid_for_start, valid_for_end) VALUES ($1,$2,$3,$4,$5,$6,"
                     "$7::timestamptz,$8::jsonb,$9::timestamptz,$10::timestamptz)",
                     pqxx::params{child_id(kOrg, id, "id", i),
                                  id,
                                  static_cast<int>(i),
                                  x.identificationType,
                                  x.identificationId,
                                  x.issuingAuthority,
                                  ts_in(x.issuingDate, "issuingDate"),
                                  j_opt(x.attachment),
                                  vf_s(x.validFor),
                                  vf_e(x.validFor)});
        }
        for (std::size_t i = 0; i < v.otherName.size(); ++i) {
            const auto& x = v.otherName[i];
            txn.exec("INSERT INTO party.other_name_organization (id, organization_id, ordinal, "
                     "name, name_type, trading_name, valid_for_start, valid_for_end) VALUES ($1,$2,"
                     "$3,$4,$5,$6,$7::timestamptz,$8::timestamptz)",
                     pqxx::params{child_id(kOrg, id, "on", i),
                                  id,
                                  static_cast<int>(i),
                                  x.name,
                                  x.nameType,
                                  x.tradingName,
                                  vf_s(x.validFor),
                                  vf_e(x.validFor)});
        }
        for (std::size_t i = 0; i < v.organizationChildRelationship.size(); ++i) {
            const auto& x = v.organizationChildRelationship[i];
            const auto& c = x.organization;
            txn.exec("INSERT INTO party.organization_child_relationship (id, organization_id, "
                     "ordinal, relationship_type, child_organization_id, child_organization_name, "
                     "child_organization_href) VALUES ($1,$2,$3,$4,$5,$6,$7)",
                     pqxx::params{child_id(kOrg, id, "ch", i),
                                  id,
                                  static_cast<int>(i),
                                  x.relationshipType,
                                  c ? c->id : std::nullopt,
                                  c ? c->name : std::nullopt,
                                  c ? c->href : std::nullopt});
        }
    } catch (const pqxx::sql_error& e) {
        rethrow_as_request_error(e);
    }
    txn.commit();
    return id;
}

std::optional<bss_sid::Organization> PartyOrganizationStore::get(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    auto all = load_organizations(txn, id);
    if (all.empty()) {
        return std::nullopt;
    }
    return std::move(all.front());
}

std::vector<bss_sid::Organization> PartyOrganizationStore::list() {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    return load_organizations(txn, std::nullopt);
}

// =================================================================================================

AccountStore::AccountStore(std::string resource_url, const std::string& conninfo)
    : resource_url_(std::move(resource_url)), conn_(conninfo) {}

std::string AccountStore::create(Account account) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto id = txn.exec("SELECT nextval('subscriber_mgmt.account_id_seq')::text AS id")
                        .one_row()["id"]
                        .as<std::string>();
    try {
        txn.exec("INSERT INTO subscriber_mgmt.account (id, account_kind, parent_account_id, "
                 "organization_id, billing_mode, cost_center, contract_sla_id, provisioning_mode) "
                 "VALUES ($1,$2,$3,$4,$5,$6,$7,$8)",
                 pqxx::params{id,
                              account.accountKind,
                              account.parentAccountId,
                              account.organizationId,
                              account.billingMode,
                              account.costCenter,
                              account.contractSlaId,
                              account.provisioningMode});
    } catch (const pqxx::sql_error& e) {
        rethrow_as_request_error(e);
    }
    txn.commit();
    return id;
}

std::optional<Account> AccountStore::get(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT * FROM subscriber_mgmt.account WHERE id = $1", pqxx::params{id});
    if (result.empty()) {
        return std::nullopt;
    }
    return account_out(result.front());
}

std::vector<Account> AccountStore::list() {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    std::vector<Account> out;
    for (auto r : txn.exec("SELECT * FROM subscriber_mgmt.account ORDER BY id")) {
        out.push_back(account_out(r));
    }
    return out;
}

// =================================================================================================

SubscriberStore::SubscriberStore(std::string resource_url, const std::string& conninfo)
    : resource_url_(std::move(resource_url)), conn_(conninfo) {}

std::string SubscriberStore::create(Subscriber v) {
    if (!v.accountId.has_value() || v.accountId->empty()) {
        throw InvalidRequest("accountId is required (subscriber_mgmt.subscriber.account_id)");
    }
    if (!v.chargingMode.has_value()) {
        throw InvalidRequest("chargingMode is required (PREPAID | POSTPAID)");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto id = txn.exec("SELECT nextval('subscriber_mgmt.subscriber_id_seq')::text AS id")
                        .one_row()["id"]
                        .as<std::string>();
    try {
        // status 'active' on API creation, as the per-service store did (the schema default
        // 'pendingActive' belongs to the provisioning workflow's own lifecycle).
        txn.exec("INSERT INTO subscriber_mgmt.subscriber (id, account_id, individual_id, "
                 "charging_mode, bill_cycle_day, status, service_preferences) VALUES ($1,$2,$3,$4,"
                 "$5,'active',$6::jsonb)",
                 pqxx::params{id,
                              v.accountId,
                              v.individualId,
                              v.chargingMode,
                              v.billCycleDay,
                              v.servicePreferences.dump()});
        txn.exec("INSERT INTO subscriber_mgmt.resource (id, subscriber_id, resource_type, "
                 "resource_value) VALUES ($1,$2,'SUPI',$3)",
                 pqxx::params{id + ":supi", id, v.supi});
        if (v.msisdn.has_value()) {
            txn.exec("INSERT INTO subscriber_mgmt.resource (id, subscriber_id, resource_type, "
                     "resource_value) VALUES ($1,$2,'MSISDN',$3)",
                     pqxx::params{id + ":msisdn", id, *v.msisdn});
        }
    } catch (const pqxx::sql_error& e) {
        rethrow_as_request_error(e);
    }
    txn.commit();
    return id;
}

std::optional<Subscriber> SubscriberStore::get(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    auto all = load_subscribers(txn, "WHERE s.id = $1", pqxx::params{id});
    if (all.empty()) {
        return std::nullopt;
    }
    return std::move(all.front());
}

std::optional<Subscriber> SubscriberStore::get_by_supi(const std::string& supi) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    // idx_resource_lookup (resource_type, resource_value) serves this.
    auto all = load_subscribers(txn,
                                "WHERE s.id = (SELECT subscriber_id FROM subscriber_mgmt.resource "
                                "WHERE resource_type = 'SUPI' AND resource_value = $1 AND status = "
                                "'active')",
                                pqxx::params{supi});
    if (all.empty()) {
        return std::nullopt;
    }
    return std::move(all.front());
}

std::vector<Subscriber> SubscriberStore::list() {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    return load_subscribers(txn, "ORDER BY s.id", pqxx::params{});
}

bool SubscriberStore::record_lifecycle_transition(const std::string& subscriber_id,
                                                  const std::string& to_status,
                                                  const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        pqxx::work txn(conn_);
        // Read the current status inside the transaction (and lock the row): taking it from the
        // caller would let a concurrent transition record a wrong from_status.
        const auto rows = txn.exec("SELECT status FROM subscriber_mgmt.subscriber WHERE id = $1 "
                                   "FOR UPDATE",
                                   pqxx::params{subscriber_id});
        if (rows.empty()) {
            return false;
        }
        const auto from_status = rows[0][0].as<std::string>("");
        txn.exec("UPDATE subscriber_mgmt.subscriber SET status = $1, updated_at = now() WHERE id = "
                 "$2",
                 pqxx::params{to_status, subscriber_id});
        txn.exec("INSERT INTO subscriber_mgmt.subscriber_lifecycle_event (subscriber_id, "
                 "from_status, to_status, reason) VALUES ($1,$2,$3,$4)",
                 pqxx::params{subscriber_id, from_status, to_status, reason});
        txn.commit();
        return true;
    } catch (const std::exception& e) {
        spdlog::error("subscriber-management: lifecycle transition for {} failed: {}",
                      subscriber_id,
                      e.what());
        return false;
    }
}

// =================================================================================================

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
