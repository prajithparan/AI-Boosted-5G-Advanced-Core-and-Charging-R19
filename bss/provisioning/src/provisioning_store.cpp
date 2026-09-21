#include "provisioning_store.hpp"

#include <pqxx/pqxx>

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
                                     std::size_t pool_size)
    : orch_(std::make_unique<nf_config::PgPool>(orchestration_conninfo, pool_size)),
      charging_(std::make_unique<nf_config::PgPool>(charging_conninfo, pool_size)) {}

ProvisionResult ProvisioningStore::create_customer_order(const json& request) {
    ProvisionResult r;
    const std::string idem = jval(request, "idempotencyKey");
    const std::string supi = jval(request, "supi");
    const std::string msisdn = jval(request, "msisdn");
    const std::string segment = jval(request, "segment", "CONSUMER");
    const std::string charging_mode = jval(request, "chargingMode", "PREPAID");
    const std::string offering = jval(request, "productOfferingId");
    const json indiv = request.value("individual", json::object());
    const std::string k = key_of(supi);
    r.order_id = "ord-" + (idem.empty() ? k : idem);
    r.supi = supi;
    r.msisdn = msisdn;
    r.customer_account_id = "acc-" + k;
    r.subscriber_id = "sub-" + k;

    // Idempotency: if this order already exists, return it without re-provisioning.
    {
        auto lease = orch_->acquire();
        pqxx::nontransaction n(lease.conn());
        auto res = n.exec_params("SELECT state FROM orchestration.product_order WHERE id=$1", r.order_id);
        if (!res.empty()) {
            r.status = res[0][0].as<std::string>() == "completed" ? "completed" : "failed";
            return r;
        }
    }

    try {
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

        // ---- Orchestration: record the order + saga tasks (BSS done, UDR pending) ----
        auto olease = orch_->acquire();
        pqxx::work o(olease.conn());
        o.exec_params("INSERT INTO orchestration.product_order(id,state,customer_id,account_id,channel) "
                      "VALUES($1,'completed',$2,$3,'gui')", r.order_id, r.customer_account_id, r.customer_account_id);
        o.exec_params("INSERT INTO orchestration.product_order_item(id,product_order_id,action,product_offering_id,segment,charging_mode) "
                      "VALUES($1,$2,'add',$3,$4,$5)", "oi-" + k, r.order_id, offering, segment, charging_mode);
        o.exec_params("INSERT INTO orchestration.service_order(id,product_order_id,order_item_id,state,service_spec,subscriber_id) "
                      "VALUES($1,$2,$3,'completed','mobile-line',$4)", "so-" + k, r.order_id, "oi-" + k, r.subscriber_id);
        o.exec_params("INSERT INTO orchestration.resource_order(id,service_order_id,state,resource_type,resource_value) "
                      "VALUES($1,$2,'completed','SUPI',$3)", "ro-s-" + k, "so-" + k, supi);
        o.exec_params("INSERT INTO orchestration.provisioning_task(id,product_order_id,node,op,status,idempotency_key) "
                      "VALUES($1,$2,'bss-charging','provision','done',$3)", "task-bss-" + k, r.order_id, "bss-" + r.order_id);
        o.exec_params("INSERT INTO orchestration.provisioning_task(id,product_order_id,node,op,status,idempotency_key) "
                      "VALUES($1,$2,'udr-subscription','provision','pending',$3)", "task-udr-" + k, r.order_id, "udr-" + r.order_id);
        o.exec_params("INSERT INTO orchestration.order_state_history(product_order_id,to_state,detail) "
                      "VALUES($1,'completed','BSS provisioned; UDR pending')", r.order_id);
        o.commit();
        r.status = "completed";
    } catch (const std::exception& e) {
        r.status = "failed";
        r.error = e.what();
        auto olease = orch_->acquire();
        pqxx::work o(olease.conn());
        o.exec_params("INSERT INTO orchestration.product_order(id,state,customer_id,account_id,channel) "
                      "VALUES($1,'failed',$2,$3,'gui') ON CONFLICT (id) DO UPDATE SET state='failed'",
                      r.order_id, r.customer_account_id, r.customer_account_id);
        o.commit();
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
    for (auto row : n.exec_params("SELECT node,status,op FROM orchestration.provisioning_task WHERE product_order_id=$1 ORDER BY node", order_id)) {
        tasks.push_back({{"node", row[0].as<std::string>()}, {"status", row[1].as<std::string>()}, {"op", row[2].as<std::string>()}});
    }
    out["provisioningTasks"] = tasks;
    return out;
}

} // namespace provisioning
