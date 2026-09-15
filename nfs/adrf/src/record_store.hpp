#pragma once

// The ADRF Data Store on Apache Doris (ADR-0367; engine per ADR-0359) -- TS 29.575 5.1's "ADRF
// Data Store Records" resource, one row per stored NadrfDataStoreRecord, schema in
// nfs/adrf/schema.doris.sql.
//
// A writer AND reader of the ADRF's own over the MySQL protocol (libmariadb C API), like the CHF's
// CdrWriter and the NWDAF's FeatureStore -- neither is reusable here (CLAUDE.md: no NF includes
// another's private headers) and neither does what a repository needs: point reads by id, reads
// by fingerprint/data set inside a time window, deletes by any of those, lifetime bookkeeping.
// One connection behind a mutex, re-opened on the first failure after a broken link -- the ADRF's
// request handlers run on several threads. Connection parameters come from config/adrf.json.

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

typedef struct st_mysql MYSQL;

namespace adrf {

struct RecordStoreOptions {
    std::string host;
    std::uint16_t port = 0; // from config; a literal default here would be a hardcoded port
    std::string user;
    std::string password;
    std::string database;
    std::int64_t max_rows = 0; // upper bound per window read, from config
};

struct StoredRecord {
    std::string store_trans_id;
    std::string kind;    // "analytics" | "data"
    std::string origin;  // "request" (StorageRequest) | "subscription" (a storage subscription)
    std::string spec_fp; // fingerprint of anaSub / dataSub (record_store.cpp: fingerprint())
    std::optional<std::string> data_set_id;
    std::chrono::system_clock::time_point collected_at;
    std::chrono::system_clock::time_point stored_at;
    std::chrono::system_clock::time_point expires_at;
    std::optional<std::string> del_notif_uri;
    std::optional<std::string> del_notif_corr_id;
    bool alert_sent = false;
    nlohmann::json record; // the NadrfDataStoreRecord as stored
};

struct TimeWindow {
    std::chrono::system_clock::time_point start;
    std::chrono::system_clock::time_point stop;
};

class RecordStore {
public:
    explicit RecordStore(const RecordStoreOptions& options);
    ~RecordStore();
    RecordStore(const RecordStore&) = delete;
    RecordStore& operator=(const RecordStore&) = delete;

    bool connected();

    // Every call throws std::runtime_error on a Doris error (the handler turns that into a 500).
    void insert(const StoredRecord& r);
    std::optional<StoredRecord> get(const std::string& store_trans_id);
    std::vector<StoredRecord> get_many(const std::vector<std::string>& ids);
    std::vector<StoredRecord> by_data_set(const std::string& data_set_id,
                                          const std::optional<TimeWindow>& window);
    std::vector<StoredRecord> by_fingerprint(const std::string& spec_fp,
                                             const std::optional<TimeWindow>& window);
    // true when the row existed.
    bool remove(const std::string& store_trans_id);
    // Rows matching `spec_fp` or `data_set_id` (exactly one given) inside the window.
    std::int64_t remove_matching(const std::optional<std::string>& spec_fp,
                                 const std::optional<std::string>& data_set_id,
                                 const TimeWindow& window);

    // Lifetime bookkeeping for the reaper (main.cpp): rows expiring before `before` whose deletion
    // alert has not been sent and that name a delNotifUri; then the outcomes.
    std::vector<StoredRecord> awaiting_alert(std::chrono::system_clock::time_point before);
    void mark_alert_sent(const std::string& store_trans_id);
    void defer_expiry(const std::string& store_trans_id,
                      std::chrono::system_clock::time_point new_expiry);
    std::int64_t remove_expired(std::chrono::system_clock::time_point now);

private:
    bool ensure_connected_locked();
    void exec_locked(const std::string& sql);
    std::vector<StoredRecord> select_locked(const std::string& where_clause);
    std::string quote_locked(const std::string& s);

    RecordStoreOptions options_;
    std::mutex mutex_;
    MYSQL* conn_ = nullptr;
};

// Doris DATETIME text (UTC, second precision) <-> time_point.
std::string doris_datetime(std::chrono::system_clock::time_point tp);
std::chrono::system_clock::time_point parse_doris_datetime(const std::string& text);

// The dedup / matching key: canonical JSON (sorted keys) of `spec` with the notification-target
// fields removed, hashed (FNV-1a 64, hex). `spec` is an anaSub (NnwdafEventsSubscription) or a
// dataSub (TS 29.575 DataSubscription, one source alternative) -- a RetrievalSubscribe naming the
// same spec as the storage subscription that produced the records lands on the same fingerprint.
// Same construction as the DCCF's (ADR-0366) so the two NFs agree on "the same data".
std::string fingerprint(const std::string& kind, const nlohmann::json& spec);

} // namespace adrf
