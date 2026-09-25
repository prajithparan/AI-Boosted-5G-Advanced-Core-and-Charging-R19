#pragma once

// Provisioning module store (project_customer_onboarding_orchestration). The workflow runner's
// data layer: it owns the orchestration DB (TMF622/641/652 orders + provisioning_task saga state)
// and writes the charging DB SID chain (Party -> Account -> Service(subscriber) -> Resource ->
// ProductSubscription -> Balance bucket) transactionally, then runs the network-side tasks through
// node adapters (UDR subscription/auth/policy: UdrAdapter, ADR-0382).
//
// Saga (ADR-0382): every order has one provisioning_task per node. The order's state is derived
// from its tasks -- completed only when all are done, failed otherwise -- and resending the same
// order (same idempotencyKey) resumes every task that is not done. The UDR write is a replace, so
// a resumed task is safe. SIM keys travel only in the request: they are never logged, never
// written to request_payload/response/last_error, and never returned by GET.
//
// Fail-fast (architecture rule): both DB pools connect at construction or the process exits.

#include "udr_adapter.hpp"

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
    std::string status; // completed | failed | rejected (invalid request, nothing written)
    std::optional<std::string> error;
    nlohmann::json tasks = nlohmann::json::array(); // [{node,status,attempts,lastError?}]
};

class ProvisioningStore {
public:
    ProvisioningStore(const std::string& orchestration_conninfo,
                      const std::string& charging_conninfo,
                      std::size_t pool_size,
                      UdrAdapter& udr);

    // Create + run a customer order. Idempotent on the caller-supplied idempotency key: a repeat
    // returns the same order without double-provisioning. Request shape (our own API, not a
    // generated DTO):
    //   { "idempotencyKey", "segment"(CONSUMER|ENTERPRISE), "chargingMode"(PREPAID|POSTPAID),
    //     "productOfferingId", "individual":{givenName,familyName,email,phone},
    //     "supi", "msisdn", "initialBalance":{unit,amount},
    //     "sim":{"k","opc","sqn"?} }   -- k/opc 32 hex (SIM vendor input file), sqn 12 hex
    ProvisionResult create_customer_order(const nlohmann::json& request);

    // Order + its provisioning tasks (status), for GET.
    std::optional<nlohmann::json> get_order(const std::string& order_id);

private:
    std::unique_ptr<nf_config::PgPool> orch_;     // orchestration DB
    std::unique_ptr<nf_config::PgPool> charging_; // charging DB (SID entities)
    UdrAdapter& udr_;

    void provision_bss(const nlohmann::json& request, const ProvisionResult& r, const std::string& k);
    // Records a task outcome; attempts is incremented on every run.
    void record_task(const std::string& task_id, bool ok, const nlohmann::json& response,
                     const std::string& error);
};

} // namespace provisioning
