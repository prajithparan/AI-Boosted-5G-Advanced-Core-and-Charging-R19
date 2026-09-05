#include "stores.hpp"

namespace udr {

AmfContextStore::AmfContextStore(const std::string& conninfo) : conn_(conninfo) {}

bool AmfContextStore::put(const std::string& ue_id, nlohmann::json context) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    // xmax = 0 is a real Postgres idiom for "this row was inserted by this command, not updated"
    // -- lets one UPSERT statement report the same 201-vs-204 distinction the in-memory version's
    // separate find()-then-write did.
    const auto row = txn.exec("INSERT INTO udr_amf_context (ue_id, context) VALUES ($1, $2::jsonb) "
                              "ON CONFLICT (ue_id) DO UPDATE SET context = EXCLUDED.context "
                              "RETURNING (xmax = 0) AS inserted",
                              pqxx::params{ue_id, context.dump()})
                         .one_row();
    txn.commit();
    return row["inserted"].as<bool>();
}

std::optional<nlohmann::json> AmfContextStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT context FROM udr_amf_context WHERE ue_id = $1", pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["context"].as<std::string>()));
}

AmfNon3GppContextStore::AmfNon3GppContextStore(const std::string& conninfo) : conn_(conninfo) {}

bool AmfNon3GppContextStore::put(const std::string& ue_id, nlohmann::json context) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto row =
        txn.exec("INSERT INTO udr_amf_non3gpp_context (ue_id, context) VALUES ($1, $2::jsonb) "
                 "ON CONFLICT (ue_id) DO UPDATE SET context = EXCLUDED.context "
                 "RETURNING (xmax = 0) AS inserted",
                 pqxx::params{ue_id, context.dump()})
            .one_row();
    txn.commit();
    return row["inserted"].as<bool>();
}

std::optional<nlohmann::json> AmfNon3GppContextStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT context FROM udr_amf_non3gpp_context WHERE ue_id = $1",
                                 pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["context"].as<std::string>()));
}

std::optional<nlohmann::json> AmfContextStore::apply_patch(const std::string& ue_id,
                                                           const nlohmann::json& patch_ops) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT context FROM udr_amf_context WHERE ue_id = $1", pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    auto context = nlohmann::json::parse(result.front()["context"].as<std::string>());
    context = context.patch(patch_ops); // may throw nlohmann::json::exception -- caller catches
    txn.exec("UPDATE udr_amf_context SET context = $2::jsonb WHERE ue_id = $1",
             pqxx::params{ue_id, context.dump()});
    txn.commit();
    return std::make_optional(context);
}

SmfRegistrationStore::SmfRegistrationStore(const std::string& conninfo) : conn_(conninfo) {}

bool SmfRegistrationStore::put(const std::string& ue_id,
                               const std::string& pdu_session_id,
                               nlohmann::json registration) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto row =
        txn.exec("INSERT INTO udr_smf_registration (ue_id, pdu_session_id, registration) "
                 "VALUES ($1, $2, $3::jsonb) "
                 "ON CONFLICT (ue_id, pdu_session_id) DO UPDATE SET registration = "
                 "EXCLUDED.registration "
                 "RETURNING (xmax = 0) AS inserted",
                 pqxx::params{ue_id, pdu_session_id, registration.dump()})
            .one_row();
    txn.commit();
    return row["inserted"].as<bool>();
}

std::optional<nlohmann::json> SmfRegistrationStore::get(const std::string& ue_id,
                                                        const std::string& pdu_session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT registration FROM udr_smf_registration "
                                 "WHERE ue_id = $1 AND pdu_session_id = $2",
                                 pqxx::params{ue_id, pdu_session_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(
        nlohmann::json::parse(result.front()["registration"].as<std::string>()));
}

std::optional<nlohmann::json> SmfRegistrationStore::apply_patch(const std::string& ue_id,
                                                                const std::string& pdu_session_id,
                                                                const nlohmann::json& patch_ops) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT registration FROM udr_smf_registration "
                                 "WHERE ue_id = $1 AND pdu_session_id = $2",
                                 pqxx::params{ue_id, pdu_session_id});
    if (result.empty()) {
        return std::nullopt;
    }
    auto registration = nlohmann::json::parse(result.front()["registration"].as<std::string>());
    registration = registration.patch(patch_ops); // may throw nlohmann::json::exception
    txn.exec("UPDATE udr_smf_registration SET registration = $3::jsonb "
             "WHERE ue_id = $1 AND pdu_session_id = $2",
             pqxx::params{ue_id, pdu_session_id, registration.dump()});
    txn.commit();
    return std::make_optional(registration);
}

bool SmfRegistrationStore::remove(const std::string& ue_id, const std::string& pdu_session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("DELETE FROM udr_smf_registration "
                                 "WHERE ue_id = $1 AND pdu_session_id = $2",
                                 pqxx::params{ue_id, pdu_session_id});
    txn.commit();
    return result.affected_rows() > 0;
}

ProvisionedDataStore::ProvisionedDataStore(const std::string& conninfo) : conn_(conninfo) {}

void ProvisionedDataStore::seed(const std::string& ue_id,
                                const std::string& serving_plmn_id,
                                std::optional<nlohmann::json> am_data,
                                std::optional<nlohmann::json> smf_sel_data,
                                std::optional<nlohmann::json> sm_data,
                                std::optional<nlohmann::json> lcs_bca_data,
                                std::optional<nlohmann::json> sms_mng_data,
                                std::optional<nlohmann::json> sms_data,
                                std::optional<nlohmann::json> trace_data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_provisioned_data "
             "(ue_id, serving_plmn_id, am_data, smf_sel_data, sm_data, lcs_bca_data, "
             "sms_mng_data, sms_data, trace_data) "
             "VALUES ($1, $2, $3::jsonb, $4::jsonb, $5::jsonb, $6::jsonb, $7::jsonb, $8::jsonb, "
             "$9::jsonb) "
             "ON CONFLICT (ue_id, serving_plmn_id) DO UPDATE SET "
             "am_data = EXCLUDED.am_data, smf_sel_data = EXCLUDED.smf_sel_data, "
             "sm_data = EXCLUDED.sm_data, lcs_bca_data = EXCLUDED.lcs_bca_data, "
             "sms_mng_data = EXCLUDED.sms_mng_data, sms_data = EXCLUDED.sms_data, "
             "trace_data = EXCLUDED.trace_data",
             pqxx::params{
                 ue_id,
                 serving_plmn_id,
                 am_data.has_value() ? std::optional<std::string>(am_data->dump()) : std::nullopt,
                 smf_sel_data.has_value() ? std::optional<std::string>(smf_sel_data->dump())
                                          : std::nullopt,
                 sm_data.has_value() ? std::optional<std::string>(sm_data->dump()) : std::nullopt,
                 lcs_bca_data.has_value() ? std::optional<std::string>(lcs_bca_data->dump())
                                          : std::nullopt,
                 sms_mng_data.has_value() ? std::optional<std::string>(sms_mng_data->dump())
                                          : std::nullopt,
                 sms_data.has_value() ? std::optional<std::string>(sms_data->dump()) : std::nullopt,
                 trace_data.has_value() ? std::optional<std::string>(trace_data->dump())
                                        : std::nullopt});
    txn.commit();
}

namespace {

std::optional<nlohmann::json> get_provisioned_column(pqxx::connection& conn,
                                                     std::mutex& mutex,
                                                     const std::string& column,
                                                     const std::string& ue_id,
                                                     const std::string& serving_plmn_id) {
    std::lock_guard<std::mutex> lock(mutex);
    pqxx::work txn(conn);
    const auto result = txn.exec(
        "SELECT " + column + " FROM udr_provisioned_data WHERE ue_id = $1 AND serving_plmn_id = $2",
        pqxx::params{ue_id, serving_plmn_id});
    if (result.empty()) {
        return std::nullopt;
    }
    const auto value = result.front()[0].as<std::optional<std::string>>();
    if (!value.has_value()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(*value));
}

} // namespace

std::optional<nlohmann::json>
ProvisionedDataStore::get_am_data(const std::string& ue_id, const std::string& serving_plmn_id) {
    return get_provisioned_column(conn_, mutex_, "am_data", ue_id, serving_plmn_id);
}

std::optional<nlohmann::json>
ProvisionedDataStore::get_smf_sel_data(const std::string& ue_id,
                                       const std::string& serving_plmn_id) {
    return get_provisioned_column(conn_, mutex_, "smf_sel_data", ue_id, serving_plmn_id);
}

std::optional<nlohmann::json>
ProvisionedDataStore::get_sm_data(const std::string& ue_id, const std::string& serving_plmn_id) {
    return get_provisioned_column(conn_, mutex_, "sm_data", ue_id, serving_plmn_id);
}

std::optional<nlohmann::json>
ProvisionedDataStore::get_lcs_bca_data(const std::string& ue_id,
                                       const std::string& serving_plmn_id) {
    return get_provisioned_column(conn_, mutex_, "lcs_bca_data", ue_id, serving_plmn_id);
}

std::optional<nlohmann::json>
ProvisionedDataStore::get_sms_mng_data(const std::string& ue_id,
                                       const std::string& serving_plmn_id) {
    return get_provisioned_column(conn_, mutex_, "sms_mng_data", ue_id, serving_plmn_id);
}

std::optional<nlohmann::json>
ProvisionedDataStore::get_sms_data(const std::string& ue_id, const std::string& serving_plmn_id) {
    return get_provisioned_column(conn_, mutex_, "sms_data", ue_id, serving_plmn_id);
}

std::optional<nlohmann::json>
ProvisionedDataStore::get_trace_data(const std::string& ue_id, const std::string& serving_plmn_id) {
    return get_provisioned_column(conn_, mutex_, "trace_data", ue_id, serving_plmn_id);
}

std::vector<nlohmann::json> SmfRegistrationStore::list_for_ue(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT registration FROM udr_smf_registration WHERE ue_id = $1",
                                 pqxx::params{ue_id});
    std::vector<nlohmann::json> out;
    out.reserve(static_cast<std::size_t>(result.size()));
    for (const auto& row : result) {
        out.push_back(nlohmann::json::parse(row["registration"].as<std::string>()));
    }
    return out;
}

SmPolicyDataStore::SmPolicyDataStore(const std::string& conninfo) : conn_(conninfo) {}

std::optional<nlohmann::json> SmPolicyDataStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT policy_data FROM udr_sm_policy_data WHERE ue_id = $1",
                                 pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(
        nlohmann::json::parse(result.front()["policy_data"].as<std::string>()));
}

nlohmann::json SmPolicyDataStore::merge_patch(const std::string& ue_id,
                                              const nlohmann::json& patch) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT policy_data FROM udr_sm_policy_data WHERE ue_id = $1",
                                 pqxx::params{ue_id});
    // Real, deliberate upsert: an absent document starts from `{}` rather than being an error --
    // see this store's own header comment on why (no real POST exists for this resource, so PATCH
    // is this project's own chosen create path).
    auto doc = result.empty()
                   ? nlohmann::json::object()
                   : nlohmann::json::parse(result.front()["policy_data"].as<std::string>());
    doc.merge_patch(patch);
    txn.exec("INSERT INTO udr_sm_policy_data (ue_id, policy_data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ue_id) DO UPDATE SET policy_data = EXCLUDED.policy_data",
             pqxx::params{ue_id, doc.dump()});
    txn.commit();
    return doc;
}

AuthenticationSubscriptionDataStore::AuthenticationSubscriptionDataStore(
    const std::string& conninfo)
    : conn_(conninfo) {}

