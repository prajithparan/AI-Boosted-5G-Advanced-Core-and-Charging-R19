#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <sw/redis++/redis++.h>
#include <utility>
#include <vector>

// Private to nfs/nwdaf. Phase C (ADR-0368): what the NWDAF has COLLECTED, and who wants it.
//
// The AnLF collects data the way TS 23.288 6.2.6.3.4 describes -- a subscription at the DCCF,
// delivery through the Messaging Framework -- and keeps the collected events in Valkey so every
// replica computes analytics over the same observations and serves Nnwdaf_DataManagement from
// the same store (ADR-0359: no in-process state).
//
//   nwdaf:collected:<source>      ZSET of collected events, score = receipt time (epoch ms),
//                                 member = {"seq", "t", "event"} -- the source NF's own
//                                 notification body (for the NRF: TS 29.510 NotificationData).
//                                 Trimmed to the observation window and a hard cap, both config.
//   nwdaf:coll:<source>           the collection this NWDAF holds at the DCCF -- SET NX, so ONE
//                                 replica opens it and every replica reads what it delivers;
//                                 nwdaf:coll:<source>:alive is the holder's heartbeat (PX)
//   nwdaf:dm:sub:<id>             an Nnwdaf_DataManagement subscription (the request as received,
//                                 plus the source it is served from); nwdaf:dm:subs the index
//   nwdaf:dm:fetch:<fetchCorrId>  a buffered NnwdafDataManagementNotif behind a fetch instruction
//                                 (consTrigNotif), with a TTL; consumed once (GETDEL)
//   nwdaf:next_id                 the INCR counter every NWDAF id comes from (shared with
//                                 subscription_store.cpp)

namespace nwdaf {

struct CollectedEvent {
    std::int64_t seq = 0;
    std::chrono::system_clock::time_point received_at;
    nlohmann::json event; // the source's notification body
};

struct DataManagementSubscription {
    std::string source;     // "nrf" -- what the subscription is served from
    nlohmann::json request; // NnwdafDataManagementSubsc as received
};
void to_json(nlohmann::json& j, const DataManagementSubscription& v);
void from_json(const nlohmann::json& j, DataManagementSubscription& v);

class CollectionStore {
public:
    CollectionStore(std::shared_ptr<sw::redis::Redis> redis,
                    std::chrono::seconds window,
                    std::int64_t max_events)
        : redis_(std::move(redis)), window_(window), max_events_(max_events) {}

    std::string next_id(const char* prefix);

    // Collected events: append (trimming to the window and the cap), and read a time slice.
    CollectedEvent append(const std::string& source, const nlohmann::json& event);
    std::vector<CollectedEvent> events(const std::string& source,
                                       std::chrono::system_clock::time_point from,
                                       std::chrono::system_clock::time_point to);
    std::chrono::seconds window() const { return window_; }

    // The collection at the DCCF: true when this call opened it (nobody held it). The holder
    // keeps nwdaf:coll:<source>:alive refreshed; when that key lapses (the holder died) another
    // replica takes the collection over.
    bool open_collection(const std::string& source, const nlohmann::json& record);
    std::optional<nlohmann::json> get_collection(const std::string& source);
    void close_collection(const std::string& source);
    void touch_holder(const std::string& source, std::chrono::milliseconds ttl);
    bool holder_alive(const std::string& source);

    std::string create_dm_subscription(const DataManagementSubscription& s);
    std::optional<DataManagementSubscription> get_dm_subscription(const std::string& id);
    bool replace_dm_subscription(const std::string& id, const DataManagementSubscription& s);
    bool remove_dm_subscription(const std::string& id);
    std::vector<std::pair<std::string, DataManagementSubscription>> all_dm_subscriptions();

    void put_fetch(const std::string& fetch_corr_id,
                   const nlohmann::json& notif,
                   std::chrono::seconds ttl);
    std::optional<nlohmann::json> take_fetch(const std::string& fetch_corr_id);

private:
    std::shared_ptr<sw::redis::Redis> redis_;
    std::chrono::seconds window_;
    std::int64_t max_events_;
};

} // namespace nwdaf
