#include "hi1_store.hpp"

#include <pqxx/pqxx>

namespace li_admf {

namespace {

constexpr const char* kSelectColumns =
    "object_id, object_type, owner_identifier, country_code, generation, external_id, "
    "authorisation_id, status, xml, "
    "to_char(last_changed AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"')";

// pqxx 8 hands out proxy types (field_ref / row_ref), so these are templates, not pqxx::field / row.
template <typename Field>
std::string text_or_empty(const Field& f) {
    return f.template as<std::optional<std::string>>().value_or(std::string());
}

template <typename Row>
StoredObject from_row(const Row& row) {
    StoredObject o;
    o.object_id = row[0].template as<std::string>();
    o.object_type = row[1].template as<std::string>();
    o.owner_identifier = text_or_empty(row[2]);
    o.country_code = text_or_empty(row[3]);
    o.generation = row[4].template as<std::uint64_t>();
    o.external_id = text_or_empty(row[5]);
    o.authorisation_id = text_or_empty(row[6]);
    o.status = text_or_empty(row[7]);
    o.xml = row[8].template as<std::string>();
    o.last_changed = text_or_empty(row[9]);
    return o;
}

// "" -> NULL, so an absent projection is a NULL column rather than an empty string.
std::optional<std::string> opt(const std::string& s) {
    return s.empty() ? std::nullopt : std::optional<std::string>(s);
}

} // namespace

void Hi1Store::ensure_schema() {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    txn.exec(R"sql(
CREATE TABLE IF NOT EXISTS hi1_object (
    object_id TEXT PRIMARY KEY, object_type TEXT NOT NULL, owner_identifier TEXT, country_code TEXT,
    generation BIGINT NOT NULL DEFAULT 0, external_id TEXT, authorisation_id TEXT, status TEXT,
    xml TEXT NOT NULL, created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_changed TIMESTAMPTZ NOT NULL DEFAULT now());
CREATE INDEX IF NOT EXISTS hi1_object_type_changed ON hi1_object (object_type, last_changed);
CREATE INDEX IF NOT EXISTS hi1_object_authorisation ON hi1_object (authorisation_id);
CREATE TABLE IF NOT EXISTS hi1_audit (
    seq BIGSERIAL PRIMARY KEY, at TIMESTAMPTZ NOT NULL DEFAULT now(), peer TEXT NOT NULL,
    path TEXT NOT NULL, transaction_id TEXT, sender TEXT, actions INTEGER NOT NULL DEFAULT 0,
    outcome TEXT NOT NULL, detail TEXT);
)sql");
    txn.commit();
}

bool Hi1Store::insert(const StoredObject& o) {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    const auto result = txn.exec(
        "INSERT INTO hi1_object (object_id, object_type, owner_identifier, country_code, generation, "
        "external_id, authorisation_id, status, xml) VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9) "
        "ON CONFLICT (object_id) DO NOTHING",
        pqxx::params{o.object_id,
                     o.object_type,
                     opt(o.owner_identifier),
                     opt(o.country_code),
                     o.generation,
                     opt(o.external_id),
                     opt(o.authorisation_id),
                     opt(o.status),
                     o.xml});
    txn.commit();
    return result.affected_rows() == 1;
}

std::optional<StoredObject> Hi1Store::get(const std::string& object_id) {
    auto lease = pool_.acquire();
    pqxx::nontransaction txn(lease.conn());
    const auto rows = txn.exec(std::string("SELECT ") + kSelectColumns +
                                   " FROM hi1_object WHERE object_id = $1",
                               pqxx::params{object_id});
    if (rows.empty()) {
        return std::nullopt;
    }
    return from_row(rows[0]);
}

bool Hi1Store::replace(const StoredObject& o, std::uint64_t expected_generation) {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    const auto result = txn.exec(
        "UPDATE hi1_object SET object_type=$2, owner_identifier=$3, country_code=$4, generation=$5, "
        "external_id=$6, authorisation_id=$7, status=$8, xml=$9, last_changed=now() "
        "WHERE object_id=$1 AND generation=$10",
        pqxx::params{o.object_id,
                     o.object_type,
                     opt(o.owner_identifier),
                     opt(o.country_code),
                     o.generation,
                     opt(o.external_id),
                     opt(o.authorisation_id),
                     opt(o.status),
                     o.xml,
                     expected_generation});
    txn.commit();
    return result.affected_rows() == 1;
}

std::vector<StoredObject> Hi1Store::list(const ListFilter& f) {
    // Every filter is a bound parameter; the SQL text only ever grows by fixed fragments.
    std::string sql = std::string("SELECT ") + kSelectColumns + " FROM hi1_object WHERE TRUE";
    pqxx::params params;
    const auto add = [&](const char* column, const std::string& value) {
        if (!value.empty()) {
            params.append(value);
            sql += std::string(" AND ") + column + " = $" + std::to_string(params.size());
        }
    };
    add("object_type", f.object_type);
    add("status", f.status);
    add("country_code", f.country_code);
    add("authorisation_id", f.authorisation_id);
    if (!f.changed_since.empty()) {
        params.append(f.changed_since);
        sql += " AND last_changed >= $" + std::to_string(params.size()) + "::timestamptz";
    }
    sql += " ORDER BY last_changed, object_id";
    if (f.maximum > 0) {
        params.append(f.maximum);
        sql += " LIMIT $" + std::to_string(params.size());
    }
    auto lease = pool_.acquire();
    pqxx::nontransaction txn(lease.conn());
    std::vector<StoredObject> out;
    for (const auto& row : txn.exec(sql, params)) {
        out.push_back(from_row(row));
    }
    return out;
}

void Hi1Store::audit(const AuditEntry& e) {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    txn.exec("INSERT INTO hi1_audit (peer, path, transaction_id, sender, actions, outcome, detail) "
             "VALUES ($1,$2,$3,$4,$5,$6,$7)",
             pqxx::params{e.peer, e.path, opt(e.transaction_id), opt(e.sender), e.actions, e.outcome, opt(e.detail)});
    txn.commit();
}

std::size_t Hi1Store::audit_count() {
    auto lease = pool_.acquire();
    pqxx::nontransaction txn(lease.conn());
    return txn.exec("SELECT count(*) FROM hi1_audit")[0][0].as<std::size_t>();
}

} // namespace li_admf
