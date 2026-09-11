#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <memory>
#include <mutex>
#include <mysql.h>
#include <optional>
#include <string>
#include <vector>

// Private to nfs/chf -- not shared with any other NF, per CLAUDE.md's "no NF includes another
// NF's private headers" rule.
//
// P4.4/ADR-0058: CDF (Charging Data Function, TS 32.240/32.296) -- real CDR generation, per
// CHARGING_PROMPT.md's P4.4 real requirements ("duplicate detection, gap detection...mandatory").
// See ../schema.doris.sql's own header for the full, honest disclosure of what this is NOT (a
// conformant TS 32.298 CDR -- that spec isn't vendored) and what it IS (a real, working usage
// record built entirely from TS 32.291 fields already confirmed and flowing through this file).
//
// ADR-0192: CDR storage is Apache Doris (that ADR records the migration, the governance concern
// that drove it, and the full engine comparison). Doris speaks the real MySQL wire protocol, so
// this uses `libmariadb`'s own real, plain C client API (`MYSQL*`,
// `mysql_real_query`/`mysql_real_escape_string`, `mysql_store_result`/`mysql_fetch_row`) --
// deliberately NOT the also-real `mariadb-connector-cpp` package, whose own C++ API mirrors
// JDBC's class shape (`Connection`/`PreparedStatement`/`ResultSet`) -- explicit, user-directed:
// no Java-flavored API surface anywhere in this project's own C/C++ code, even where the
// underlying library has zero actual Java/JVM dependency. Plain `mysql_real_query` with
// `mysql_real_escape_string`-escaped values (not prepared statements) is used deliberately: OLAP
// engines like Doris are not optimized for high-frequency single-row prepared-statement execution
// the way OLTP engines are, and this project's own CDR write is a single-row-at-a-time real-time
// path, not a batch loader -- escaped plain SQL is the real, idiomatic choice here, not a
// shortcut.
//
// Real bug found via live verification, not caught by reasoning alone (this project's own prior
// finding, ADR-0058, still true after the ADR-0192 migration): a database client's own connect
// call can fail at construction/startup, and that failure must never be able to crash or block
// the higher-priority real-time charging/balance-reservation path this same file already treats
// as best-effort at the per-write level (see `write()`'s own comment). `CdrWriter`'s constructor
// catches connection failure and degrades to a real, logged "CDR generation disabled" state --
// `write()`/`detect_gaps()` become safe no-ops (with a warning) rather than every call site
// needing its own null-check.

namespace chf {

struct CdrRecord {
    std::string charging_data_ref;
    std::int64_t invocation_sequence_number = 0;
    std::string service_type; // "ConvergedCharging" | "OfflineOnlyCharging" (project-internal)
    std::string operation;    // "Create" | "Update" | "Release" (project-internal)
    std::string subscriber_identifier;
    std::string nf_consumer_node_functionality;
    // Gap-closure (task #108, ADR-0089): this CHF instance's own UUID -- real TS 32.298 field
    // [1] `recordingNetworkFunctionID` needs it. Not used by the pre-existing Doris columns
    // below (unaffected); read only by cdr_asn1.cpp's own encode_chf_cdr.
    std::string recording_network_function_id;
    std::optional<std::int64_t> rating_group;
    std::optional<std::uint64_t> granted_total_volume;
    std::optional<std::uint64_t> granted_service_specific_units;
    std::optional<std::uint64_t> used_total_volume;
    std::optional<double> reserved_cost;
    std::optional<std::string> reserved_cost_currency;
    std::time_t invocation_time_stamp = 0;
    // ADR-0311: who served this usage, and whether that made it roaming. Both come from the
    // attributes ADR-0303/ADR-0305 already collect off the real TS 32.291 request -- this is the
    // first time they are PERSISTED, which is what makes a roaming CDR findable later.
    std::string serving_plmn; // "<mcc>-<mnc>", empty when the request carried none
    bool is_roaming = false;
};

// Real connection parameters for Doris's own FE MySQL-protocol query port (default 9030, real
// port confirmed from apache/doris's own official all-in-one Docker image docs -- ADR-0192).
struct DorisOptions {
    // ADR-0338: rows buffered before a multi-row INSERT is issued. 1 = every write goes to Doris
    // immediately, which is the behaviour every deployment has had until now and remains the
    // DEFAULT. Values > 1 trade durability for throughput and must be chosen deliberately -- see
    // CdrWriter::write's own comment for exactly what is at risk.
    int batch_size = 1;
    // Maximum time a buffered row may wait before being flushed regardless of batch fullness, so
    // a quiet period cannot strand a CDR in memory indefinitely.
    int flush_interval_ms = 1000;
    std::string host;
    std::uint16_t port = 9030;
    std::string user;
    std::string password;
    std::string database;
};

class CdrWriter {
public:
    explicit CdrWriter(const DorisOptions& options);
    ~CdrWriter();
    CdrWriter(const CdrWriter&) = delete;
    CdrWriter& operator=(const CdrWriter&) = delete;

