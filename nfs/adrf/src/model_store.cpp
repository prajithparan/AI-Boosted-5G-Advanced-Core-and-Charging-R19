#include "model_store.hpp"

#include <spdlog/spdlog.h>

#include <pqxx/pqxx>
#include <stdexcept>

namespace adrf {

ModelStore::ModelStore(const std::string& conninfo) : conninfo_(conninfo) {
    const std::lock_guard<std::mutex> lock(mutex_);
    ensure_connected_locked();
}

ModelStore::~ModelStore() = default;

bool ModelStore::connected() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return ensure_connected_locked();
}

bool ModelStore::ensure_connected_locked() {
    if (conn_ && conn_->is_open()) {
        return true;
    }
    try {
        conn_ = std::make_unique<pqxx::connection>(conninfo_);
        spdlog::info("adrf: connected to the ML model store");
        return true;
    } catch (const std::exception& e) {
        spdlog::warn("adrf: ML model store unreachable: {}", e.what());
        conn_.reset();
        return false;
    }
}

namespace {

template <typename F> auto with_txn(std::unique_ptr<pqxx::connection>& conn, F&& f) {
    try {
        pqxx::work txn(*conn);
        auto out = f(txn);
        txn.commit();
        return out;
    } catch (const pqxx::broken_connection& e) {
        conn.reset();
        throw std::runtime_error(std::string("ML model store connection lost: ") + e.what());
    } catch (const pqxx::sql_error& e) {
        throw std::runtime_error(std::string("ML model store statement failed: ") + e.what());
    }
}

// pqxx 8 iterates results as row_ref and front() returns one too; both convert to a row.
StoredModel row_to_model(const pqxx::row& r) {
    StoredModel m;
    m.model_unique_id = r["model_unique_id"].as<std::int64_t>();
    m.store_trans_id = r["store_trans_id"].as<std::string>();
    if (!r["owner_nf_instance_id"].is_null()) {
        m.owner_nf_instance_id = r["owner_nf_instance_id"].as<std::string>();
    }
    if (!r["owner_nf_set_id"].is_null()) {
        m.owner_nf_set_id = r["owner_nf_set_id"].as<std::string>();
    }
    if (!r["source_addr"].is_null()) {
        m.source_addr = nlohmann::json::parse(r["source_addr"].as<std::string>());
    }
    if (!r["allow_consumers"].is_null()) {
        m.allow_consumers = nlohmann::json::parse(r["allow_consumers"].as<std::string>());
    }
    m.storage_size = r["storage_size"].as<std::int64_t>();
    return m;
}

constexpr const char* kModelColumns =
    "m.model_unique_id, m.store_trans_id, r.owner_nf_instance_id, r.owner_nf_set_id, "
    "m.source_addr::text AS source_addr, m.allow_consumers::text AS allow_consumers, "
    "m.storage_size";

} // namespace

void ModelStore::create_record(const std::string& id,
                               const std::optional<std::string>& owner_nf_instance_id,
                               const std::optional<std::string>& owner_nf_set_id) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!ensure_connected_locked()) {
        throw std::runtime_error("ML model store unreachable");
    }
    with_txn(conn_, [&](pqxx::work& txn) {
        txn.exec("INSERT INTO ml_store_records (store_trans_id, owner_nf_instance_id, "
                 "owner_nf_set_id) VALUES ($1, $2, $3) ON CONFLICT (store_trans_id) DO UPDATE SET "
                 "updated_at = now()",
                 pqxx::params{id, owner_nf_instance_id, owner_nf_set_id});
        return 0;
    });
}

bool ModelStore::record_exists(const std::string& id) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!ensure_connected_locked()) {
        throw std::runtime_error("ML model store unreachable");
    }
    return with_txn(conn_, [&](pqxx::work& txn) {
        return !txn.exec("SELECT 1 FROM ml_store_records WHERE store_trans_id = $1",
                         pqxx::params{id})
                    .empty();
    });
}

