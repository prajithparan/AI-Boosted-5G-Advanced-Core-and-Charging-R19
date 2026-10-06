#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "nf_config/pg_pool.hpp"

// The ADMF's warrant store (ADR-0462 decision 2): its own PostgreSQL, schema in
// nfs/li-admf/schema.sql. Every HI1 object is kept as its own self-contained XML element -- GET
// returns exactly what the LEA created -- with a few typed columns as lookup/LIST projections.
// Connections come from the shared bounded PgPool (acquire deadline, exhaustion metric).
namespace li_admf {

struct StoredObject {
    std::string object_id;
    std::string object_type; // Authorisation | LITask | Document | Notification | Other
    std::string owner_identifier;
    std::string country_code;
    std::uint64_t generation = 0;
    std::string external_id;
    std::string
        authorisation_id; // the Authorisation a task/document/notification belongs to ("" if none)
    std::string status;   // dictionary Value of the object's status ("" if none)
    std::string lea;      // "<CountryCode>/<UniqueIdentifier>" of the owning LEA
    std::string last_txn; // TransactionIdentifier of the request that last changed it
    std::string xml;
    std::string last_changed; // QualifiedDateTime (UTC), filled by the database on read
};

struct ListFilter {
    std::string object_type;      // "" = any
    std::string status;           // "" = any
    std::string country_code;     // "" = any
    std::string authorisation_id; // "" = any
    std::string lea;              // "" = any LEA (internal use); set it to scope a query to one LEA
    std::string changed_since;    // QualifiedDateTime, "" = any
    std::uint64_t maximum = 0;    // 0 = no limit
};

// What the LIPF has provisioned for one LI task (table lipf_task).
struct LipfTaskState {
    std::string object_id;
    std::uint64_t provisioned_generation = 0;
    std::vector<std::string> destinations; // "ip:port" addresses provisioned on the MDF2
    std::string state;                     // provisioned | deprovisioned | failed
    std::uint64_t age_seconds = 0;         // since the row was last written
};

struct AuditEntry {
    std::string peer;
    std::string path;
    std::string transaction_id;
    std::string sender;
    int actions = 0;
    std::string outcome; // ok | rejected | error
    std::string detail;
};

// One write of an all-or-nothing batch (Hi1Store::apply).
struct StoreOp {
    enum class Kind { Insert, Replace } kind = Kind::Insert;
    StoredObject object;
    std::uint64_t expected_generation = 0; // Replace only
};

class Hi1Store {
public:
    explicit Hi1Store(nf_config::PgPool& pool) : pool_(pool) {}

    // Create the schema if it does not exist (nfs/li-admf/schema.sql is the canonical copy and is
    // what the compose database initialises from; this makes a bare database usable too).
    void ensure_schema();

    // false if an object with this id already exists (the caller maps that to error 3010).
    bool insert(const StoredObject& object);
    // Every op in ONE transaction: all apply or none do (a request that cannot be applied in full
    // must change nothing, TS 103 120 H.5.2.2.3). false on any conflict (duplicate id, stale
    // generation).
    bool apply(const std::vector<StoreOp>& ops);
    [[nodiscard]] std::optional<StoredObject> get(const std::string& object_id);
    // Optimistic concurrency: replaces only if the stored generation is still
    // `expected_generation`. false if the object is gone or was changed by someone else meanwhile.
    bool replace(const StoredObject& object, std::uint64_t expected_generation);
    [[nodiscard]] std::vector<StoredObject> list(const ListFilter& filter);

    [[nodiscard]] std::optional<LipfTaskState> lipf_state(const std::string& object_id);
    void set_lipf_state(const LipfTaskState& state);

    void audit(const AuditEntry& entry);
    [[nodiscard]] std::size_t audit_count();

private:
    nf_config::PgPool& pool_;
};

} // namespace li_admf
