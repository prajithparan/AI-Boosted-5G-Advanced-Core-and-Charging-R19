#pragma once

// Provisioning module store (project_customer_onboarding_orchestration). The workflow runner's
// data layer: it owns the orchestration DB (TMF622/641/652 orders + provisioning_task saga state)
// and writes the charging DB SID chain (Party -> Account -> Service(subscriber) -> Resource ->
// ProductSubscription -> Balance bucket) transactionally. Network-side provisioning (UDR
// subscription/auth/policy) is a separate task executed by a NodeAdapter (added incrementally) --
// this store does the BSS half fully and records the UDR task as pending, never faking it.
//
// Fail-fast (architecture rule): both DB pools connect at construction or the process exits.

#include <nf_config/pg_pool.hpp>

#include <nlohmann/json.hpp>

#include <memory>
#include <optional>
#include <string>

namespace provisioning {

// The result of provisioning one customer order: the created SID entity ids + the order id.
struct ProvisionResult {
    std::string order_id;
    std::string customer_account_id;
    std::string subscriber_id;
    std::string supi;
    std::string msisdn;
    std::string status; // completed | failed
    std::optional<std::string> error;
};

class ProvisioningStore {
public:
    ProvisioningStore(const std::string& orchestration_conninfo,
                      const std::string& charging_conninfo,
                      std::size_t pool_size);

    // Create + run a customer order. Idempotent on the caller-supplied idempotency key: a repeat
    // returns the same order without double-provisioning. Request shape (our own API, not a
    // generated DTO):
    //   { "idempotencyKey", "segment"(CONSUMER|ENTERPRISE), "chargingMode"(PREPAID|POSTPAID),
    //     "productOfferingId", "individual":{givenName,familyName,email,phone},
    //     "supi", "msisdn", "initialBalance":{unit,amount} }
    ProvisionResult create_customer_order(const nlohmann::json& request);

    // Order + its provisioning tasks (status), for GET.
    std::optional<nlohmann::json> get_order(const std::string& order_id);

private:
    std::unique_ptr<nf_config::PgPool> orch_;     // orchestration DB
    std::unique_ptr<nf_config::PgPool> charging_; // charging DB (SID entities)
};

} // namespace provisioning
