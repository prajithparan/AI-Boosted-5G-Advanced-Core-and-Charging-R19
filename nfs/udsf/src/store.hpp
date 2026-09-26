#pragma once

// UDSF persistence in Valkey (ADR-0400). Every piece of state -- records, blocks, tag indexes,
// notification subscriptions, meta schemas, timers, expiry schedules and the outbound
// notification queue -- lives here, so any number of UDSF replicas serve any request and the
// process holds nothing that a restart would lose.
//
// Key layout. P = "udsf:{<realmId>/<storageId>}" -- the braces are a Valkey Cluster hash tag, so
// every key of one storage hashes to one slot and the MULTI/EXEC transactions below stay legal
// on a cluster (Tier-1 scale mandate: a storage is the unit of sharding).
//
//   P:rec:<recordId>        HASH meta (RecordMeta JSON as received), etag, lm (epoch s)
//   P:blkd:<recordId>       HASH blockId -> block bytes (opaque, never re-encoded)
//   P:blkm:<recordId>       HASH blockId -> {"ct","cte","etag","lm"}
//   P:records               SET of recordIds
//   P:rtag:<n>:<tag>:<v>    SET recordIds whose meta.tags[tag] contains v   (n = len(tag), so a
//                           ':' inside a tag name cannot alias another tag/value pair)
//   P:rtagz:<n>:<tag>       ZSET "<v>\0<recordId>", score 0 -- ZRANGEBYLEX gives GT/GTE/LT/LTE
//   P:due:rec               ZSET recordId by ttl (epoch ms)
//   P:sub:<subscriptionId>  HASH body (NotificationSubscription JSON), etag, lm
//   P:subs                  SET of subscriptionIds
//   P:due:subexp / subnot   ZSET subscriptionId by expiry / expiry-notification time (epoch ms)
//   P:schema:<schemaId>     HASH body (MetaSchema JSON), etag, lm
//   P:tmr:<timerId>         HASH body (Timer JSON, no timerId), etag, lm, next (ms), left
//   P:timers                SET of timerIds
//   P:ttag... / P:ttagz...  as rtag/rtagz, over Timer.metaTags
//   P:tschema:<schemaId>    SET timerIds stored with that schemaId
//   P:due:tmr / tmrdel      ZSET timerId by next expiry / deletion time (epoch ms)
//   P:notifq                LIST of pending outbound notifications (JSON, see Notification)
//
// Concurrency: every write is an optimistic transaction -- WATCH the resource's own hash, read,
// decide, MULTI ... EXEC, retry on a lost race. Expiry work is claimed with ZREM (exactly one
// replica wins each due entry).
//
// DISCLOSED: RecordMeta.schemaId. TS 29.598 table 6.1.6.2.3-1 lists it, but in the R19 YAML the
// `schemaId:` lines sit inside RecordMeta's `example: >-` folded scalar, so the parsed schema has
// no such property (verified with a YAML parser, ADR-0400). The meta is stored verbatim as
// received, so a schemaId a consumer sends is kept and returned -- but it is NOT interpreted:
// with_schema() on the record index answers nullopt and the search layer turns that into a 400.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <sw/redis++/redis++.h>
#include <vector>

#include "search.hpp"

namespace udsf {

struct StorageRef {
    std::string realm;
    std::string storage;
    std::string prefix() const { return "udsf:{" + realm + "/" + storage + "}:"; }
};

struct Block {
    std::string id;
    std::string content_type;
    std::string cte;
    std::string etag;
    std::int64_t lm = 0;
    std::string data;
};

struct Record {
    std::string id;
    nlohmann::json meta = nlohmann::json::object();
    std::string etag;
    std::int64_t lm = 0;
    std::vector<Block> blocks;
    const Block* find_block(const std::string& id) const;
};

struct Doc { // subscriptions, meta schemas, timers
    std::string id;
    nlohmann::json body;
    std::string etag;
    std::int64_t lm = 0;
    // timers only
    std::int64_t next_ms = 0;
    std::int64_t left = 0;
};

// What a mutator decided after looking at the current state.
enum class Verdict { Write, Delete, Abort };

// A queued outbound notification. kind: "recordExpired" | "onDataChange" |
// "subscriptionExpiry" | "timerExpiry". The deliverer (main.cpp) turns it into the HTTP POST.
using Notification = nlohmann::json;

class Store {
public:
    Store(std::shared_ptr<sw::redis::Redis> redis, std::string dr_api_root);

