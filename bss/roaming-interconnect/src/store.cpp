#include "store.hpp"

#include "sbi_core/datetime.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <map>

namespace roaming_interconnect {

RoamingCdrFile make_tap3_roaming_cdr_file(std::optional<std::string> agreementId,
                                          const tap3_core::DataInterchange& data) {
    RoamingCdrFile file;
    file.agreementId = std::move(agreementId);
    file.format = "TAP3";
    const auto bytes = tap3_core::encode_data_interchange(data);
    file.rawPayload.resize(bytes.size());
    std::transform(bytes.begin(), bytes.end(), file.rawPayload.begin(), [](std::uint8_t b) {
        return static_cast<std::byte>(b);
    });
    return file;
}

std::optional<tap3_core::DataInterchange> decode_tap3_roaming_cdr_file(const RoamingCdrFile& file) {
    if (file.format != "TAP3") {
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes(file.rawPayload.size());
    std::transform(file.rawPayload.begin(), file.rawPayload.end(), bytes.begin(), [](std::byte b) {
        return static_cast<std::uint8_t>(b);
    });
    return tap3_core::decode_data_interchange(bytes);
}

namespace {

// ADR-0387: an interconnect agreement is a TMF651 Agreement (normalized in
// subscriber_mgmt.agreement*
// -- the domain's one TMF651 home) plus its roaming specifics (roaming.interconnect_agreement:
// partner PLMN, rate terms), sharing ONE server-assigned id from subscriber_mgmt.agreement_id_seq.
// TAP3 files live in roaming.roaming_cdr_file. Date-times return as UTC RFC 3339 with milliseconds.
// Every statement is schema-qualified.

using nlohmann::json;

constexpr const char* kActor = "bss/roaming-interconnect";

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

template <typename Row> std::optional<std::string> s(const Row& row, const char* col) {
    return row[col].template as<std::optional<std::string>>();
}

template <typename Row>
std::optional<bss_sid::TimePeriod> period_out(const Row& row, const char* a, const char* b) {
    auto x = s(row, a);
    auto y = s(row, b);
    if (!x && !y) {
        return std::nullopt;
    }
    return bss_sid::TimePeriod{std::move(x), std::move(y)};
}

void write_audit(pqxx::work& txn,
                 const std::string& entity_type,
                 const std::string& entity_id,
                 const std::string& action,
                 const std::optional<std::string>& after) {
    const auto id = txn.exec("SELECT nextval('roaming.audit_record_id_seq')::text AS id")
                        .one_row()["id"]
                        .as<std::string>();
    txn.exec("INSERT INTO roaming.audit_record (id, entity_type, entity_id, action, actor, "
             "after_snapshot) VALUES ($1,$2,$3,$4,$5,$6::jsonb)",
             pqxx::params{id, entity_type, entity_id, action, kActor, after});
}

std::vector<InterconnectAgreement> load_agreements(pqxx::work& txn,
                                                   const std::optional<std::string>& only) {
    std::vector<InterconnectAgreement> out;
    std::map<std::string, std::size_t> idx;
    for (auto r :
         txn.exec("SELECT i.id, a.href, i.partner_plmn, i.terms::text AS terms, a.agreement_type, "
                  "a.description, a.document_number, " +
                      ts("a.initial_date") +
                      " AS initial_date, a.name, a.statement_of_intent, a.status, a.version, " +
                      ts("a.agreement_period_start") + " AS ap_start, " +
                      ts("a.agreement_period_end") + " AS ap_end, " + ts("a.completion_date") +
                      " AS cd_start, " + ts("a.completion_date_end") +
                      " AS cd_end, a.agreement_specification_id, a.agreement_specification_href, "
                      "a.agreement_specification_name, a.agreement_specification_description FROM "
                      "roaming.interconnect_agreement i JOIN subscriber_mgmt.agreement a ON a.id = "
                      "i.agreement_ref WHERE ($1::text IS NULL OR i.id = $1) ORDER BY i.id",
                  pqxx::params{only})) {
        InterconnectAgreement v;
        v.id = s(r, "id");
        v.href = s(r, "href");
        v.partnerOperatorPlmnId = s(r, "partner_plmn");
        v.rateTerms = json::parse(r["terms"].as<std::string>());
        auto& a = v.agreement;
        a.id = v.id;
        a.href = v.href;
        a.agreementType = s(r, "agreement_type");
        a.description = s(r, "description");
        a.documentNumber = r["document_number"].as<std::optional<int>>();
        a.initialDate = s(r, "initial_date");
        a.name = s(r, "name");
        a.statementOfIntent = s(r, "statement_of_intent");
        a.status = s(r, "status");
        a.version = s(r, "version");
        a.agreementPeriod = period_out(r, "ap_start", "ap_end");
        a.completionDate = period_out(r, "cd_start", "cd_end");
        auto sid = s(r, "agreement_specification_id");
        auto shref = s(r, "agreement_specification_href");
        auto sname = s(r, "agreement_specification_name");
        auto sdesc = s(r, "agreement_specification_description");
        if (sid || shref || sname || sdesc) {
            a.agreementSpecification = bss_sid::AgreementSpecificationRef{sid, shref, sdesc, sname};
        }
        idx[*v.id] = out.size();
        out.push_back(std::move(v));
    }
    if (out.empty()) {
        return out;
    }
    const auto at = [&](const std::string& id) -> bss_sid::Agreement* {
        auto it = idx.find(id);
        return it == idx.end() ? nullptr : &out[it->second].agreement;
    };
    const std::string only_sql = " WHERE ($1::text IS NULL OR agreement_id = $1) AND agreement_id "
                                 "IN (SELECT agreement_ref FROM roaming.interconnect_agreement)";
    for (auto r : txn.exec("SELECT agreement_id, " + ts("auth_date") +
                               " AS auth_date, signature_representation, state FROM "
                               "subscriber_mgmt.agreement_authorization" +
                               only_sql + " ORDER BY agreement_id, ordinal, id",
                           pqxx::params{only})) {
        if (auto* a = at(r["agreement_id"].as<std::string>())) {
            a->agreementAuthorization.push_back(
                {s(r, "auth_date"), s(r, "signature_representation"), s(r, "state")});
        }
    }
    for (auto r : txn.exec("SELECT agreement_id, ref_id, href, name FROM "
                           "subscriber_mgmt.agreement_associated" +
                               only_sql + " ORDER BY agreement_id, ordinal",
                           pqxx::params{only})) {
        if (auto* a = at(r["agreement_id"].as<std::string>())) {
            a->associatedAgreement.push_back(
                {r["ref_id"].as<std::string>(), s(r, "href"), s(r, "name")});
        }
    }
    for (auto r : txn.exec("SELECT agreement_id, name, value_type, value::text AS value FROM "
                           "subscriber_mgmt.agreement_characteristic" +
                               only_sql + " ORDER BY agreement_id, ordinal, id",
                           pqxx::params{only})) {
        if (auto* a = at(r["agreement_id"].as<std::string>())) {
            a->characteristic.push_back({r["name"].as<std::string>(),
                                         s(r, "value_type"),
                                         json::parse(s(r, "value").value_or("null"))});
        }
    }
    for (auto r : txn.exec("SELECT agreement_id, party_id, href, name, role FROM "
                           "subscriber_mgmt.agreement_engaged_party" +
                               only_sql + " ORDER BY agreement_id, ordinal, id",
                           pqxx::params{only})) {
        if (auto* a = at(r["agreement_id"].as<std::string>())) {
            a->engagedParty.push_back(
                {r["party_id"].as<std::string>(), s(r, "href"), s(r, "name"), s(r, "role")});
        }
    }
    // Agreement items and their three lists.
    std::map<std::string, std::pair<std::string, std::size_t>> item_at; // item id -> (agreement, i)
    for (auto r : txn.exec("SELECT id, agreement_id FROM subscriber_mgmt.agreement_item" +
                               only_sql + " ORDER BY agreement_id, ordinal",
                           pqxx::params{only})) {
        if (auto* a = at(r["agreement_id"].as<std::string>())) {
            item_at[r["id"].as<std::string>()] = {r["agreement_id"].as<std::string>(),
                                                  a->agreementItem.size()};
            a->agreementItem.emplace_back();
        }
    }
    const auto item = [&](const std::string& item_id) -> bss_sid::AgreementItem* {
        auto it = item_at.find(item_id);
        if (it == item_at.end()) {
            return nullptr;
        }
        auto* a = at(it->second.first);
        return a == nullptr ? nullptr : &a->agreementItem[it->second.second];
    };
    const std::string item_sql =
        " WHERE item_id IN (SELECT id FROM subscriber_mgmt.agreement_item" + only_sql + ")";
    for (auto r : txn.exec("SELECT item_id, ref_id, href, name FROM "
                           "subscriber_mgmt.agreement_item_product" +
                               item_sql + " ORDER BY item_id, ordinal",
                           pqxx::params{only})) {
        if (auto* it = item(r["item_id"].as<std::string>())) {
            it->product.push_back({r["ref_id"].as<std::string>(), s(r, "href"), s(r, "name")});
        }
    }
    for (auto r : txn.exec("SELECT item_id, ref_id, href, name FROM "
                           "subscriber_mgmt.agreement_item_offering" +
                               item_sql + " ORDER BY item_id, ordinal",
                           pqxx::params{only})) {
        if (auto* it = item(r["item_id"].as<std::string>())) {
            it->productOffering.push_back(
                {r["ref_id"].as<std::string>(), s(r, "href"), s(r, "name")});
        }
    }
    for (auto r : txn.exec("SELECT item_id, term_id, description, " + ts("valid_for_start") +
                               " AS vf_start, " + ts("valid_for_end") +
                               " AS vf_end FROM subscriber_mgmt.agreement_term_or_condition" +
                               item_sql + " ORDER BY item_id, ordinal",
                           pqxx::params{only})) {
        if (auto* it = item(r["item_id"].as<std::string>())) {
            it->termOrCondition.push_back(
                {s(r, "term_id"), s(r, "description"), period_out(r, "vf_start", "vf_end")});
        }
    }
    return out;
}

} // namespace

InterconnectAgreementStore::InterconnectAgreementStore(std::string resource_url,
                                                       const std::string& conninfo)
    : resource_url_(std::move(resource_url)), conn_(conninfo) {}

std::string InterconnectAgreementStore::create(InterconnectAgreement agreement) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto id = txn.exec("SELECT nextval('subscriber_mgmt.agreement_id_seq')::text AS id")
                        .one_row()["id"]
                        .as<std::string>();
    agreement.id = id;
    agreement.href = resource_url_ + "/" + id;
    const auto& a = agreement.agreement;
    const auto& ap = a.agreementPeriod;
    const auto& cd = a.completionDate;
    const auto& spec = a.agreementSpecification;
    try {
        txn.exec("INSERT INTO subscriber_mgmt.agreement (id, href, agreement_type, description, "
                 "document_number, initial_date, name, statement_of_intent, status, version, "
                 "agreement_period_start, agreement_period_end, completion_date, "
                 "completion_date_end, agreement_specification_id, agreement_specification_href, "
                 "agreement_specification_name, agreement_specification_description) VALUES ($1,"
                 "$2,$3,$4,$5,$6::timestamptz,$7,$8,$9,$10,$11::timestamptz,$12::timestamptz,"
                 "$13::timestamptz,$14::timestamptz,$15,$16,$17,$18)",
                 pqxx::params{
                     id,
                     agreement.href,
                     a.agreementType,
                     a.description,
                     a.documentNumber,
                     ts_in(a.initialDate, "initialDate"),
                     a.name,
                     a.statementOfIntent,
                     a.status,
                     a.version,
                     ap ? ts_in(ap->startDateTime, "agreementPeriod.startDateTime") : std::nullopt,
                     ap ? ts_in(ap->endDateTime, "agreementPeriod.endDateTime") : std::nullopt,
                     cd ? ts_in(cd->startDateTime, "completionDate.startDateTime") : std::nullopt,
                     cd ? ts_in(cd->endDateTime, "completionDate.endDateTime") : std::nullopt,
                     spec ? spec->id : std::nullopt,
                     spec ? spec->href : std::nullopt,
                     spec ? spec->name : std::nullopt,
                     spec ? spec->description : std::nullopt});
        for (std::size_t i = 0; i < a.agreementAuthorization.size(); ++i) {
            const auto& x = a.agreementAuthorization[i];
            txn.exec("INSERT INTO subscriber_mgmt.agreement_authorization (agreement_id, "
                     "auth_date, signature_representation, state, ordinal) VALUES ($1,"
                     "$2::timestamptz,$3,$4,$5)",
                     pqxx::params{id,
                                  ts_in(x.date, "agreementAuthorization.date"),
                                  x.signatureRepresentation,
                                  x.state,
                                  static_cast<int>(i)});
        }
        for (std::size_t i = 0; i < a.associatedAgreement.size(); ++i) {
            const auto& x = a.associatedAgreement[i];
            txn.exec("INSERT INTO subscriber_mgmt.agreement_associated (agreement_id, ordinal, "
                     "ref_id, href, name) VALUES ($1,$2,$3,$4,$5)",
                     pqxx::params{id, static_cast<int>(i), x.id, x.href, x.name});
        }
        for (std::size_t i = 0; i < a.characteristic.size(); ++i) {
            const auto& x = a.characteristic[i];
            txn.exec("INSERT INTO subscriber_mgmt.agreement_characteristic (agreement_id, name, "
                     "value_type, value, ordinal) VALUES ($1,$2,$3,$4::jsonb,$5)",
                     pqxx::params{id, x.name, x.valueType, x.value.dump(), static_cast<int>(i)});
        }
        for (std::size_t i = 0; i < a.engagedParty.size(); ++i) {
            const auto& x = a.engagedParty[i];
            txn.exec("INSERT INTO subscriber_mgmt.agreement_engaged_party (agreement_id, party_id, "
                     "href, name, role, ordinal) VALUES ($1,$2,$3,$4,$5,$6)",
                     pqxx::params{id, x.id, x.href, x.name, x.role, static_cast<int>(i)});
        }
        for (std::size_t i = 0; i < a.agreementItem.size(); ++i) {
            const auto& it = a.agreementItem[i];
            const std::string item_id = "ag:" + id + ":i" + std::to_string(i);
            txn.exec("INSERT INTO subscriber_mgmt.agreement_item (id, agreement_id, ordinal) "
                     "VALUES ($1,$2,$3)",
                     pqxx::params{item_id, id, static_cast<int>(i)});
            for (std::size_t k = 0; k < it.product.size(); ++k) {
                const auto& p = it.product[k];
                txn.exec("INSERT INTO subscriber_mgmt.agreement_item_product (item_id, ordinal, "
                         "ref_id, href, name) VALUES ($1,$2,$3,$4,$5)",
                         pqxx::params{item_id, static_cast<int>(k), p.id, p.href, p.name});
            }
            for (std::size_t k = 0; k < it.productOffering.size(); ++k) {
                const auto& p = it.productOffering[k];
                txn.exec("INSERT INTO subscriber_mgmt.agreement_item_offering (item_id, ordinal, "
                         "ref_id, href, name) VALUES ($1,$2,$3,$4,$5)",
                         pqxx::params{item_id, static_cast<int>(k), p.id, p.href, p.name});
            }
            for (std::size_t k = 0; k < it.termOrCondition.size(); ++k) {
                const auto& t = it.termOrCondition[k];
                const auto& vf = t.validFor;
                txn.exec("INSERT INTO subscriber_mgmt.agreement_term_or_condition (id, "
                         "agreement_id, item_id, term_id, description, valid_for_start, "
                         "valid_for_end, ordinal) VALUES ($1,$2,$3,$4,$5,$6::timestamptz,"
                         "$7::timestamptz,$8)",
                         pqxx::params{
                             item_id + ":t" + std::to_string(k),
                             id,
                             item_id,
                             t.id,
                             t.description,
                             vf ? ts_in(vf->startDateTime, "validFor.startDateTime") : std::nullopt,
                             vf ? ts_in(vf->endDateTime, "validFor.endDateTime") : std::nullopt,
                             static_cast<int>(k)});
            }
        }
        // Roaming specifics; rateTerms stays opaque (its shape is partner-specific, not guessed).
        // A JSON null (not SQL NULL) keeps "not supplied" distinct from "{}".
        txn.exec("INSERT INTO roaming.interconnect_agreement (id, partner_plmn, agreement_ref, "
                 "status, terms) VALUES ($1,$2,$1,$3,$4::jsonb)",
                 pqxx::params{
                     id, agreement.partnerOperatorPlmnId, a.status, agreement.rateTerms.dump()});
    } catch (const pqxx::foreign_key_violation&) {
        throw InvalidRequest("a referenced entity does not exist");
    } catch (const pqxx::not_null_violation&) {
        throw InvalidRequest("a mandatory field is missing (e.g. characteristic.name, "
                             "engagedParty.id)");
    }
    write_audit(txn, "INTERCONNECT_AGREEMENT", id, "interconnectAgreement.create", json(a).dump());
    txn.commit();
    return id;
}

std::optional<InterconnectAgreement> InterconnectAgreementStore::get(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    auto all = load_agreements(txn, id);
    if (all.empty()) {
        return std::nullopt;
    }
    return std::move(all.front());
}

std::vector<InterconnectAgreement> InterconnectAgreementStore::list() {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    return load_agreements(txn, std::nullopt);
}

RoamingCdrFileStore::RoamingCdrFileStore(const std::string& conninfo) : conn_(conninfo) {}

std::string RoamingCdrFileStore::create(RoamingCdrFile file) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto id = txn.exec("SELECT nextval('roaming.roaming_cdr_file_id_seq')::text AS id")
                        .one_row()["id"]
                        .as<std::string>();
    file.id = id;
    const pqxx::bytes_view payload_view(file.rawPayload);
    try {
        txn.exec("INSERT INTO roaming.roaming_cdr_file (id, agreement_id, format, raw_payload) "
                 "VALUES ($1,$2,$3,$4)",
                 pqxx::params{id, file.agreementId, file.format, payload_view});
    } catch (const pqxx::foreign_key_violation&) {
        throw InvalidRequest("agreementId does not name an interconnect agreement");
    }
    write_audit(txn, "ROAMING_CDR_FILE", id, "roamingCdrFile.create", std::nullopt);
    txn.commit();
    return id;
}