void ModelStore::upsert_model(const StoredModel& m, const std::string& bytes) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!ensure_connected_locked()) {
        throw std::runtime_error("ML model store unreachable");
    }
    with_txn(conn_, [&](pqxx::work& txn) {
        const std::optional<std::string> source_addr =
            m.source_addr ? std::optional<std::string>(m.source_addr->dump()) : std::nullopt;
        const std::optional<std::string> allow =
            m.allow_consumers ? std::optional<std::string>(m.allow_consumers->dump())
                              : std::nullopt;
        const pqxx::bytes_view blob(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
        txn.exec("INSERT INTO ml_models (model_unique_id, store_trans_id, source_addr, "
                 "allow_consumers, storage_size, model_bytes) VALUES ($1, $2, $3::jsonb, "
                 "$4::jsonb, $5, $6) ON CONFLICT (model_unique_id) DO UPDATE SET store_trans_id = "
                 "EXCLUDED.store_trans_id, source_addr = EXCLUDED.source_addr, allow_consumers = "
                 "EXCLUDED.allow_consumers, storage_size = EXCLUDED.storage_size, model_bytes = "
                 "EXCLUDED.model_bytes, stored_at = now()",
                 pqxx::params{m.model_unique_id,
                              m.store_trans_id,
                              source_addr,
                              allow,
                              static_cast<std::int64_t>(bytes.size()),
                              blob});
        return 0;
    });
}

std::vector<StoredModel> ModelStore::models_in_record(const std::string& id) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!ensure_connected_locked()) {
        throw std::runtime_error("ML model store unreachable");
    }
    return with_txn(conn_, [&](pqxx::work& txn) {
        std::vector<StoredModel> out;
        for (const auto& r : txn.exec(std::string("SELECT ") + kModelColumns +
                                          " FROM ml_models m JOIN ml_store_records r USING "
                                          "(store_trans_id) WHERE m.store_trans_id = $1 ORDER BY "
                                          "m.model_unique_id",
                                      pqxx::params{id})) {
            out.push_back(row_to_model(pqxx::row(r)));
        }
        return out;
    });
}

std::vector<StoredModel> ModelStore::models_by_ids(const std::vector<std::int64_t>& ids) {
    if (ids.empty()) {
        return {};
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!ensure_connected_locked()) {
        throw std::runtime_error("ML model store unreachable");
    }
    return with_txn(conn_, [&](pqxx::work& txn) {
        std::vector<StoredModel> out;
        for (const auto& r : txn.exec(std::string("SELECT ") + kModelColumns +
                                          " FROM ml_models m JOIN ml_store_records r USING "
                                          "(store_trans_id) WHERE m.model_unique_id = ANY($1) "
                                          "ORDER BY m.model_unique_id",
                                      pqxx::params{ids})) {
            out.push_back(row_to_model(pqxx::row(r)));
        }
        return out;
    });
}

std::optional<StoredModel> ModelStore::get_bytes(std::int64_t id) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!ensure_connected_locked()) {
        throw std::runtime_error("ML model store unreachable");
    }
    return with_txn(conn_, [&](pqxx::work& txn) -> std::optional<StoredModel> {
        const auto rows = txn.exec(std::string("SELECT ") + kModelColumns +
                                       ", m.model_bytes FROM ml_models m JOIN ml_store_records r "
                                       "USING (store_trans_id) WHERE m.model_unique_id = $1",
                                   pqxx::params{id});
        if (rows.empty()) {
            return std::nullopt;
        }
        StoredModel m = row_to_model(pqxx::row(rows.front()));
        const auto blob = rows.front()["model_bytes"].as<pqxx::bytes>();
        m.bytes.assign(reinterpret_cast<const char*>(blob.data()), blob.size());
        return m;
    });
}

bool ModelStore::remove_model(std::int64_t id) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!ensure_connected_locked()) {
        throw std::runtime_error("ML model store unreachable");
    }
    return with_txn(conn_, [&](pqxx::work& txn) {
        return txn.exec("DELETE FROM ml_models WHERE model_unique_id = $1", pqxx::params{id})
                   .affected_rows() > 0;
    });
}

bool ModelStore::remove_record(const std::string& id) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!ensure_connected_locked()) {
        throw std::runtime_error("ML model store unreachable");
    }
    return with_txn(conn_, [&](pqxx::work& txn) {
        return txn.exec("DELETE FROM ml_store_records WHERE store_trans_id = $1", pqxx::params{id})
                   .affected_rows() > 0;
    });
}

} // namespace adrf