std::optional<nlohmann::json> AuthenticationSubscriptionDataStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec(
        "SELECT data FROM udr_authentication_subscription WHERE ue_id = $1", pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

nlohmann::json AuthenticationSubscriptionDataStore::apply_patch(const std::string& ue_id,
                                                                const nlohmann::json& patch_ops) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec(
        "SELECT data FROM udr_authentication_subscription WHERE ue_id = $1", pqxx::params{ue_id});
    auto doc = result.empty() ? nlohmann::json::object()
                              : nlohmann::json::parse(result.front()["data"].as<std::string>());
    doc = doc.patch(patch_ops); // may throw nlohmann::json::exception -- caller catches
    txn.exec("INSERT INTO udr_authentication_subscription (ue_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_id, doc.dump()});
    txn.commit();
    return doc;
}

AuthenticationStatusStore::AuthenticationStatusStore(const std::string& conninfo)
    : conn_(conninfo) {}

void AuthenticationStatusStore::put(const std::string& ue_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_authentication_status (ue_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> AuthenticationStatusStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_authentication_status WHERE ue_id = $1",
                                 pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

bool AuthenticationStatusStore::remove(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("DELETE FROM udr_authentication_status WHERE ue_id = $1", pqxx::params{ue_id});
    txn.commit();
    return result.affected_rows() > 0;
}

IndividualAuthenticationStatusStore::IndividualAuthenticationStatusStore(
    const std::string& conninfo)
    : conn_(conninfo) {}

void IndividualAuthenticationStatusStore::put(const std::string& ue_id,
                                              const std::string& serving_network_name,
                                              nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_individual_authentication_status "
             "(ue_id, serving_network_name, data) VALUES ($1, $2, $3::jsonb) "
             "ON CONFLICT (ue_id, serving_network_name) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_id, serving_network_name, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json>
IndividualAuthenticationStatusStore::get(const std::string& ue_id,
                                         const std::string& serving_network_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_individual_authentication_status "
                                 "WHERE ue_id = $1 AND serving_network_name = $2",
                                 pqxx::params{ue_id, serving_network_name});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

bool IndividualAuthenticationStatusStore::remove(const std::string& ue_id,
                                                 const std::string& serving_network_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("DELETE FROM udr_individual_authentication_status "
                                 "WHERE ue_id = $1 AND serving_network_name = $2",
                                 pqxx::params{ue_id, serving_network_name});
    txn.commit();
    return result.affected_rows() > 0;
}

AmPolicyDataStore::AmPolicyDataStore(const std::string& conninfo) : conn_(conninfo) {}

std::optional<nlohmann::json> AmPolicyDataStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT policy_data FROM udr_am_policy_data WHERE ue_id = $1",
                                 pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(
        nlohmann::json::parse(result.front()["policy_data"].as<std::string>()));
}

nlohmann::json AmPolicyDataStore::merge_patch(const std::string& ue_id,
                                              const nlohmann::json& patch) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT policy_data FROM udr_am_policy_data WHERE ue_id = $1",
                                 pqxx::params{ue_id});
    auto doc = result.empty()
                   ? nlohmann::json::object()
                   : nlohmann::json::parse(result.front()["policy_data"].as<std::string>());
    doc.merge_patch(patch);
    txn.exec("INSERT INTO udr_am_policy_data (ue_id, policy_data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ue_id) DO UPDATE SET policy_data = EXCLUDED.policy_data",
             pqxx::params{ue_id, doc.dump()});
    txn.commit();
    return doc;
}

SmsfContext3gppStore::SmsfContext3gppStore(const std::string& conninfo) : conn_(conninfo) {}

void SmsfContext3gppStore::put(const std::string& ue_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_smsf_3gpp_context (ue_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> SmsfContext3gppStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_smsf_3gpp_context WHERE ue_id = $1", pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

bool SmsfContext3gppStore::remove(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("DELETE FROM udr_smsf_3gpp_context WHERE ue_id = $1", pqxx::params{ue_id});
    txn.commit();
    return result.affected_rows() > 0;
}

SmsfNon3GppContextStore::SmsfNon3GppContextStore(const std::string& conninfo) : conn_(conninfo) {}

void SmsfNon3GppContextStore::put(const std::string& ue_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_smsf_non3gpp_context (ue_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> SmsfNon3GppContextStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_smsf_non3gpp_context WHERE ue_id = $1", pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

bool SmsfNon3GppContextStore::remove(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("DELETE FROM udr_smsf_non3gpp_context WHERE ue_id = $1", pqxx::params{ue_id});
    txn.commit();
    return result.affected_rows() > 0;
}

IpSmGwContextStore::IpSmGwContextStore(const std::string& conninfo) : conn_(conninfo) {}

void IpSmGwContextStore::put(const std::string& ue_id, nlohmann::json context) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_ip_sm_gw_context (ue_id, context) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ue_id) DO UPDATE SET context = EXCLUDED.context",
             pqxx::params{ue_id, context.dump()});
    txn.commit();
}

std::optional<nlohmann::json> IpSmGwContextStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT context FROM udr_ip_sm_gw_context WHERE ue_id = $1", pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["context"].as<std::string>()));
}

std::optional<nlohmann::json> IpSmGwContextStore::apply_patch(const std::string& ue_id,
                                                              const nlohmann::json& patch_ops) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT context FROM udr_ip_sm_gw_context WHERE ue_id = $1", pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    auto context = nlohmann::json::parse(result.front()["context"].as<std::string>());
    context = context.patch(patch_ops); // may throw nlohmann::json::exception -- caller catches
    txn.exec("UPDATE udr_ip_sm_gw_context SET context = $2::jsonb WHERE ue_id = $1",
             pqxx::params{ue_id, context.dump()});
    txn.commit();
    return std::make_optional(context);
}

bool IpSmGwContextStore::remove(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("DELETE FROM udr_ip_sm_gw_context WHERE ue_id = $1", pqxx::params{ue_id});
    txn.commit();
    return result.affected_rows() > 0;
}

MessageWaitingDataStore::MessageWaitingDataStore(const std::string& conninfo) : conn_(conninfo) {}

bool MessageWaitingDataStore::put(const std::string& ue_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto row = txn.exec("INSERT INTO udr_mwd (ue_id, data) VALUES ($1, $2::jsonb) "
                              "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data "
                              "RETURNING (xmax = 0) AS inserted",
                              pqxx::params{ue_id, data.dump()})
                         .one_row();
    txn.commit();
    return row["inserted"].as<bool>();
}

std::optional<nlohmann::json> MessageWaitingDataStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_mwd WHERE ue_id = $1", pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

std::optional<nlohmann::json>
MessageWaitingDataStore::apply_patch(const std::string& ue_id, const nlohmann::json& patch_ops) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_mwd WHERE ue_id = $1", pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    auto data = nlohmann::json::parse(result.front()["data"].as<std::string>());
    data = data.patch(patch_ops); // may throw nlohmann::json::exception -- caller catches
    txn.exec("UPDATE udr_mwd SET data = $2::jsonb WHERE ue_id = $1",
             pqxx::params{ue_id, data.dump()});
    txn.commit();
    return std::make_optional(data);
}

bool MessageWaitingDataStore::remove(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("DELETE FROM udr_mwd WHERE ue_id = $1", pqxx::params{ue_id});
    txn.commit();
    return result.affected_rows() > 0;
}

RoamingInformationStore::RoamingInformationStore(const std::string& conninfo) : conn_(conninfo) {}

bool RoamingInformationStore::put(const std::string& ue_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto row =
        txn.exec("INSERT INTO udr_roaming_information (ue_id, data) VALUES ($1, $2::jsonb) "
                 "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data "
                 "RETURNING (xmax = 0) AS inserted",
                 pqxx::params{ue_id, data.dump()})
            .one_row();
    txn.commit();
    return row["inserted"].as<bool>();
}

std::optional<nlohmann::json> RoamingInformationStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_roaming_information WHERE ue_id = $1", pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

PeiInfoStore::PeiInfoStore(const std::string& conninfo) : conn_(conninfo) {}

bool PeiInfoStore::put(const std::string& ue_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto row = txn.exec("INSERT INTO udr_pei_info (ue_id, data) VALUES ($1, $2::jsonb) "
                              "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data "
                              "RETURNING (xmax = 0) AS inserted",
                              pqxx::params{ue_id, data.dump()})
                         .one_row();
    txn.commit();
    return row["inserted"].as<bool>();
}

std::optional<nlohmann::json> PeiInfoStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_pei_info WHERE ue_id = $1", pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

CoverageRestrictionDataStore::CoverageRestrictionDataStore(const std::string& conninfo)
    : conn_(conninfo) {}