    // ---- records ---------------------------------------------------------------------------
    std::optional<Record> get_record(const StorageRef& s, const std::string& id);
    // Read-modify-write of one record. `fn(current, next)`: current is the stored record (or
    // nullopt); `next` starts as a copy of current (or an empty record with the id set) and is
    // what gets written on Verdict::Write. May be called more than once (lost-race retry), so it
    // must only write to captured state it fully overwrites on each call. Blocks in `next` whose
    // etag equals the stored one are not rewritten. Data-change notifications for matching
    // subscriptions (TS 29.598 5.2.2.6.3) are queued in the same transaction.
    struct RecordTx {
        bool committed = false;
        std::optional<Record> before;
        Record after;
    };
    RecordTx modify_record(const StorageRef& s,
                           const std::string& id,
                           const std::function<Verdict(const std::optional<Record>&, Record&)>& fn);
    std::unique_ptr<TagIndex> record_index(const StorageRef& s);
    std::string record_uri(const StorageRef& s, const std::string& id) const;

    // ---- notification subscriptions -----------------------------------------------------------
    std::optional<Doc> get_sub(const StorageRef& s, const std::string& id);
    std::vector<Doc> list_subs(const StorageRef& s);
    struct DocTx {
        bool committed = false;
        std::optional<Doc> before;
        Doc after;
    };
    // `due_exp_ms` / `due_notify_ms` (0 = none) are scheduled with the write.
    DocTx modify_sub(const StorageRef& s,
                     const std::string& id,
                     const std::function<Verdict(const std::optional<Doc>&, Doc&)>& fn,
                     const std::function<std::pair<std::int64_t, std::int64_t>(const Doc&)>& due);

    // ---- meta schemas --------------------------------------------------------------------------
    std::optional<Doc> get_schema(const StorageRef& s, const std::string& id);
    DocTx modify_schema(const StorageRef& s,
                        const std::string& id,
                        const std::function<Verdict(const std::optional<Doc>&, Doc&)>& fn);
    std::int64_t timers_using_schema(const StorageRef& s, const std::string& schema_id);

    // ---- timers ---------------------------------------------------------------------------------
    std::optional<Doc> get_timer(const StorageRef& s, const std::string& id);
    // The write schedules P:due:tmr at next_ms (if left >= 0 and next_ms > 0) and P:due:tmrdel
    // at `delete_at_ms` returned by `del` (0 = none).
    DocTx modify_timer(const StorageRef& s,
                       const std::string& id,
                       const std::function<Verdict(const std::optional<Doc>&, Doc&)>& fn,
                       const std::function<std::int64_t(const Doc&)>& del);
    std::unique_ptr<TagIndex> timer_index(const StorageRef& s);

    // ---- expiry + delivery ----------------------------------------------------------------------
    // Claims up to `limit` members of P:due:<which> whose score <= now_ms (ZREM decides the
    // single winner among replicas).
    std::vector<std::string>
    claim_due(const StorageRef& s, const std::string& which, std::int64_t now_ms, long limit);
    void schedule(const StorageRef& s,
                  const std::string& which,
                  const std::string& member,
                  std::int64_t at_ms);
    void enqueue(const StorageRef& s, const Notification& n);
    std::vector<Notification> dequeue(const StorageRef& s, long max);

    // Test/diagnostic: deletes every key of one storage (SCAN MATCH P*). Never FLUSH.
    void purge_storage(const StorageRef& s);

private:
    std::optional<Record>
    load_record(sw::redis::Redis& r, const StorageRef& s, const std::string& id);
    std::optional<Doc> load_doc(sw::redis::Redis& r, const std::string& key, const std::string& id);
    std::vector<Notification> data_change_notifications(sw::redis::Redis& r,
                                                        const StorageRef& s,
                                                        const std::optional<Record>& before,
                                                        const std::optional<Record>& after);

    std::shared_ptr<sw::redis::Redis> redis_;
    std::string dr_api_root_;
};

// Tags of a RecordMeta / Timer.metaTags value: map(array(string)). Anything else yields {}.
std::vector<std::pair<std::string, std::string>> tag_pairs(const nlohmann::json& tags);

// Resolves a monitoredResourceUri (absolute URI or absolute-path reference, TS 29.598 table
// 6.1.6.2.13-1 NOTE 1: only the apiSpecificResourceUriPart counts) to realm/storage/recordId.
struct ResourceRef {
    std::string realm;
    std::string storage;
    std::string record_id;
};
std::optional<ResourceRef> parse_record_uri(const std::string& uri);

} // namespace udsf