    // Real INSERT into Doris's `cdr` table (schema: ../schema.doris.sql). `MYSQL*` is not
    // documented as thread-safe for concurrent use from multiple threads the way
    // `sw::redis::Redis` is (confirmed when nfs/chf's Redis stores were built, ADR-0055). Real
    // concurrency requirement carried over unchanged from the pre-migration version
    // (P4.5/ADR-0060 Stage 3): `mutex_` serializes CHF's HTTP io_context thread and the Diameter
    // Gy CCR path's own dedicated thread, both sharing this one connection.
    void write(const CdrRecord& record);

    // Real gap detection (CHARGING_PROMPT.md's own explicit P4.4 requirement): queries every
    // invocation_sequence_number recorded for charging_data_ref, and returns the list of missing
    // values in the contiguous range [min_seen, max_seen] -- e.g. sequences {1,2,4,5} returns
    // {3}. Returns an empty vector if fewer than 2 distinct sequence numbers exist (no range to
    // have a gap in) or no gap is found.
    std::vector<std::int64_t> detect_gaps(const std::string& charging_data_ref);

    // ADR-0311: the query both billing and roaming settlement were missing.
    //
    // Before this, `CdrWriter` could only INSERT, count sequence gaps, and sweep old rows. Nothing
    // could read a CDR back, which is why `billing::run_bill` had no way to obtain line items and
    // why TAP OUT had no way to select a partner's usage -- both were disclosed as "nothing
    // selects the rows" in ADR-0306 and ADR-0310.
    //
    // Only `Release` rows are returned. A session writes Create/Update/Release rows and the
    // reserved cost accumulates across them; billing every row would charge a session several
    // times over. Release is the row that closes a session, so it is the one a bill or a TAP batch
    // is built from -- stated here because it is the difference between a correct invoice and a
    // multiplied one.
    struct CdrQuery {
        std::string period_start;                         // "YYYY-MM-DD HH:MM:SS", inclusive
        std::string period_end;                           // exclusive
        std::optional<std::string> subscriber_identifier; // for a bill run
        std::optional<std::string> serving_plmn;          // for a TAP OUT batch
        std::optional<bool> is_roaming;
    };
    std::vector<CdrRecord> query(const CdrQuery& q);

    // ADR-0338: force any buffered rows out now. Called by the destructor and by the retention
    // sweep; exposed because a caller that is about to read its own writes needs them visible.
    // Returns the number of rows flushed.
    std::size_t flush();

    // P14 (ADR-0283): retention-driven archival. Archives every `cdr` row older than
    // `retention_days` into `archive_dir` as newline-delimited JSON, then deletes ONLY the rows it
    // successfully archived.
    //
    // Order is the whole design. These are billing records: deleting one that was not archived
    // destroys revenue evidence, so nothing is deleted until its archive file is written and
    // flushed. A failed archive means nothing is deleted this cycle and the sweep is retried next
    // time -- data kept twice is a storage cost, data deleted once is gone.
    //
    // Returns the number of rows archived-and-deleted. 0 with no error means nothing was old
    // enough. retention_days <= 0 disables the sweep entirely (the default).
    struct RetentionResult {
        std::int64_t archived = 0;
        std::int64_t deleted = 0;
        bool failed = false;
    };
    RetentionResult apply_retention(int retention_days, const std::string& archive_dir);

    // True if the constructor connected successfully. False means CDR generation is disabled for
    // this process's lifetime (see this file's own header) -- callers use this only for an
    // accurate startup log line, not as a precondition check before write()/detect_gaps(), which
    // are already safe to call regardless.
    bool is_connected() const { return conn_ != nullptr; }

private:
    std::mutex mutex_;
    // nullptr if construction failed to connect -- see this file's own header for why that's a
    // real, deliberate degraded state, not an error CdrWriter itself surfaces to its caller.
    // Raw MYSQL* (libmariadb's own C handle type, not RAII-wrappable via unique_ptr without a
    // custom deleter) -- freed explicitly in the destructor via mysql_close(), the real, correct
    // C-API cleanup call, same "own the raw C handle, free it in our own dtor" pattern this
    // project already uses for other C libraries.
    MYSQL* conn_ = nullptr;

    // ADR-0338: batching state. `pending_` holds fully-rendered VALUES tuples, not CdrRecords --
    // rendering (including BER encoding) happens at write() time so a flush is a string join
    // rather than a second pass over the records, and so a record's escaping uses the connection
    // it was written against.
    int batch_size_ = 1;
    std::chrono::milliseconds flush_interval_{1000};
    std::vector<std::string> pending_;
    std::chrono::steady_clock::time_point last_flush_ = std::chrono::steady_clock::now();

    // Caller must hold mutex_. Split out because write(), flush() and the destructor all need it
    // and only one of them may take the lock.
    std::size_t flush_locked();
};

} // namespace chf
