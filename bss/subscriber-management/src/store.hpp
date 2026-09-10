#pragma once

#include <nlohmann/json.hpp>

#include <mutex>
#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <vector>

#include "bss_sid/party.hpp"

// Private to bss/subscriber-management. Real PostgreSQL persistence (libpqxx), same "one shared
// connection, one mutex" discipline this project already applied to bss/product-catalog (ADR-0054)
// and bss/balance-management -- see schema.sql's own header for the full scoping disclosure
// (schema + store library only this turn, no new HTTP service yet -- CHARGING_PROMPT.md's P4.7
// owns that).

namespace subscriber_management {

class PartyIndividualStore {
public:
    // resource_url is this real TMF632 collection's own URL (e.g.
    // "https://host:port/tmf-api/party/v4/individual"), used to build each stored resource's real
    // `href`.
    PartyIndividualStore(std::string resource_url, const std::string& conninfo);

    std::string create(bss_sid::Individual individual);
    std::optional<bss_sid::Individual> get(const std::string& id);
    std::vector<bss_sid::Individual> list();

private:
    std::string resource_url_;
    std::mutex mutex_;
    pqxx::connection conn_;
};

class PartyOrganizationStore {
public:
    PartyOrganizationStore(std::string resource_url, const std::string& conninfo);

    std::string create(bss_sid::Organization organization);
    std::optional<bss_sid::Organization> get(const std::string& id);
    std::vector<bss_sid::Organization> list();

private:
    std::string resource_url_;
    std::mutex mutex_;
    pqxx::connection conn_;
};

// E10 Account -- project-internal (docs/DATA_MODEL.md's own disclosure, schema.sql's header).
struct Account {
    std::optional<std::string> id;
    std::string accountKind; // "CONSUMER" | "ENTERPRISE"
    std::optional<std::string> parentAccountId;
    std::optional<std::string> organizationId; // ENTERPRISE only, FK -> party_organization
    std::optional<std::string> billingMode;    // "INDIVIDUAL" | "SPLIT"
    std::optional<std::string> costCenter;
    std::optional<std::string> contractSlaId;
    std::optional<std::string> provisioningMode; // "INDIVIDUAL" | "BULK"
};

void to_json(nlohmann::json& j, const Account& v);
void from_json(const nlohmann::json& j, Account& v);

class AccountStore {
public:
    AccountStore(std::string resource_url, const std::string& conninfo);

    std::string create(Account account);
    std::optional<Account> get(const std::string& id);
    std::vector<Account> list();

private:
    std::string resource_url_;
    std::mutex mutex_;
    pqxx::connection conn_;
};

// E1 Subscriber -- project-internal linking table between a real SUPI and its real TMF632
// Individual record (schema.sql's own header).
struct Subscriber {
    std::optional<std::string> id;
    std::string supi;
    std::optional<std::string> individualId;
    std::optional<std::string> msisdn;
    std::optional<std::string> accountId;
    std::optional<std::string> chargingMode; // "PREPAID" | "POSTPAID"
    std::optional<int> billCycleDay;         // 1-28
    nlohmann::json servicePreferences = nlohmann::json::object();
};

void to_json(nlohmann::json& j, const Subscriber& v);
void from_json(const nlohmann::json& j, Subscriber& v);

class SubscriberStore {
public:
    SubscriberStore(std::string resource_url, const std::string& conninfo);

    std::string create(Subscriber subscriber);
    std::optional<Subscriber> get(const std::string& id);
    std::optional<Subscriber> get_by_supi(const std::string& supi);
    std::vector<Subscriber> list();

    // ADR-0335: record a lifecycle transition, and the status change with it, in ONE transaction.
    //
    // Churn is currently unobservable here -- `subscriber` records created_at/updated_at and
    // nothing about a subscriber leaving. This is the collection prerequisite for any future
    // churn model: without a labelled "who left, when, and from what state" history there is
    // nothing to train against, and that history can only be gathered forward in time.
    //
    // Atomic on purpose: a status changed without its event, or an event without the status
    // change, produces a history that disagrees with the record it describes -- and a churn model
    // trained on it would learn from the disagreement.
    //
    // Returns false if there is no such subscriber. `from_status` is read inside the transaction
    // rather than supplied by the caller, so it cannot be misreported.
    bool record_lifecycle_transition(const std::string& subscriber_id,
                                     const std::string& to_status,
                                     const std::string& reason);

private:
    std::string resource_url_;
    std::mutex mutex_;
    pqxx::connection conn_;
};

} // namespace subscriber_management