void CoverageRestrictionDataStore::seed(const std::string& ue_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_coverage_restriction_data (ue_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> CoverageRestrictionDataStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_coverage_restriction_data WHERE ue_id = $1",
                                 pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

LcsPrivacyDataStore::LcsPrivacyDataStore(const std::string& conninfo) : conn_(conninfo) {}

void LcsPrivacyDataStore::seed(const std::string& ue_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_lcs_privacy_data (ue_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> LcsPrivacyDataStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_lcs_privacy_data WHERE ue_id = $1", pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

LcsSubscriptionDataStore::LcsSubscriptionDataStore(const std::string& conninfo) : conn_(conninfo) {}

void LcsSubscriptionDataStore::seed(const std::string& ue_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_lcs_subscription_data (ue_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> LcsSubscriptionDataStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_lcs_subscription_data WHERE ue_id = $1",
                                 pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

LcsMoDataStore::LcsMoDataStore(const std::string& conninfo) : conn_(conninfo) {}

void LcsMoDataStore::seed(const std::string& ue_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_lcs_mo_data (ue_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> LcsMoDataStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_lcs_mo_data WHERE ue_id = $1", pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

PpDataStore::PpDataStore(const std::string& conninfo) : conn_(conninfo) {}

std::optional<nlohmann::json> PpDataStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_pp_data WHERE ue_id = $1", pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

nlohmann::json PpDataStore::apply_patch(const std::string& ue_id, const nlohmann::json& patch_ops) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_pp_data WHERE ue_id = $1", pqxx::params{ue_id});
    auto doc = result.empty() ? nlohmann::json::object()
                              : nlohmann::json::parse(result.front()["data"].as<std::string>());
    doc = doc.patch(patch_ops); // may throw nlohmann::json::exception -- caller catches
    txn.exec("INSERT INTO udr_pp_data (ue_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_id, doc.dump()});
    txn.commit();
    return doc;
}

PpProfileDataStore::PpProfileDataStore(const std::string& conninfo) : conn_(conninfo) {}

void PpProfileDataStore::seed(const std::string& ue_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_pp_profile_data (ue_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> PpProfileDataStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_pp_profile_data WHERE ue_id = $1", pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

PpDataEntryStore::PpDataEntryStore(const std::string& conninfo) : conn_(conninfo) {}

bool PpDataEntryStore::put(const std::string& ue_id,
                           const std::string& af_instance_id,
                           nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto row = txn.exec("INSERT INTO udr_pp_data_entry (ue_id, af_instance_id, data) "
                              "VALUES ($1, $2, $3::jsonb) "
                              "ON CONFLICT (ue_id, af_instance_id) DO UPDATE SET data = "
                              "EXCLUDED.data "
                              "RETURNING (xmax = 0) AS inserted",
                              pqxx::params{ue_id, af_instance_id, data.dump()})
                         .one_row();
    txn.commit();
    return row["inserted"].as<bool>();
}

std::optional<nlohmann::json> PpDataEntryStore::get(const std::string& ue_id,
                                                    const std::string& af_instance_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_pp_data_entry "
                                 "WHERE ue_id = $1 AND af_instance_id = $2",
                                 pqxx::params{ue_id, af_instance_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

bool PpDataEntryStore::remove(const std::string& ue_id, const std::string& af_instance_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("DELETE FROM udr_pp_data_entry "
                                 "WHERE ue_id = $1 AND af_instance_id = $2",
                                 pqxx::params{ue_id, af_instance_id});
    txn.commit();
    return result.affected_rows() > 0;
}

std::vector<nlohmann::json> PpDataEntryStore::list_for_ue(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_pp_data_entry WHERE ue_id = $1", pqxx::params{ue_id});
    std::vector<nlohmann::json> out;
    out.reserve(static_cast<std::size_t>(result.size()));
    for (const auto& row : result) {
        out.push_back(nlohmann::json::parse(row["data"].as<std::string>()));
    }
    return out;
}

SharedDataStore::SharedDataStore(const std::string& conninfo) : conn_(conninfo) {}

void SharedDataStore::seed(const std::string& shared_data_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_shared_data (shared_data_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (shared_data_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{shared_data_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> SharedDataStore::get(const std::string& shared_data_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_shared_data WHERE shared_data_id = $1",
                                 pqxx::params{shared_data_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

OperatorSpecificDataStore::OperatorSpecificDataStore(const std::string& conninfo)
    : conn_(conninfo) {}

std::optional<nlohmann::json> OperatorSpecificDataStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_operator_specific_data WHERE ue_id = $1",
                                 pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

nlohmann::json OperatorSpecificDataStore::apply_patch(const std::string& ue_id,
                                                      const nlohmann::json& patch_ops) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_operator_specific_data WHERE ue_id = $1",
                                 pqxx::params{ue_id});
    auto doc = result.empty() ? nlohmann::json::object()
                              : nlohmann::json::parse(result.front()["data"].as<std::string>());
    doc = doc.patch(patch_ops); // may throw nlohmann::json::exception -- caller catches
    txn.exec("INSERT INTO udr_operator_specific_data (ue_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_id, doc.dump()});
    txn.commit();
    return doc;
}

bool OperatorSpecificDataStore::put(const std::string& ue_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    // Real PostgreSQL idiom for detecting INSERT vs UPDATE in a single upsert: `xmax = 0` is true
    // only for a row this transaction just inserted (a real, updated row's xmax is set to the
    // updating transaction's id) -- not a fabricated technique, standard PostgreSQL behavior.
    const auto result =
        txn.exec("INSERT INTO udr_operator_specific_data (ue_id, data) VALUES ($1, $2::jsonb) "
                 "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data "
                 "RETURNING (xmax = 0) AS inserted",
                 pqxx::params{ue_id, data.dump()});
    txn.commit();
    return result.front()["inserted"].as<bool>();
}

bool OperatorSpecificDataStore::remove(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("DELETE FROM udr_operator_specific_data WHERE ue_id = $1", pqxx::params{ue_id});
    txn.commit();
    return result.affected_rows() > 0;
}

EeProfileDataStore::EeProfileDataStore(const std::string& conninfo) : conn_(conninfo) {}

void EeProfileDataStore::seed(const std::string& ue_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_ee_profile_data (ue_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> EeProfileDataStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_ee_profile_data WHERE ue_id = $1", pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

UePolicySetStore::UePolicySetStore(const std::string& conninfo) : conn_(conninfo) {}

bool UePolicySetStore::put(const std::string& ue_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto row = txn.exec("INSERT INTO udr_ue_policy_set (ue_id, data) VALUES ($1, $2::jsonb) "
                              "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data "
                              "RETURNING (xmax = 0) AS inserted",
                              pqxx::params{ue_id, data.dump()})
                         .one_row();
    txn.commit();
    return row["inserted"].as<bool>();
}

std::optional<nlohmann::json> UePolicySetStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_ue_policy_set WHERE ue_id = $1", pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

nlohmann::json UePolicySetStore::merge_patch(const std::string& ue_id,
                                             const nlohmann::json& patch) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_ue_policy_set WHERE ue_id = $1", pqxx::params{ue_id});
    auto doc = result.empty() ? nlohmann::json::object()
                              : nlohmann::json::parse(result.front()["data"].as<std::string>());
    doc.merge_patch(patch);
    txn.exec("INSERT INTO udr_ue_policy_set (ue_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_id, doc.dump()});
    txn.commit();
    return doc;
}

PolicyOperatorSpecificDataStore::PolicyOperatorSpecificDataStore(const std::string& conninfo)
    : conn_(conninfo) {}

std::optional<nlohmann::json> PolicyOperatorSpecificDataStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec(
        "SELECT data FROM udr_policy_operator_specific_data WHERE ue_id = $1", pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

nlohmann::json PolicyOperatorSpecificDataStore::apply_patch(const std::string& ue_id,
                                                            const nlohmann::json& patch_ops) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec(
        "SELECT data FROM udr_policy_operator_specific_data WHERE ue_id = $1", pqxx::params{ue_id});
    auto doc = result.empty() ? nlohmann::json::object()
                              : nlohmann::json::parse(result.front()["data"].as<std::string>());
    doc = doc.patch(patch_ops); // may throw nlohmann::json::exception -- caller catches
    txn.exec("INSERT INTO udr_policy_operator_specific_data (ue_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_id, doc.dump()});
    txn.commit();
    return doc;
}

bool PolicyOperatorSpecificDataStore::put(const std::string& ue_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec(
        "INSERT INTO udr_policy_operator_specific_data (ue_id, data) VALUES ($1, $2::jsonb) "
        "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data "
        "RETURNING (xmax = 0) AS inserted",
        pqxx::params{ue_id, data.dump()});
    txn.commit();
    return result.front()["inserted"].as<bool>();
}

bool PolicyOperatorSpecificDataStore::remove(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("DELETE FROM udr_policy_operator_specific_data WHERE ue_id = $1",
                                 pqxx::params{ue_id});
    txn.commit();
    return result.affected_rows() > 0;
}

SponsorConnectivityDataStore::SponsorConnectivityDataStore(const std::string& conninfo)
    : conn_(conninfo) {}

void SponsorConnectivityDataStore::seed(const std::string& sponsor_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_sponsor_connectivity_data (sponsor_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (sponsor_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{sponsor_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> SponsorConnectivityDataStore::get(const std::string& sponsor_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_sponsor_connectivity_data WHERE sponsor_id = $1",
                 pqxx::params{sponsor_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

BdtDataStore::BdtDataStore(const std::string& conninfo) : conn_(conninfo) {}

void BdtDataStore::put(const std::string& bdt_ref_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_bdt_data (bdt_ref_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (bdt_ref_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{bdt_ref_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> BdtDataStore::get(const std::string& bdt_ref_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_bdt_data WHERE bdt_ref_id = $1", pqxx::params{bdt_ref_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

std::optional<nlohmann::json> BdtDataStore::merge_patch(const std::string& bdt_ref_id,
                                                        const nlohmann::json& patch) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_bdt_data WHERE bdt_ref_id = $1", pqxx::params{bdt_ref_id});
    if (result.empty()) {
        // Real, disclosed: unlike AmPolicyDataStore/UePolicySetStore, this PATCH is NOT
        // upsert-capable -- the real spec documents a real 404 for UpdateIndividualBdtData when
        // the resource doesn't already exist (PUT is the real create path for this resource).
        return std::nullopt;
    }
    auto doc = nlohmann::json::parse(result.front()["data"].as<std::string>());
    doc.merge_patch(patch);
    txn.exec("UPDATE udr_bdt_data SET data = $2::jsonb WHERE bdt_ref_id = $1",
             pqxx::params{bdt_ref_id, doc.dump()});
    txn.commit();
    return std::make_optional(doc);
}

bool BdtDataStore::remove(const std::string& bdt_ref_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("DELETE FROM udr_bdt_data WHERE bdt_ref_id = $1", pqxx::params{bdt_ref_id});
    txn.commit();
    return result.affected_rows() > 0;
}

std::vector<nlohmann::json> BdtDataStore::list_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_bdt_data");
    std::vector<nlohmann::json> out;
    out.reserve(static_cast<std::size_t>(result.size()));
    for (const auto& row : result) {
        out.push_back(nlohmann::json::parse(row["data"].as<std::string>()));
    }
    return out;
}

PlmnUePolicySetStore::PlmnUePolicySetStore(const std::string& conninfo) : conn_(conninfo) {}

void PlmnUePolicySetStore::seed(const std::string& plmn_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_plmn_ue_policy_set (plmn_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (plmn_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{plmn_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> PlmnUePolicySetStore::get(const std::string& plmn_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_plmn_ue_policy_set WHERE plmn_id = $1",
                                 pqxx::params{plmn_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

SlicePolicyDataStore::SlicePolicyDataStore(const std::string& conninfo) : conn_(conninfo) {}

std::optional<nlohmann::json> SlicePolicyDataStore::get(const std::string& snssai) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_slice_control_data WHERE snssai = $1", pqxx::params{snssai});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

nlohmann::json SlicePolicyDataStore::merge_patch(const std::string& snssai,
                                                 const nlohmann::json& patch) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_slice_control_data WHERE snssai = $1", pqxx::params{snssai});
    auto doc = result.empty() ? nlohmann::json::object()
                              : nlohmann::json::parse(result.front()["data"].as<std::string>());
    doc.merge_patch(patch);
    txn.exec("INSERT INTO udr_slice_control_data (snssai, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (snssai) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{snssai, doc.dump()});
    txn.commit();
    return doc;
}

GroupPolicyDataStore::GroupPolicyDataStore(const std::string& conninfo) : conn_(conninfo) {}

std::optional<nlohmann::json> GroupPolicyDataStore::get(const std::string& int_group_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_group_control_data WHERE int_group_id = $1",
                                 pqxx::params{int_group_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

nlohmann::json GroupPolicyDataStore::merge_patch(const std::string& int_group_id,
                                                 const nlohmann::json& patch) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_group_control_data WHERE int_group_id = $1",
                                 pqxx::params{int_group_id});
    auto doc = result.empty() ? nlohmann::json::object()
                              : nlohmann::json::parse(result.front()["data"].as<std::string>());
    doc.merge_patch(patch);
    txn.exec("INSERT INTO udr_group_control_data (int_group_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (int_group_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{int_group_id, doc.dump()});
    txn.commit();
    return doc;
}

RoutingIdStore::RoutingIdStore(const std::string& conninfo) : conn_(conninfo) {}

void RoutingIdStore::seed(const std::string& nf_type,
                          const std::string& nf_group_id,
                          nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_routing_ids (nf_type, nf_group_id, data) "
             "VALUES ($1, $2, $3::jsonb) "
             "ON CONFLICT (nf_type, nf_group_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{nf_type, nf_group_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> RoutingIdStore::get(const std::string& nf_type,
                                                  const std::string& nf_group_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_routing_ids WHERE nf_type = $1 AND nf_group_id = $2",
                 pqxx::params{nf_type, nf_group_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

NiddAuthorizationInfoStore::NiddAuthorizationInfoStore(const std::string& conninfo)
    : conn_(conninfo) {}

bool NiddAuthorizationInfoStore::put(const std::string& ue_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto row =
        txn.exec("INSERT INTO udr_nidd_authorization_info (ue_id, data) VALUES ($1, $2::jsonb) "
                 "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data "
                 "RETURNING (xmax = 0) AS inserted",
                 pqxx::params{ue_id, data.dump()})
            .one_row();
    txn.commit();
    return row["inserted"].as<bool>();
}

std::optional<nlohmann::json> NiddAuthorizationInfoStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_nidd_authorization_info WHERE ue_id = $1",
                                 pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

std::optional<nlohmann::json>
NiddAuthorizationInfoStore::apply_patch(const std::string& ue_id, const nlohmann::json& patch_ops) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_nidd_authorization_info WHERE ue_id = $1",
                                 pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    auto data = nlohmann::json::parse(result.front()["data"].as<std::string>());
    data = data.patch(patch_ops); // may throw nlohmann::json::exception -- caller catches
    txn.exec("UPDATE udr_nidd_authorization_info SET data = $2::jsonb WHERE ue_id = $1",
             pqxx::params{ue_id, data.dump()});
    txn.commit();
    return std::make_optional(data);
}

bool NiddAuthorizationInfoStore::remove(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("DELETE FROM udr_nidd_authorization_info WHERE ue_id = $1", pqxx::params{ue_id});
    txn.commit();
    return result.affected_rows() > 0;
}

IdentityDataStore::IdentityDataStore(const std::string& conninfo) : conn_(conninfo) {}

std::optional<nlohmann::json> IdentityDataStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_identity_data WHERE ue_id = $1", pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

nlohmann::json IdentityDataStore::apply_patch(const std::string& ue_id,
                                              const nlohmann::json& patch_ops) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_identity_data WHERE ue_id = $1", pqxx::params{ue_id});
    auto doc = result.empty() ? nlohmann::json::object()
                              : nlohmann::json::parse(result.front()["data"].as<std::string>());
    doc = doc.patch(patch_ops); // may throw nlohmann::json::exception -- caller catches
    txn.exec("INSERT INTO udr_identity_data (ue_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_id, doc.dump()});
    txn.commit();
    return doc;
}

OdbDataStore::OdbDataStore(const std::string& conninfo) : conn_(conninfo) {}

void OdbDataStore::seed(const std::string& ue_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_odb_data (ue_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> OdbDataStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_odb_data WHERE ue_id = $1", pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

V2xDataStore::V2xDataStore(const std::string& conninfo) : conn_(conninfo) {}

void V2xDataStore::seed(const std::string& ue_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_v2x_data (ue_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> V2xDataStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_v2x_data WHERE ue_id = $1", pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

ProseDataStore::ProseDataStore(const std::string& conninfo) : conn_(conninfo) {}

void ProseDataStore::seed(const std::string& ue_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_prose_data (ue_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> ProseDataStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_prose_data WHERE ue_id = $1", pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

UcDataStore::UcDataStore(const std::string& conninfo) : conn_(conninfo) {}

void UcDataStore::seed(const std::string& ue_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_uc_data (ue_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> UcDataStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_uc_data WHERE ue_id = $1", pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

TimeSyncDataStore::TimeSyncDataStore(const std::string& conninfo) : conn_(conninfo) {}

void TimeSyncDataStore::seed(const std::string& ue_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_time_sync_data (ue_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> TimeSyncDataStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_time_sync_data WHERE ue_id = $1", pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

LocationDataStore::LocationDataStore(const std::string& conninfo) : conn_(conninfo) {}

void LocationDataStore::seed(const std::string& ue_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_location_data (ue_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> LocationDataStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_location_data WHERE ue_id = $1", pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

A2xDataStore::A2xDataStore(const std::string& conninfo) : conn_(conninfo) {}

void A2xDataStore::seed(const std::string& ue_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_a2x_data (ue_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> A2xDataStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_a2x_data WHERE ue_id = $1", pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

RangingSlPrivacyDataStore::RangingSlPrivacyDataStore(const std::string& conninfo)
    : conn_(conninfo) {}

void RangingSlPrivacyDataStore::seed(const std::string& ue_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_rangingsl_privacy_data (ue_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> RangingSlPrivacyDataStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_rangingsl_privacy_data WHERE ue_id = $1",
                                 pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

RangingSlPosDataStore::RangingSlPosDataStore(const std::string& conninfo) : conn_(conninfo) {}

void RangingSlPosDataStore::seed(const std::string& ue_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_ranging_slpos_data (ue_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> RangingSlPosDataStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_ranging_slpos_data WHERE ue_id = $1", pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

MbsDataStore::MbsDataStore(const std::string& conninfo) : conn_(conninfo) {}

void MbsDataStore::seed(const std::string& ue_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_5mbs_data (ue_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> MbsDataStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_5mbs_data WHERE ue_id = $1", pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

ServiceSpecificAuthorizationInfoStore::ServiceSpecificAuthorizationInfoStore(
    const std::string& conninfo)
    : conn_(conninfo) {}

bool ServiceSpecificAuthorizationInfoStore::put(const std::string& ue_id,
                                                const std::string& service_type,
                                                nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto row =
        txn.exec("INSERT INTO udr_service_specific_auth_info (ue_id, service_type, data) "
                 "VALUES ($1, $2, $3::jsonb) "
                 "ON CONFLICT (ue_id, service_type) DO UPDATE SET data = EXCLUDED.data "
                 "RETURNING (xmax = 0) AS inserted",
                 pqxx::params{ue_id, service_type, data.dump()})
            .one_row();
    txn.commit();
    return row["inserted"].as<bool>();
}

std::optional<nlohmann::json>
ServiceSpecificAuthorizationInfoStore::get(const std::string& ue_id,
                                           const std::string& service_type) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_service_specific_auth_info "
                                 "WHERE ue_id = $1 AND service_type = $2",
                                 pqxx::params{ue_id, service_type});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

std::optional<nlohmann::json> ServiceSpecificAuthorizationInfoStore::apply_patch(
    const std::string& ue_id, const std::string& service_type, const nlohmann::json& patch_ops) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_service_specific_auth_info "
                                 "WHERE ue_id = $1 AND service_type = $2",
                                 pqxx::params{ue_id, service_type});
    if (result.empty()) {
        return std::nullopt;
    }
    auto data = nlohmann::json::parse(result.front()["data"].as<std::string>());
    data = data.patch(patch_ops); // may throw nlohmann::json::exception -- caller catches
    txn.exec("UPDATE udr_service_specific_auth_info SET data = $3::jsonb "
             "WHERE ue_id = $1 AND service_type = $2",
             pqxx::params{ue_id, service_type, data.dump()});
    txn.commit();
    return std::make_optional(data);
}

bool ServiceSpecificAuthorizationInfoStore::remove(const std::string& ue_id,
                                                   const std::string& service_type) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("DELETE FROM udr_service_specific_auth_info "
                                 "WHERE ue_id = $1 AND service_type = $2",
                                 pqxx::params{ue_id, service_type});
    txn.commit();
    return result.affected_rows() > 0;
}

GroupIdentifiersStore::GroupIdentifiersStore(const std::string& conninfo) : conn_(conninfo) {}

void GroupIdentifiersStore::seed(const std::string& ext_group_id,
                                 const std::string& int_group_id,
                                 nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_group_identifiers (ext_group_id, int_group_id, data) "
             "VALUES ($1, $2, $3::jsonb) "
             "ON CONFLICT (ext_group_id) DO UPDATE SET int_group_id = EXCLUDED.int_group_id, "
             "data = EXCLUDED.data",
             pqxx::params{ext_group_id, int_group_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json>
GroupIdentifiersStore::get_by_ext_group_id(const std::string& ext_group_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_group_identifiers WHERE ext_group_id = $1",
                                 pqxx::params{ext_group_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

std::optional<nlohmann::json>
GroupIdentifiersStore::get_by_int_group_id(const std::string& int_group_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_group_identifiers WHERE int_group_id = $1",
                                 pqxx::params{int_group_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

NssaiAckDataStore::NssaiAckDataStore(const std::string& conninfo) : conn_(conninfo) {}

void NssaiAckDataStore::put(const std::string& ue_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_nssai_ack_data (ue_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> NssaiAckDataStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_nssai_ack_data WHERE ue_id = $1", pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

CagAckDataStore::CagAckDataStore(const std::string& conninfo) : conn_(conninfo) {}

void CagAckDataStore::put(const std::string& ue_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_cag_ack_data (ue_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> CagAckDataStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_cag_ack_data WHERE ue_id = $1", pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

SorDataStore::SorDataStore(const std::string& conninfo) : conn_(conninfo) {}

void SorDataStore::put(const std::string& ue_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_sor_data (ue_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> SorDataStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_sor_data WHERE ue_id = $1", pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

std::optional<nlohmann::json> SorDataStore::apply_patch(const std::string& ue_id,
                                                        const nlohmann::json& patch_ops) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_sor_data WHERE ue_id = $1", pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    auto data = nlohmann::json::parse(result.front()["data"].as<std::string>());
    data = data.patch(patch_ops); // may throw nlohmann::json::exception -- caller catches
    txn.exec("UPDATE udr_sor_data SET data = $2::jsonb WHERE ue_id = $1",
             pqxx::params{ue_id, data.dump()});
    txn.commit();
    return std::make_optional(data);
}

UpuDataStore::UpuDataStore(const std::string& conninfo) : conn_(conninfo) {}

void UpuDataStore::put(const std::string& ue_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_upu_data (ue_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ue_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> UpuDataStore::get(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_upu_data WHERE ue_id = $1", pqxx::params{ue_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

FiveGVnGroupStore::FiveGVnGroupStore(const std::string& conninfo) : conn_(conninfo) {}

void FiveGVnGroupStore::put(const std::string& ext_group_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_5g_vn_groups (ext_group_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ext_group_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ext_group_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> FiveGVnGroupStore::get(const std::string& ext_group_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_5g_vn_groups WHERE ext_group_id = $1",
                                 pqxx::params{ext_group_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

std::optional<nlohmann::json> FiveGVnGroupStore::apply_patch(const std::string& ext_group_id,
                                                             const nlohmann::json& patch_ops) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_5g_vn_groups WHERE ext_group_id = $1",
                                 pqxx::params{ext_group_id});
    if (result.empty()) {
        return std::nullopt;
    }
    auto data = nlohmann::json::parse(result.front()["data"].as<std::string>());
    data = data.patch(patch_ops); // may throw nlohmann::json::exception -- caller catches
    txn.exec("UPDATE udr_5g_vn_groups SET data = $2::jsonb WHERE ext_group_id = $1",
             pqxx::params{ext_group_id, data.dump()});
    txn.commit();
    return std::make_optional(data);
}

bool FiveGVnGroupStore::remove(const std::string& ext_group_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("DELETE FROM udr_5g_vn_groups WHERE ext_group_id = $1",
                                 pqxx::params{ext_group_id});
    txn.commit();
    return result.affected_rows() > 0;
}

std::vector<std::pair<std::string, nlohmann::json>> FiveGVnGroupStore::list_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT ext_group_id, data FROM udr_5g_vn_groups");
    std::vector<std::pair<std::string, nlohmann::json>> out;
    out.reserve(static_cast<std::size_t>(result.size()));
    for (const auto& row : result) {
        out.emplace_back(row["ext_group_id"].as<std::string>(),
                         nlohmann::json::parse(row["data"].as<std::string>()));
    }
    return out;
}

MbsGroupMembershipStore::MbsGroupMembershipStore(const std::string& conninfo) : conn_(conninfo) {}

void MbsGroupMembershipStore::put(const std::string& ext_group_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_mbs_group_membership (ext_group_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ext_group_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ext_group_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> MbsGroupMembershipStore::get(const std::string& ext_group_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_mbs_group_membership WHERE ext_group_id = $1",
                 pqxx::params{ext_group_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

std::optional<nlohmann::json>
MbsGroupMembershipStore::apply_patch(const std::string& ext_group_id,
                                     const nlohmann::json& patch_ops) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_mbs_group_membership WHERE ext_group_id = $1",
                 pqxx::params{ext_group_id});
    if (result.empty()) {
        return std::nullopt;
    }
    auto data = nlohmann::json::parse(result.front()["data"].as<std::string>());
    data = data.patch(patch_ops); // may throw nlohmann::json::exception -- caller catches
    txn.exec("UPDATE udr_mbs_group_membership SET data = $2::jsonb WHERE ext_group_id = $1",
             pqxx::params{ext_group_id, data.dump()});
    txn.commit();
    return std::make_optional(data);
}

bool MbsGroupMembershipStore::remove(const std::string& ext_group_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("DELETE FROM udr_mbs_group_membership WHERE ext_group_id = $1",
                                 pqxx::params{ext_group_id});
    txn.commit();
    return result.affected_rows() > 0;
}

std::vector<std::pair<std::string, nlohmann::json>> MbsGroupMembershipStore::list_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT ext_group_id, data FROM udr_mbs_group_membership");
    std::vector<std::pair<std::string, nlohmann::json>> out;
    out.reserve(static_cast<std::size_t>(result.size()));
    for (const auto& row : result) {
        out.emplace_back(row["ext_group_id"].as<std::string>(),
                         nlohmann::json::parse(row["data"].as<std::string>()));
    }
    return out;
}

GroupEeProfileDataStore::GroupEeProfileDataStore(const std::string& conninfo) : conn_(conninfo) {}

void GroupEeProfileDataStore::seed(const std::string& ue_group_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_group_ee_profile_data (ue_group_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (ue_group_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_group_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> GroupEeProfileDataStore::get(const std::string& ue_group_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_group_ee_profile_data WHERE ue_group_id = $1",
                 pqxx::params{ue_group_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

EeSubscriptionsStore::EeSubscriptionsStore(const std::string& conninfo) : conn_(conninfo) {}

void EeSubscriptionsStore::create(const std::string& ue_id,
                                  const std::string& subs_id,
                                  nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_ee_subscriptions (ue_id, subs_id, data) VALUES ($1, $2, $3::jsonb)",
             pqxx::params{ue_id, subs_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> EeSubscriptionsStore::get(const std::string& ue_id,
                                                        const std::string& subs_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_ee_subscriptions WHERE ue_id = $1 AND subs_id = $2",
                 pqxx::params{ue_id, subs_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

std::vector<nlohmann::json> EeSubscriptionsStore::list(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_ee_subscriptions WHERE ue_id = $1", pqxx::params{ue_id});
    std::vector<nlohmann::json> out;
    out.reserve(static_cast<std::size_t>(result.size()));
    for (const auto& row : result) {
        out.push_back(nlohmann::json::parse(row["data"].as<std::string>()));
    }
    return out;
}

bool EeSubscriptionsStore::update(const std::string& ue_id,
                                  const std::string& subs_id,
                                  nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec(
        "UPDATE udr_ee_subscriptions SET data = $3::jsonb WHERE ue_id = $1 AND subs_id = $2",
        pqxx::params{ue_id, subs_id, data.dump()});
    txn.commit();
    return result.affected_rows() > 0;
}

std::optional<nlohmann::json> EeSubscriptionsStore::apply_patch(const std::string& ue_id,
                                                                const std::string& subs_id,
                                                                const nlohmann::json& patch_ops) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_ee_subscriptions WHERE ue_id = $1 AND subs_id = $2",
                 pqxx::params{ue_id, subs_id});
    if (result.empty()) {
        return std::nullopt;
    }
    auto data = nlohmann::json::parse(result.front()["data"].as<std::string>());
    data = data.patch(patch_ops); // may throw nlohmann::json::exception -- caller catches
    txn.exec("UPDATE udr_ee_subscriptions SET data = $3::jsonb WHERE ue_id = $1 AND subs_id = $2",
             pqxx::params{ue_id, subs_id, data.dump()});
    txn.commit();
    return std::make_optional(data);
}

bool EeSubscriptionsStore::remove(const std::string& ue_id, const std::string& subs_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("DELETE FROM udr_ee_subscriptions WHERE ue_id = $1 AND subs_id = $2",
                 pqxx::params{ue_id, subs_id});
    txn.commit();
    return result.affected_rows() > 0;
}

SubsToNotifyStore::SubsToNotifyStore(const std::string& conninfo) : conn_(conninfo) {}

void SubsToNotifyStore::create(const std::string& subs_id,
                               const std::optional<std::string>& ue_id,
                               nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_subs_to_notify (subs_id, ue_id, data) VALUES ($1, $2, $3::jsonb)",
             pqxx::params{subs_id, ue_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> SubsToNotifyStore::get(const std::string& subs_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_subs_to_notify WHERE subs_id = $1", pqxx::params{subs_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

std::vector<nlohmann::json> SubsToNotifyStore::list_by_ue_id(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_subs_to_notify WHERE ue_id = $1", pqxx::params{ue_id});
    std::vector<nlohmann::json> out;
    out.reserve(static_cast<std::size_t>(result.size()));
    for (const auto& row : result) {
        out.push_back(nlohmann::json::parse(row["data"].as<std::string>()));
    }
    return out;
}

std::vector<nlohmann::json> SubsToNotifyStore::list_ue_less() {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_subs_to_notify WHERE ue_id IS NULL");
    std::vector<nlohmann::json> out;
    out.reserve(static_cast<std::size_t>(result.size()));
    for (const auto& row : result) {
        out.push_back(nlohmann::json::parse(row["data"].as<std::string>()));
    }
    return out;
}

std::optional<nlohmann::json> SubsToNotifyStore::apply_patch(const std::string& subs_id,
                                                             const nlohmann::json& patch_ops) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_subs_to_notify WHERE subs_id = $1", pqxx::params{subs_id});
    if (result.empty()) {
        return std::nullopt;
    }
    auto data = nlohmann::json::parse(result.front()["data"].as<std::string>());
    data = data.patch(patch_ops); // may throw nlohmann::json::exception -- caller catches
    txn.exec("UPDATE udr_subs_to_notify SET data = $2::jsonb WHERE subs_id = $1",
             pqxx::params{subs_id, data.dump()});
    txn.commit();
    return std::make_optional(data);
}

bool SubsToNotifyStore::remove(const std::string& subs_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("DELETE FROM udr_subs_to_notify WHERE subs_id = $1", pqxx::params{subs_id});
    txn.commit();
    return result.affected_rows() > 0;
}

SdmSubscriptionsStore::SdmSubscriptionsStore(const std::string& conninfo) : conn_(conninfo) {}

void SdmSubscriptionsStore::create(const std::string& ue_id,
                                   const std::string& subs_id,
                                   nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_sdm_subscriptions (ue_id, subs_id, data) VALUES ($1, $2, $3::jsonb)",
             pqxx::params{ue_id, subs_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> SdmSubscriptionsStore::get(const std::string& ue_id,
                                                         const std::string& subs_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_sdm_subscriptions WHERE ue_id = $1 AND subs_id = $2",
                 pqxx::params{ue_id, subs_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

std::vector<nlohmann::json> SdmSubscriptionsStore::list(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_sdm_subscriptions WHERE ue_id = $1", pqxx::params{ue_id});
    std::vector<nlohmann::json> out;
    out.reserve(static_cast<std::size_t>(result.size()));
    for (const auto& row : result) {
        out.push_back(nlohmann::json::parse(row["data"].as<std::string>()));
    }
    return out;
}

bool SdmSubscriptionsStore::update(const std::string& ue_id,
                                   const std::string& subs_id,
                                   nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec(
        "UPDATE udr_sdm_subscriptions SET data = $3::jsonb WHERE ue_id = $1 AND subs_id = $2",
        pqxx::params{ue_id, subs_id, data.dump()});
    txn.commit();
    return result.affected_rows() > 0;
}

std::optional<nlohmann::json> SdmSubscriptionsStore::apply_patch(const std::string& ue_id,
                                                                 const std::string& subs_id,
                                                                 const nlohmann::json& patch_ops) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_sdm_subscriptions WHERE ue_id = $1 AND subs_id = $2",
                 pqxx::params{ue_id, subs_id});
    if (result.empty()) {
        return std::nullopt;
    }
    auto data = nlohmann::json::parse(result.front()["data"].as<std::string>());
    data = data.patch(patch_ops); // may throw nlohmann::json::exception -- caller catches
    txn.exec("UPDATE udr_sdm_subscriptions SET data = $3::jsonb WHERE ue_id = $1 AND subs_id = $2",
             pqxx::params{ue_id, subs_id, data.dump()});
    txn.commit();
    return std::make_optional(data);
}

bool SdmSubscriptionsStore::remove(const std::string& ue_id, const std::string& subs_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("DELETE FROM udr_sdm_subscriptions WHERE ue_id = $1 AND subs_id = $2",
                 pqxx::params{ue_id, subs_id});
    txn.commit();
    return result.affected_rows() > 0;
}

EeAmfSubscriptionInfoStore::EeAmfSubscriptionInfoStore(const std::string& conninfo)
    : conn_(conninfo) {}

bool EeAmfSubscriptionInfoStore::put(const std::string& ue_id,
                                     const std::string& subs_id,
                                     nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto row = txn.exec("INSERT INTO udr_ee_amf_subscription_info (ue_id, subs_id, data) "
                              "VALUES ($1, $2, $3::jsonb) "
                              "ON CONFLICT (ue_id, subs_id) DO UPDATE SET data = EXCLUDED.data "
                              "RETURNING (xmax = 0) AS inserted",
                              pqxx::params{ue_id, subs_id, data.dump()})
                         .one_row();
    txn.commit();
    return row["inserted"].as<bool>();
}

std::optional<nlohmann::json> EeAmfSubscriptionInfoStore::get(const std::string& ue_id,
                                                              const std::string& subs_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_ee_amf_subscription_info WHERE ue_id = $1 AND subs_id = $2",
                 pqxx::params{ue_id, subs_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

std::optional<nlohmann::json> EeAmfSubscriptionInfoStore::apply_patch(
    const std::string& ue_id, const std::string& subs_id, const nlohmann::json& patch_ops) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_ee_amf_subscription_info WHERE ue_id = $1 AND subs_id = $2",
                 pqxx::params{ue_id, subs_id});
    if (result.empty()) {
        return std::nullopt;
    }
    auto data = nlohmann::json::parse(result.front()["data"].as<std::string>());
    data = data.patch(patch_ops); // may throw nlohmann::json::exception -- caller catches
    txn.exec("UPDATE udr_ee_amf_subscription_info SET data = $3::jsonb "
             "WHERE ue_id = $1 AND subs_id = $2",
             pqxx::params{ue_id, subs_id, data.dump()});
    txn.commit();
    return std::make_optional(data);
}

bool EeAmfSubscriptionInfoStore::remove(const std::string& ue_id, const std::string& subs_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("DELETE FROM udr_ee_amf_subscription_info WHERE ue_id = $1 AND subs_id = $2",
                 pqxx::params{ue_id, subs_id});
    txn.commit();
    return result.affected_rows() > 0;
}

EeSmfSubscriptionInfoStore::EeSmfSubscriptionInfoStore(const std::string& conninfo)
    : conn_(conninfo) {}

bool EeSmfSubscriptionInfoStore::put(const std::string& ue_id,
                                     const std::string& subs_id,
                                     nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto row = txn.exec("INSERT INTO udr_ee_smf_subscription_info (ue_id, subs_id, data) "
                              "VALUES ($1, $2, $3::jsonb) "
                              "ON CONFLICT (ue_id, subs_id) DO UPDATE SET data = EXCLUDED.data "
                              "RETURNING (xmax = 0) AS inserted",
                              pqxx::params{ue_id, subs_id, data.dump()})
                         .one_row();
    txn.commit();
    return row["inserted"].as<bool>();
}

std::optional<nlohmann::json> EeSmfSubscriptionInfoStore::get(const std::string& ue_id,
                                                              const std::string& subs_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_ee_smf_subscription_info WHERE ue_id = $1 AND subs_id = $2",
                 pqxx::params{ue_id, subs_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

std::optional<nlohmann::json> EeSmfSubscriptionInfoStore::apply_patch(
    const std::string& ue_id, const std::string& subs_id, const nlohmann::json& patch_ops) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_ee_smf_subscription_info WHERE ue_id = $1 AND subs_id = $2",
                 pqxx::params{ue_id, subs_id});
    if (result.empty()) {
        return std::nullopt;
    }
    auto data = nlohmann::json::parse(result.front()["data"].as<std::string>());
    data = data.patch(patch_ops); // may throw nlohmann::json::exception -- caller catches
    txn.exec("UPDATE udr_ee_smf_subscription_info SET data = $3::jsonb "
             "WHERE ue_id = $1 AND subs_id = $2",
             pqxx::params{ue_id, subs_id, data.dump()});
    txn.commit();
    return std::make_optional(data);
}

bool EeSmfSubscriptionInfoStore::remove(const std::string& ue_id, const std::string& subs_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("DELETE FROM udr_ee_smf_subscription_info WHERE ue_id = $1 AND subs_id = $2",
                 pqxx::params{ue_id, subs_id});
    txn.commit();
    return result.affected_rows() > 0;
}

EeHssSubscriptionInfoStore::EeHssSubscriptionInfoStore(const std::string& conninfo)
    : conn_(conninfo) {}

bool EeHssSubscriptionInfoStore::put(const std::string& ue_id,
                                     const std::string& subs_id,
                                     nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto row = txn.exec("INSERT INTO udr_ee_hss_subscription_info (ue_id, subs_id, data) "
                              "VALUES ($1, $2, $3::jsonb) "
                              "ON CONFLICT (ue_id, subs_id) DO UPDATE SET data = EXCLUDED.data "
                              "RETURNING (xmax = 0) AS inserted",
                              pqxx::params{ue_id, subs_id, data.dump()})
                         .one_row();
    txn.commit();
    return row["inserted"].as<bool>();
}

std::optional<nlohmann::json> EeHssSubscriptionInfoStore::get(const std::string& ue_id,
                                                              const std::string& subs_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_ee_hss_subscription_info WHERE ue_id = $1 AND subs_id = $2",
                 pqxx::params{ue_id, subs_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

std::optional<nlohmann::json> EeHssSubscriptionInfoStore::apply_patch(
    const std::string& ue_id, const std::string& subs_id, const nlohmann::json& patch_ops) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_ee_hss_subscription_info WHERE ue_id = $1 AND subs_id = $2",
                 pqxx::params{ue_id, subs_id});
    if (result.empty()) {
        return std::nullopt;
    }
    auto data = nlohmann::json::parse(result.front()["data"].as<std::string>());
    data = data.patch(patch_ops); // may throw nlohmann::json::exception -- caller catches
    txn.exec("UPDATE udr_ee_hss_subscription_info SET data = $3::jsonb "
             "WHERE ue_id = $1 AND subs_id = $2",
             pqxx::params{ue_id, subs_id, data.dump()});
    txn.commit();
    return std::make_optional(data);
}

bool EeHssSubscriptionInfoStore::remove(const std::string& ue_id, const std::string& subs_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("DELETE FROM udr_ee_hss_subscription_info WHERE ue_id = $1 AND subs_id = $2",
                 pqxx::params{ue_id, subs_id});
    txn.commit();
    return result.affected_rows() > 0;
}

SdmHssSubscriptionInfoStore::SdmHssSubscriptionInfoStore(const std::string& conninfo)
    : conn_(conninfo) {}

void SdmHssSubscriptionInfoStore::put(const std::string& ue_id,
                                      const std::string& subs_id,
                                      nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_sdm_hss_subscription_info (ue_id, subs_id, data) "
             "VALUES ($1, $2, $3::jsonb) "
             "ON CONFLICT (ue_id, subs_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_id, subs_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> SdmHssSubscriptionInfoStore::get(const std::string& ue_id,
                                                               const std::string& subs_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_sdm_hss_subscription_info WHERE ue_id = $1 AND subs_id = $2",
                 pqxx::params{ue_id, subs_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

std::optional<nlohmann::json> SdmHssSubscriptionInfoStore::apply_patch(
    const std::string& ue_id, const std::string& subs_id, const nlohmann::json& patch_ops) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_sdm_hss_subscription_info WHERE ue_id = $1 AND subs_id = $2",
                 pqxx::params{ue_id, subs_id});
    if (result.empty()) {
        return std::nullopt;
    }
    auto data = nlohmann::json::parse(result.front()["data"].as<std::string>());
    data = data.patch(patch_ops); // may throw nlohmann::json::exception -- caller catches
    txn.exec("UPDATE udr_sdm_hss_subscription_info SET data = $3::jsonb "
             "WHERE ue_id = $1 AND subs_id = $2",
             pqxx::params{ue_id, subs_id, data.dump()});
    txn.commit();
    return std::make_optional(data);
}

bool SdmHssSubscriptionInfoStore::remove(const std::string& ue_id, const std::string& subs_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("DELETE FROM udr_sdm_hss_subscription_info WHERE ue_id = $1 AND subs_id = $2",
                 pqxx::params{ue_id, subs_id});
    txn.commit();
    return result.affected_rows() > 0;
}

GroupEeSubscriptionsStore::GroupEeSubscriptionsStore(const std::string& conninfo)
    : conn_(conninfo) {}

void GroupEeSubscriptionsStore::create(const std::string& ue_group_id,
                                       const std::string& subs_id,
                                       nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_group_ee_subscriptions (ue_group_id, subs_id, data) "
             "VALUES ($1, $2, $3::jsonb)",
             pqxx::params{ue_group_id, subs_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> GroupEeSubscriptionsStore::get(const std::string& ue_group_id,
                                                             const std::string& subs_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec(
        "SELECT data FROM udr_group_ee_subscriptions WHERE ue_group_id = $1 AND subs_id = $2",
        pqxx::params{ue_group_id, subs_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

std::vector<nlohmann::json> GroupEeSubscriptionsStore::list(const std::string& ue_group_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_group_ee_subscriptions WHERE ue_group_id = $1",
                 pqxx::params{ue_group_id});
    std::vector<nlohmann::json> out;
    out.reserve(static_cast<std::size_t>(result.size()));
    for (const auto& row : result) {
        out.push_back(nlohmann::json::parse(row["data"].as<std::string>()));
    }
    return out;
}

bool GroupEeSubscriptionsStore::update(const std::string& ue_group_id,
                                       const std::string& subs_id,
                                       nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("UPDATE udr_group_ee_subscriptions SET data = $3::jsonb "
                                 "WHERE ue_group_id = $1 AND subs_id = $2",
                                 pqxx::params{ue_group_id, subs_id, data.dump()});
    txn.commit();
    return result.affected_rows() > 0;
}

std::optional<nlohmann::json> GroupEeSubscriptionsStore::apply_patch(
    const std::string& ue_group_id, const std::string& subs_id, const nlohmann::json& patch_ops) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec(
        "SELECT data FROM udr_group_ee_subscriptions WHERE ue_group_id = $1 AND subs_id = $2",
        pqxx::params{ue_group_id, subs_id});
    if (result.empty()) {
        return std::nullopt;
    }
    auto data = nlohmann::json::parse(result.front()["data"].as<std::string>());
    data = data.patch(patch_ops); // may throw nlohmann::json::exception -- caller catches
    txn.exec("UPDATE udr_group_ee_subscriptions SET data = $3::jsonb "
             "WHERE ue_group_id = $1 AND subs_id = $2",
             pqxx::params{ue_group_id, subs_id, data.dump()});
    txn.commit();
    return std::make_optional(data);
}

bool GroupEeSubscriptionsStore::remove(const std::string& ue_group_id, const std::string& subs_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("DELETE FROM udr_group_ee_subscriptions WHERE ue_group_id = $1 AND subs_id = $2",
                 pqxx::params{ue_group_id, subs_id});
    txn.commit();
    return result.affected_rows() > 0;
}

GroupAmfSubscriptionInfoStore::GroupAmfSubscriptionInfoStore(const std::string& conninfo)
    : conn_(conninfo) {}

bool GroupAmfSubscriptionInfoStore::put(const std::string& ue_group_id,
                                        const std::string& subs_id,
                                        nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto row =
        txn.exec("INSERT INTO udr_group_amf_subscription_info (ue_group_id, subs_id, data) "
                 "VALUES ($1, $2, $3::jsonb) "
                 "ON CONFLICT (ue_group_id, subs_id) DO UPDATE SET data = EXCLUDED.data "
                 "RETURNING (xmax = 0) AS inserted",
                 pqxx::params{ue_group_id, subs_id, data.dump()})
            .one_row();
    txn.commit();
    return row["inserted"].as<bool>();
}

std::optional<nlohmann::json> GroupAmfSubscriptionInfoStore::get(const std::string& ue_group_id,
                                                                 const std::string& subs_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec(
        "SELECT data FROM udr_group_amf_subscription_info WHERE ue_group_id = $1 AND subs_id = $2",
        pqxx::params{ue_group_id, subs_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

std::optional<nlohmann::json> GroupAmfSubscriptionInfoStore::apply_patch(
    const std::string& ue_group_id, const std::string& subs_id, const nlohmann::json& patch_ops) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec(
        "SELECT data FROM udr_group_amf_subscription_info WHERE ue_group_id = $1 AND subs_id = $2",
        pqxx::params{ue_group_id, subs_id});
    if (result.empty()) {
        return std::nullopt;
    }
    auto data = nlohmann::json::parse(result.front()["data"].as<std::string>());
    data = data.patch(patch_ops); // may throw nlohmann::json::exception -- caller catches
    txn.exec("UPDATE udr_group_amf_subscription_info SET data = $3::jsonb "
             "WHERE ue_group_id = $1 AND subs_id = $2",
             pqxx::params{ue_group_id, subs_id, data.dump()});
    txn.commit();
    return std::make_optional(data);
}

bool GroupAmfSubscriptionInfoStore::remove(const std::string& ue_group_id,
                                           const std::string& subs_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec(
        "DELETE FROM udr_group_amf_subscription_info WHERE ue_group_id = $1 AND subs_id = $2",
        pqxx::params{ue_group_id, subs_id});
    txn.commit();
    return result.affected_rows() > 0;
}

GroupSmfSubscriptionInfoStore::GroupSmfSubscriptionInfoStore(const std::string& conninfo)
    : conn_(conninfo) {}

bool GroupSmfSubscriptionInfoStore::put(const std::string& ue_group_id,
                                        const std::string& subs_id,
                                        nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto row =
        txn.exec("INSERT INTO udr_group_smf_subscription_info (ue_group_id, subs_id, data) "
                 "VALUES ($1, $2, $3::jsonb) "
                 "ON CONFLICT (ue_group_id, subs_id) DO UPDATE SET data = EXCLUDED.data "
                 "RETURNING (xmax = 0) AS inserted",
                 pqxx::params{ue_group_id, subs_id, data.dump()})
            .one_row();
    txn.commit();
    return row["inserted"].as<bool>();
}

std::optional<nlohmann::json> GroupSmfSubscriptionInfoStore::get(const std::string& ue_group_id,
                                                                 const std::string& subs_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec(
        "SELECT data FROM udr_group_smf_subscription_info WHERE ue_group_id = $1 AND subs_id = $2",
        pqxx::params{ue_group_id, subs_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

std::optional<nlohmann::json> GroupSmfSubscriptionInfoStore::apply_patch(
    const std::string& ue_group_id, const std::string& subs_id, const nlohmann::json& patch_ops) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec(
        "SELECT data FROM udr_group_smf_subscription_info WHERE ue_group_id = $1 AND subs_id = $2",
        pqxx::params{ue_group_id, subs_id});
    if (result.empty()) {
        return std::nullopt;
    }
    auto data = nlohmann::json::parse(result.front()["data"].as<std::string>());
    data = data.patch(patch_ops); // may throw nlohmann::json::exception -- caller catches
    txn.exec("UPDATE udr_group_smf_subscription_info SET data = $3::jsonb "
             "WHERE ue_group_id = $1 AND subs_id = $2",
             pqxx::params{ue_group_id, subs_id, data.dump()});
    txn.commit();
    return std::make_optional(data);
}

bool GroupSmfSubscriptionInfoStore::remove(const std::string& ue_group_id,
                                           const std::string& subs_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec(
        "DELETE FROM udr_group_smf_subscription_info WHERE ue_group_id = $1 AND subs_id = $2",
        pqxx::params{ue_group_id, subs_id});
    txn.commit();
    return result.affected_rows() > 0;
}

GroupHssSubscriptionInfoStore::GroupHssSubscriptionInfoStore(const std::string& conninfo)
    : conn_(conninfo) {}

bool GroupHssSubscriptionInfoStore::put(const std::string& ue_group_id,
                                        const std::string& subs_id,
                                        nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto row =
        txn.exec("INSERT INTO udr_group_hss_subscription_info (ue_group_id, subs_id, data) "
                 "VALUES ($1, $2, $3::jsonb) "
                 "ON CONFLICT (ue_group_id, subs_id) DO UPDATE SET data = EXCLUDED.data "
                 "RETURNING (xmax = 0) AS inserted",
                 pqxx::params{ue_group_id, subs_id, data.dump()})
            .one_row();
    txn.commit();
    return row["inserted"].as<bool>();
}

std::optional<nlohmann::json> GroupHssSubscriptionInfoStore::get(const std::string& ue_group_id,
                                                                 const std::string& subs_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec(
        "SELECT data FROM udr_group_hss_subscription_info WHERE ue_group_id = $1 AND subs_id = $2",
        pqxx::params{ue_group_id, subs_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

std::optional<nlohmann::json> GroupHssSubscriptionInfoStore::apply_patch(
    const std::string& ue_group_id, const std::string& subs_id, const nlohmann::json& patch_ops) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec(
        "SELECT data FROM udr_group_hss_subscription_info WHERE ue_group_id = $1 AND subs_id = $2",
        pqxx::params{ue_group_id, subs_id});
    if (result.empty()) {
        return std::nullopt;
    }
    auto data = nlohmann::json::parse(result.front()["data"].as<std::string>());
    data = data.patch(patch_ops); // may throw nlohmann::json::exception -- caller catches
    txn.exec("UPDATE udr_group_hss_subscription_info SET data = $3::jsonb "
             "WHERE ue_group_id = $1 AND subs_id = $2",
             pqxx::params{ue_group_id, subs_id, data.dump()});
    txn.commit();
    return std::make_optional(data);
}

bool GroupHssSubscriptionInfoStore::remove(const std::string& ue_group_id,
                                           const std::string& subs_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec(
        "DELETE FROM udr_group_hss_subscription_info WHERE ue_group_id = $1 AND subs_id = $2",
        pqxx::params{ue_group_id, subs_id});
    txn.commit();
    return result.affected_rows() > 0;
}

PdtqDataStore::PdtqDataStore(const std::string& conninfo) : conn_(conninfo) {}

void PdtqDataStore::put(const std::string& pdtq_ref_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_pdtq_data (pdtq_ref_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (pdtq_ref_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{pdtq_ref_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> PdtqDataStore::get(const std::string& pdtq_ref_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_pdtq_data WHERE pdtq_ref_id = $1",
                                 pqxx::params{pdtq_ref_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

std::vector<nlohmann::json> PdtqDataStore::list() {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_pdtq_data");
    std::vector<nlohmann::json> out;
    out.reserve(static_cast<std::size_t>(result.size()));
    for (const auto& row : result) {
        out.push_back(nlohmann::json::parse(row["data"].as<std::string>()));
    }
    return out;
}

std::optional<nlohmann::json> PdtqDataStore::merge_patch(const std::string& pdtq_ref_id,
                                                         const nlohmann::json& patch) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_pdtq_data WHERE pdtq_ref_id = $1",
                                 pqxx::params{pdtq_ref_id});
    if (result.empty()) {
        return std::nullopt;
    }
    auto doc = nlohmann::json::parse(result.front()["data"].as<std::string>());
    doc.merge_patch(patch);
    txn.exec("UPDATE udr_pdtq_data SET data = $2::jsonb WHERE pdtq_ref_id = $1",
             pqxx::params{pdtq_ref_id, doc.dump()});
    txn.commit();
    return std::make_optional(doc);
}

bool PdtqDataStore::remove(const std::string& pdtq_ref_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("DELETE FROM udr_pdtq_data WHERE pdtq_ref_id = $1", pqxx::params{pdtq_ref_id});
    txn.commit();
    return result.affected_rows() > 0;
}

NfGroupIdStore::NfGroupIdStore(const std::string& conninfo) : conn_(conninfo) {}

void NfGroupIdStore::seed(const std::string& subscriber_id,
                          const std::string& nf_type,
                          std::string group_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_nf_group_ids (subscriber_id, nf_type, group_id) "
             "VALUES ($1, $2, $3) "
             "ON CONFLICT (subscriber_id, nf_type) DO UPDATE SET group_id = EXCLUDED.group_id",
             pqxx::params{subscriber_id, nf_type, group_id});
    txn.commit();
}

std::optional<std::string> NfGroupIdStore::get(const std::string& subscriber_id,
                                               const std::string& nf_type) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT group_id FROM udr_nf_group_ids WHERE subscriber_id = $1 AND nf_type = $2",
                 pqxx::params{subscriber_id, nf_type});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(result.front()["group_id"].as<std::string>());
}

NiddAuthorizationDataStore::NiddAuthorizationDataStore(const std::string& conninfo)
    : conn_(conninfo) {}

void NiddAuthorizationDataStore::seed(const std::string& ue_id,
                                      int sst,
                                      const std::string& sd,
                                      const std::string& dnn,
                                      const std::string& mtc_provider_information,
                                      nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_nidd_authorization_data "
             "(ue_id, sst, sd, dnn, mtc_provider_information, data) "
             "VALUES ($1, $2, $3, $4, $5, $6::jsonb) "
             "ON CONFLICT (ue_id, sst, sd, dnn, mtc_provider_information) "
             "DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_id, sst, sd, dnn, mtc_provider_information, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json>
NiddAuthorizationDataStore::get(const std::string& ue_id,
                                int sst,
                                const std::string& sd,
                                const std::string& dnn,
                                const std::string& mtc_provider_information) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_nidd_authorization_data WHERE ue_id = $1 AND sst = $2 "
                 "AND sd = $3 AND dnn = $4 AND mtc_provider_information = $5",
                 pqxx::params{ue_id, sst, sd, dnn, mtc_provider_information});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

FiveGVnGroupPpProfileDataStore::FiveGVnGroupPpProfileDataStore(const std::string& conninfo)
    : conn_(conninfo) {}

void FiveGVnGroupPpProfileDataStore::seed(nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_5g_vn_group_pp_profile_data (id, data) VALUES (1, $1::jsonb) "
             "ON CONFLICT (id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> FiveGVnGroupPpProfileDataStore::get() {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_5g_vn_group_pp_profile_data WHERE id = 1");
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

MbsGroupPpProfileDataStore::MbsGroupPpProfileDataStore(const std::string& conninfo)
    : conn_(conninfo) {}

void MbsGroupPpProfileDataStore::seed(nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_mbs_group_pp_profile_data (id, data) VALUES (1, $1::jsonb) "
             "ON CONFLICT (id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> MbsGroupPpProfileDataStore::get() {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_mbs_group_pp_profile_data WHERE id = 1");
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

NfGroupIdSubscriptionStore::NfGroupIdSubscriptionStore(const std::string& conninfo)
    : conn_(conninfo) {}

void NfGroupIdSubscriptionStore::create(const std::string& subscription_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_nf_group_id_subscriptions (subscription_id, data) "
             "VALUES ($1, $2::jsonb)",
             pqxx::params{subscription_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> NfGroupIdSubscriptionStore::get(const std::string& subscription_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_nf_group_id_subscriptions WHERE subscription_id = $1",
                 pqxx::params{subscription_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

std::optional<nlohmann::json>
NfGroupIdSubscriptionStore::apply_patch(const std::string& subscription_id,
                                        const nlohmann::json& patch_ops) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_nf_group_id_subscriptions WHERE subscription_id = $1",
                 pqxx::params{subscription_id});
    if (result.empty()) {
        return std::nullopt;
    }
    auto data = nlohmann::json::parse(result.front()["data"].as<std::string>());
    data = data.patch(patch_ops); // may throw nlohmann::json::exception -- caller catches
    txn.exec("UPDATE udr_nf_group_id_subscriptions SET data = $2::jsonb WHERE subscription_id = $1",
             pqxx::params{subscription_id, data.dump()});
    txn.commit();
    return std::make_optional(data);
}

bool NfGroupIdSubscriptionStore::remove(const std::string& subscription_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("DELETE FROM udr_nf_group_id_subscriptions WHERE subscription_id = $1",
                 pqxx::params{subscription_id});
    txn.commit();
    return result.affected_rows() > 0;
}

std::vector<nlohmann::json> NfGroupIdSubscriptionStore::list_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_nf_group_id_subscriptions");
    std::vector<nlohmann::json> out;
    out.reserve(static_cast<std::size_t>(result.size()));
    for (const auto& row : result) {
        out.push_back(nlohmann::json::parse(row["data"].as<std::string>()));
    }
    return out;
}

MbsSessionPolicyDataStore::MbsSessionPolicyDataStore(const std::string& conninfo)
    : conn_(conninfo) {}

void MbsSessionPolicyDataStore::seed(const std::string& pol_session_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_mbs_session_pol_data (pol_session_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (pol_session_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{pol_session_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> MbsSessionPolicyDataStore::get(const std::string& pol_session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_mbs_session_pol_data WHERE pol_session_id = $1",
                 pqxx::params{pol_session_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

UsageMonDataStore::UsageMonDataStore(const std::string& conninfo) : conn_(conninfo) {}

void UsageMonDataStore::put(const std::string& ue_id,
                            const std::string& usage_mon_id,
                            nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_usage_mon_data (ue_id, usage_mon_id, data) "
             "VALUES ($1, $2, $3::jsonb) "
             "ON CONFLICT (ue_id, usage_mon_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_id, usage_mon_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> UsageMonDataStore::get(const std::string& ue_id,
                                                     const std::string& usage_mon_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_usage_mon_data "
                                 "WHERE ue_id = $1 AND usage_mon_id = $2",
                                 pqxx::params{ue_id, usage_mon_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

bool UsageMonDataStore::remove(const std::string& ue_id, const std::string& usage_mon_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("DELETE FROM udr_usage_mon_data "
                                 "WHERE ue_id = $1 AND usage_mon_id = $2",
                                 pqxx::params{ue_id, usage_mon_id});
    txn.commit();
    return result.affected_rows() > 0;
}

std::vector<nlohmann::json> UsageMonDataStore::list_by_ue_id(const std::string& ue_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_usage_mon_data WHERE ue_id = $1", pqxx::params{ue_id});
    std::vector<nlohmann::json> out;
    out.reserve(static_cast<std::size_t>(result.size()));
    for (const auto& row : result) {
        out.push_back(nlohmann::json::parse(row["data"].as<std::string>()));
    }
    return out;
}

PolicyDataSubsToNotifyStore::PolicyDataSubsToNotifyStore(const std::string& conninfo)
    : conn_(conninfo) {}

void PolicyDataSubsToNotifyStore::create(const std::string& subs_id,
                                         const std::optional<std::string>& ue_id,
                                         nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_policy_data_subs_to_notify (subs_id, ue_id, data) "
             "VALUES ($1, $2, $3::jsonb)",
             pqxx::params{subs_id, ue_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> PolicyDataSubsToNotifyStore::get(const std::string& subs_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_policy_data_subs_to_notify "
                                 "WHERE subs_id = $1",
                                 pqxx::params{subs_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

std::vector<nlohmann::json> PolicyDataSubsToNotifyStore::list_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_policy_data_subs_to_notify");
    std::vector<nlohmann::json> out;
    out.reserve(static_cast<std::size_t>(result.size()));
    for (const auto& row : result) {
        out.push_back(nlohmann::json::parse(row["data"].as<std::string>()));
    }
    return out;
}

std::optional<nlohmann::json> PolicyDataSubsToNotifyStore::replace(const std::string& subs_id,
                                                                   nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("UPDATE udr_policy_data_subs_to_notify SET data = $2::jsonb "
                                 "WHERE subs_id = $1",
                                 pqxx::params{subs_id, data.dump()});
    if (result.affected_rows() == 0) {
        return std::nullopt;
    }
    txn.commit();
    return std::make_optional(data);
}

bool PolicyDataSubsToNotifyStore::remove(const std::string& subs_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("DELETE FROM udr_policy_data_subs_to_notify WHERE subs_id = $1",
                                 pqxx::params{subs_id});
    txn.commit();
    return result.affected_rows() > 0;
}

} // namespace udr

namespace udr {

// --- ADR-0253: application-data traffic-influence family ---

TrafficInfluenceDataStore::TrafficInfluenceDataStore(const std::string& conninfo)
    : conn_(conninfo) {}

std::vector<nlohmann::json> TrafficInfluenceDataStore::list() {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_traffic_influence_data ORDER BY "
                                 "influence_id");
    std::vector<nlohmann::json> out;
    out.reserve(result.size());
    for (const auto& row : result) {
        out.push_back(nlohmann::json::parse(row["data"].as<std::string>()));
    }
    return out;
}

std::vector<std::pair<std::string, nlohmann::json>> TrafficInfluenceDataStore::list_with_ids() {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT influence_id, data FROM udr_traffic_influence_data ORDER "
                                 "BY influence_id");
    std::vector<std::pair<std::string, nlohmann::json>> out;
    out.reserve(result.size());
    for (const auto& row : result) {
        out.emplace_back(row["influence_id"].as<std::string>(),
                         nlohmann::json::parse(row["data"].as<std::string>()));
    }
    return out;
}

std::optional<nlohmann::json> TrafficInfluenceDataStore::get(const std::string& influence_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_traffic_influence_data WHERE "
                                 "influence_id = $1",
                                 pqxx::params{influence_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

bool TrafficInfluenceDataStore::put(const std::string& influence_id, const nlohmann::json& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto existing = txn.exec("SELECT 1 FROM udr_traffic_influence_data WHERE "
                                   "influence_id = $1",
                                   pqxx::params{influence_id});
    const bool is_new = existing.empty();
    txn.exec("INSERT INTO udr_traffic_influence_data (influence_id, data) VALUES ($1, $2::jsonb) "
             "ON CONFLICT (influence_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{influence_id, data.dump()});
    txn.commit();
    return is_new;
}

std::optional<nlohmann::json>
TrafficInfluenceDataStore::merge_patch(const std::string& influence_id,
                                       const nlohmann::json& patch) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_traffic_influence_data WHERE "
                                 "influence_id = $1",
                                 pqxx::params{influence_id});
    // Unlike udr_am_policy_data's own upsert-capable patch, the real spec here defines PATCH only
    // on an existing resource (404 is a documented response), so a missing row is NOT created.
    if (result.empty()) {
        return std::nullopt;
    }
    auto doc = nlohmann::json::parse(result.front()["data"].as<std::string>());
    doc.merge_patch(patch);
    txn.exec("UPDATE udr_traffic_influence_data SET data = $2::jsonb WHERE influence_id = $1",
             pqxx::params{influence_id, doc.dump()});
    txn.commit();
    return std::make_optional(doc);
}

bool TrafficInfluenceDataStore::remove(const std::string& influence_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("DELETE FROM udr_traffic_influence_data WHERE influence_id = $1",
                                 pqxx::params{influence_id});
    txn.commit();
    return result.affected_rows() > 0;
}

TrafficInfluenceSubStore::TrafficInfluenceSubStore(const std::string& conninfo) : conn_(conninfo) {}

std::vector<nlohmann::json> TrafficInfluenceSubStore::list() {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_traffic_influence_sub ORDER BY "
                                 "subscription_id");
    std::vector<nlohmann::json> out;
    out.reserve(result.size());
    for (const auto& row : result) {
        out.push_back(nlohmann::json::parse(row["data"].as<std::string>()));
    }
    return out;
}

std::optional<nlohmann::json> TrafficInfluenceSubStore::get(const std::string& subscription_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_traffic_influence_sub WHERE "
                                 "subscription_id = $1",
                                 pqxx::params{subscription_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

bool TrafficInfluenceSubStore::put(const std::string& subscription_id, const nlohmann::json& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto existing = txn.exec("SELECT 1 FROM udr_traffic_influence_sub WHERE "
                                   "subscription_id = $1",
                                   pqxx::params{subscription_id});
    const bool is_new = existing.empty();
    txn.exec("INSERT INTO udr_traffic_influence_sub (subscription_id, data) VALUES "
             "($1, $2::jsonb) ON CONFLICT (subscription_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{subscription_id, data.dump()});
    txn.commit();
    return is_new;
}

bool TrafficInfluenceSubStore::remove(const std::string& subscription_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("DELETE FROM udr_traffic_influence_sub WHERE "
                                 "subscription_id = $1",
                                 pqxx::params{subscription_id});
    txn.commit();
    return result.affected_rows() > 0;
}

// --- ADR-0254: generic single-key JSON document store (renamed by ADR-0255 when
// exposure-data started sharing it; behaviour unchanged) ---
// table_/id_column_ are NOT request-derived -- they are fixed literals chosen at construction in
// main.cpp, so interpolating them into the SQL below cannot be influenced by a peer. Values are
// still bound as parameters, never interpolated.
KeyedJsonDocStore::KeyedJsonDocStore(const std::string& conninfo,
                                     std::string table,
                                     std::string id_column)
    : conn_(conninfo), table_(std::move(table)), id_column_(std::move(id_column)) {}

std::vector<nlohmann::json> KeyedJsonDocStore::list() {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM " + table_ + " ORDER BY " + id_column_);
    std::vector<nlohmann::json> out;
    out.reserve(result.size());
    for (const auto& row : result) {
        out.push_back(nlohmann::json::parse(row["data"].as<std::string>()));
    }
    return out;
}

std::optional<nlohmann::json> KeyedJsonDocStore::get(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM " + table_ + " WHERE " + id_column_ + " = $1", pqxx::params{id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

bool KeyedJsonDocStore::put(const std::string& id, const nlohmann::json& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto existing =
        txn.exec("SELECT 1 FROM " + table_ + " WHERE " + id_column_ + " = $1", pqxx::params{id});
    const bool is_new = existing.empty();
    txn.exec("INSERT INTO " + table_ + " (" + id_column_ +
                 ", data) VALUES ($1, $2::jsonb) ON CONFLICT (" + id_column_ +
                 ") DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{id, data.dump()});
    txn.commit();
    return is_new;
}

std::optional<nlohmann::json> KeyedJsonDocStore::merge_patch(const std::string& id,
                                                             const nlohmann::json& patch) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM " + table_ + " WHERE " + id_column_ + " = $1", pqxx::params{id});
    if (result.empty()) {
        return std::nullopt;
    }
    auto doc = nlohmann::json::parse(result.front()["data"].as<std::string>());
    doc.merge_patch(patch);
    txn.exec("UPDATE " + table_ + " SET data = $2::jsonb WHERE " + id_column_ + " = $1",
             pqxx::params{id, doc.dump()});
    txn.commit();
    return std::make_optional(doc);
}

bool KeyedJsonDocStore::remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("DELETE FROM " + table_ + " WHERE " + id_column_ + " = $1", pqxx::params{id});
    txn.commit();
    return result.affected_rows() > 0;
}

// --- ADR-0255: exposure-data session-management-data, keyed by (ueId, pduSessionId) ---
// pduSessionId is stored as TEXT rather than an integer: it arrives as a path segment, and the
// real PduSessionId schema is an integer, but keeping the raw segment avoids inventing a
// normalisation the spec does not define (e.g. whether "07" and "7" are the same resource).
// Disclosed rather than silently decided.
ExposureSessionManagementDataStore::ExposureSessionManagementDataStore(const std::string& conninfo)
    : conn_(conninfo) {}

std::optional<nlohmann::json>
ExposureSessionManagementDataStore::get(const std::string& ue_id,
                                        const std::string& pdu_session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_exposure_session_management_data "
                                 "WHERE ue_id = $1 AND pdu_session_id = $2",
                                 pqxx::params{ue_id, pdu_session_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

bool ExposureSessionManagementDataStore::put(const std::string& ue_id,
                                             const std::string& pdu_session_id,
                                             const nlohmann::json& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto existing = txn.exec("SELECT 1 FROM udr_exposure_session_management_data "
                                   "WHERE ue_id = $1 AND pdu_session_id = $2",
                                   pqxx::params{ue_id, pdu_session_id});
    const bool is_new = existing.empty();
    txn.exec("INSERT INTO udr_exposure_session_management_data (ue_id, pdu_session_id, data) "
             "VALUES ($1, $2, $3::jsonb) ON CONFLICT (ue_id, pdu_session_id) "
             "DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{ue_id, pdu_session_id, data.dump()});
    txn.commit();
    return is_new;
}

bool ExposureSessionManagementDataStore::remove(const std::string& ue_id,
                                                const std::string& pdu_session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("DELETE FROM udr_exposure_session_management_data "
                                 "WHERE ue_id = $1 AND pdu_session_id = $2",
                                 pqxx::params{ue_id, pdu_session_id});
    txn.commit();
    return result.affected_rows() > 0;
}

// --- ADR-0256: AIoT device profile data (TS29506_Aiot_Data.yaml paths, TS29369_Nadm_DM.yaml
// schemas). GET + RFC 6902 PATCH only -- the spec defines no create/replace/delete, so seed() is
// the only write path this build has. ---
AiotDeviceProfileDataStore::AiotDeviceProfileDataStore(const std::string& conninfo)
    : conn_(conninfo) {}

void AiotDeviceProfileDataStore::seed(const std::string& aiot_dev_perm_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_aiot_device_profile_data (aiot_dev_perm_id, data) "
             "VALUES ($1, $2::jsonb) "
             "ON CONFLICT (aiot_dev_perm_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{aiot_dev_perm_id, data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> AiotDeviceProfileDataStore::get(const std::string& aiot_dev_perm_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_aiot_device_profile_data WHERE aiot_dev_perm_id = $1",
                 pqxx::params{aiot_dev_perm_id});
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

std::vector<nlohmann::json>
AiotDeviceProfileDataStore::get_many(const std::vector<std::string>& aiot_dev_perm_ids) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    std::vector<nlohmann::json> out;
    out.reserve(aiot_dev_perm_ids.size());
    // Queried one id at a time, deliberately: it preserves the caller's requested order (which a
    // single WHERE ... IN (...) would not) and keeps every id a bound parameter.
    for (const auto& id : aiot_dev_perm_ids) {
        const auto result =
            txn.exec("SELECT data FROM udr_aiot_device_profile_data WHERE aiot_dev_perm_id = $1",
                     pqxx::params{id});
        if (!result.empty()) {
            out.push_back(nlohmann::json::parse(result.front()["data"].as<std::string>()));
        }
    }
    return out;
}

std::optional<nlohmann::json> AiotDeviceProfileDataStore::patch(const std::string& aiot_dev_perm_id,
                                                                const nlohmann::json& patch_ops) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result =
        txn.exec("SELECT data FROM udr_aiot_device_profile_data WHERE aiot_dev_perm_id = $1",
                 pqxx::params{aiot_dev_perm_id});
    if (result.empty()) {
        return std::nullopt;
    }
    auto doc = nlohmann::json::parse(result.front()["data"].as<std::string>());
    doc = doc.patch(patch_ops); // may throw nlohmann::json::exception -- caller catches
    txn.exec("UPDATE udr_aiot_device_profile_data SET data = $2::jsonb "
             "WHERE aiot_dev_perm_id = $1",
             pqxx::params{aiot_dev_perm_id, doc.dump()});
    txn.commit();
    return std::make_optional(doc);
}

AiotAfAuthorizationDataStore::AiotAfAuthorizationDataStore(const std::string& conninfo)
    : conn_(conninfo) {}

void AiotAfAuthorizationDataStore::seed(nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_aiot_af_authorization_data (id, data) VALUES (1, $1::jsonb) "
             "ON CONFLICT (id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{data.dump()});
    txn.commit();
}

std::optional<nlohmann::json> AiotAfAuthorizationDataStore::get() {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT data FROM udr_aiot_af_authorization_data WHERE id = 1");
    if (result.empty()) {
        return std::nullopt;
    }
    return std::make_optional(nlohmann::json::parse(result.front()["data"].as<std::string>()));
}

// --- ADR-0256: data restoration subscriptions. Opaque by necessity: the real request body schema
// in TS29504_Nudr_DR.yaml is `{}`. ---
DataRestorationSubscriptionStore::DataRestorationSubscriptionStore(const std::string& conninfo)
    : conn_(conninfo) {}

void DataRestorationSubscriptionStore::add(const std::string& subscription_id,
                                           const nlohmann::json& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    txn.exec("INSERT INTO udr_data_restoration_subscriptions (subscription_id, data) "
             "VALUES ($1, $2::jsonb) "
             "ON CONFLICT (subscription_id) DO UPDATE SET data = EXCLUDED.data",
             pqxx::params{subscription_id, data.dump()});
    txn.commit();
}

} // namespace udr
