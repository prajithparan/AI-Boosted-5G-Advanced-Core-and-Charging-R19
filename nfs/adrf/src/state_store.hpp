#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <sw/redis++/redis++.h>
#include <vector>

// ADRF control state in Valkey (ADR-0367; the ADR-0359 rule: no in-process state). The records
// themselves are in Doris (record_store.hpp) and the models in PostgreSQL (model_store.hpp); what
// lives here is everything a request on one replica must see from another:
//
//   adrf:storesub:<transRefId>   StorageSubscription -- one Nadrf_DataManagement_
//                                StorageSubscriptionRequest, mapped to the collection serving it
//   adrf:coll:<fingerprint>      Collection -- ONE subscription at the DCCF (or NWDAF) whose
//                                notifications the ADRF stores, and the transRefIds it serves
//                                (4.2.2.3.2 NOTE 2: the same data is not collected twice)
//   adrf:dataset:<dataSetId>     set of transRefIds tagged with that data set (EnhDataMgmt
//                                removal by dataSetId, 4.2.2.4.2)
//   adrf:rsub:<subscriptionId>   RetrievalSubscription; adrf:rsubs the index every replica scans
//                                when a record lands
//   adrf:fetch:<fetchCorrId>     storeTransId behind a fetch instruction (consTrigNotif), TTL
//   adrf:lease:<name>            SET NX PX leases: the lifetime reaper and the retrieval-window
//                                sweeper run on one replica at a time
//   adrf:next_id                 INCR counter behind every id

namespace adrf {

struct StorageSubscription {
    std::string kind; // "analytics" | "data"
    std::string fingerprint;
    nlohmann::json request; // NadrfDataStoreSubscription as received
    std::optional<std::string> data_set_id;
};
void to_json(nlohmann::json& j, const StorageSubscription& v);
void from_json(const nlohmann::json& j, StorageSubscription& v);

struct Collection {
    std::string target;       // "DCCF" | "NWDAF"
    std::string resource_uri; // the subscription resource at the target, for DELETE
    std::string notif_corr_id;
    std::string kind;
    nlohmann::json spec;                       // the anaSub / dataSub as subscribed
    std::optional<nlohmann::json> store_handl; // StorageHandlingInfo applied to stored records
    std::optional<nlohmann::json> data_set_tag;
    std::vector<std::string> trans_ref_ids;
};
void to_json(nlohmann::json& j, const Collection& v);
void from_json(const nlohmann::json& j, Collection& v);

struct RetrievalSubscription {
    nlohmann::json request; // NadrfDataRetrievalSubscription as received
    std::optional<std::string> fingerprint;
    std::optional<std::string> data_set_id;
};
void to_json(nlohmann::json& j, const RetrievalSubscription& v);
void from_json(const nlohmann::json& j, RetrievalSubscription& v);

class StateStore {
public:
    explicit StateStore(std::shared_ptr<sw::redis::Redis> redis) : redis_(std::move(redis)) {}

    std::string next_id(const char* prefix);

    std::string create_storage_subscription(const StorageSubscription& s);
    std::optional<StorageSubscription> get_storage_subscription(const std::string& trans_ref_id);
    void remove_storage_subscription(const std::string& trans_ref_id);
    std::vector<std::string> data_set_members(const std::string& data_set_id);

    std::optional<Collection> get_collection(const std::string& fingerprint);
    void put_collection(const std::string& fingerprint, const Collection& c);
    void remove_collection(const std::string& fingerprint);

    std::string create_retrieval_subscription(const RetrievalSubscription& s);
    std::optional<RetrievalSubscription> get_retrieval_subscription(const std::string& id);
    bool remove_retrieval_subscription(const std::string& id);
    std::vector<std::string> retrieval_subscription_ids();

    void put_fetch(const std::string& fetch_corr_id,
                   const std::string& store_trans_id,
                   std::chrono::seconds ttl);
    std::optional<std::string> take_fetch(const std::string& fetch_corr_id);

    bool acquire_lease(const std::string& name, std::chrono::milliseconds ttl);

private:
    std::shared_ptr<sw::redis::Redis> redis_;
};

} // namespace adrf
