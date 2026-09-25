#include "provisioning_store.hpp"

#include <pqxx/pqxx>

#include <regex>

namespace provisioning {

using nlohmann::json;

namespace {
std::string jval(const json& j, const char* key, const std::string& dflt = "") {
    return (j.contains(key) && j[key].is_string()) ? j[key].get<std::string>() : dflt;
}
// digits of a SUPI ("imsi-99970...") for deterministic, idempotent entity ids.
std::string key_of(const std::string& supi) {
    std::string k;
    for (char c : supi) {
        if (c >= '0' && c <= '9') k += c;
    }
    return k.empty() ? supi : k;
}
} // namespace

ProvisioningStore::ProvisioningStore(const std::string& orchestration_conninfo,
                                     const std::string& charging_conninfo,
                                     std::size_t pool_size,
                                     UdrAdapter& udr)
    : orch_(std::make_unique<nf_config::PgPool>(orchestration_conninfo, pool_size)),
      charging_(std::make_unique<nf_config::PgPool>(charging_conninfo, pool_size)), udr_(udr) {}

void ProvisioningStore::provision_bss(const json& request, const ProvisionResult& r,
                                      const std::string& k) {
    const std::string supi = r.supi;
    const std::string msisdn = r.msisdn;
    const std::string segment = jval(request, "segment", "CONSUMER");
    const std::string charging_mode = jval(request, "chargingMode", "PREPAID");
    const std::string offering = jval(request, "productOfferingId");
    const json indiv = request.value("individual", json::object());
    // ---- BSS provisioning: the full SID chain, ONE atomic charging-DB transaction ----
    auto clease = charging_->acquire();
    pqxx::work c(clease.conn());
    const std::string ind_id = "ind-" + k;
    c.exec_params("INSERT INTO party.individual(id,given_name,family_name,full_name,status) "
                  "VALUES($1,$2,$3,$4,'validated') ON CONFLICT (id) DO NOTHING",
                  ind_id, jval(indiv, "givenName"), jval(indiv, "familyName"),
                  jval(indiv, "givenName") + " " + jval(indiv, "familyName"));
    if (indiv.contains("email") || indiv.contains("phone")) {
        c.exec_params("INSERT INTO party.contact_medium(id,individual_id,medium_type,email_address,phone_number) "
                      "VALUES($1,$2,'email',$3,$4) ON CONFLICT (id) DO NOTHING",
                      "cm-" + k, ind_id, jval(indiv, "email"), jval(indiv, "phone"));
    }
    // SUPI as the party's identity (docs/CHARGING_MAPPING.md resolution)
    c.exec_params("INSERT INTO party.individual_identification(id,individual_id,identification_type,identification_id) "
                  "VALUES($1,$2,'SUPI',$3) ON CONFLICT (id) DO NOTHING", "iid-" + k, ind_id, supi);
    c.exec_params("INSERT INTO subscriber_mgmt.account(id,account_kind,individual_id,status) "
                  "VALUES($1,$2,$3,'active') ON CONFLICT (id) DO NOTHING", r.customer_account_id, segment, ind_id);
    c.exec_params("INSERT INTO subscriber_mgmt.subscriber(id,account_id,individual_id,charging_mode,status) "
                  "VALUES($1,$2,$3,$4,'active') ON CONFLICT (id) DO NOTHING",
                  r.subscriber_id, r.customer_account_id, ind_id, charging_mode);
    c.exec_params("INSERT INTO subscriber_mgmt.resource(id,subscriber_id,resource_type,resource_value) "
                  "VALUES($1,$2,'SUPI',$3) ON CONFLICT (id) DO NOTHING", "res-s-" + k, r.subscriber_id, supi);
    if (!msisdn.empty()) {
        c.exec_params("INSERT INTO subscriber_mgmt.resource(id,subscriber_id,resource_type,resource_value) "
                      "VALUES($1,$2,'MSISDN',$3) ON CONFLICT (id) DO NOTHING", "res-m-" + k, r.subscriber_id, msisdn);
    }
    if (!offering.empty()) {
        c.exec_params("INSERT INTO subscriber_mgmt.product_subscription(id,subscriber_id,account_id,product_offering_id,status) "
                      "VALUES($1,$2,$3,$4,'active') ON CONFLICT (id) DO NOTHING",
                      "ps-" + k, r.subscriber_id, r.customer_account_id, offering);
    }
    const json bal = request.value("initialBalance", json::object());
    c.exec_params("INSERT INTO balance_mgmt.bucket(id,party_account_id,usage_type,remaining_value_unit,remaining_value_amount,status) "
                  "VALUES($1,$2,$3,$4,$5,'active') ON CONFLICT (id) DO NOTHING",
                  "bkt-" + k, r.customer_account_id, bal.value("usageType", "monetary"),
                  bal.value("unit", "USD"), bal.value("amount", 0.0));
    c.exec_params("INSERT INTO balance_mgmt.bucket_logical_resource(bucket_id,resource_id) "
                  "VALUES($1,$2) ON CONFLICT DO NOTHING", "bkt-" + k, supi);
    c.commit();
}

void ProvisioningStore::record_task(const std::string& task_id, bool ok, const json& response,
                                    const std::string& error) {
    auto lease = orch_->acquire();
    pqxx::work o(lease.conn());
    o.exec_params("UPDATE orchestration.provisioning_task SET status=$2, attempts=attempts+1, "
                  "response=$3::jsonb, last_error=NULLIF($4,''), updated_at=now() WHERE id=$1",
                  task_id, ok ? "done" : "failed", response.dump(), error);
    o.commit();
}

ProvisionResult ProvisioningStore::create_customer_order(const json& request) {
    ProvisionResult r;
    const std::string idem = jval(request, "idempotencyKey");
    const std::string supi = jval(request, "supi");
    const std::string msisdn = jval(request, "msisdn");
    const std::string segment = jval(request, "segment", "CONSUMER");
    const std::string charging_mode = jval(request, "chargingMode", "PREPAID");
    const std::string offering = jval(request, "productOfferingId");
    const std::string k = key_of(supi);
    r.order_id = "ord-" + (idem.empty() ? k : idem);
    r.supi = supi;
    r.msisdn = msisdn;
    r.customer_account_id = "acc-" + k;
    r.subscriber_id = "sub-" + k;

    // ---- Validate before writing anything. Error text names fields, never echoes values. ----
    static const std::regex kImsiSupi("^imsi-[0-9]{5,15}$");
    static const std::regex kMsisdn("^[0-9]{5,15}$");
    static const std::regex kHex128("^[A-Fa-f0-9]{32}$");
    static const std::regex kSqn("^[A-Fa-f0-9]{12}$");
    const json sim = request.value("sim", json::object());
    SubscriberSpec spec{supi, msisdn, offering,
                        SimCredentials{jval(sim, "k"), jval(sim, "opc"), jval(sim, "sqn")}};
    std::string invalid;
    if (!std::regex_match(supi, kImsiSupi)) {
        invalid = "supi must be imsi-<5..15 digits>";
    } else if (!msisdn.empty() && !std::regex_match(msisdn, kMsisdn)) {
        invalid = "msisdn must be 5..15 digits";
    } else if (!std::regex_match(spec.sim.k, kHex128) || !std::regex_match(spec.sim.opc, kHex128)) {
        invalid = "sim.k and sim.opc are required, 32 hex digits each";
    } else if (!spec.sim.sqn.empty() && !std::regex_match(spec.sim.sqn, kSqn)) {
        invalid = "sim.sqn must be 12 hex digits";
    }
    if (!invalid.empty()) {
        r.status = "rejected";
        r.error = invalid;
        return r;
    }

    // ---- Record the order + one task per node. Idempotent: a resend finds them already there. ----
    const std::string bss_task = "task-bss-" + r.order_id;
    const std::string udr_task = "task-udr-" + r.order_id;
    {
        auto lease = orch_->acquire();
        pqxx::work o(lease.conn());
        o.exec_params("INSERT INTO orchestration.product_order(id,state,customer_id,account_id,channel) "
                      "VALUES($1,'inProgress',$2,$3,'gui') ON CONFLICT (id) DO UPDATE "
                      "SET state='inProgress', updated_at=now()",
                      r.order_id, r.customer_account_id, r.customer_account_id);
        o.exec_params("INSERT INTO orchestration.product_order_item(id,product_order_id,action,product_offering_id,segment,charging_mode) "
                      "VALUES($1,$2,'add',$3,$4,$5) ON CONFLICT (id) DO NOTHING",
                      "oi-" + k, r.order_id, offering, segment, charging_mode);
        o.exec_params("INSERT INTO orchestration.service_order(id,product_order_id,order_item_id,state,service_spec,subscriber_id) "
                      "VALUES($1,$2,$3,'inProgress','mobile-line',$4) ON CONFLICT (id) DO NOTHING",
                      "so-" + k, r.order_id, "oi-" + k, r.subscriber_id);
        o.exec_params("INSERT INTO orchestration.resource_order(id,service_order_id,state,resource_type,resource_value) "
                      "VALUES($1,$2,'inProgress','SUPI',$3) ON CONFLICT (id) DO NOTHING",
                      "ro-s-" + k, "so-" + k, supi);
        // request_payload holds only non-secret routing facts; the SIM keys are never stored.
        const json payload{{"supi", supi}, {"offeringId", offering}};
        o.exec_params("INSERT INTO orchestration.provisioning_task(id,product_order_id,node,op,status,idempotency_key,request_payload) "
                      "VALUES($1,$2,'bss-charging','provision','pending',$3,$4::jsonb) ON CONFLICT (id) DO NOTHING",
                      bss_task, r.order_id, "bss-" + r.order_id, payload.dump());
        o.exec_params("INSERT INTO orchestration.provisioning_task(id,product_order_id,node,op,status,idempotency_key,request_payload) "
                      "VALUES($1,$2,'udr-subscription','provision','pending',$3,$4::jsonb) ON CONFLICT (id) DO NOTHING",
                      udr_task, r.order_id, "udr-" + r.order_id, payload.dump());
        o.commit();
    }

    const auto task_done = [this](const std::string& task_id) {
        auto lease = orch_->acquire();
        pqxx::nontransaction n(lease.conn());
        auto res = n.exec_params("SELECT status FROM orchestration.provisioning_task WHERE id=$1", task_id);
        return !res.empty() && res[0][0].as<std::string>() == "done";
    };

    // ---- Task 1: BSS SID chain (one atomic charging-DB transaction). ----
    if (!task_done(bss_task)) {
        try {
            provision_bss(request, r, k);
            record_task(bss_task, true, json::object(), "");
        } catch (const std::exception& e) {
            record_task(bss_task, false, json::object(), e.what());
        }
    }
    // ---- Task 2: UDR subscription/auth/policy (only after BSS succeeded). ----
    if (task_done(bss_task) && !task_done(udr_task)) {
        auto outcome = udr_.provision(spec);
        if (outcome.has_value()) {
            record_task(udr_task, true,
                        json{{"httpStatus", outcome->http_status}, {"networkProfile", outcome->profile_key}},
                        "");
        } else {
            record_task(udr_task, false, json::object(), outcome.error());
        }
    }

    // ---- Order state is derived from the tasks. ----
    const bool all_done = task_done(bss_task) && task_done(udr_task);
    const std::string state = all_done ? "completed" : "failed";
    {
        auto lease = orch_->acquire();
        pqxx::work o(lease.conn());
        o.exec_params("UPDATE orchestration.product_order SET state=$2, updated_at=now() WHERE id=$1",
                      r.order_id, state);
        o.exec_params("UPDATE orchestration.service_order SET state=$2 WHERE product_order_id=$1",
                      r.order_id, state);
        o.exec_params("UPDATE orchestration.resource_order SET state=$2 WHERE service_order_id=$1",
                      "so-" + k, state);
        o.exec_params("INSERT INTO orchestration.order_state_history(product_order_id,to_state,detail) "
                      "VALUES($1,$2,$3)", r.order_id, state,
                      all_done ? "all provisioning tasks done" : "one or more provisioning tasks not done");
        o.commit();
    }
    r.status = state;
    if (auto order = get_order(r.order_id)) {
        r.tasks = (*order)["provisioningTasks"];
    }
    if (!all_done) {
        r.error = "provisioning incomplete; resend the same order to resume unfinished tasks";
    }
    return r;
}

std::optional<json> ProvisioningStore::get_order(const std::string& order_id) {
    auto lease = orch_->acquire();
    pqxx::nontransaction n(lease.conn());
    auto ord = n.exec_params("SELECT state,customer_id,account_id,order_date FROM orchestration.product_order WHERE id=$1", order_id);
    if (ord.empty()) return std::nullopt;
    json out{{"id", order_id}, {"state", ord[0][0].as<std::string>()},
             {"customerId", ord[0][1].as<std::string>()}, {"accountId", ord[0][2].as<std::string>()}};
    json tasks = json::array();
    for (auto row : n.exec_params("SELECT node,status,op,attempts,last_error FROM orchestration.provisioning_task "
                                  "WHERE product_order_id=$1 ORDER BY node", order_id)) {
        json t{{"node", row[0].as<std::string>()}, {"status", row[1].as<std::string>()},
               {"op", row[2].as<std::string>()}, {"attempts", row[3].as<int>()}};
        if (!row[4].is_null()) t["lastError"] = row[4].as<std::string>();
        tasks.push_back(t);
    }
    out["provisioningTasks"] = tasks;
    return out;
}

} // namespace provisioning
