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
    std::string authorisation_id; // the Authorisation a task/document/notification belongs to ("" if none)
    std::string status;           // dictionary Value of the object's status ("" if none)
    std::string xml;
    std::string last_changed; // QualifiedDateTime (UTC), filled by the database on read
};

struct ListFilter {
    std::string object_type;      // "" = any
    std::string status;           // "" = any
    std::string country_code;     // "" = any
    std::string authorisation_id; // "" = any
    std::string changed_since;    // QualifiedDateTime, "" = any
    std::uint64_t maximum = 0;    // 0 = no limit
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

class Hi1Store {
public:
    explicit Hi1Store(nf_config::PgPool& pool) : pool_(pool) {}

    // Create the schema if it does not exist (nfs/li-admf/schema.sql is the canonical copy and is
    // what the compose database initialises from; this makes a bare database usable too).
    void ensure_schema();

    // false if an object with this id already exists (the caller maps that to error 3010).
    bool insert(const StoredObject& object);
    [[nodiscard]] std::optional<StoredObject> get(const std::string& object_id);
    // Optimistic concurrency: replaces only if the stored generation is still `expected_generation`.
    // false if the object is gone or was changed by someone else meanwhile.
    bool replace(const StoredObject& object, std::uint64_t expected_generation);
    [[nodiscard]] std::vector<StoredObject> list(const ListFilter& filter);

    void audit(const AuditEntry& entry);
    [[nodiscard]] std::size_t audit_count();

private:
    nf_config::PgPool& pool_;
};

} // namespace li_admf
