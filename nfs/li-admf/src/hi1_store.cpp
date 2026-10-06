#include "hi1_store.hpp"

#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

namespace li_admf {

namespace {

constexpr const char* kSelectColumns =
    "object_id, object_type, owner_identifier, country_code, generation, external_id, "
    "authorisation_id, status, xml, lea, last_txn, "
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
    o.lea = text_or_empty(row[9]);
    o.last_txn = text_or_empty(row[10]);
    o.last_changed = text_or_empty(row[11]);
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
    lea TEXT, last_txn TEXT,
    xml TEXT NOT NULL, created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_changed TIMESTAMPTZ NOT NULL DEFAULT now());
ALTER TABLE hi1_object ADD COLUMN IF NOT EXISTS lea TEXT;
ALTER TABLE hi1_object ADD COLUMN IF NOT EXISTS last_txn TEXT;
CREATE TABLE IF NOT EXISTS lipf_task (
    object_id TEXT PRIMARY KEY, provisioned_generation BIGINT NOT NULL,
    destinations TEXT NOT NULL DEFAULT '[]', state TEXT NOT NULL,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now());
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
        "external_id, authorisation_id, status, xml, lea, last_txn) "
        "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11) ON CONFLICT (object_id) DO NOTHING",
        pqxx::params{o.object_id,
                     o.object_type,
                     opt(o.owner_identifier),
                     opt(o.country_code),
                     o.generation,
                     opt(o.external_id),
                     opt(o.authorisation_id),
                     opt(o.status),
                     o.xml,
                     opt(o.lea),
                     opt(o.last_txn)});
    txn.commit();
    return result.affected_rows() == 1;
}

bool Hi1Store::apply(const std::vector<StoreOp>& ops) {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    for (const auto& op : ops) {
        const auto& o = op.object;
        if (op.kind == StoreOp::Kind::Insert) {
            const auto r = txn.exec(
                "INSERT INTO hi1_object (object_id, object_type, owner_identifier, country_code, generation, "
                "external_id, authorisation_id, status, xml, lea, last_txn) "
                "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11) ON CONFLICT (object_id) DO NOTHING",
                pqxx::params{o.object_id, o.object_type, opt(o.owner_identifier), opt(o.country_code),
                             o.generation, opt(o.external_id), opt(o.authorisation_id), opt(o.status), o.xml,
                             opt(o.lea), opt(o.last_txn)});
            if (r.affected_rows() != 1) {
                return false; // the transaction rolls back when `txn` goes out of scope uncommitted
            }
        } else {
            const auto r = txn.exec(
                "UPDATE hi1_object SET object_type=$2, owner_identifier=$3, country_code=$4, generation=$5, "
                "external_id=$6, authorisation_id=$7, status=$8, xml=$9, lea=$10, last_txn=$11, "
                "last_changed=now() WHERE object_id=$1 AND generation=$12",
                pqxx::params{o.object_id, o.object_type, opt(o.owner_identifier), opt(o.country_code),
                             o.generation, opt(o.external_id), opt(o.authorisation_id), opt(o.status), o.xml,
                             opt(o.lea), opt(o.last_txn), op.expected_generation});
            if (r.affected_rows() != 1) {
                return false;
            }
        }
    }
    txn.commit();
    return true;
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
        "external_id=$6, authorisation_id=$7, status=$8, xml=$9, lea=$10, last_txn=$11, "
        "last_changed=now() WHERE object_id=$1 AND generation=$12",
        pqxx::params{o.object_id,
                     o.object_type,
                     opt(o.owner_identifier),
                     opt(o.country_code),
                     o.generation,
                     opt(o.external_id),
                     opt(o.authorisation_id),
                     opt(o.status),
                     o.xml,
                     opt(o.lea),
                     opt(o.last_txn),
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
    add("lea", f.lea);
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

std::optional<LipfTaskState> Hi1Store::lipf_state(const std::string& object_id) {
    auto lease = pool_.acquire();
    pqxx::nontransaction txn(lease.conn());
    const auto rows = txn.exec(
        "SELECT object_id, provisioned_generation, destinations, state, "
        "extract(epoch from (now() - updated_at))::bigint FROM lipf_task WHERE object_id = $1",
        pqxx::params{object_id});
    if (rows.empty()) {
        return std::nullopt;
    }
    LipfTaskState st;
    st.object_id = rows[0][0].template as<std::string>();
    st.provisioned_generation = rows[0][1].template as<std::uint64_t>();
    const auto parsed = nlohmann::json::parse(rows[0][2].template as<std::string>(), nullptr, false);
    if (parsed.is_array()) {
        for (const auto& a : parsed) {
            st.destinations.push_back(a.get<std::string>());
        }
    }
    st.state = rows[0][3].template as<std::string>();
    st.age_seconds = rows[0][4].template as<std::uint64_t>();
    return st;
}

void Hi1Store::set_lipf_state(const LipfTaskState& st) {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    txn.exec("INSERT INTO lipf_task (object_id, provisioned_generation, destinations, state) "
             "VALUES ($1,$2,$3,$4) ON CONFLICT (object_id) DO UPDATE SET "
             "provisioned_generation=$2, destinations=$3, state=$4, updated_at=now()",
             pqxx::params{st.object_id, st.provisioned_generation, nlohmann::json(st.destinations).dump(), st.state});
    txn.commit();
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
