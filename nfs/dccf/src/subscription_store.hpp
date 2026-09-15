#pragma once

#include <nlohmann/json.hpp>

#include <memory>
#include <optional>
#include <string>
#include <sw/redis++/redis++.h>
#include <utility>
#include <vector>

#include "TS26510_CommonData_grp.hpp"
#include "TS29574_Ndccf_ContextManagement.hpp"

// DCCF state in Valkey (ADR-0366; the ADR-0359 rule: no in-process state).
//
//   dccf:sub:<subscriptionId>      ConsumerSubscription -- what one consumer asked for, plus the
//                                  fingerprint of the source subscription that serves it
//   dccf:src:<fingerprint>         SourceCollection -- ONE subscription at the Data Source (or
//                                  NWDAF), the MFAF configuration that fans it out, and the set of
//                                  consumer subscriptionIds it serves. This is TS 23.288 5A.2's
//                                  "determine whether the data are already being collected": two
//                                  consumers asking for the same data share one source subscription
//   dccf:profile:<profileId>       NdccfDataCollectionProfile (Ndccf_ContextManagement)
//   dccf:next_id                   INCR counter behind every id
//
// The fingerprint is the source-facing subscription with its notification target and
// correlation fields removed, serialised canonically (sorted keys) and hashed -- the DCCF's own
// definition of "the same data", since TS 23.288 leaves the matching logic unspecified (NOTE in
// 5A.3.2: "The internal logic of DCCF ... is not specified").

namespace dccf {

enum class Kind { Data, Analytics };

struct ConsumerSubscription {
    Kind kind = Kind::Data;
    nlohmann::json request; // the NdccfDataSubscription / NdccfAnalyticsSubscription as received
    std::string fingerprint;
};
void to_json(nlohmann::json& j, const ConsumerSubscription& v);
void from_json(const nlohmann::json& j, ConsumerSubscription& v);

struct SourceCollection {
    std::string source;              // "AMF", "NRF", "NSACF", "NWDAF"
    std::string source_resource_uri; // the subscription resource at the source, for DELETE
    std::string mfaf_trans_ref_id;   // the MFAF configuration fanning this collection out
    std::string mfaf_notif_uri;
    std::string mfaf_corre_id;
    std::vector<std::string> consumers; // dccf subscriptionIds
};
void to_json(nlohmann::json& j, const SourceCollection& v);
void from_json(const nlohmann::json& j, SourceCollection& v);

class SubscriptionStore {
public:
    explicit SubscriptionStore(std::shared_ptr<sw::redis::Redis> redis)
        : redis_(std::move(redis)) {}

    std::string next_id(const char* prefix);

    std::string create_subscription(const ConsumerSubscription& sub);
    std::optional<ConsumerSubscription> get_subscription(const std::string& id);
    bool replace_subscription(const std::string& id, const ConsumerSubscription& sub);
    bool remove_subscription(const std::string& id);

    std::optional<SourceCollection> get_source(const std::string& fingerprint);
    void put_source(const std::string& fingerprint, const SourceCollection& src);
    void remove_source(const std::string& fingerprint);

    std::string create_profile(const sbi_gen::NdccfDataCollectionProfile& p);
    bool replace_profile(const std::string& id, const sbi_gen::NdccfDataCollectionProfile& p);
    bool remove_profile(const std::string& id);

private:
    std::shared_ptr<sw::redis::Redis> redis_;
};

// The dedup key: canonical JSON of `source_sub` minus the notification fields, prefixed by the
// source kind. FNV-1a over the canonical text -- a stable, dependency-free hash.
std::string fingerprint(const std::string& source,
                        nlohmann::json source_sub,
                        const std::vector<std::string>& notification_fields);

} // namespace dccf