std::optional<RoamingCdrFile> RoamingCdrFileStore::get(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT id, agreement_id, format, raw_payload FROM "
                                 "roaming.roaming_cdr_file WHERE id = $1",
                                 pqxx::params{id});
    if (result.empty()) {
        return std::nullopt;
    }
    const auto& row = result.front();
    RoamingCdrFile v;
    v.id = row["id"].as<std::optional<std::string>>();
    v.agreementId = row["agreement_id"].as<std::optional<std::string>>();
    v.format = row["format"].as<std::string>();
    const auto payload = row["raw_payload"].as<std::optional<pqxx::bytes>>();
    if (payload.has_value()) {
        v.rawPayload = *payload;
    }
    return v;
}

void to_json(nlohmann::json& j, const InterconnectAgreement& v) {
    j = nlohmann::json(v.agreement);
    if (v.id.has_value()) {
        j["id"] = *v.id;
    }
    if (v.href.has_value()) {
        j["href"] = *v.href;
    }
    if (v.partnerOperatorPlmnId.has_value()) {
        j["partnerOperatorPlmnId"] = *v.partnerOperatorPlmnId;
    }
    if (!v.rateTerms.is_null()) {
        j["rateTerms"] = v.rateTerms;
    }
}

void from_json(const nlohmann::json& j, InterconnectAgreement& v) {
    v.agreement = j.template get<bss_sid::Agreement>();
    if (const auto it = j.find("id"); it != j.end() && !it->is_null()) {
        v.id = it->get<std::string>();
    }
    if (const auto it = j.find("href"); it != j.end() && !it->is_null()) {
        v.href = it->get<std::string>();
    }
    if (const auto it = j.find("partnerOperatorPlmnId"); it != j.end() && !it->is_null()) {
        v.partnerOperatorPlmnId = it->get<std::string>();
    }
    if (const auto it = j.find("rateTerms"); it != j.end()) {
        v.rateTerms = *it;
    }
}

} // namespace roaming_interconnect
